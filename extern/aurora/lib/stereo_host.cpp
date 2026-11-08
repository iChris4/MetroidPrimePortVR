#include "stereo_host.hpp"

#include "internal.hpp"
#include "stereo_overlay.hpp"
#include "gfx/recording.hpp"
#include "gfx/render_worker.hpp"
#include "gfx/stereo_eyes.hpp"
#include "gfx/stereo_foveation.hpp"
#include "gfx/stereo_multiview.hpp"
#include "gfx/stereo_replay.hpp"
#include "gfx/floating_hud.hpp"
#include "gfx/stereo_shadow.hpp"
#include "gx/fifo.hpp"
#include "webgpu/fdm.hpp"
#include "webgpu/gpu.hpp"

#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include <aurora/imgui.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace aurora::stereo_host {
namespace {

Module Log("aurora::stereo");

// --- provider and sink registration (changed only while the frame worker is idle) ---
AuroraStereoFrameProvider g_provider = nullptr;
void* g_providerUserdata = nullptr;
stereo::SinkCallback g_sink = nullptr;
stereo::SubmitCallback g_sinkSubmitted = nullptr;
void* g_sinkUserdata = nullptr;

// --- settings the host may set from any thread ---
std::atomic_bool g_panelLayer{false};
std::atomic_bool g_frameInterpolation{false};
std::atomic_bool g_motionLogging{false};
std::atomic_bool g_stopAtDisplayCopy{true};
std::atomic_bool g_skipCopyClears{false};
std::atomic_bool g_singlePassEyes{true};
std::atomic_bool g_hudScreenEnabled{false};
std::atomic<float> g_hudScreenWidth{2.0f};
std::atomic<float> g_hudScreenDistance{1.5f};
std::atomic<int> g_mirrorView{AURORA_STEREO_MIRROR_NORMAL};
std::atomic_bool g_hostEventPump{false};
std::atomic_bool g_pipelineCacheIdleStore{false};
std::atomic_bool g_immersiveReplay{true};
std::atomic<float> g_headLockedSize{1.0f};
std::atomic<float> g_headLockedDistance{1.0f};
// AURORA_STEREO_ROUTE_VISOR's eye offset factor (aurora_set_stereo_visor).
std::atomic<float> g_visorEyeOffsetScale{1.0f};
// AURORA_STEREO_ROUTE_SCREEN_2D's virtual screen, in the recorded space's units.
std::atomic<float> g_screen2DHalfWidth{0.0f};
std::atomic<float> g_screen2DDistance{0.0f};
// AURORA_STEREO_ROUTE_ANCHORED_SCREEN's plane is held (aurora_set_stereo_screen_anchor).
std::atomic_bool g_screenAnchorHeld{false};

// --- the scene anchor the producer publishes per frame (latched by end_frame) ---
std::mutex g_anchorMutex;
struct SceneAnchor {
  bool valid = false;
  float anchorFromScene[12]{};
  float unitsPerMeter = 0.0f;
  bool viewValid = false;
  float viewFromWorld[12]{};
};
SceneAnchor g_pendingAnchor;

// --- the mono-copy target of the virtual screen, written on the frame worker ---
struct EyeTarget {
  webgpu::TextureWithSampler texture;
  uint32_t width = 0;
  uint32_t height = 0;
};
std::array<EyeTarget, AURORA_STEREO_EYE_COUNT> g_eyeTargets;
uint32_t g_logCounter = 0;

bool ensure_eye_target(uint32_t eye, uint32_t width, uint32_t height) noexcept {
  auto& target = g_eyeTargets[eye];
  if (width == 0 || height == 0) {
    return false;
  }
  if (target.texture.texture && target.width == width && target.height == height &&
      target.texture.format == webgpu::g_graphicsConfig.surfaceConfiguration.format) {
    return true;
  }
  target.texture = webgpu::create_render_texture(width, height, false);
  target.width = width;
  target.height = height;
  Log.info("Stereo eye {} screen target {}x{}", eye, width, height);
  return static_cast<bool>(target.texture.texture);
}

uint32_t clamp_coord(float value, uint32_t limit) noexcept {
  if (!(value > 0.0f)) {
    return 0;
  }
  const float max = static_cast<float>(limit);
  return static_cast<uint32_t>(value > max ? max : value);
}

// The mono present source, letterboxed into one eye target with the window's
// own copy pipeline: what the virtual screen shows, and the fail-safe for an
// immersive packet whose replay could not happen.
void copy_mono_into_eye(wgpu::CommandEncoder& encoder, uint32_t eye) noexcept {
  const auto& target = g_eyeTargets[eye];
  const auto& source = webgpu::present_source();
  const auto viewport =
      webgpu::calculate_present_viewport(target.width, target.height, source.size.width, source.size.height);
  const auto& resampled = webgpu::resample_present_source(encoder, viewport);
  const auto bindGroup = webgpu::create_copy_bind_group(resampled);
  const std::array attachments{
      wgpu::RenderPassColorAttachment{
          .view = target.texture.view,
          .loadOp = wgpu::LoadOp::Clear,
          .storeOp = wgpu::StoreOp::Store,
          .clearValue = {0.0, 0.0, 0.0, 1.0},
      },
  };
  const wgpu::RenderPassDescriptor descriptor{
      .label = eye == 0 ? "Stereo left eye copy" : "Stereo right eye copy",
      .colorAttachmentCount = attachments.size(),
      .colorAttachments = attachments.data(),
  };
  const auto pass = encoder.BeginRenderPass(&descriptor);
  pass.SetPipeline(webgpu::g_CopyPipeline);
  pass.SetBindGroup(0, bindGroup, 0, nullptr);
  pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar);
  const uint32_t x = clamp_coord(std::floor(viewport.left), target.width);
  const uint32_t y = clamp_coord(std::floor(viewport.top), target.height);
  const uint32_t right = clamp_coord(std::ceil(viewport.left + viewport.width), target.width);
  const uint32_t bottom = clamp_coord(std::ceil(viewport.top + viewport.height), target.height);
  pass.SetScissorRect(x, y, right - x, bottom - y);
  pass.Draw(3);
  pass.End();
}

bool finite(const float* values, size_t count) noexcept {
  for (size_t i = 0; i < count; ++i) {
    if (!std::isfinite(values[i])) {
      return false;
    }
  }
  return true;
}

void load_eye(gfx::StereoEyeParams& out, const AuroraStereoEye& eye) noexcept {
  out.width = eye.width;
  out.height = eye.height;
  static_assert(sizeof(out.projection) == sizeof(eye.projection));
  static_assert(sizeof(out.viewFromCenter) == sizeof(eye.viewFromCenter));
  std::memcpy(&out.projection, eye.projection, sizeof(out.projection));
  std::memcpy(&out.viewFromCenter, eye.viewFromCenter, sizeof(out.viewFromCenter));
}

} // namespace

gfx::StereoFrameState begin_frame(uint64_t contentTag) noexcept {
  static gfx::stereo_replay::FloatingHud floatingHud;
  static uint64_t floatingTime = 0;
  static uint64_t floatingTag = 0;
  static bool screenAnchorPlaced = false;
  static uint64_t screenAnchorTag = 0;
  static uint64_t screenAnchorOrigin = 0;
  static decltype(gfx::StereoEyeParams::viewFromCenter) screenAnchor{};
  gfx::StereoFrameState state{};
  if (g_provider == nullptr || g_sink == nullptr) {
    return state;
  }
  AuroraStereoFrame packet{};
  if (!g_provider(gfx::current_frame(), &packet, g_providerUserdata) || packet.frameToken == 0) {
    return state;
  }
  if (packet.mode != AURORA_STEREO_FRAME_IMMERSIVE_REPLAY && packet.mode != AURORA_STEREO_FRAME_VIRTUAL_SCREEN) {
    Log.warn("Stereo packet {} has an unknown mode {}; dropped", packet.frameToken, static_cast<int>(packet.mode));
    return state;
  }
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    if (!finite(packet.eyes[eye].projection, 16) || !finite(packet.eyes[eye].viewFromCenter, 12)) {
      Log.warn("Stereo packet {} eye {} is not finite; dropped", packet.frameToken, eye);
      return state;
    }
  }
  state.active = true;
  state.frameToken = packet.frameToken;
  state.contentTag = packet.contentTag;
  state.mode = packet.mode;
  const float headLockedDistance = g_headLockedDistance.load(std::memory_order_relaxed);
  state.headLockedScaleXY = g_headLockedSize.load(std::memory_order_relaxed) * headLockedDistance;
  state.headLockedScaleZ = headLockedDistance;
  state.screen2DHalfWidth = g_screen2DHalfWidth.load(std::memory_order_relaxed);
  state.screen2DDistance = g_screen2DDistance.load(std::memory_order_relaxed);
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    load_eye(state.eyes[eye], packet.eyes[eye]);
  }
  // A head-locked draw keeps the head's rotation and position out: what
  // remains is this eye's offset from the head centre (half the IPD), taken as
  // its translation minus the two eyes' mean. The two eye rotations differ by
  // a canted display at most, so the mean cancels the head's own position.
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    auto& headLocked = state.eyes[eye].headLockedViewFromCenter;
    headLocked = {};
    headLocked.m0 = Vec4<float>{1.0f, 0.0f, 0.0f, 0.0f};
    headLocked.m1 = Vec4<float>{0.0f, 1.0f, 0.0f, 0.0f};
    headLocked.m2 = Vec4<float>{0.0f, 0.0f, 1.0f, 0.0f};
    for (size_t row = 0; row < 3; ++row) {
      float mean = 0.0f;
      for (uint32_t other = 0; other < AURORA_STEREO_EYE_COUNT; ++other) {
        mean += (*(&state.eyes[other].viewFromCenter.m0 + row))[3];
      }
      mean /= static_cast<float>(AURORA_STEREO_EYE_COUNT);
      (*(&headLocked.m0 + row))[3] = (*(&state.eyes[eye].viewFromCenter.m0 + row))[3] - mean;
    }
  }
  // A visor draw is head-locked too, its eye offset scaled so a draw at the
  // near plane reads farther out (aurora_set_stereo_visor).
  const float visorEyeOffsetScale = g_visorEyeOffsetScale.load(std::memory_order_relaxed);
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    state.eyes[eye].visorViewFromCenter =
        gfx::stereo_replay::scaled_translation(state.eyes[eye].headLockedViewFromCenter, visorEyeOffsetScale);
  }
  // A sky draw keeps the head's rotation and drops every translation: the eye
  // looks from the camera's centre, so the sky has no disparity.
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    state.eyes[eye].skyViewFromCenter = gfx::stereo_replay::without_translation(state.eyes[eye].viewFromCenter);
    state.eyes[eye].anchoredScreenViewFromCenter = state.eyes[eye].viewFromCenter;
  }
  if (packet.mode != AURORA_STEREO_FRAME_IMMERSIVE_REPLAY) {
    floatingHud.Reset();
    floatingTime = 0;
    screenAnchorPlaced = false;
    return state;
  }
  if (!g_immersiveReplay.load(std::memory_order_relaxed)) {
    // Replay switched off: the packet is shown as mono on the virtual screen.
    return state;
  }
  if (contentTag == AURORA_STEREO_CONTENT_TAG_UNKNOWN || packet.contentTag != contentTag) {
    // The packet was built against another safety state than the content this
    // frame will record: never replay it as immersive. The sink still gets the
    // mono image so the compositor is not starved.
    if ((g_logCounter++ % 120) == 0) {
      Log.info("Stereo packet {} tag {:#x} does not match frame tag {:#x}; shown as mono", packet.frameToken,
               packet.contentTag, contentTag);
    }
    return state;
  }
  // The floating HUD plane is separate from the fixed menu/cinema screen.
  // Average the two eye poses to get the complete tracked head pose, ease it,
  // then compose each actual eye against it (retaining stereo disparity).
  const auto headPose = gfx::stereo_replay::center_head_pose(state.eyes[0].viewFromCenter,
                                                           state.eyes[1].viewFromCenter);
  if (floatingTag != packet.contentTag) floatingHud.Reset();
  const float followDt = floatingTime != 0 && packet.displayTimeNanos >= floatingTime
                            ? float(double(packet.displayTimeNanos - floatingTime) * 1.e-9)
                            : 1.f / 60.f;
  const auto floatingPose = floatingHud.Update(headPose, followDt);
  floatingTime = packet.displayTimeNanos;
  floatingTag = packet.contentTag;
  for (auto& eye : state.eyes) {
    eye.floatingHudViewFromCenter = gfx::stereo_replay::compose_affine(eye.viewFromCenter, floatingPose);
  }
  // The held screen: placed once, upright ahead of the head, on the first immersive frame of a hold,
  // then kept still while the head looks around it; the eyes keep their own positions (disparity).
  // A recentre or a height set gives the eyes a new centre space, so the screen is placed again.
  const bool anchorHeld = g_screenAnchorHeld.load(std::memory_order_relaxed);
  if (!anchorHeld || screenAnchorTag != packet.contentTag || screenAnchorOrigin != packet.originGeneration) {
    screenAnchorPlaced = false;
  }
  if (anchorHeld && !screenAnchorPlaced) {
    screenAnchor = gfx::stereo_replay::upright_screen_anchor(headPose);
    screenAnchorPlaced = true;
    screenAnchorTag = packet.contentTag;
    screenAnchorOrigin = packet.originGeneration;
  }
  if (screenAnchorPlaced) {
    for (auto& eye : state.eyes) {
      eye.anchoredScreenViewFromCenter = gfx::stereo_replay::compose_affine(eye.viewFromCenter, screenAnchor);
    }
  }
  // Both eyes in one render pass when the device and the frame allow it
  // (gfx/stereo_multiview.hpp).
  const bool multiview = gfx::stereo_multiview::usable(state.eyes[0].width, state.eyes[0].height,
                                                       state.eyes[1].width, state.eyes[1].height,
                                                       webgpu::g_graphicsConfig.msaaSamples);
  if (!gfx::ensure_stereo_eye_targets(state.eyes, multiview)) {
    Log.warn("Stereo packet {} has no usable eye size ({}x{}, {}x{}); shown as mono", packet.frameToken,
             state.eyes[0].width, state.eyes[0].height, state.eyes[1].width, state.eyes[1].height);
    return state;
  }
  state.immersive = true;
  state.multiview = multiview;
  // Foveated eye passes, through the density maps' views of the targets
  // (gfx/stereo_foveation.hpp).
  const auto foveation = gfx::stereo_foveation::prepare(state.eyes, multiview);
  state.foveated = foveation.foveated;
  state.foveatedMultiviewColorView = foveation.multiviewColorView;
  state.foveatedEyeColorViews = foveation.eyeColorViews;
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    const auto& target = gfx::stereo_eye_target(eye);
    const auto& output = target.output();
    state.outputs[eye] = gfx::StereoEyeOutput{
        .texture = output.texture,
        .view = output.view,
        .size = {target.width, target.height, 1},
        .format = output.format,
        .layer = target.layer,
    };
  }
  return state;
}

std::optional<PendingSink> encode(wgpu::CommandEncoder& encoder, const gfx::StereoFrameState& state,
                                  uint32_t logicalFrame, uint64_t contentTag) noexcept {
  if (!state.active || g_sink == nullptr) {
    return std::nullopt;
  }
  bool immersive = state.immersive && state.replayed;
  if (immersive && contentTag != state.contentTag) {
    // The content changed between begin and end: the eyes hold replayed
    // content of another safety state. Fall back to the mono image.
    if ((g_logCounter++ % 120) == 0) {
      Log.info("Stereo packet {} sealed with tag {:#x} after beginning with {:#x}; shown as mono",
               state.frameToken, contentTag, state.contentTag);
    }
    immersive = false;
  }
  PendingSink pending{};
  pending.frame.frameToken = state.frameToken;
  pending.frame.logicalFrame = logicalFrame;
  if (immersive) {
    pending.frame.mode = AURORA_STEREO_FRAME_IMMERSIVE_REPLAY;
    for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
      const auto& output = state.outputs[eye];
      pending.frame.eyes[eye] = stereo::EyeImage{
          .texture = &output.texture,
          .view = &output.view,
          .size = output.size,
          .format = output.format,
          .layer = output.layer,
      };
    }
  } else {
    // The finished mono frame, letterboxed. The packet's mode decides what the
    // host prepared for this frame token: one screen target for a virtual-screen
    // packet, both eye targets for an immersive one. An immersive packet that
    // was not replayed (its tag did not match this frame's content, as on the
    // first frame after a pause moves the game onto the virtual screen) still
    // has to fill both eyes, or the host's copy fails and it gives up on the
    // session; for that one frame each eye shows the flat image.
    const bool immersivePacket = state.mode == AURORA_STEREO_FRAME_IMMERSIVE_REPLAY;
    const uint32_t eyeCount = immersivePacket ? AURORA_STEREO_EYE_COUNT : 1;
    for (uint32_t eye = 0; eye < eyeCount; ++eye) {
      if (!ensure_eye_target(eye, state.eyes[eye].width, state.eyes[eye].height)) {
        Log.warn("Stereo packet {} eye {} has no usable size ({}x{}); dropped", state.frameToken, eye,
                 state.eyes[eye].width, state.eyes[eye].height);
        return std::nullopt;
      }
      copy_mono_into_eye(encoder, eye);
    }
    pending.frame.mode = immersivePacket ? AURORA_STEREO_FRAME_IMMERSIVE_REPLAY : AURORA_STEREO_FRAME_VIRTUAL_SCREEN;
    for (uint32_t eye = 0; eye < eyeCount; ++eye) {
      const auto& target = g_eyeTargets[eye];
      pending.frame.eyes[eye] = stereo::EyeImage{
          .texture = &target.texture.texture,
          .view = &target.texture.view,
          .size = {target.width, target.height, 1},
          .format = target.texture.format,
      };
    }
  }
  if (!g_sink(encoder, pending.frame, g_sinkUserdata)) {
    // The bridge declined (no targets pending, or a size mismatch) and has
    // already published that result; nothing to submit.
    return std::nullopt;
  }
  return pending;
}

void submitted(const std::optional<PendingSink>& pending) noexcept {
  if (!pending || g_sinkSubmitted == nullptr) {
    return;
  }
  g_sinkSubmitted(pending->frame, g_sinkUserdata);
}

void shutdown() noexcept {
  g_provider = nullptr;
  g_providerUserdata = nullptr;
  for (auto& target : g_eyeTargets) {
    target = {};
  }
  gfx::stereo_foveation::release();
  gfx::release_stereo_eye_targets();
  gfx::stereo_shadow::shutdown();
  stereo_overlay::shutdown();
}

bool headset_owns_display() noexcept {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_VISION)
  // The Quest and Apple Vision Pro have no screen beside the headset: the window is never shown
  // (an offscreen one on visionOS), so presenting it would be wasted work.
  return g_provider != nullptr && g_sink != nullptr;
#else
  // The desktop window mirrors the game for whoever watches.
  return false;
#endif
}

int mirror_view() noexcept { return g_mirrorView.load(std::memory_order_relaxed); }

bool mirror_skips_mono() noexcept {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_VISION)
  return false;
#else
  return g_provider != nullptr && g_sink != nullptr && mirror_view() != AURORA_STEREO_MIRROR_NORMAL;
#endif
}

} // namespace aurora::stereo_host

namespace aurora::stereo {

void set_sink(SinkCallback callback, SubmitCallback submitted, void* userdata) noexcept {
  stereo_host::g_sink = callback;
  stereo_host::g_sinkSubmitted = submitted;
  stereo_host::g_sinkUserdata = userdata;
}

} // namespace aurora::stereo

// ---------------------------------------------------------------------------
// C API (aurora.h / gfx.h / imgui.h)
// ---------------------------------------------------------------------------

using namespace aurora::stereo_host;

void aurora_set_stereo_frame_provider(AuroraStereoFrameProvider provider, void* userdata) {
  g_provider = provider;
  g_providerUserdata = userdata;
}

void aurora_notify_stereo_frame() {
  // Retained replay (interpolation) is not ported: the next sealed frame
  // consumes the packet.
}

void aurora_set_stereo_panel_layer(bool enabled) {
  g_panelLayer.store(enabled, std::memory_order_relaxed);
  aurora::stereo_overlay::set_layer_mode(enabled);
}

void aurora_set_host_event_pump(bool hostPumps) { g_hostEventPump.store(hostPumps, std::memory_order_relaxed); }

void aurora_set_stereo_scene_anchor(const float anchorFromScene[12]) {
  std::lock_guard lock(g_anchorMutex);
  if (anchorFromScene == nullptr) {
    g_pendingAnchor = {};
    return;
  }
  g_pendingAnchor = {};
  g_pendingAnchor.valid = true;
  std::copy(anchorFromScene, anchorFromScene + 12, g_pendingAnchor.anchorFromScene);
}

void aurora_set_stereo_scene_anchor_scaled(const float anchorFromScene[12], float unitsPerMeter) {
  aurora_set_stereo_scene_anchor(anchorFromScene);
  std::lock_guard lock(g_anchorMutex);
  g_pendingAnchor.unitsPerMeter = unitsPerMeter;
}

void aurora_set_stereo_scene_view(const float viewFromWorld[12]) {
  std::lock_guard lock(g_anchorMutex);
  g_pendingAnchor.viewValid = viewFromWorld != nullptr;
  if (viewFromWorld != nullptr) {
    std::copy(viewFromWorld, viewFromWorld + 12, g_pendingAnchor.viewFromWorld);
  }
}

void aurora_wait_for_frame_worker() { aurora::gfx::synchronize(); }

bool aurora_wait_for_frame_worker_for(uint32_t /*timeoutMicros*/) {
  aurora::gfx::synchronize();
  return true;
}

void aurora_quiesce_frame_worker() { aurora::gfx::synchronize(); }

void aurora_store_pipeline_caches() {
  // The pipeline cache persists itself on shutdown in this aurora lineage.
}

void aurora_request_pipeline_cache_store() {}

void aurora_set_pipeline_cache_idle_store(bool allowed) {
  g_pipelineCacheIdleStore.store(allowed, std::memory_order_relaxed);
}

uint32_t aurora_get_frame_worker_native_thread_id(void) { return aurora::gfx::render_worker::native_thread_id(); }

uint32_t aurora_get_gx_worker_native_thread_id(void) { return aurora::gx::fifo::native_thread_id(); }

void aurora_set_present_schedule(uint64_t /*baseNanos*/, uint64_t /*intervalNanos*/) {}

void aurora_report_producer_paced(bool /*paced*/) {}

// gfx.h

void aurora_set_stereo_frame_interpolation(bool enabled) {
  // Retained replay is not ported; the flag is kept so the host sees it off.
  g_frameInterpolation.store(false, std::memory_order_relaxed);
  (void)enabled;
}

bool aurora_get_stereo_frame_interpolation() { return g_frameInterpolation.load(std::memory_order_relaxed); }

void aurora_set_stereo_motion_logging(bool enabled) {
  g_motionLogging.store(enabled, std::memory_order_relaxed);
  aurora::gfx::set_stereo_diagnostics(enabled);
}

void aurora_set_stereo_stop_at_display_copy(bool enabled) {
  g_stopAtDisplayCopy.store(enabled, std::memory_order_relaxed);
}

void aurora_set_stereo_skip_copy_clears(bool enabled) { g_skipCopyClears.store(enabled, std::memory_order_relaxed); }

void aurora_set_stereo_single_pass_eyes(bool enabled) { g_singlePassEyes.store(enabled, std::memory_order_relaxed); }

void aurora_set_stereo_foveation(uint32_t level) { aurora::gfx::stereo_foveation::set_level(level); }

uint32_t aurora_get_stereo_foveation() { return aurora::gfx::stereo_foveation::level(); }

bool aurora_stereo_foveation_available() { return aurora::webgpu::fdm::available(); }

void aurora_set_stereo_hud_screen(bool enabled, float widthMeters, float distanceMeters) {
  g_hudScreenEnabled.store(enabled, std::memory_order_relaxed);
  g_hudScreenWidth.store(widthMeters, std::memory_order_relaxed);
  g_hudScreenDistance.store(distanceMeters, std::memory_order_relaxed);
}

bool aurora_get_stereo_hud_screen_enabled() { return g_hudScreenEnabled.load(std::memory_order_relaxed); }

bool aurora_get_stereo_screen_aspects(float* pictureAspect, float* snapshotAspect) {
  const auto& source = aurora::webgpu::present_source();
  if (!source.texture || source.size.height == 0) {
    return false;
  }
  const float aspect = static_cast<float>(source.size.width) / static_cast<float>(source.size.height);
  // The port fits the EFB to the game's aspect and letterboxes on present, so
  // the picture fills the snapshot.
  if (pictureAspect != nullptr) {
    *pictureAspect = aspect;
  }
  if (snapshotAspect != nullptr) {
    *snapshotAspect = aspect;
  }
  return true;
}

void aurora_set_stereo_mirror_view(AuroraStereoMirrorView view) {
  g_mirrorView.store(static_cast<int>(view), std::memory_order_relaxed);
}

// imgui.h

void aurora_imgui_set_stereo_overlay(ImDrawData* /*drawData*/, float /*widthFraction*/) {
  // The headset panel's ImGui rendering is not ported yet.
}

void aurora_set_stereo_head_locked(float sizeScale, float distanceScale) {
  g_headLockedSize.store(sizeScale > 0.0f ? sizeScale : 1.0f, std::memory_order_relaxed);
  g_headLockedDistance.store(distanceScale > 0.0f ? distanceScale : 1.0f, std::memory_order_relaxed);
}

void aurora_set_stereo_visor(float eyeOffsetScale) {
  g_visorEyeOffsetScale.store(eyeOffsetScale >= 0.0f && eyeOffsetScale <= 1.0f ? eyeOffsetScale : 1.0f,
                              std::memory_order_relaxed);
}

void aurora_set_stereo_screen_2d(float widthMeters, float distanceMeters, float unitsPerMeter) {
  const bool valid = widthMeters > 0.0f && distanceMeters > 0.0f && unitsPerMeter > 0.0f;
  g_screen2DHalfWidth.store(valid ? 0.5f * widthMeters * unitsPerMeter : 0.0f, std::memory_order_relaxed);
  g_screen2DDistance.store(valid ? distanceMeters * unitsPerMeter : 0.0f, std::memory_order_relaxed);
}

void aurora_set_stereo_screen_anchor(bool held) { g_screenAnchorHeld.store(held, std::memory_order_relaxed); }

void aurora_set_stereo_immersive_replay(bool enabled) { g_immersiveReplay.store(enabled, std::memory_order_relaxed); }

bool aurora_get_stereo_immersive_replay(void) { return g_immersiveReplay.load(std::memory_order_relaxed); }

void aurora_set_stereo_multiview(bool enabled) { aurora::gfx::stereo_multiview::set_requested(enabled); }

bool aurora_get_stereo_multiview_available(void) {
  return aurora::gfx::stereo_multiview::kApiAvailable && aurora::webgpu::g_multiviewSupported;
}
