#include "recording.hpp"
#include "overdraw.hpp"

#include <cstdlib>
#include <atomic>

#include <aurora/phase.hpp>

#include "encoding.hpp"
#include "floating_hud.hpp"
#include "frame.hpp"
#include "geometry_buffer.hpp"
#include "resource_cache.hpp"

#include "bloom.hpp"
#include "clear.hpp"
#include "draw_payload.hpp"
#include "perf_counters.hpp"
#include "depth_peek.hpp"
#include "pipeline_cache.hpp"
#include "stereo_eyes.hpp"
#include "stereo_shadow.hpp"
#include "stereo_uniform.hpp"
#include "render_worker.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "texture.hpp"
#include "../gx/fifo.hpp"
#include "../gx/gx.hpp"
#include "../gx/pipeline.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../window.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <magic_enum.hpp>
#include <tracy/Tracy.hpp>
#include "texture_sites.hpp"

namespace aurora::gfx {
using namespace detail;

// stereo_uniform.hpp mirrors the GX uniform's fixed counts so the tests need no GX headers.
static_assert(kStereoPositionMatrices == gx::MaxPnMtx);
static_assert(kStereoTextureMatrices == gx::MaxTexMtx);
static_assert(kStereoLights == GX::MaxLights);
static_assert(kStereoLightBytes == sizeof(gx::Light));

namespace {
constexpr Module Log{"aurora::gfx"};

struct FrameRecorder {
  FramePacket* packet = nullptr;
  size_t frameSlot = 0;
  uint32_t currentRenderPass = UINT32_MAX;
  uint32_t drawCallCount = 0;
  // Draws recorded under each stereo route this frame (AURORA_STEREO_ROUTE_*), for the
  // diagnostics line (log_stereo_frame_stats).
  std::array<uint32_t, 12> routeDrawCounts{};
  uint32_t mergedDrawCallCount = 0;
  uint32_t renderPassCount = 0;
  bool inOffscreen = false;
  // EFB passes draw into the linear HDR scene (webgpu::g_sceneHdr) instead of g_frameBuffer; reset
  // every frame. hdrSceneCleared: this frame's HDR scene was cleared, so a later enable loads it.
  bool hdrScene = false;
  bool hdrSceneCleared = false;
  std::optional<RenderPass> suspendedEfbPass;
  Viewport suspendedEfbViewport;
  ClipRect suspendedEfbScissor;
  webgpu::TextureWithSampler offscreenColor;
  webgpu::TextureWithSampler offscreenDepth;
  Viewport cachedViewport;
  ClipRect cachedScissor;
  bool suppressRenderWorker = false;
  uint8_t stereoRoute = AURORA_STEREO_ROUTE_WORLD;
  bool finalPassMonoUnneeded = false;
  stereo_replay::HeadLockedPlane headLockedPlane;
  StereoPlacement3D placement3D;
  StereoScreenTexMtx stereoScreenTexMtx;
  uint8_t multiviewMode = 0; // gx::MultiviewMode of the last stage_stereo_uniforms
  // DepthAfter::IfUnread's sealed pass and task, whose ops aren't handed to the render worker yet,
  // and whether a draw in the pass after them may have tested against their depth.
  struct HeldDepth {
    uint32_t passOp;
    uint32_t taskOp;
    uint32_t sealedPass;
    uint32_t nextPass;
    bool read = false;
  };
  std::optional<HeldDepth> heldDepth;
  // The nearest GX depth (0 near, 1 far) the EFB may hold: the least viewport near plane of this
  // frame's depth-writing EFB draws, and its depth clears.
  float efbDepthNear = 0.f;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugGroupStack;
#endif

  [[nodiscard]] bool active() const noexcept { return packet != nullptr; }

  FramePacket& frame() const {
    CHECK(packet != nullptr, "No active recording session");
    return *packet;
  }

  RenderPassList& passes() const { return frame().renderPasses; }
};

FrameRecorder g_recorder;
// MP_FRAME_STATS keeps the frame statistics on whatever the VR settings say,
// for scripted runs without a headset.
bool frame_stats_forced() noexcept { return std::getenv("MP_FRAME_STATS") != nullptr; }
std::atomic_bool g_stereoDiagnostics{frame_stats_forced()};
// What composing the eye uniform copies costs on the FIFO thread, between two
// stereo statistics lines (log_stereo_frame_stats), while diagnostics are on.
std::atomic<uint64_t> g_eyeUniformNs{0};
std::atomic<uint32_t> g_eyeUniformDraws{0};
} // namespace

namespace perf {
std::atomic<uint64_t> g_fifoNs{0};
std::atomic<uint64_t> g_fifoBytes{0};
std::atomic<uint64_t> g_encodeNs{0};
std::atomic<uint64_t> g_submitNs{0};
std::atomic<uint64_t> g_fifoXfTicks{0};
std::atomic<uint64_t> g_fifoBpTicks{0};
std::atomic<uint64_t> g_fifoCpTicks{0};
std::atomic<uint64_t> g_fifoAuroraTicks{0};
std::atomic<uint64_t> g_fifoVertsTicks{0};
std::atomic<uint64_t> g_fifoPipelineTicks{0};
std::atomic<uint64_t> g_fifoBindsTicks{0};
std::atomic<uint64_t> g_fifoUniformTicks{0};
std::atomic<uint64_t> g_fifoPushTicks{0};
std::atomic<uint32_t> g_fifoXfLoads{0};
std::atomic<uint32_t> g_fifoBpLoads{0};
std::atomic<uint32_t> g_fifoDrawsMerged{0};
std::atomic<uint32_t> g_fifoDrawsPushed{0};
std::atomic<uint32_t> g_fifoPipelineBuilds{0};
std::atomic<uint32_t> g_fifoBindGroupBuilds{0};
std::atomic<uint32_t> g_fifoBindGroupMisses{0};
std::atomic<uint64_t> g_fifoResolveTicks{0};
std::atomic<uint32_t> g_fifoUniformBuilds{0};
std::atomic<uint32_t> g_mergeBreaks[BreakCount]{};
std::atomic<uint64_t> g_fifoDeindexTicks{0};
std::atomic<uint32_t> g_fifoDeindexedVerts{0};
std::atomic<uint32_t> g_fifoCachedDlCalls{0};
std::atomic<uint64_t> g_fifoCachedDlBytes{0};
std::atomic<uint64_t> g_fifoCachedDlVertBytes{0};
std::atomic<uint64_t> g_fifoCachedDlIndexBytes{0};
std::atomic<uint32_t> g_fifoCachedDlVerts{0};
std::atomic<uint32_t> g_fifoGeometryHits{0};
std::atomic<uint32_t> g_fifoGeometryMisses{0};
std::atomic<uint32_t> g_fifoNativeDraws{0};
std::atomic<uint32_t> g_geometryEntries{0};
std::atomic<uint64_t> g_geometryResidentBytes{0};
std::atomic<uint64_t> g_drainWaitNs{0};
std::atomic<uint32_t> g_drainCalls{0};
bool enabled() noexcept { return g_stereoDiagnostics.load(std::memory_order_relaxed); }
} // namespace perf

namespace {

void log_stereo_frame_stats(const FramePacket& frame) {
  // Every 600th frame, immersive or not (a desktop run measures the same FIFO)
  static uint32_t sFrames = 0;
  if ((sFrames++ % 600) != 0) {
    return;
  }
  uint32_t efbPasses = 0;
  uint32_t eyePasses = 0;
  uint32_t monoSkipped = 0;
  uint32_t foveated = 0;
  uint32_t copies = 0;
  uint32_t eyeCopies = 0;
  uint32_t discarded = 0;
  for (const auto& pass : frame.renderPasses) {
    efbPasses += pass.efb ? 1 : 0;
    eyePasses += pass.stereo.enabled ? 1 : 0;
    monoSkipped += pass.stereo.enabled && pass.stereo.skipMono ? 1 : 0;
    foveated += pass.stereo.enabled && pass.stereo.foveated ? 1 : 0;
    copies += pass.resolveTarget ? 1 : 0;
    eyeCopies += pass.stereo.copyTargets[0] ? 1 : 0;
    discarded += pass.discardable ? 1 : 0;
  }
  const auto& routes = g_recorder.routeDrawCounts;
  const double eyeUniformMs =
      static_cast<double>(g_eyeUniformNs.exchange(0, std::memory_order_relaxed)) / 600.0 / 1.0e6;
  const uint32_t eyeUniformDraws = g_eyeUniformDraws.exchange(0, std::memory_order_relaxed) / 600;
  const auto perFrameMs = [](std::atomic<uint64_t>& ns) {
    return static_cast<double>(ns.exchange(0, std::memory_order_relaxed)) / 600.0 / 1.0e6;
  };
  const double fifoMs = perFrameMs(perf::g_fifoNs);
  const uint64_t fifoKb = perf::g_fifoBytes.exchange(0, std::memory_order_relaxed) / 600 / 1024;
  const double drainWaitMs = perFrameMs(perf::g_drainWaitNs);
  const uint32_t drainCalls = perf::g_drainCalls.exchange(0, std::memory_order_relaxed) / 600;
  const double encodeMs = perFrameMs(perf::g_encodeNs);
  const double submitMs = perFrameMs(perf::g_submitNs);
  const auto& uploads = resources().stats;
  Log.info("stereo frame: {} passes ({} EFB, {} replayed per eye, {} foveated, {} eyes only, {} discarded), "
           "{} EFB copies "
           "({} taken per eye), {} draws (world {}, head-locked {}, head-locked 2D {}, screen 2D {}, fullscreen {}, "
           "sky {}, skipped {}), eye uniforms {:.2f} ms/frame ({} draws); per frame: fifo {:.2f} ms ({} KB), game waits "
           "for fifo {:.2f} ms ({} drains), encode {:.2f} ms, submit {:.2f} ms; uploads verts {} KB, indices {} KB, "
           "uniforms {} KB, storage {} KB, textures {} KB",
           frame.renderPasses.size(), efbPasses, eyePasses, foveated, monoSkipped, discarded, copies, eyeCopies,
           g_recorder.drawCallCount, routes[AURORA_STEREO_ROUTE_WORLD], routes[AURORA_STEREO_ROUTE_HEAD_LOCKED],
           routes[AURORA_STEREO_ROUTE_HEAD_LOCKED_2D], routes[AURORA_STEREO_ROUTE_SCREEN_2D],
           routes[AURORA_STEREO_ROUTE_FULLSCREEN],
           routes[AURORA_STEREO_ROUTE_SKY], routes[AURORA_STEREO_ROUTE_SKIP], eyeUniformMs, eyeUniformDraws, fifoMs,
           fifoKb, drainWaitMs, drainCalls, encodeMs, submitMs, uploads.lastVertSize / 1024,
           uploads.lastIndexSize / 1024, uploads.lastUniformSize / 1024, uploads.lastStorageSize / 1024,
           uploads.lastTextureUploadSize / 1024);
  // The FIFO processor's buckets, in ticks calibrated over the window since the
  // last line (the first line, with no window yet, shows zeros).
  static uint64_t sLastTick = perf::tick();
  static auto sLastSteady = std::chrono::steady_clock::now();
  const uint64_t nowTick = perf::tick();
  const auto nowSteady = std::chrono::steady_clock::now();
  const double windowNs =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowSteady - sLastSteady).count());
  const double ticksPerNs = windowNs > 0.0 ? static_cast<double>(nowTick - sLastTick) / windowNs : 0.0;
  sLastTick = nowTick;
  sLastSteady = nowSteady;
  const auto bucketMs = [ticksPerNs](std::atomic<uint64_t>& ticks) {
    const auto value = static_cast<double>(ticks.exchange(0, std::memory_order_relaxed));
    return ticksPerNs > 0.0 ? value / ticksPerNs / 600.0 / 1.0e6 : 0.0;
  };
  const auto perFrame = [](std::atomic<uint32_t>& calls) { return calls.exchange(0, std::memory_order_relaxed) / 600; };
  const double xfMs = bucketMs(perf::g_fifoXfTicks);
  const uint32_t xfLoads = perFrame(perf::g_fifoXfLoads);
  const double bpMs = bucketMs(perf::g_fifoBpTicks);
  const uint32_t bpLoads = perFrame(perf::g_fifoBpLoads);
  const double cpMs = bucketMs(perf::g_fifoCpTicks);
  const double auroraMs = bucketMs(perf::g_fifoAuroraTicks);
  const double vertsMs = bucketMs(perf::g_fifoVertsTicks);
  const uint32_t merged = perFrame(perf::g_fifoDrawsMerged);
  const uint32_t pushed = perFrame(perf::g_fifoDrawsPushed);
  const double pipelineMs = bucketMs(perf::g_fifoPipelineTicks);
  const uint32_t pipelineBuilds = perFrame(perf::g_fifoPipelineBuilds);
  const double bindsMs = bucketMs(perf::g_fifoBindsTicks);
  const uint32_t bindGroupBuilds = perFrame(perf::g_fifoBindGroupBuilds);
  const uint32_t bindGroupMisses = perFrame(perf::g_fifoBindGroupMisses);
  const double resolveMs = bucketMs(perf::g_fifoResolveTicks);
  const double uniformMs = bucketMs(perf::g_fifoUniformTicks);
  const uint32_t uniformBuilds = perFrame(perf::g_fifoUniformBuilds);
  const double pushMs = bucketMs(perf::g_fifoPushTicks);
  const double deindexMs = bucketMs(perf::g_fifoDeindexTicks);
  const uint32_t deindexedVerts = perFrame(perf::g_fifoDeindexedVerts);
  const auto perFrameKb = [](std::atomic<uint64_t>& bytes) {
    return bytes.exchange(0, std::memory_order_relaxed) / 600 / 1024;
  };
  const uint32_t cachedDlCalls = perFrame(perf::g_fifoCachedDlCalls);
  const uint64_t cachedDlKb = perFrameKb(perf::g_fifoCachedDlBytes);
  const uint64_t cachedDlVertKb = perFrameKb(perf::g_fifoCachedDlVertBytes);
  const uint64_t cachedDlIndexKb = perFrameKb(perf::g_fifoCachedDlIndexBytes);
  const uint32_t cachedDlVerts = perFrame(perf::g_fifoCachedDlVerts);
  const uint32_t geometryHits = perFrame(perf::g_fifoGeometryHits);
  const uint32_t geometryMisses = perFrame(perf::g_fifoGeometryMisses);
  const uint32_t nativeDraws = perFrame(perf::g_fifoNativeDraws);
  Log.info("native vertex input: {} draws per frame", nativeDraws);
  const uint32_t geometryEntries = perf::g_geometryEntries.load(std::memory_order_relaxed);
  const uint64_t geometryMb = perf::g_geometryResidentBytes.load(std::memory_order_relaxed) >> 20;
  std::array<uint32_t, perf::BreakCount> breaks{};
  for (size_t i = 0; i < breaks.size(); ++i) {
    breaks[i] = perFrame(perf::g_mergeBreaks[i]);
  }
  Log.info("fifo processor per frame: xf {:.2f} ms ({} loads), bp {:.2f} ms ({} loads), cp {:.2f} ms, aurora {:.2f} "
           "ms; draws {} merged + {} pushed: verts {:.2f} ms, pipeline {:.2f} ms ({} builds), bind groups {:.2f} ms "
           "({} builds, {} cache misses, resolve {:.2f} ms), uniform {:.2f} ms ({} builds), push {:.2f} ms; "
           "de-indexing {:.2f} ms ({} vertices); in-place dl {} calls ({} KB): {} KB of records ({} de-indexed "
           "vertices), {} KB of indices; geometry cache {} hits, {} misses ({} entries, {} MiB); tick {:.3f} GHz",
           xfMs, xfLoads, bpMs, bpLoads, cpMs, auroraMs, merged, pushed, vertsMs, pipelineMs, pipelineBuilds, bindsMs,
           bindGroupBuilds, bindGroupMisses, resolveMs, uniformMs, uniformBuilds, pushMs, deindexMs, deindexedVerts,
           cachedDlCalls, cachedDlKb, cachedDlVertKb, cachedDlVerts, cachedDlIndexKb, geometryHits, geometryMisses,
           geometryEntries, geometryMb, ticksPerNs);
  Log.info("new draws per frame, by what kept them from joining the last one: pipeline state {}, textures {}, "
           "uniform data {}, immediates {}, vertex format or primitive kind {}, no draw to join {}",
           breaks[perf::BreakPipeline], breaks[perf::BreakTextures], breaks[perf::BreakUniform],
           breaks[perf::BreakImmediates], breaks[perf::BreakFormat], breaks[perf::BreakNoDraw]);
}

std::string pass_label(std::string_view kind) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (!g_recorder.debugGroupStack.empty()) {
    return fmt::format("{} ({})", kind, g_recorder.debugGroupStack.back());
  }
#endif
  return std::string{kind};
}

// The active EFB colour target: the HDR scene while it is on, else the display EFB.
const webgpu::TextureWithSampler& efb_color() noexcept {
  return g_recorder.hdrScene ? webgpu::g_sceneHdr : webgpu::g_frameBuffer;
}
const webgpu::TextureWithSampler& efb_color_resolved() noexcept {
  return g_recorder.hdrScene ? webgpu::g_sceneHdrResolved : webgpu::g_frameBufferResolved;
}

void set_efb_targets(RenderPass& pass) {
  const auto layout = scene_render_target_layout();
  const auto& color = efb_color();
  const auto& resolved = efb_color_resolved();
  pass.colorAttachmentCount = layout.colorAttachmentCount;
  auto& sceneColor = pass.colorAttachments[SceneColorAttachmentIndex];
  sceneColor.semantic = ColorAttachmentSemantic::SceneColor;
  sceneColor.format = layout.colorAttachments[SceneColorAttachmentIndex].format;
  sceneColor.size = color.size;
  sceneColor.view = color.view;
  sceneColor.resolveView = layout.sampleCount > 1 ? resolved.view : nullptr;
  for (uint32_t i = SceneColorAttachmentIndex + 1; i < layout.colorAttachmentCount; ++i) {
    auto& extra = pass.colorAttachments[i];
    extra.semantic = layout.colorAttachments[i].semantic;
    extra.format = layout.colorAttachments[i].format;
    AURORA_ASSERT(false, "Scene render-target attachment {} has no backing texture", i);
  }
  pass.depthStencilView = webgpu::g_depthBuffer.view;
  pass.depthStencilFormat = layout.depthStencilFormat;
  // EFB copies read the active target; an RGBA16F one is converted per copy (resolve_pass_into's
  // ResolveHdr, tex_copy_conv::CopyDomain).
  pass.copySourceTexture = webgpu::g_graphicsConfig.msaaSamples > 1 ? resolved.texture : color.texture;
  pass.copySourceView = webgpu::g_graphicsConfig.msaaSamples > 1 ? resolved.view : color.view;
  pass.copySourceDepthView = webgpu::g_depthBuffer.view;
  pass.msaaSamples = layout.sampleCount;
  pass.hasDepth = true;
  pass.hasStencil = false;
  pass.efb = true;
}

void set_single_color_target(RenderPass& pass, wgpu::TextureFormat format, const wgpu::Extent3D size,
                             wgpu::TextureView view, wgpu::TextureView resolveView = {}) {
  auto& color = pass.colorAttachments[SceneColorAttachmentIndex];
  color.semantic = ColorAttachmentSemantic::SceneColor;
  color.format = format;
  color.size = size;
  color.view = std::move(view);
  color.resolveView = std::move(resolveView);
  pass.colorAttachmentCount = 1;
}

struct OffscreenCacheKey {
  uint32_t width;
  uint32_t height;

  bool operator==(const OffscreenCacheKey& rhs) const { return width == rhs.width && height == rhs.height; }
  template <typename H>
  friend H AbslHashValue(H h, const OffscreenCacheKey& key) {
    return H::combine(std::move(h), key.width, key.height);
  }
};
struct OffscreenCacheEntry {
  webgpu::TextureWithSampler color;
  webgpu::TextureWithSampler depth;
};
absl::flat_hash_map<OffscreenCacheKey, OffscreenCacheEntry> g_offscreenCache;

// Pooled destinations for the public resolve_pass API. Entries are recycled
// per frame slot: a slot is only re-acquired after the render worker has
// submitted its previous frame, and queue serialization orders the new frame's
// copies after the old frame's reads.
struct PassSnapshotEntry {
  webgpu::TextureWithSampler color; // the pass's colour attachment format (RGBA16F in an HDR scene pass)
  uint32_t colorSourceSamples = 0;  // the pass's MSAA count when color was made (the copy reads it resolved)
  webgpu::TextureWithSampler depth; // R32Float raw depth
};
struct PassSnapshotPool {
  std::vector<PassSnapshotEntry> entries;
  size_t used = 0;
};
std::array<PassSnapshotPool, FrameSlotCount> g_passSnapshotPools;

// format and sourceSamples: the snapshotted pass's colour attachment, which the raw copy reads
// (resolved), so the snapshot has its format.
PassSnapshotEntry& acquire_pass_snapshot(uint32_t width, uint32_t height, wgpu::TextureFormat format,
                                         uint32_t sourceSamples, bool wantColor, bool wantDepth) {
  auto& pool = g_passSnapshotPools[g_recorder.frameSlot];
  if (pool.used == pool.entries.size()) {
    pool.entries.emplace_back();
  }
  auto& entry = pool.entries[pool.used++];
  const wgpu::Extent3D size{width, height, 1};
  if (wantColor && (!entry.color.texture || entry.color.size.width != width || entry.color.size.height != height ||
                    entry.color.format != format || entry.colorSourceSamples != sourceSamples)) {
    entry.colorSourceSamples = sourceSamples;
    const wgpu::TextureDescriptor desc{
        .label = "Pass Snapshot Color",
        .usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::TextureBinding,
        .dimension = wgpu::TextureDimension::e2D,
        .size = size,
        .format = format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    AURORA_NOTE_TEXTURE(&desc);
    auto texture = webgpu::g_device.CreateTexture(&desc);
    auto view = texture.CreateView();
    entry.color = webgpu::TextureWithSampler{
        .texture = std::move(texture),
        .view = std::move(view),
        .size = size,
        .format = format,
    };
  }
  if (wantDepth && (!entry.depth.texture || entry.depth.size.width != width || entry.depth.size.height != height)) {
    const wgpu::TextureDescriptor desc{
        .label = "Pass Snapshot Depth",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .dimension = wgpu::TextureDimension::e2D,
        .size = size,
        .format = wgpu::TextureFormat::R32Float,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    AURORA_NOTE_TEXTURE(&desc);
    auto texture = webgpu::g_device.CreateTexture(&desc);
    auto view = texture.CreateView();
    entry.depth = webgpu::TextureWithSampler{
        .texture = std::move(texture),
        .view = std::move(view),
        .size = size,
        .format = wgpu::TextureFormat::R32Float,
    };
  }
  return entry;
}

FramePacket& current_frame_packet() { return g_recorder.frame(); }

RenderPassList& current_render_passes() { return g_recorder.passes(); }

StagingHighWater current_high_water(const FramePacket& frame) noexcept {
  return {
      .verts = static_cast<uint32_t>(logical_size(frame.verts, frame.spillVerts)),
      .uniforms = static_cast<uint32_t>(logical_size(frame.uniforms, frame.spillUniforms)),
      .indices = static_cast<uint32_t>(logical_size(frame.indices, frame.spillIndices)),
      .storage = static_cast<uint32_t>(logical_size(frame.storage, frame.spillStorage)),
      .textureUpload = static_cast<uint32_t>(frame.textureUpload.size()),
      .textureUploadCount = frame.textureUploads.size(),
      .geometryUploadCount = frame.geometryUploads.size(),
      .bufferUploadCount = frame.bufferUploads.size(),
  };
}

FrameOp capture_frame_op(FramePacket& frame, FrameOpType type, uint32_t index) {
  FrameOp op{
      .type = type,
      .index = index,
      .renderPass =
          type == FrameOpType::RenderPass && index < frame.renderPasses.size() ? &frame.renderPasses[index] : nullptr,
      .textureCopy = type == FrameOpType::TextureCopy && index < frame.textureCopies.size()
                         ? &frame.textureCopies[index]
                         : nullptr,
      .encoderTask =
          type == FrameOpType::EncoderTask && index < frame.encoderTasks.size() ? &frame.encoderTasks[index] : nullptr,
      .highWater = current_high_water(frame),
  };
  op.textureUploads.reserve(op.highWater.textureUploadCount);
  for (size_t i = 0; i < op.highWater.textureUploadCount; ++i) {
    op.textureUploads.push_back(&frame.textureUploads[i]);
  }
  op.geometryUploads.reserve(op.highWater.geometryUploadCount);
  for (size_t i = 0; i < op.highWater.geometryUploadCount; ++i) {
    op.geometryUploads.push_back(&frame.geometryUploads[i]);
  }
  op.bufferUploads.reserve(op.highWater.bufferUploadCount);
  for (size_t i = 0; i < op.highWater.bufferUploadCount; ++i) {
    op.bufferUploads.push_back(&frame.bufferUploads[i]);
  }
  return op;
}

void seal_pass(FramePacket& frame, uint32_t passIndex) {
  if (passIndex >= frame.renderPasses.size()) {
    return;
  }
  auto& pass = frame.renderPasses[passIndex];
  if (pass.sealed) {
    return;
  }
  pass.sealed = true;
}

// The frame's buffers wrap mapped staging memory of a fixed size; growing one past it aborts.
static bool fits(const ByteBuffer& target, size_t length, size_t alignment) {
  const size_t begin = alignment != 0 ? AURORA_ALIGN(target.size(), alignment) : target.size();
  if (target.can_append(begin - target.size() + length))
    LIKELY { return true; }
  static bool warned = false;
  if (!warned) {
    warned = true;
    Log.warn("frame buffer full at {} bytes: dropping a {}-byte push (warned once)", target.size(), length);
  }
  return false;
}

Range map(ByteBuffer& target, size_t length, size_t alignment) {
  if (!fits(target, length, alignment)) {
    return OverflowRange;
  }
  if (alignment != 0) {
    const size_t begin = target.size();
    const size_t alignedBegin = AURORA_ALIGN(begin, alignment);
    if (alignedBegin > begin) {
      target.append_zeroes(alignedBegin - begin);
    }
  }
  auto begin = target.size();
  if (length > 0) {
    target.append_zeroes(length);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length)};
}

// The device buffer's per-frame part grows, once and with a wait for the render worker, when
// this frame needs more of it than it has.
static void reserve_device_bytes(StagingKind kind, uint64_t end) {
  if (end > frame_buffer_sizes().device(kind))
    UNLIKELY { grow_frame_buffer(kind, end, current_frame_packet().encoder); }
}

// Space for length bytes at the next alignment in a frame buffer: in its staging buffer while
// that has room, else (the first time is logged) in the spill, as far as the device buffer
// can hold (its limit). The offset is where the data lands in the device buffer, after its
// resident region. `window` is extra room the device buffer must have after it (a uniform's
// bound window), not written. Null, and no space taken, when the frame outgrows the limit.
static uint8_t* reserve_frame_bytes(ByteBuffer& target, SpillBuffer& spill, StagingKind kind, size_t length,
                                    size_t alignment, size_t window, const char* what, uint32_t& offset) {
  const auto& sizes = frame_buffer_sizes();
  const uint64_t limit = sizes.limit(kind);
  const uint64_t base = sizes.base(kind);
  const size_t cap = target.capacity();
  size_t begin = alignment != 0 ? AURORA_ALIGN(target.size() + spill.length, alignment) : target.size() + spill.length;
  if (spill.length == 0 && begin + length <= cap && begin + length + window <= limit) {
    reserve_device_bytes(kind, begin + length + window);
    if (begin > target.size()) {
      target.append_zeroes(begin - target.size());
    }
    offset = static_cast<uint32_t>(base + begin);
    return length > 0 ? target.append_uninit(length) : target.data() + begin;
  }
  begin = std::max(begin, cap);
  if (begin + length + window > limit) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      Log.warn("frame {} buffer full at {} bytes: dropping a {}-byte push (warned once)", what, begin, length);
    }
    return nullptr;
  }
  reserve_device_bytes(kind, begin + length + window);
  if (!spill.data) {
    spill.capacity = static_cast<size_t>(limit - cap);
    spill.data = std::unique_ptr<uint8_t[]>(new uint8_t[spill.capacity]);
    Log.info("frame {} data outgrew its {} MiB staging buffer: spilling into a transient buffer (up to {} MiB)",
             what, cap >> 20, limit >> 20);
  }
  target.fill_to_capacity();
  const size_t at = begin - cap;
  if (at > spill.length) {
    memset(spill.data.get() + spill.length, 0, at - spill.length);
  }
  spill.length = at + length;
  offset = static_cast<uint32_t>(base + begin);
  return spill.data.get() + at;
}

static Range push(ByteBuffer& target, SpillBuffer& spill, StagingKind kind, const uint8_t* data, size_t length,
                  size_t alignment, size_t window, const char* what) {
  uint32_t offset = 0;
  // An unaligned push is meant to continue the previous one (merged draws), so one that crosses the
  // end of the staging buffer is split across it and the spill instead of moving to the spill.
  const size_t cap = target.capacity();
  const size_t used = target.size() + spill.length;
  if (alignment == 0 && window == 0 && spill.length == 0 && used < cap && used + length > cap &&
      used + length <= frame_buffer_sizes().limit(kind)) {
    const size_t head = cap - used;
    uint8_t* first = reserve_frame_bytes(target, spill, kind, head, 0, 0, what, offset);
    if (first == nullptr) {
      return OverflowRange;
    }
    memcpy(first, data, head);
    uint32_t tailOffset = 0;
    uint8_t* tail = reserve_frame_bytes(target, spill, kind, length - head, 0, 0, what, tailOffset);
    if (tail == nullptr) {
      return OverflowRange;
    }
    memcpy(tail, data + head, length - head);
    return {offset, static_cast<uint32_t>(length)};
  }
  uint8_t* dst = reserve_frame_bytes(target, spill, kind, length, alignment, window, what, offset);
  if (dst == nullptr) {
    return OverflowRange;
  }
  if (length > 0) {
    memcpy(dst, data, length);
  }
  return {offset, static_cast<uint32_t>(length)};
}

// For our public API, warn instead of fatal-ing when called outside an active recording frame.
bool check_recording(const char* name) {
  if (!g_recorder.active())
    UNLIKELY {
      Log.warn("{}: called outside an active frame", name);
      return false;
    }
  return true;
}

void push_command(CommandType type, const Command::Data& data) {
  if (g_recorder.currentRenderPass == UINT32_MAX)
    UNLIKELY {
      Log.warn("Dropping command {}", magic_enum::enum_name(type));
      return;
    }
  auto& renderPass = current_render_passes()[g_recorder.currentRenderPass];
  AURORA_ASSERT(!renderPass.sealed, "Attempted to append command {} to sealed render pass {}",
                magic_enum::enum_name(type), g_recorder.currentRenderPass);
  if (type == CommandType::Draw || type == CommandType::CustomDraw) {
    renderPass.hasDraws = true;
  }
  renderPass.commands.push_back({
      .type = type,
#ifdef AURORA_GFX_DEBUG_GROUPS
      .debugGroupStack = g_recorder.debugGroupStack,
#endif
      .data = data,
  });
}

// Inline payload helpers: draw_payload.hpp (shared with the stereo eye encoder).

void push_draw_command(DrawCommand data) {
  ++g_recorder.routeDrawCounts[std::min<size_t>(g_recorder.stereoRoute, g_recorder.routeDrawCounts.size() - 1)];
  push_command(CommandType::Draw, Command::Data{.draw = data});
  ++g_recorder.drawCallCount;
}

float gx_depth(float depth) { return gx::UseReversedZ ? 1.f - depth : depth; }

// Every EFB draw says whether it may test against depth (reads) and write it (writes), for
// DepthAfter::IfUnread. Its fragments' depths lie within the viewport's depth range.
void note_draw_depth(bool reads, bool writes) {
  if (g_recorder.inOffscreen) {
    return;
  }
  const auto& vp = g_recorder.cachedViewport;
  if (g_recorder.heldDepth) {
    // A test that only meets depths nearer than the EFB's nearest passes or fails the same against
    // a cleared depth buffer, whatever the compare function.
    if (reads && std::max(vp.znear, vp.zfar) >= g_recorder.efbDepthNear) {
      g_recorder.heldDepth->read = true;
    }
  } else if (writes) {
    g_recorder.efbDepthNear = std::min(g_recorder.efbDepthNear, std::min(vp.znear, vp.zfar));
  }
}

OffscreenCacheEntry get_offscreen_textures(uint32_t width, uint32_t height) {
  OffscreenCacheKey key{width, height};
  if (const auto it = g_offscreenCache.find(key); it != g_offscreenCache.end()) {
    return it->second;
  }
  const auto colorFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const wgpu::Extent3D size{width, height, 1};
  const wgpu::TextureDescriptor colorDesc{
      .label = "Offscreen Color",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc |
               wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = colorFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  AURORA_NOTE_TEXTURE(&colorDesc);
  auto colorTexture = webgpu::g_device.CreateTexture(&colorDesc);
  auto colorView = colorTexture.CreateView();
  webgpu::TextureWithSampler color{
      .texture = std::move(colorTexture),
      .view = std::move(colorView),
      .size = size,
      .format = colorFormat,
  };
  const auto depthFormat = webgpu::g_graphicsConfig.depthFormat;
  const wgpu::TextureDescriptor depthDesc{
      .label = "Offscreen Depth",
      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = depthFormat,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  AURORA_NOTE_TEXTURE(&depthDesc);
  auto depthTexture = webgpu::g_device.CreateTexture(&depthDesc);
  auto depthView = depthTexture.CreateView();
  webgpu::TextureWithSampler depth{
      .texture = std::move(depthTexture),
      .view = std::move(depthView),
      .size = size,
      .format = depthFormat,
  };
  OffscreenCacheEntry entry{
      .color = std::move(color),
      .depth = std::move(depth),
  };
  auto [insertIt, _] = g_offscreenCache.emplace(key, std::move(entry));
  return insertIt->second;
}

uint32_t enqueue_pass(FramePacket& frame, uint32_t passIndex, bool dispatch = true);
void discard_dead_stores(RenderPass& pass, bool colorDead, bool depthDead);
void release_held_depth(bool keep);

void resume_efb_pass_loading(const RenderPass& prevPass) {
  RenderPass newPass{
      .label = pass_label("EFB"),
      .colorAttachments = prevPass.colorAttachments,
      .colorAttachmentCount = prevPass.colorAttachmentCount,
      .depthStencilView = prevPass.depthStencilView,
      .depthStencilFormat = prevPass.depthStencilFormat,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .msaaSamples = prevPass.msaaSamples,
      .clearDepth = false,
      .hasDepth = prevPass.hasDepth,
      .hasStencil = prevPass.hasStencil,
      // Still the EFB: the stereo replay carries on into the eyes after a copy.
      .efb = prevPass.efb,
  };
  for (uint32_t i = 0; i < newPass.colorAttachmentCount; ++i) {
    newPass.colorAttachments[i].loadOp = wgpu::LoadOp::Undefined;
    newPass.colorAttachments[i].clear = false;
  }
  newPass.commands.reserve(2048);
  current_render_passes().emplace_back(std::move(newPass));
  ++g_recorder.currentRenderPass;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
}

void suspend_efb() {
  AURORA_ASSERT(g_recorder.active() && g_recorder.currentRenderPass != UINT32_MAX,
                "suspend_efb called outside of an active recording frame");
  AURORA_ASSERT(!g_recorder.inOffscreen, "suspend_efb called while offscreen rendering is active");
  AURORA_ASSERT(!g_recorder.suspendedEfbPass, "suspend_efb called with an EFB pass already suspended");
  if (g_recorder.heldDepth) {
    // The held pass's continuation may be moved out of the list below.
    release_held_depth(true);
  }

  auto& currentPass = current_render_passes()[g_recorder.currentRenderPass];
  if (!currentPass.has_consumer()) {
    g_recorder.suspendedEfbPass = std::move(currentPass);
    current_render_passes().pop_back();
    --g_recorder.currentRenderPass;
  } else {
    enqueue_pass(current_frame_packet(), g_recorder.currentRenderPass);
  }
  g_recorder.suspendedEfbViewport = g_recorder.cachedViewport;
  g_recorder.suspendedEfbScissor = g_recorder.cachedScissor;
}

void finish_current_offscreen() {
  AURORA_ASSERT(g_recorder.active() && g_recorder.currentRenderPass != UINT32_MAX,
                "finish_current_offscreen called outside of an active recording frame");
  AURORA_ASSERT(g_recorder.inOffscreen, "finish_current_offscreen called without an active offscreen pass");

  auto& offscreenPass = current_render_passes()[g_recorder.currentRenderPass];
  offscreenPass.discardable = !offscreenPass.has_consumer();
  discard_dead_stores(offscreenPass, true, true);
  enqueue_pass(current_frame_packet(), g_recorder.currentRenderPass);
  g_recorder.offscreenColor = {};
  g_recorder.offscreenDepth = {};
}

void start_offscreen(uint32_t width, uint32_t height) {
  AURORA_ASSERT(width != 0 && height != 0, "start_offscreen requires nonzero dimensions ({}x{})", width, height);
  AURORA_ASSERT(g_recorder.active(), "start_offscreen called outside of an active recording frame");

  auto offscreenEntry = get_offscreen_textures(width, height);
  g_recorder.offscreenColor = std::move(offscreenEntry.color);
  g_recorder.offscreenDepth = std::move(offscreenEntry.depth);

  RenderPass newPass{
      .label = pass_label("Offscreen"),
      .depthStencilView = g_recorder.offscreenDepth.view,
      .depthStencilFormat = g_recorder.offscreenDepth.format,
      .copySourceTexture = g_recorder.offscreenColor.texture,
      .copySourceView = g_recorder.offscreenColor.view,
      .copySourceDepthView = g_recorder.offscreenDepth.view,
      .msaaSamples = 1,
      .clearDepthValue = gx::UseReversedZ ? 0.f : 1.f,
      .clearDepth = true,
      .hasDepth = true,
      .hasStencil = false,
  };
  set_single_color_target(newPass, g_recorder.offscreenColor.format, {width, height}, g_recorder.offscreenColor.view);
  current_render_passes().emplace_back(std::move(newPass));
  ++g_recorder.currentRenderPass;
  g_recorder.inOffscreen = true;

  g_recorder.cachedViewport = {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
  g_recorder.cachedScissor = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
}

void restore_efb() {
  AURORA_ASSERT(g_recorder.active(), "restore_efb called outside of an active recording frame");
  AURORA_ASSERT(g_recorder.inOffscreen, "restore_efb called without a suspended EFB pass");

  g_recorder.inOffscreen = false;
  if (g_recorder.suspendedEfbPass) {
    current_render_passes().emplace_back(std::move(*g_recorder.suspendedEfbPass));
    g_recorder.suspendedEfbPass.reset();
  } else {
    auto& pass = current_render_passes().emplace_back();
    pass.label = pass_label("EFB");
    set_efb_targets(pass);
    for (uint32_t i = 0; i < pass.colorAttachmentCount; ++i) {
      pass.colorAttachments[i].clear = false;
      pass.colorAttachments[i].loadOp = wgpu::LoadOp::Undefined;
    }
    pass.clearDepth = false;
  }
  ++g_recorder.currentRenderPass;
  set_efb_targets(current_render_passes()[g_recorder.currentRenderPass]);

  g_recorder.cachedViewport = g_recorder.suspendedEfbViewport;
  g_recorder.cachedScissor = g_recorder.suspendedEfbScissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
}

void log_pass(const FramePacket& frame, const RenderPass& p);

void enqueue_op(FramePacket& frame, uint32_t opIndex) {
  if (g_recorder.heldDepth) {
    // Whatever comes next may load the depth, so the held ops go first, keeping it.
    release_held_depth(true);
  }
  if (opIndex >= frame.ops.size()) {
    return;
  }
  if (frame.ops[opIndex].renderPass != nullptr) {
    log_pass(frame, *frame.ops[opIndex].renderPass);
  }
  if (g_recorder.suppressRenderWorker) {
    return;
  }
  auto op = frame.ops[opIndex];
  render_worker::enqueue_encode_pass(frame.frameId, opIndex, [packet = &frame, op = std::move(op)] {
    if (op.renderPass == nullptr && op.textureCopy == nullptr && op.encoderTask == nullptr) {
      return;
    }
    const perf::Timer timer{perf::g_encodeNs};
    encode_op(packet->encoder, *packet, op);
  });
}

// Gives an EFB pass of an immersive frame its eye passes: encoding.cpp
// re-encodes the pass into each eye target right after the mono pass.
void stereo_seal_pass(FramePacket& frame, uint32_t passIndex) {
  if (passIndex >= frame.renderPasses.size()) {
    return;
  }
  auto& pass = frame.renderPasses[passIndex];
  auto& state = frame.stereo;
  if (!state.immersive || !pass.efb || pass.sealed || pass.stereo.enabled || pass.stereo.monoOnly) {
    return;
  }
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    const auto& target = stereo_eye_target(eye);
    if (!target.valid()) {
      return;
    }
    // A foveated frame's eye pass renders through the foveated view of the
    // same image (stereo_foveation.hpp); the copies keep the eye's own view.
    const bool foveatedEye = !state.multiview && state.foveated && state.foveatedEyeColorViews[eye];
    pass.stereo.eyes[eye] = StereoEyePass{
        .colorView = foveatedEye ? state.foveatedEyeColorViews[eye] : target.color.view,
        .resolveView = target.sampleCount > 1 ? target.resolved.view : wgpu::TextureView{},
        .depthView = target.depth.view,
        .copySourceView = target.color.view,
        .size = {target.width, target.height, 1},
    };
  }
  if (state.multiview) {
    // Both eyes in one pass over the targets' two layers (stereo_multiview.hpp).
    const auto& multiview = stereo_multiview_target();
    if (!multiview.colorView || !multiview.depthView) {
      return;
    }
    const bool foveated = state.foveated && state.foveatedMultiviewColorView;
    pass.stereo.multiview = true;
    pass.stereo.multiviewColorView = foveated ? state.foveatedMultiviewColorView : multiview.colorView;
    pass.stereo.multiviewDepthView = multiview.depthView;
    pass.stereo.foveated = foveated;
  } else {
    pass.stereo.foveated = state.foveated && state.foveatedEyeColorViews[0] && state.foveatedEyeColorViews[1];
  }
  pass.stereo.enabled = true;
  state.replayed = true;
}

// Lets the GPU skip writing out attachments that nothing reads again, which saves a tile-based
// (mobile) GPU a full-resolution store. colorDead and depthDead say no later pass loads the
// contents (the next one clears them, or none follows). Call before the pass is enqueued.
bool pass_copies_depth(const RenderPass& pass) {
  return pass.snapshotDepthDst || (pass.resolveTarget && gx::is_depth_format(pass.resolveFormat)) ||
         (pass.captureDepthSnapshot && depth_peek::enabled());
}

void discard_dead_stores(RenderPass& pass, bool colorDead, bool depthDead) {
  auto& color = pass.colorAttachments[SceneColorAttachmentIndex];
  // Without a resolve target the attachment itself is what copies and presentation read.
  if (colorDead && pass.colorAttachmentCount > 0 && color.resolveView) {
    color.storeOp = wgpu::StoreOp::Discard;
  }
  if (depthDead && pass.hasDepth && !pass_copies_depth(pass)) {
    pass.depthStoreOp = wgpu::StoreOp::Discard;
  }
}

void log_pass(const FramePacket& frame, const RenderPass& p) {
  // MP_PASS_LOG=N logs every pass of one frame in N, to find what breaks the frame up.
  static const int every = [] {
    const char* env = std::getenv("MP_PASS_LOG");
    return env != nullptr ? std::atoi(env) : 0;
  }();
  if (every <= 0 || frame.frameId % every != 0) {
    return;
  }
  const auto& c = p.colorAttachments[SceneColorAttachmentIndex];
  size_t draws = 0;
  for (const auto& cmd : p.commands) {
    draws += cmd.type == CommandType::Draw || cmd.type == CommandType::CustomDraw;
  }
  Log.info("pass f{} '{}' {}x{} msaa{} draws{} cload{} cstore{} dclear{} dstore{} resolve{} {}x{}+{},{} fmt{} probe{} "
           "snapC{} snapD{} discardable{}",
           frame.frameId, p.label, c.size.width, c.size.height, p.msaaSamples, draws,
           c.clear ? "C" : "L", static_cast<int>(c.storeOp), p.clearDepth,
           static_cast<int>(p.depthStoreOp), static_cast<bool>(p.resolveTarget), p.resolveRect.width,
           p.resolveRect.height, p.resolveRect.x, p.resolveRect.y, static_cast<int>(p.resolveFormat), p.probeFace,
           static_cast<bool>(p.snapshotColorDst), static_cast<bool>(p.snapshotDepthDst), p.discardable);
}

// Without dispatch the op is only recorded; enqueue_op hands it to the render worker later.
uint32_t enqueue_pass(FramePacket& frame, uint32_t passIndex, bool dispatch) {
  stereo_seal_pass(frame, passIndex);
  if (!frame.renderPasses[passIndex].discardable) {
    ++g_recorder.renderPassCount;
  }
  seal_pass(frame, passIndex);
  const auto opIndex = static_cast<uint32_t>(frame.ops.size());
  frame.ops.emplace_back(capture_frame_op(frame, FrameOpType::RenderPass, passIndex));
  if (dispatch) {
    enqueue_op(frame, opIndex);
  }
  return opIndex;
}

// keep: the pass after the held ones loads their depth; else it clears it and their store is dropped.
void release_held_depth(bool keep) {
  const auto held = *g_recorder.heldDepth;
  g_recorder.heldDepth.reset();
  auto& frame = current_frame_packet();
  if (keep) {
    frame.renderPasses[held.nextPass].clearDepth = false;
  } else {
    discard_dead_stores(frame.renderPasses[held.sealedPass], false, true);
  }
  enqueue_op(frame, held.passOp);
  enqueue_op(frame, held.taskOp);
}
} // namespace

namespace detail {

void begin_recording(FramePacket& packet, size_t frameSlot) {
  CHECK(!g_recorder.active(), "A recording session is already active");
  g_recorder.packet = &packet;
  g_recorder.frameSlot = frameSlot;
  g_passSnapshotPools[frameSlot].used = 0;
  g_recorder.drawCallCount = 0;
  g_recorder.routeDrawCounts = {};
  g_recorder.mergedDrawCallCount = 0;
  g_recorder.renderPassCount = 0;
  g_recorder.suspendedEfbPass.reset();
  g_recorder.stereoRoute = AURORA_STEREO_ROUTE_WORLD;
  g_recorder.stereoScreenTexMtx = {};
  g_recorder.placement3D = {};
  g_recorder.hdrScene = false;
  g_recorder.hdrSceneCleared = false;

  current_render_passes().emplace_back();
  auto& pass = current_render_passes()[0];
  pass.label = pass_label("EFB");
  set_efb_targets(pass);
  pass.colorAttachments[SceneColorAttachmentIndex].clearValue = gx::g_gxState.clearColor;
  pass.clearDepthValue = gx::clear_depth_value();
  g_recorder.heldDepth.reset();
  g_recorder.efbDepthNear = gx_depth(pass.clearDepthValue);
  g_recorder.currentRenderPass = 0;
  g_recorder.cachedViewport = gx::map_logical_viewport(gx::g_gxState.logicalViewport);
  g_recorder.cachedScissor = gx::map_logical_scissor(gx::g_gxState.logicalScissor);
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
}

RecordedFrame end_recording() {
  CHECK(g_recorder.active(), "No active recording session");
  AURORA_ASSERT(!g_recorder.inOffscreen, "end_frame called while offscreen rendering is active");
  AURORA_ASSERT(g_recorder.currentRenderPass == UINT32_MAX,
                "end_frame called before finish finalized the current render pass");
  if (g_recorder.heldDepth) {
    release_held_depth(true);
  }
  auto& frame = g_recorder.frame();
  frame.stats.drawCallCount = g_recorder.drawCallCount;
  frame.stats.mergedDrawCallCount = g_recorder.mergedDrawCallCount;
  frame.stats.renderPassCount = g_recorder.renderPassCount;
  const size_t vertSize = logical_size(frame.verts, frame.spillVerts);
  const size_t uniformSize = logical_size(frame.uniforms, frame.spillUniforms);
  const size_t indexSize = logical_size(frame.indices, frame.spillIndices);
  const size_t storageSize = logical_size(frame.storage, frame.spillStorage);
  frame.stats.lastVertSize = vertSize;
  frame.stats.lastUniformSize = uniformSize;
  frame.stats.lastIndexSize = indexSize;
  frame.stats.lastStorageSize = storageSize;
  frame.stats.lastTextureUploadSize = frame.textureUpload.size();
  if (frame.spillVerts.length != 0) {
    request_staging_growth(StagingKind::Vertex, vertSize);
  }
  if (frame.spillUniforms.length != 0) {
    request_staging_growth(StagingKind::Uniform, uniformSize);
  }
  if (frame.spillIndices.length != 0) {
    request_staging_growth(StagingKind::Index, indexSize);
  }
  if (frame.spillStorage.length != 0) {
    request_staging_growth(StagingKind::Storage, storageSize);
  }
  {
    // AURORA_FRAME_PEAKS=1 logs each new high-water mark of a frame's buffer use.
    static const bool enabled = std::getenv("AURORA_FRAME_PEAKS") != nullptr;
    static uint64_t peakVert, peakUniform, peakIndex, peakStorage;
    if (enabled && (vertSize > peakVert || uniformSize > peakUniform || indexSize > peakIndex ||
                    storageSize > peakStorage)) {
      peakVert = std::max<uint64_t>(peakVert, vertSize);
      peakUniform = std::max<uint64_t>(peakUniform, uniformSize);
      peakIndex = std::max<uint64_t>(peakIndex, indexSize);
      peakStorage = std::max<uint64_t>(peakStorage, storageSize);
      Log.info("frame peaks: vertex {} uniform {} index {} storage {} (bytes)", peakVert, peakUniform, peakIndex,
               peakStorage);
    }
  }

  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
#if defined(AURORA_GFX_DEBUG_GROUPS)
  if (!g_recorder.debugGroupStack.empty()) {
    for (auto& item : std::ranges::reverse_view(g_recorder.debugGroupStack)) {
      Log.warn("Debug group was not popped at end of frame: {}", item);
    }
    g_recorder.debugGroupStack.clear();
  }
#endif
  const RecordedFrame recorded{.packet = g_recorder.packet, .frameSlot = g_recorder.frameSlot};
  g_recorder.hdrScene = false;
  g_recorder.hdrSceneCleared = false;
  g_recorder.packet = nullptr;
  g_recorder.frameSlot = 0;
  return recorded;
}

void shutdown_recording() {
  for (auto& pool : g_passSnapshotPools) {
    pool = {};
  }
  g_recorder.currentRenderPass = UINT32_MAX;
  g_offscreenCache.clear();
  g_recorder.offscreenColor = {};
  g_recorder.offscreenDepth = {};
  g_recorder.suspendedEfbPass.reset();
  g_recorder.heldDepth.reset();
  g_recorder.inOffscreen = false;
  g_recorder.hdrScene = false;
  g_recorder.hdrSceneCleared = false;
  g_recorder.packet = nullptr;
  g_recorder.frameSlot = 0;
  g_recorder.suppressRenderWorker = false;
}

namespace testing {

void suppress_render_worker(bool suppress) noexcept { g_recorder.suppressRenderWorker = suppress; }

void seed_offscreen_cache(uint32_t width, uint32_t height, wgpu::TextureFormat colorFormat,
                          wgpu::TextureFormat depthFormat) {
  const wgpu::Extent3D size{width, height, 1};
  g_offscreenCache.insert_or_assign(OffscreenCacheKey{width, height},
                                    OffscreenCacheEntry{
                                        .color = {.size = size, .format = colorFormat},
                                        .depth = {.size = size, .format = depthFormat},
                                    });
}

} // namespace testing

void increment_merged_draw_count(uint32_t count) noexcept {
  if (g_recorder.active()) {
    g_recorder.mergedDrawCallCount += count;
  }
}

} // namespace detail

void queue_texture_upload(TextureUpload upload) {
  if (g_recorder.currentRenderPass != UINT32_MAX) {
    AURORA_ASSERT(!current_render_passes()[g_recorder.currentRenderPass].sealed,
                  "Attempted to append texture upload to sealed render pass {}", g_recorder.currentRenderPass);
  }
  current_frame_packet().textureUploads.emplace_back(std::move(upload));
}

void queue_texture_upload_data(const uint8_t* data, uint32_t bytesPerRow, uint32_t rowsPerImage,
                               wgpu::TexelCopyTextureInfo tex, wgpu::Extent3D size) {
  const auto copyBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
  auto& frame = current_frame_packet();
  if (frame.textureUpload.size() + copyBytesPerRow * rowsPerImage <= frame_buffer_sizes().textureUpload) {
    const auto range = push_texture_data(data, bytesPerRow, rowsPerImage);
    const wgpu::TexelCopyBufferLayout layout{
        .offset = range.offset,
        .bytesPerRow = bytesPerRow,
        .rowsPerImage = rowsPerImage,
    };
    queue_texture_upload(TextureUpload{layout, std::move(tex), size});
    return;
  }

  const uint64_t uploadSize = copyBytesPerRow * rowsPerImage;
  const wgpu::BufferDescriptor descriptor{
      .label = "Overflow Texture Upload Buffer",
      .usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc,
      .size = uploadSize,
      .mappedAtCreation = true,
  };
  auto buffer = webgpu::g_device.CreateBuffer(&descriptor);
  auto* dst = static_cast<uint8_t*>(buffer.GetMappedRange(0, uploadSize));
  for (uint32_t row = 0; row < rowsPerImage; ++row) {
    memcpy(dst, data, bytesPerRow);
    data += bytesPerRow;
    dst += copyBytesPerRow;
  }
  buffer.Unmap();

  const wgpu::TexelCopyBufferLayout layout{
      .offset = 0,
      .bytesPerRow = bytesPerRow,
      .rowsPerImage = rowsPerImage,
  };
  queue_texture_upload(TextureUpload{layout, std::move(tex), size, std::move(buffer)});
}

bool queue_resident_upload(ResidentBuffer kind, uint32_t offset, const uint8_t* data, size_t size) {
  if (size == 0) {
    return true;
  }
  if (!check_recording("queue_resident_upload")) {
    return false;
  }
  // A buffer copy moves whole words.
  const uint64_t copySize = AURORA_ALIGN(uint64_t(size), 4);
  const wgpu::BufferDescriptor descriptor{
      .label = "Resident Upload Buffer",
      .usage = wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc,
      .size = copySize,
      .mappedAtCreation = true,
  };
  auto buffer = webgpu::g_device.CreateBuffer(&descriptor);
  auto* mapped = static_cast<uint8_t*>(buffer.GetMappedRange(0, copySize));
  memcpy(mapped, data, size);
  if (copySize > size) {
    memset(mapped + size, 0, copySize - size);
  }
  buffer.Unmap();
  if (g_recorder.currentRenderPass != UINT32_MAX) {
    AURORA_ASSERT(!current_render_passes()[g_recorder.currentRenderPass].sealed,
                  "Attempted to append buffer upload to sealed render pass {}", g_recorder.currentRenderPass);
  }
  // Copied with the staging data before the pass being recorded is encoded.
  current_frame_packet().bufferUploads.emplace_back(BufferUpload{
      .src = std::move(buffer),
      .dst = kind,
      .dstOffset = offset,
      .size = copySize,
  });
  return true;
}

void queue_texture_copy(wgpu::TexelCopyTextureInfo src, wgpu::TexelCopyTextureInfo dst, wgpu::Extent3D size) {
  ZoneScoped;
  auto& frame = current_frame_packet();
  if (g_recorder.currentRenderPass != UINT32_MAX) {
    enqueue_pass(frame, g_recorder.currentRenderPass);
    g_recorder.currentRenderPass = UINT32_MAX;
  }

  const auto copyIndex = static_cast<uint32_t>(frame.textureCopies.size());
  frame.textureCopies.emplace_back(TextureCopy{
      .src = std::move(src),
      .dst = std::move(dst),
      .size = size,
  });
  const auto opIndex = static_cast<uint32_t>(frame.ops.size());
  frame.ops.emplace_back(capture_frame_op(frame, FrameOpType::TextureCopy, copyIndex));
  enqueue_op(frame, opIndex);
}

void begin_color_pass(const ColorPassDescriptor& desc) {
  ZoneScoped;
  auto& frame = current_frame_packet();
  if (g_recorder.currentRenderPass != UINT32_MAX) {
    enqueue_pass(frame, g_recorder.currentRenderPass);
  }

  RenderPass pass{
      .label = desc.label != nullptr ? desc.label : "",
      .depthStencilView = desc.depthStencilView,
      .depthStencilFormat = desc.depthStencilFormat,
      .msaaSamples = desc.sampleCount,
      .clearDepthValue = desc.depthClearValue,
      .depthLoadOp = desc.depthLoadOp,
      .depthStoreOp = desc.depthStoreOp,
      .stencilLoadOp = desc.stencilLoadOp,
      .stencilStoreOp = desc.stencilStoreOp,
      .stencilClearValue = desc.stencilClearValue,
      .clearDepth = desc.depthLoadOp == wgpu::LoadOp::Clear,
      .hasDepth = desc.hasDepth,
      .hasStencil = desc.hasStencil,
  };
  set_single_color_target(pass, desc.colorFormat, desc.targetSize, desc.colorView, desc.resolveView);
  auto& color = pass.colorAttachments[SceneColorAttachmentIndex];
  color.clearValue = {
      static_cast<float>(desc.clearColor.r),
      static_cast<float>(desc.clearColor.g),
      static_cast<float>(desc.clearColor.b),
      static_cast<float>(desc.clearColor.a),
  };
  color.loadOp = desc.colorLoadOp;
  color.storeOp = desc.colorStoreOp;
  color.clear = desc.colorLoadOp == wgpu::LoadOp::Clear;
  pass.commands.reserve(128);
  frame.renderPasses.emplace_back(std::move(pass));
  g_recorder.currentRenderPass = static_cast<uint32_t>(frame.renderPasses.size() - 1);

  g_recorder.cachedViewport = {
      0.f, 0.f, static_cast<float>(desc.targetSize.width), static_cast<float>(desc.targetSize.height), 0.f, 1.f};
  g_recorder.cachedScissor = {0, 0, static_cast<int32_t>(desc.targetSize.width),
                              static_cast<int32_t>(desc.targetSize.height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
}

void end_color_pass() {
  ZoneScoped;
  if (g_recorder.currentRenderPass == UINT32_MAX) {
    return;
  }
  enqueue_pass(current_frame_packet(), g_recorder.currentRenderPass);
  g_recorder.currentRenderPass = UINT32_MAX;
}

template <>
gx::DrawData* get_last_draw_command() {
  if (g_recorder.currentRenderPass >= current_render_passes().size()) {
    return nullptr;
  }
  auto& last = current_render_passes()[g_recorder.currentRenderPass].commands.back();
  if (last.type != CommandType::Draw || last.data.draw.encoder != encode_draw<gx::render, gx::DrawData>) {
    return nullptr;
  }
  return &inline_payload<gx::DrawData>(last.data.draw.payload.data());
}

Vec2<uint32_t> get_render_target_size() noexcept {
  // The GX FIFO worker can process a viewport command after the frame's
  // recording session has ended (e.g. during a world transition that waits on
  // the FIFO). frame() would then dereference a null packet; fall back to the
  // window size instead.
  if (g_recorder.active() && g_recorder.currentRenderPass < current_render_passes().size()) {
    const auto& size =
        current_render_passes()[g_recorder.currentRenderPass].colorAttachments[SceneColorAttachmentIndex].size;
    return {size.width, size.height};
  }
  const auto windowSize = window::get_window_size();
  return {windowSize.fb_width, windowSize.fb_height};
}

void set_viewport(const Viewport& cmd) noexcept {
  if (cmd != g_recorder.cachedViewport) {
    push_command(CommandType::SetViewport, Command::Data{.setViewport = cmd});
    g_recorder.cachedViewport = cmd;
  }
}

void set_scissor(const ClipRect& cmd) noexcept {
  if (cmd != g_recorder.cachedScissor) {
    push_command(CommandType::SetScissor, Command::Data{.setScissor = cmd});
    g_recorder.cachedScissor = cmd;
  }
}

// --- stereo replay ---

void set_frame_stereo(const StereoFrameState& state) noexcept {
  stereo_shadow::begin_frame(state.immersive, state.multiview);
  if (!g_recorder.active()) {
    return;
  }
  g_recorder.frame().stereo = state;
}

StereoFrameState recorded_stereo_state() noexcept {
  if (!g_recorder.active()) {
    return {};
  }
  return g_recorder.frame().stereo;
}

void set_stereo_draw_route(uint8_t route) noexcept { g_recorder.stereoRoute = route; }

bool recording_multiview() noexcept { return g_recorder.active() && g_recorder.frame().stereo.multiview; }

void set_final_pass_mono_unneeded(bool unneeded) noexcept { g_recorder.finalPassMonoUnneeded = unneeded; }

void set_stereo_diagnostics(bool enabled) noexcept {
  g_stereoDiagnostics.store(enabled || frame_stats_forced(), std::memory_order_relaxed);
}

uint8_t stereo_draw_route() noexcept { return g_recorder.stereoRoute; }

void set_stereo_head_locked_plane(float tanHalfWidth, float tanHalfHeight, float distance) noexcept {
  g_recorder.headLockedPlane = {tanHalfWidth, tanHalfHeight, distance};
}

stereo_replay::HeadLockedPlane stereo_head_locked_plane() noexcept { return g_recorder.headLockedPlane; }

void set_stereo_placement_3d(const StereoPlacement3D& placement) noexcept { g_recorder.placement3D = placement; }

StereoPlacement3D stereo_placement_3d() noexcept { return g_recorder.placement3D; }

void set_stereo_screen_tex_mtx(uint8_t texSlot, uint8_t pnSlot) noexcept {
  g_recorder.stereoScreenTexMtx = {texSlot, pnSlot};
}

StereoScreenTexMtx stereo_screen_tex_mtx() noexcept { return g_recorder.stereoScreenTexMtx; }

std::array<uint32_t, 2> stage_stereo_uniforms(const uint8_t* mono, Range monoRange,
                                              const StereoUniformLayout& layout) noexcept {
  constexpr std::array<uint32_t, 2> none{UINT32_MAX, UINT32_MAX};
  g_recorder.multiviewMode = gx::MultiviewNone;
  if (!g_recorder.active()) {
    return none;
  }
  auto& frame = g_recorder.frame();
  const auto& state = frame.stereo;
  if (!state.immersive || g_recorder.inOffscreen || g_recorder.currentRenderPass == UINT32_MAX) {
    return none;
  }
  const uint8_t route = g_recorder.stereoRoute;
  if (route == AURORA_STEREO_ROUTE_SKIP) {
    return none;
  }
  if (!layout.valid() || monoRange.size != layout.size) {
    return none;
  }
  const size_t alignment = resources().limits.minUniformBufferOffsetAlignment;
  const auto exhausted = [&] {
    if (!frame.stereo.uniformsExhausted) {
      Log.warn("Stereo replay ran out of uniform space at {} bytes; the rest of frame {} is mono only",
               frame.uniforms.size(), frame.frameId);
      frame.stereo.uniformsExhausted = true;
    }
    return none;
  };
  const auto& pass = current_render_passes()[g_recorder.currentRenderPass];
  const auto& monoSize = pass.colorAttachments[SceneColorAttachmentIndex].size;
  Mat4x4<float> monoProjection;
  std::memcpy(&monoProjection, mono + layout.projectionOffset, sizeof(monoProjection));
  // gx/shader.cpp MultiviewClip (and EyeClipImmediate for a frame replayed per eye):
  // the draw keeps its mono uniform, followed by each eye's clip matrix and the eyes'
  // render size (the eye target's, as compose_stereo_uniform makes it, or for 2D
  // content the mono one).
  const auto stage_eye_clips = [&](const std::array<Mat4x4<float>, AURORA_STEREO_EYE_COUNT>& clips,
                                   bool eyeRenderSize) {
    struct EyeClipBlock {
      std::array<Mat4x4<float>, AURORA_STEREO_EYE_COUNT> clip;
      std::array<float, 4> render;
    };
    static_assert(sizeof(EyeClipBlock) == 144);
    float renderSize[2];
    std::memcpy(renderSize, mono, sizeof(renderSize));
    if (eyeRenderSize) {
      const auto& params = state.eyes[0]; // both eyes share a size under multiview
      renderSize[0] *=
          monoSize.width != 0 ? static_cast<float>(params.width) / static_cast<float>(monoSize.width) : 1.0f;
      renderSize[1] *=
          monoSize.height != 0 ? static_cast<float>(params.height) / static_cast<float>(monoSize.height) : 1.0f;
    }
    const EyeClipBlock block{
        .clip = clips,
        .render = {renderSize[0], renderSize[1], 0.0f, 0.0f},
    };
    const size_t used = logical_size(frame.uniforms, frame.spillUniforms);
    const size_t next = AURORA_ALIGN(used, alignment);
    // A clip block must immediately follow its mono uniform, including when the
    // new staging allocator moves a push into the spill buffer.
    if (next != monoRange.offset + AURORA_ALIGN(monoRange.size, alignment) ||
        (frame.spillUniforms.length == 0 && next + sizeof(block) > frame.uniforms.capacity())) {
      ByteBuffer combined;
      combined.append(mono, monoRange.size);
      const size_t clipOffset = AURORA_ALIGN(monoRange.size, alignment);
      combined.append_zeroes(clipOffset - monoRange.size);
      combined.append(reinterpret_cast<const uint8_t*>(&block), sizeof(block));
      const auto relocated = push(frame.uniforms, frame.spillUniforms, StagingKind::Uniform,
                                  combined.data(), combined.size(), alignment, 2 * gx::MaxUniformSize, "stereo uniform");
      if (overflowed(relocated)) { return exhausted(); }
      g_recorder.multiviewMode = gx::MultiviewClip;
      return std::array<uint32_t, 2>{relocated.offset, relocated.offset};
    }
    const auto range = push(frame.uniforms, frame.spillUniforms, StagingKind::Uniform,
                            reinterpret_cast<const uint8_t*>(&block), sizeof(block), alignment,
                            2 * gx::MaxUniformSize, "stereo uniform");
    if (overflowed(range)) { return exhausted(); }
    // The shader finds the block at the mono uniform's aligned end.
    if (range.offset != monoRange.offset + AURORA_ALIGN(monoRange.size, alignment)) {
      static bool sReported = false;
      if (!sReported) {
        sReported = true;
        Log.error("Multiview eye clips staged at {}, not after the uniform at {} ({} bytes)", range.offset,
                  monoRange.offset, monoRange.size);
      }
      return none;
    }
    g_recorder.multiviewMode = gx::MultiviewClip;
    return std::array<uint32_t, 2>{monoRange.offset, monoRange.offset};
  };
  // The virtual screen (AURORA_STEREO_ROUTE_SCREEN_2D) takes the recorded
  // picture's aspect, and the draw's viewport keeps its place in that picture.
  stereo_replay::HudScreen screen{};
  stereo_replay::HudNdcRemap screenNdc{};
  const bool screenRoute = route == AURORA_STEREO_ROUTE_SCREEN_2D || route == AURORA_STEREO_ROUTE_FLOATING_HUD ||
                           route == AURORA_STEREO_ROUTE_ANCHORED_SCREEN;
  if (screenRoute && monoSize.width != 0 && monoSize.height != 0) {
    const float width = static_cast<float>(monoSize.width);
    const float height = static_cast<float>(monoSize.height);
    screen = {
        .halfWidth = state.screen2DHalfWidth,
        .halfHeight = state.screen2DHalfWidth * height / width,
        .distance = state.screen2DDistance,
    };
    const auto& viewport = g_recorder.cachedViewport;
    screenNdc = stereo_replay::make_hud_ndc_remap(viewport.left, viewport.top, viewport.width, viewport.height, 0.0f,
                                                  0.0f, width, height);
  }
  const bool onScreen = screen.valid();
  // 2D content and full-screen effects: the same uniform in both eyes.
  const std::array<uint32_t, 2> same{monoRange.offset, monoRange.offset};
  if (route == AURORA_STEREO_ROUTE_FULLSCREEN || (screenRoute && !onScreen)) {
    return state.multiview ? stage_eye_clips({monoProjection, monoProjection}, false) : same;
  }
  // An orthographic draw is 2D content, identical in both eyes, unless its
  // route lays it on the head-locked plane (stereo_replay.hpp HeadLockedPlane)
  // or on the virtual screen.
  const bool perspective = stereo_uniform_is_perspective(mono, layout);
  const bool onPlane =
      !perspective && route == AURORA_STEREO_ROUTE_HEAD_LOCKED_2D && g_recorder.headLockedPlane.valid();
  if (!perspective && !onPlane && !onScreen) {
    return state.multiview ? stage_eye_clips({monoProjection, monoProjection}, false) : same;
  }
  const stereo_replay::HudScreen plane =
      onPlane ? stereo_replay::head_locked_plane_screen(g_recorder.headLockedPlane, state.headLockedScaleXY,
                                                        state.headLockedScaleZ)
              : stereo_replay::HudScreen{};
  const bool headLocked = route == AURORA_STEREO_ROUTE_HEAD_LOCKED || route == AURORA_STEREO_ROUTE_HEAD_LOCKED_2D;
  // A 3D draw in the held anchor's (or floating pose's) space: its camera space scaled and moved there
  // first (AuroraSetStereoPlacement3D), then seen through that pose.
  const bool placed3D = route == AURORA_STEREO_ROUTE_ANCHORED_3D || route == AURORA_STEREO_ROUTE_FLOATING_3D;
  const StereoPlacement3D placement = g_recorder.placement3D;
  const auto placedView = [&](const StereoEyeParams& params) {
    const Mat3x4<float>& poseView = route == AURORA_STEREO_ROUTE_FLOATING_3D ? params.floatingHudViewFromCenter
                                                                             : params.anchoredScreenViewFromCenter;
    Mat3x4<float> offset{};
    offset.m0 = {1.0f, 0.0f, 0.0f, placement.x};
    offset.m1 = {0.0f, 1.0f, 0.0f, placement.y};
    offset.m2 = {0.0f, 0.0f, 1.0f, placement.z};
    if (placement.follow != 0.0f) {
      // Part of the way into the head's own space (the head-locked HUD's),
      // moved and turned about the point.
      const std::array<float, 3> pivot{placement.headX, placement.headY, placement.headZ};
      offset = stereo_replay::compose_affine(
          stereo_replay::follow_head(poseView, params.headLockedViewFromCenter, pivot, placement.follow), offset);
    }
    return stereo_replay::compose_affine(poseView, offset);
  };
  const float positionScaleXY = headLocked ? state.headLockedScaleXY : placed3D ? placement.scale : 1.0f;
  const float positionScaleZ = headLocked ? state.headLockedScaleZ : placed3D ? placement.scale : 1.0f;
  // A screen-projecting texture matrix (AuroraSetStereoScreenTexMtx) differs per eye:
  // under multiview such a draw takes the full eye copies (MultiviewFull).
  const auto screenTexMtx = g_recorder.stereoScreenTexMtx;
  const bool perEyeTexMtx = !onPlane && !onScreen && screenTexMtx.texSlot != 0xFF;
  if (!perEyeTexMtx) {
    std::array<Mat4x4<float>, AURORA_STEREO_EYE_COUNT> clips;
    for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
      const auto& params = state.eyes[eye];
      if (onScreen) {
        const auto& screenView = route == AURORA_STEREO_ROUTE_FLOATING_HUD    ? params.floatingHudViewFromCenter
                                 : route == AURORA_STEREO_ROUTE_ANCHORED_SCREEN ? params.anchoredScreenViewFromCenter
                                                                               : params.viewFromCenter;
        clips[eye] = stereo_replay::compose_screen_2d_projection(params.projection, screenView, screen,
                                                                 monoProjection, screenNdc);
      } else if (onPlane) {
        clips[eye] = stereo_replay::compose_head_locked_2d_projection(
            params.projection, params.headLockedViewFromCenter, plane, monoProjection);
      } else {
        const Mat3x4<float> placedViewFromCenter = placed3D ? placedView(params) : params.viewFromCenter;
        const Mat3x4<float>& viewFromCenter = headLocked                            ? params.headLockedViewFromCenter
                                              : route == AURORA_STEREO_ROUTE_SKY ? params.skyViewFromCenter
                                                                                 : placedViewFromCenter;
        clips[eye] = compose_eye_clip(stereo_replay::compose_projection(params.projection, monoProjection),
                                      viewFromCenter, positionScaleXY, positionScaleZ);
      }
    }
    return stage_eye_clips(clips, true);
  }
  const size_t needed =
      2 * (AURORA_ALIGN(static_cast<size_t>(layout.size), alignment) + alignment) + 2 * gx::MaxUniformSize;
  if (logical_size(frame.uniforms, frame.spillUniforms) + needed > frame_buffer_sizes().maxUniform) {
    return exhausted();
  }
  // Diagnostics: what the eye copies cost (log_stereo_frame_stats).
  struct EyeUniformTimer {
    const bool on = g_stereoDiagnostics.load(std::memory_order_relaxed);
    const std::chrono::steady_clock::time_point start =
        on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~EyeUniformTimer() {
      if (on) {
        g_eyeUniformNs.fetch_add(
            static_cast<uint64_t>((std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
        g_eyeUniformDraws.fetch_add(1, std::memory_order_relaxed);
      }
    }
  } eyeUniformTimer;
  // A screen-projecting texture matrix is derived from position matrix pnSlot, so
  // that slot must be composed whichever one the vertices use.
  StereoUniformLayout eyeLayout = layout;
  if (screenTexMtx.texSlot != 0xFF) {
    eyeLayout.fixedPositionSlot = -1;
  }
  thread_local std::vector<uint8_t> scratch;
  std::array<uint32_t, 2> offsets{};
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    scratch.assign(mono, mono + layout.size);
    const auto& params = state.eyes[eye];
    const float renderScaleX =
        monoSize.width != 0 ? static_cast<float>(params.width) / static_cast<float>(monoSize.width) : 1.0f;
    const float renderScaleY =
        monoSize.height != 0 ? static_cast<float>(params.height) / static_cast<float>(monoSize.height) : 1.0f;
    if (onScreen) {
      const auto& screenView = route == AURORA_STEREO_ROUTE_FLOATING_HUD    ? params.floatingHudViewFromCenter
                               : route == AURORA_STEREO_ROUTE_ANCHORED_SCREEN ? params.anchoredScreenViewFromCenter
                                                                             : params.viewFromCenter;
      compose_stereo_screen_2d_uniform(scratch.data(), layout,
                                       StereoEyeScreenCompose{
                                           .projection = &params.projection,
                                           .viewFromCenter = &screenView,
                                           .screen = screen,
                                           .ndcRemap = screenNdc,
                                           .renderScaleX = renderScaleX,
                                           .renderScaleY = renderScaleY,
                                       });
    } else if (onPlane) {
      compose_stereo_2d_uniform(scratch.data(), layout,
                                StereoEye2DCompose{
                                    .projection = &params.projection,
                                    .headLockedViewFromCenter = &params.headLockedViewFromCenter,
                                    .plane = plane,
                                    .renderScaleX = renderScaleX,
                                    .renderScaleY = renderScaleY,
                                });
    } else {
      // A sky draw (AURORA_STEREO_ROUTE_SKY) takes the eye's rotation only; an
      // anchored 3D draw (AURORA_STEREO_ROUTE_ANCHORED_3D) the eye's view from the held anchor.
      const Mat3x4<float> placedViewFromCenter = placed3D ? placedView(params) : params.viewFromCenter;
      const Mat3x4<float>* viewFromCenter = headLocked                            ? &params.headLockedViewFromCenter
                                            : route == AURORA_STEREO_ROUTE_SKY ? &params.skyViewFromCenter
                                                                               : &placedViewFromCenter;
      compose_stereo_uniform(
          scratch.data(), eyeLayout,
          StereoEyeCompose{
              .projection = &params.projection,
              .viewFromCenter = viewFromCenter,
              .positionScaleXY = positionScaleXY,
              .positionScaleZ = positionScaleZ,
              .renderScaleX = renderScaleX,
              .renderScaleY = renderScaleY,
          });
      // A screen-projecting texture matrix (AuroraSetStereoScreenTexMtx) is
      // derived again from the eye's composed projection and position matrix.
      if (screenTexMtx.texSlot != 0xFF) {
        compose_stereo_screen_tex_mtx(scratch.data(), eyeLayout, screenTexMtx.texSlot, screenTexMtx.pnSlot);
      }
    }
    const auto range = push(frame.uniforms, frame.spillUniforms, StagingKind::Uniform,
                            scratch.data(), layout.size, alignment, 2 * gx::MaxUniformSize, "stereo uniform");
    if (overflowed(range)) { return exhausted(); }
    offsets[eye] = range.offset;
  }
  if (state.multiview) {
    g_recorder.multiviewMode = gx::MultiviewFull;
  }
  return offsets;
}

uint8_t stereo_multiview_mode() noexcept { return g_recorder.multiviewMode; }

template <>
void push_draw_command(clear::DrawData data) {
  push_draw_command(make_draw_command<clear::render>(data));
}

template <>
PipelineRef pipeline_ref(const clear::PipelineConfig& config) {
  return find_pipeline(ShaderType::Clear, config, [=] { return create_pipeline(config); });
}

PipelineRef clear_multiview_pipeline_ref(const clear::PipelineConfig& config) noexcept {
  if (!recording_multiview()) {
    return {};
  }
  clear::PipelineConfig multiviewConfig = config;
  multiviewConfig.multiview = true;
  return pipeline_ref(multiviewConfig);
}

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                       int probeFace, bool resolveMips, const ResolveHdr* hdr, bool opaqueAlpha) {
  if (scene_hdr_pass_active()) {
    // The scene holds exposed linear light: a display copy post-tones it, a linear copy keeps it,
    // and the clear colour (display-referred) goes in as the light that post-tones back to it.
    const hdr::Tone tone = hdr != nullptr ? hdr->tone : hdr::Tone{};
    auto& pass = current_render_passes()[g_recorder.currentRenderPass];
    if (hdr != nullptr && hdr->linear) {
      pass.resolveDomain = tex_copy_conv::CopyDomain::Linear;
    } else {
      pass.resolveDomain = tex_copy_conv::CopyDomain::HdrDisplay;
      pass.resolveToneRange = push_uniform(tone);
    }
    clearColorValue = {hdr::exposed(clearColorValue.x(), tone), hdr::exposed(clearColorValue.y(), tone),
                       hdr::exposed(clearColorValue.z(), tone), clearColorValue.w()};
  }
  // Resolve current render pass
  auto& prevPass = current_render_passes()[g_recorder.currentRenderPass];
  prevPass.resolveTarget = std::move(texture);
  prevPass.resolveRect = rect;
  prevPass.resolveFormat = resolveFormat;
  prevPass.resolveMips = resolveMips;
  if (resolveMips) {
    prevPass.resolveMipsUniformRange = push_uniform(tex_copy_conv::Uniforms{});
  }
  // tex_copy_conv's uniforms: the crop region in UV space, and whether the source has alpha
  const auto srcW = static_cast<float>(prevPass.colorAttachments[SceneColorAttachmentIndex].size.width);
  const auto srcH = static_cast<float>(prevPass.colorAttachments[SceneColorAttachmentIndex].size.height);
  const tex_copy_conv::Uniforms uniforms{
      .offset = {static_cast<float>(rect.x) / srcW, static_cast<float>(rect.y) / srcH},
      .scale = {static_cast<float>(rect.width) / srcW, static_cast<float>(rect.height) / srcH},
      .opaqueAlpha = opaqueAlpha,
  };
  prevPass.resolveUniformRange = push_uniform(uniforms);
  if (probeFace >= 0) {
    prevPass.probeFace = probeFace;
    prevPass.probeUniformRange = push_uniform(tex_copy_conv::Uniforms{});
  }
  // GXCopyTex clears only the rectangle it copies: the whole target clears through the
  // continuation's load ops, a part of it with a clear draw scissored to it.
  const auto targetWidth = static_cast<int32_t>(prevPass.colorAttachments[SceneColorAttachmentIndex].size.width);
  const auto targetHeight = static_cast<int32_t>(prevPass.colorAttachments[SceneColorAttachmentIndex].size.height);
  const auto left = std::clamp<int32_t>(rect.x, 0, targetWidth);
  const auto top = std::clamp<int32_t>(rect.y, 0, targetHeight);
  const auto right = std::clamp<int32_t>(rect.x + rect.width, left, targetWidth);
  const auto bottom = std::clamp<int32_t>(rect.y + rect.height, top, targetHeight);
  const ClipRect clearRect{.x = left, .y = top, .width = right - left, .height = bottom - top};
  const bool fullTarget = left == 0 && top == 0 && right == targetWidth && bottom == targetHeight;
  // Stereo replay: a copy from an EFB pass that replays per eye is also taken
  // from each eye, into the copy's stand-ins (stereo_shadow.hpp), so effects
  // that sample it (the thermal and X-ray visors) see each eye's own view. Set
  // before the pass is enqueued: the frame worker may encode it at once.
  // A copy made under AURORA_STEREO_ROUTE_SKIP is not a view (a shadow rendered
  // from a light): it is taken from the mono EFB only and the eyes sample it.
  // When it also clears the whole EFB, the next pass's eye passes clear the
  // eyes too, so nothing could see this pass in them: it is not replayed.
  const bool monoCopy = g_recorder.stereoRoute == AURORA_STEREO_ROUTE_SKIP && probeFace < 0;
  if (monoCopy && fullTarget && clearColor && clearAlpha && (clearDepth || !prevPass.hasDepth) && !prevPass.hasStencil) {
    prevPass.stereo.monoOnly = true;
  }
  stereo_seal_pass(current_frame_packet(), g_recorder.currentRenderPass);
  if (probeFace < 0) {
    // VR MERGE NOTE: keep depth copies eligible alongside colour copies.
    // Upstream can change an effect's GXCopyTex format without touching this
    // code: retail lava/fog changed A8 -> Z16 in 0.32 and silently became mono
    // when depth was excluded here. See RetailFogDepthCopiesHaveSeparateEyeTargets.
    if (prevPass.stereo.enabled && !monoCopy) {
      // Each eye's copy at that eye's resolution (stereo_shadow.hpp).
      const auto& efbSize = prevPass.colorAttachments[SceneColorAttachmentIndex].size;
      std::array<stereo_shadow::EyeSize, AURORA_STEREO_EYE_COUNT> eyeTargets{};
      for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
        eyeTargets[eye] = {prevPass.stereo.eyes[eye].size.width, prevPass.stereo.eyes[eye].size.height};
      }
      prevPass.stereo.copyTargets = stereo_shadow::copy_targets(
          prevPass.resolveTarget, {efbSize.width, efbSize.height}, eyeTargets);
      // A copy made under AURORA_STEREO_ROUTE_HEAD_LOCKED_2D is taken from
      // each eye through the head-locked plane, so it holds what that eye sees
      // behind the copied rectangle: the scan visor's window magnifies what
      // is straight ahead of the head (stereo_replay.hpp HeadLockedPlane).
      if (g_recorder.stereoRoute == AURORA_STEREO_ROUTE_HEAD_LOCKED_2D && g_recorder.headLockedPlane.valid()) {
        const auto& stereo = current_frame_packet().stereo;
        const auto plane = stereo_replay::head_locked_plane_screen(g_recorder.headLockedPlane,
                                                                   stereo.headLockedScaleXY, stereo.headLockedScaleZ);
        const float ndcLeft = 2.0f * static_cast<float>(rect.x) / srcW - 1.0f;
        const float ndcRight = 2.0f * static_cast<float>(rect.x + rect.width) / srcW - 1.0f;
        const float ndcTop = 1.0f - 2.0f * static_cast<float>(rect.y) / srcH;
        const float ndcBottom = 1.0f - 2.0f * static_cast<float>(rect.y + rect.height) / srcH;
        for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
          const auto& params = stereo.eyes[eye];
          const auto uv = stereo_replay::head_locked_plane_uv_rect(params.projection, params.headLockedViewFromCenter,
                                                                   plane, ndcLeft, ndcTop, ndcRight, ndcBottom);
          if (uv.valid()) {
            prevPass.stereo.copyUniformRanges[eye] = push_uniform(tex_copy_conv::Uniforms{.offset = {uv.u, uv.v}, .scale = {uv.width, uv.height}, .opaqueAlpha = opaqueAlpha});
          }
        }
      }
    } else {
      stereo_shadow::invalidate(prevPass.resolveTarget.get());
    }
  }
  // The continuation's load op clears whatever the copy clears.
  discard_dead_stores(prevPass, clearColor && clearAlpha && fullTarget, clearDepth && fullTarget);
  enqueue_pass(current_frame_packet(), g_recorder.currentRenderPass);
  if (clearDepth && !g_recorder.inOffscreen) {
    g_recorder.efbDepthNear = std::min(g_recorder.efbDepthNear, gx_depth(clearDepthValue));
  }

  // Populate new render pass from previous
  const auto msaaSamples = prevPass.msaaSamples;
  RenderPass newPass{
      .label = pass_label(g_recorder.inOffscreen ? "Offscreen" : "EFB"),
      .colorAttachments = prevPass.colorAttachments,
      .colorAttachmentCount = prevPass.colorAttachmentCount,
      .depthStencilView = prevPass.depthStencilView,
      .depthStencilFormat = prevPass.depthStencilFormat,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .msaaSamples = msaaSamples,
      .clearDepthValue = clearDepthValue,
      .clearDepth = clearDepth && fullTarget,
      .hasDepth = prevPass.hasDepth,
      .hasStencil = prevPass.hasStencil,
      // Still the EFB after a copy: the stereo replay carries on into the eyes.
      .efb = prevPass.efb,
  };
  const bool fullColorClear = clearColor && clearAlpha && fullTarget;
  for (uint32_t i = 0; i < newPass.colorAttachmentCount; ++i) {
    auto& color = newPass.colorAttachments[i];
    color.loadOp = wgpu::LoadOp::Undefined;
    color.clear = false;
    // prevPass may have just discarded its store; this pass decides its own.
    color.storeOp = wgpu::StoreOp::Store;
  }
  if (fullColorClear) {
    auto& sceneColor = newPass.colorAttachments[SceneColorAttachmentIndex];
    sceneColor.clear = true;
    sceneColor.clearValue = clearColorValue;
  }
  newPass.commands.reserve(2048);
  current_render_passes().emplace_back(std::move(newPass));
  ++g_recorder.currentRenderPass;

  const bool drawClearColor = clearColor && !fullColorClear;
  const bool drawClearAlpha = clearAlpha && !fullColorClear;
  const bool drawClearDepth = clearDepth && !fullTarget;
  if (clearRect.width > 0 && clearRect.height > 0 && (drawClearColor || drawClearAlpha || drawClearDepth)) {
    // Part of the target, or only its colour or alpha: a clear draw
    const auto targetLayout = current_render_passes()[g_recorder.currentRenderPass].target_layout();
    const auto clearConfig = clear::make_pipeline_config(targetLayout, drawClearColor, drawClearAlpha, drawClearDepth);
    push_draw_command(clear::DrawData{
        .pipeline = pipeline_ref(clearConfig),
        .color =
            wgpu::Color{
                .r = clearColorValue.x(),
                .g = clearColorValue.y(),
                .b = clearColorValue.z(),
                .a = clearColorValue.w(),
            },
        .multiviewPipeline = clear_multiview_pipeline_ref(clearConfig),
        .depth = clearDepthValue,
        .rect = clearRect,
        .rectReferenceWidth = static_cast<uint32_t>(targetWidth),
        .rectReferenceHeight = static_cast<uint32_t>(targetHeight),
    });
  }
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
}

void queue_palette_conv(tex_palette_conv::ConvRequest req) {
  auto& renderPass = current_render_passes()[g_recorder.currentRenderPass];
  AURORA_ASSERT(!renderPass.sealed, "Attempted to append palette conversion to sealed render pass {}",
                g_recorder.currentRenderPass);
  // Stereo replay: a palette read of a copy with eye stand-ins runs per eye too
  // (DolphinXR's layered palette conversion), so the visor's coloured image
  // stays per eye.
  std::array<tex_palette_conv::ConvRequest, 2> eyeConvs;
  if (!g_recorder.inOffscreen && stereo_shadow::palette_conv(req, eyeConvs)) {
    for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
      renderPass.stereo.paletteConvs[eye].push_back(std::move(eyeConvs[eye]));
    }
  }
  renderPass.paletteConvs.push_back(std::move(req));
}

bool is_offscreen() noexcept { return g_recorder.inOffscreen; }

uint32_t get_sample_count() noexcept {
  CHECK(g_recorder.currentRenderPass != UINT32_MAX, "get_sample_count called outside of a frame");
  return current_render_passes()[g_recorder.currentRenderPass].msaaSamples;
}

RenderTargetLayout get_render_target_layout() noexcept {
  CHECK(g_recorder.currentRenderPass != UINT32_MAX, "get_render_target_layout called outside of a frame");
  return current_render_passes()[g_recorder.currentRenderPass].target_layout();
}

bool scene_hdr_active() noexcept { return g_recorder.hdrScene; }

bool scene_hdr_pass_active() noexcept {
  // hdrScene stays set through offscreen and begin_color_pass passes: only the HDR target's own pass counts.
  if (!g_recorder.hdrScene || !g_recorder.active() || g_recorder.inOffscreen ||
      g_recorder.currentRenderPass == UINT32_MAX || !webgpu::g_sceneHdr.view) {
    return false;
  }
  const auto& pass = current_render_passes()[g_recorder.currentRenderPass];
  return pass.colorAttachments[SceneColorAttachmentIndex].view.Get() == webgpu::g_sceneHdr.view.Get();
}

bool set_scene_hdr(bool on, bool pause) {
  if (g_recorder.hdrScene == on && !pause) {
    return true;
  }
  // Only between EFB draws: not outside a frame, offscreen, or in a begin_color_pass pass.
  if (!g_recorder.active() || g_recorder.inOffscreen || g_recorder.currentRenderPass == UINT32_MAX) {
    return false;
  }
  // The current pass must be the EFB itself: a null EFB view, or a pass on another target, is refused.
  auto& prevPass = current_render_passes()[g_recorder.currentRenderPass];
  if (!efb_color().view || !webgpu::g_depthBuffer.view ||
      prevPass.colorAttachments[SceneColorAttachmentIndex].view.Get() != efb_color().view.Get() ||
      prevPass.depthStencilView.Get() != webgpu::g_depthBuffer.view.Get()) {
    return false;
  }
  // An HDR scene must be convertible before its first draw. Check the same FIFO-owned post budget
  // used by bloom::record so an unsupported third segment is refused here, and draws without the post.
  if (on && !bloom::scene_post_available()) {
    return false;
  }
  if (on && !webgpu::ensure_scene_hdr_target()) {
    // No retail fallback: the frame draws without the Remastered post, each PBR draw toned on its own.
    static bool warned = false;
    if (!warned) {
      warned = true;
      Log.warn("HDR scene unavailable: the frame draws without the Remastered post (warned once)");
    }
    return false;
  }

  // Seal the current EFB pass and continue on the other target, loading depth (shared) and, unless
  // this frame's HDR scene is new, colour.
  prevPass.discardable = !prevPass.has_consumer() && !prevPass.has_content();
  enqueue_pass(current_frame_packet(), g_recorder.currentRenderPass);
  resume_efb_pass_loading(prevPass);
  g_recorder.hdrScene = on;
  auto& pass = current_render_passes()[g_recorder.currentRenderPass];
  set_efb_targets(pass);
  if (pause) {
    // RenderPauseScreenElements clears depth (flags 2), not the reused colour.
    auto& color = pass.colorAttachments[SceneColorAttachmentIndex];
    color.clear = false;
    color.loadOp = wgpu::LoadOp::Load;
    pass.clearDepth = true;
    pass.depthLoadOp = wgpu::LoadOp::Clear;
    pass.clearDepthValue = gx::UseReversedZ ? 0.f : 1.f;
    g_recorder.hdrSceneCleared = true;
  } else if (on && !g_recorder.hdrSceneCleared) {
    auto& color = pass.colorAttachments[SceneColorAttachmentIndex];
    color.clear = true;
    color.loadOp = wgpu::LoadOp::Clear;
    color.clearValue = {0.f, 0.f, 0.f, 0.f};
    g_recorder.hdrSceneCleared = true;
  }
  return true;
}

void clear_caches() noexcept {
  g_offscreenCache.clear();
  clear_bind_group_cache();
}

// What an EFB-scale change invalidates: the offscreen targets are kept per size, so the
// old size's are only dead weight. Bind groups are cached by what they reference, so stale
// ones are never hit; rebuilding all of them made the frame after a dynamic resolution step
// slower still, so instead the ones the next frames do not use are dropped two frames on
// (detail::expire_unused_bind_groups_soon), freeing the old size's targets.
void clear_offscreen_cache() noexcept { g_offscreenCache.clear(); }

bool recording_active() noexcept { return g_recorder.active(); }

bool push_custom_draw(DrawTypeId type, const void* payload, size_t payloadSize) {
  if (type == InvalidDrawType) {
    Log.warn("push_custom_draw: invalid draw type");
    return false;
  }
  if (payloadSize > InlineDrawPayloadSize) {
    Log.warn("push_custom_draw: payload size {} exceeds inline payload size {}", payloadSize, InlineDrawPayloadSize);
    return false;
  }
  if (payloadSize > 0 && payload == nullptr) {
    Log.warn("push_custom_draw: non-zero payload size with null payload");
    return false;
  }
  if (!find_runtime_draw_type(type)) {
    Log.warn("push_custom_draw: unregistered draw type {:#x}", type);
    return false;
  }

  gx::fifo::drain();

  if (!g_recorder.active() || g_recorder.currentRenderPass == UINT32_MAX) {
    Log.warn("push_custom_draw: called outside an active render pass");
    return false;
  }

  CustomDrawCommand draw{};
  draw.type = type;
  draw.payloadSize = static_cast<uint32_t>(payloadSize);
  if (payloadSize > 0) {
    std::memcpy(draw.payload.data(), payload, payloadSize);
  }
  note_draw_depth(true, true);
  push_command(CommandType::CustomDraw, Command::Data{.customDraw = draw});
  ++g_recorder.drawCallCount;
  return true;
}

void begin_offscreen(uint32_t width, uint32_t height) {
  ZoneScoped;
  AURORA_ASSERT(width != 0 && height != 0, "begin_offscreen requires nonzero dimensions ({}x{})", width, height);
  AURORA_ASSERT(g_recorder.active() && g_recorder.currentRenderPass != UINT32_MAX,
                "begin_offscreen called outside of an active recording frame");

  if (g_recorder.inOffscreen) {
    finish_current_offscreen();
  } else {
    suspend_efb();
  }
  start_offscreen(width, height);
}

void end_offscreen() {
  ZoneScoped;
  finish_current_offscreen();
  restore_efb();
}

bool create_pass(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0) {
    Log.warn("create_pass: invalid size {}x{}", width, height);
    return false;
  }

  gx::fifo::drain();

  if (!g_recorder.active() || g_recorder.currentRenderPass == UINT32_MAX) {
    Log.warn("create_pass: called outside an active render pass");
    return false;
  }
  if (g_recorder.inOffscreen) {
    Log.warn("create_pass: an offscreen pass is already active (nesting is unsupported)");
    return false;
  }

  begin_offscreen(width, height);
  return true;
}

bool resolve_scene_copy(TextureHandle color, const ResolveHdr* hdr, wgpu::TextureView& depth) {
  depth = {};
  if (!color || !g_recorder.active() || g_recorder.currentRenderPass == UINT32_MAX || g_recorder.inOffscreen) {
    return false;
  }
  auto& pass = current_render_passes()[g_recorder.currentRenderPass];
  const auto& attachment = pass.colorAttachments[SceneColorAttachmentIndex];
  const uint32_t width = attachment.size.width;
  const uint32_t height = attachment.size.height;
  if (pass.hasDepth && tex_copy_conv::snapshot_depth_supported(pass.msaaSamples)) {
    auto& entry = acquire_pass_snapshot(width, height, attachment.format, pass.msaaSamples, false, true);
    pass.snapshotDepthDst = entry.depth.view;
    depth = entry.depth.view;
  }
  const bool mips = color->mipCount > 1;
  resolve_pass_into(std::move(color), ClipRect{0, 0, int32_t(width), int32_t(height)}, false, false, false, {}, 0.f,
                    GX_TF_RGBA8, -1, mips, hdr);
  return true;
}

size_t current_frame_slot() noexcept { return g_recorder.frameSlot; }

bool resolve_pass(const ResolveDesc& desc, ResolvedTargets& out) {
  out = {};
  gx::fifo::drain();

  if (!g_recorder.active() || g_recorder.currentRenderPass == UINT32_MAX) {
    Log.warn("resolve_pass: called outside an active render pass");
    return false;
  }

  auto& prevPass = current_render_passes()[g_recorder.currentRenderPass];
  bool wantDepth = desc.depth;
  if (wantDepth && !tex_copy_conv::snapshot_depth_supported(prevPass.msaaSamples)) {
    Log.warn("resolve_pass: depth snapshots are unsupported on this device");
    wantDepth = false;
  }

  const uint32_t width = prevPass.colorAttachments[SceneColorAttachmentIndex].size.width;
  const uint32_t height = prevPass.colorAttachments[SceneColorAttachmentIndex].size.height;
  // Requesting no snapshots is a plain pass break (or offscreen close, discarding its output).
  if (desc.color || wantDepth) {
    auto& entry = acquire_pass_snapshot(width, height, prevPass.colorAttachments[SceneColorAttachmentIndex].format,
                                        prevPass.msaaSamples, desc.color, wantDepth);
    if (desc.color) {
      prevPass.snapshotColorDst = entry.color.texture;
      out.color = entry.color.view;
      out.colorFormat = entry.color.format;
    }
    if (wantDepth) {
      prevPass.snapshotDepthDst = entry.depth.view;
      out.depth = entry.depth.view;
    }
  }
  out.width = width;
  out.height = height;

  if (g_recorder.inOffscreen) {
    // Seal the offscreen pass and resume the EFB.
    end_offscreen();
    return true;
  }

  // Seal the current EFB pass and continue on a new one that loads the existing contents.
  // EFB writes persist into the loading continuation, so content keeps the pass alive even
  // without a consumer.
  prevPass.discardable = !prevPass.has_consumer() && !prevPass.has_content();
  enqueue_pass(current_frame_packet(), g_recorder.currentRenderPass);
  resume_efb_pass_loading(prevPass);
  return true;
}

bool push_encoder_task(EncoderTaskId type, const void* payload, size_t payloadSize) {
  if (type == InvalidEncoderTask) {
    Log.warn("push_encoder_task: invalid encoder task type");
    return false;
  }
  if (payloadSize > InlineDrawPayloadSize) {
    Log.warn("push_encoder_task: payload size {} exceeds inline payload size {}", payloadSize, InlineDrawPayloadSize);
    return false;
  }
  if (payloadSize > 0 && payload == nullptr) {
    Log.warn("push_encoder_task: non-zero payload size with null payload");
    return false;
  }
  if (!find_runtime_encoder_task_type(type)) {
    Log.warn("push_encoder_task: unregistered encoder task type {:#x}", type);
    return false;
  }

  gx::fifo::drain();
  return record_encoder_task(type, payload, payloadSize);
}

namespace {
bool record_encoder_task_between(EncoderTaskId type, const void* payload, size_t payloadSize, DepthAfter depth) {
  if (!g_recorder.active() || g_recorder.currentRenderPass == UINT32_MAX) {
    Log.warn("push_encoder_task: called outside an active render pass");
    return false;
  }
  if (g_recorder.inOffscreen) {
    Log.warn("push_encoder_task: unsupported while an offscreen pass is active");
    return false;
  }

  // Seal the current EFB pass, record the task between it and a continuation
  // pass that loads the existing contents. EFB writes persist into the continuation, so
  // content keeps the sealed pass alive even without a consumer.
  auto& frame = current_frame_packet();
  auto& prevPass = current_render_passes()[g_recorder.currentRenderPass];
  prevPass.discardable = !prevPass.has_consumer() && !prevPass.has_content();
  discard_dead_stores(prevPass, false, depth == DepthAfter::Clear);
  const bool hold = depth == DepthAfter::IfUnread;
  const auto sealedPass = g_recorder.currentRenderPass;
  const auto passOp = enqueue_pass(frame, sealedPass, !hold);

  const auto taskIndex = static_cast<uint32_t>(frame.encoderTasks.size());
  auto& task = frame.encoderTasks.emplace_back(EncoderTask{.type = type});
  task.payloadSize = static_cast<uint32_t>(payloadSize);
  if (payloadSize > 0) {
    std::memcpy(task.payload.data(), payload, payloadSize);
  }
  const auto opIndex = static_cast<uint32_t>(frame.ops.size());
  frame.ops.emplace_back(capture_frame_op(frame, FrameOpType::EncoderTask, taskIndex));
  if (!hold) {
    enqueue_op(frame, opIndex);
  }

  resume_efb_pass_loading(prevPass);
  if (depth != DepthAfter::Load) {
    auto& pass = current_render_passes()[g_recorder.currentRenderPass];
    pass.clearDepth = true;
    pass.clearDepthValue = gx::UseReversedZ ? 0.f : 1.f;
  }
  if (hold) {
    g_recorder.heldDepth = FrameRecorder::HeldDepth{
        .passOp = passOp, .taskOp = opIndex, .sealedPass = sealedPass, .nextPass = g_recorder.currentRenderPass};
  }
  return true;
}
} // namespace

bool record_encoder_task(EncoderTaskId type, const void* payload, size_t payloadSize) {
  return record_encoder_task_between(type, payload, payloadSize, DepthAfter::Load);
}

bool record_encoder_task_overwriting(EncoderTaskId type, const void* payload, size_t payloadSize,
                                     DrawTypeId drawType, DepthAfter depth, const void* drawPayload,
                                     size_t drawPayloadSize, bool leaveSceneHdr) {
  if (drawPayloadSize > InlineDrawPayloadSize || (drawPayloadSize > 0 && drawPayload == nullptr)) {
    Log.warn("record_encoder_task_overwriting: bad draw payload ({} bytes)", drawPayloadSize);
    return false;
  }
  if (leaveSceneHdr && (!scene_hdr_pass_active() || !webgpu::g_frameBuffer.view)) {
    Log.warn("record_encoder_task_overwriting: no HDR scene pass to leave");
    return false;
  }
  if (!record_encoder_task_between(type, payload, payloadSize, depth)) {
    return false;
  }
  auto& pass = current_render_passes()[g_recorder.currentRenderPass];
  if (leaveSceneHdr) {
    // The continuation was made on the sealed pass's (HDR) targets: retarget it to the display EFB. Its
    // colour is cleared below, so nothing of the display EFB is loaded; depth is shared.
    g_recorder.hdrScene = false;
    // A scene post ends one independent HDR scene. The next segment (e.g. scan zoom's main-world
    // pass) starts from black rather than loading the already-composited, previous scene.
    g_recorder.hdrSceneCleared = false;
    set_efb_targets(pass);
  }
  pass.colorAttachments[SceneColorAttachmentIndex].clear = true;
  CustomDrawCommand draw{};
  draw.type = drawType;
  draw.payloadSize = static_cast<uint32_t>(drawPayloadSize);
  if (drawPayloadSize > 0) {
    std::memcpy(draw.payload.data(), drawPayload, drawPayloadSize);
  }
  push_command(CommandType::CustomDraw, Command::Data{.customDraw = draw});
  return true;
}

template <>
void push_draw_command(gx::DrawData data) {
  note_draw_depth(gx::g_gxState.depthCompare, gx::g_gxState.depthCompare && gx::g_gxState.depthUpdate);
  push_draw_command(make_draw_command<gx::render>(data));
}

#ifdef AURORA_ENABLE_RMLUI
template <>
void push_draw_command(rmlui::DrawData data) {
  note_draw_depth(true, true);
  push_draw_command(make_draw_command<rmlui::render>(data));
}
#endif

// A GX draw's pipeline: blocking when the caller must not lose its draw (gx::blocking_pipelines).
static PipelineRef gx_pipeline_ref(const gx::PipelineConfig& config, bool blocking) {
  const auto ref = find_pipeline(
      ShaderType::GX, config,
      [=] {
        phase::Scope compiling(render_worker::is_worker_thread() ? phase::Render : phase::Main,
                               "compiling a GX pipeline");
        return create_pipeline(config);
      },
      blocking ? PipelinePriority::Blocking : PipelinePriority::Normal);
  overdraw::note_pipeline(ref, config);
  return ref;
}

template <>
PipelineRef pipeline_ref(const gx::PipelineConfig& config) {
  return gx_pipeline_ref(config, false);
}

PipelineRef pipeline_ref(const gx::PipelineConfig& config, bool blocking) {
  return gx_pipeline_ref(config, blocking);
}

#ifdef AURORA_ENABLE_RMLUI
template <>
PipelineRef pipeline_ref(const rmlui::PipelineConfig& config) {
  return find_pipeline(ShaderType::Rml, config, [=] { return rmlui::create_pipeline(config); });
}
#endif

void finish() {
  ZoneScoped;
  // One frame's worth: set again before every finish() (aurora.cpp end_frame).
  const bool finalPassMonoUnneeded = std::exchange(g_recorder.finalPassMonoUnneeded, false);
  if (!g_recorder.active()) {
    return;
  }
  AURORA_ASSERT(!g_recorder.inOffscreen, "finish called while offscreen rendering is active");
  if (g_recorder.currentRenderPass != UINT32_MAX) {
    auto& frame = current_frame_packet();
    uint32_t padOffset = 0;
    if (uint8_t* pad = reserve_frame_bytes(frame.uniforms, frame.spillUniforms, StagingKind::Uniform,
                                           2 * gx::MaxUniformSize, 0, 0, "uniform", padOffset)) {
      memset(pad, 0, 2 * gx::MaxUniformSize);
    }
    auto& pass = frame.renderPasses[g_recorder.currentRenderPass];
    // Only when a peek waits for it: the snapshot needs the depth stored, which a tile-based GPU
    // would otherwise skip. A request that comes later is served by the next frame.
    pass.captureDepthSnapshot = depth_peek::snapshot_wanted();
    if (g_recorder.heldDepth) {
      release_held_depth(g_recorder.heldDepth->read || g_recorder.heldDepth->nextPass != g_recorder.currentRenderPass ||
                         pass_copies_depth(pass));
    }
    // The next frame starts by clearing the EFB.
    discard_dead_stores(pass, true, true);
    // Honoured only if sealing gives the pass its eye passes (encoding.cpp).
    pass.stereo.skipMono = finalPassMonoUnneeded;
    enqueue_pass(frame, g_recorder.currentRenderPass);
    if (g_stereoDiagnostics.load(std::memory_order_relaxed)) {
      log_stereo_frame_stats(frame);
    }
    g_recorder.currentRenderPass = UINT32_MAX;
  }
}

Range push_verts(const uint8_t* data, size_t length, size_t alignment) {
  ZoneScoped;
  if (!check_recording("push_verts")) {
    return {};
  }
  auto& frame = current_frame_packet();
  return push(frame.verts, frame.spillVerts, StagingKind::Vertex, data, length, alignment, 0, "vertex");
}

Range push_indices(const uint8_t* data, size_t length, size_t alignment) {
  ZoneScoped;
  if (!check_recording("push_indices")) {
    return {};
  }
  auto& frame = current_frame_packet();
  return push(frame.indices, frame.spillIndices, StagingKind::Index, data, length, alignment, 0, "index");
}

ByteBuffer* staging_verts() noexcept { return g_recorder.active() ? &current_frame_packet().verts : nullptr; }

void queue_geometry_upload(uint32_t offset, const uint8_t* data, uint32_t size) {
  if (!check_recording("queue_geometry_upload") || size == 0) {
    return;
  }
  // Through this frame's vertex staging: the frame worker copies it on to the
  // geometry buffer before the frame's next op (encoding.cpp).
  auto& frame = current_frame_packet();
  const Range range = push(frame.verts, frame.spillVerts, StagingKind::Vertex, data, size, 4, 0, "geometry");
  if (overflowed(range)) return;
  const auto src = static_cast<uint32_t>(range.offset - frame_buffer_sizes().base(StagingKind::Vertex));
  frame.geometryUploads.push_back(GeometryUpload{.src = src, .dst = offset, .size = size});
}

ByteBuffer* staging_indices() noexcept { return g_recorder.active() ? &current_frame_packet().indices : nullptr; }

Range push_uniform(const uint8_t* data, size_t length) {
  ZoneScoped;
  if (!check_recording("push_uniform")) {
    return {};
  }
  auto& frame = current_frame_packet();
  const auto alignment = resources().limits.minUniformBufferOffsetAlignment;
  // A uniform is bound as a MaxUniformSize window, which must stay inside the buffer.
  return push(frame.uniforms, frame.spillUniforms, StagingKind::Uniform, data, length, alignment,
              2 * gx::MaxUniformSize, "uniform");
}

Range map_uniform(uint8_t*& out) {
  out = nullptr;
  if (!check_recording("map_uniform")) {
    return {};
  }
  auto& frame = current_frame_packet();
  const auto alignment = resources().limits.minUniformBufferOffsetAlignment;
  uint32_t offset = 0;
  out = reserve_frame_bytes(frame.uniforms, frame.spillUniforms, StagingKind::Uniform, gx::MaxUniformSize,
                            alignment, 2 * gx::MaxUniformSize, "uniform", offset);
  if (out == nullptr) {
    return OverflowRange;
  }
  return {offset, gx::MaxUniformSize};
}

void unmap_uniform(Range& range, size_t length) {
  auto& frame = current_frame_packet();
  const size_t end = range.offset - frame_buffer_sizes().base(StagingKind::Uniform) + length;
  const size_t cap = frame.uniforms.capacity();
  if (end >= cap && frame.spillUniforms.length != 0) {
    frame.spillUniforms.length = end - cap;
  } else {
    frame.uniforms.truncate(end);
  }
  range.size = static_cast<uint32_t>(length);
}

Range push_storage(const uint8_t* data, size_t length) {
  ZoneScoped;
  if (!check_recording("push_storage")) {
    return {};
  }
  auto& frame = current_frame_packet();
  return push(frame.storage, frame.spillStorage, StagingKind::Storage, data, length,
              resources().limits.minStorageBufferOffsetAlignment, 0, "storage");
}

Range push_texture_data(const uint8_t* data, u32 bytesPerRow, u32 rowsPerImage) {
  // For CopyBufferToTexture, we need an alignment of 256 per row (see Dawn kTextureBytesPerRowAlignment)
  const auto copyBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
  const auto range = map(current_frame_packet().textureUpload, copyBytesPerRow * rowsPerImage, 0);
  if (overflowed(range)) {
    return range;
  }
  u8* dst = current_frame_packet().textureUpload.data() + range.offset;
  for (u32 i = 0; i < rowsPerImage; ++i) {
    memcpy(dst, data, bytesPerRow);
    data += bytesPerRow;
    dst += copyBytesPerRow;
  }
  return range;
}

uint32_t align_uniform(uint32_t value) {
  return AURORA_ALIGN(value, detail::resources().limits.minUniformBufferOffsetAlignment);
}

void insert_debug_marker(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  auto& markers = current_frame_packet().debugMarkers;
  const auto idx = markers.size();
  markers.emplace_back(std::move(label));
  push_command(CommandType::DebugMarker, {.debugMarkerIndex = idx});
#endif
}

void push_debug_group(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  g_recorder.debugGroupStack.push_back(std::move(label));
#endif
}
} // namespace aurora::gfx

void push_debug_group(const char* label) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  aurora::gfx::g_recorder.debugGroupStack.emplace_back(label);
#endif
}
void pop_debug_group() {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (aurora::gfx::g_recorder.debugGroupStack.empty()) {
    aurora::gfx::Log.error("Debug group stack underflowed!");
    return;
  }

  aurora::gfx::g_recorder.debugGroupStack.pop_back();
#endif
}
