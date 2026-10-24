#pragma once

// The stereo (headset) state of one recorded frame.
//
// Record once, replay per eye: the game draws its frame once, into the EFB,
// through the normal recording path. When the frame's stereo packet is an
// immersive one, every EFB render pass is also encoded twice more, into a left
// and a right eye target, from the same recorded commands. A GX draw in an eye
// pass binds an eye copy of its uniform (gfx/stereo_uniform.hpp) staged when
// the mono uniform was built, with the eye's frustum and pose composed in; a
// 2D (orthographic) draw binds its mono uniform and so lands identically in
// both eyes; a draw routed AURORA_STEREO_ROUTE_SKIP is left out of the eyes;
// a draw routed AURORA_STEREO_ROUTE_SKY takes the eye's rotation only.
//
// The packet is consumed at frame begin (stereo_host::begin_frame), on the
// game thread, so the eye parameters are fixed before the first uniform is
// staged. The recorder keeps a copy in the FramePacket; the frame worker reads
// that copy, never the host's globals.

#include "tex_palette_conv.hpp"
#include "types.hpp"

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <aurora/math.hpp>
#include <webgpu/webgpu_cpp.h>

#include <array>
#include <cstdint>
#include <vector>

namespace aurora::gfx {

// One eye of the frame's packet, in Aurora's matrix layout (see
// stereo_replay.hpp: m0..m3 are the rows of the column-vector matrices the GX
// shader multiplies with vec4 * mat).
struct StereoEyeParams {
  uint32_t width = 0;
  uint32_t height = 0;
  Mat4x4<float> projection{};     // the OpenXR asymmetric frustum
  Mat3x4<float> viewFromCenter{}; // this eye's view from the game camera (centre) space
  // The same without the head's rotation and position: only this eye's offset
  // from the head centre (half the IPD), for draws that stay in front of the
  // head (AURORA_STEREO_ROUTE_HEAD_LOCKED).
  Mat3x4<float> headLockedViewFromCenter{};
  // viewFromCenter without its translation: the head's rotation and this
  // eye's cant, the eye at the camera's centre, for draws that stand for
  // infinity (AURORA_STEREO_ROUTE_SKY).
  Mat3x4<float> skyViewFromCenter{};
  Mat3x4<float> floatingHudViewFromCenter{};
  // viewFromCenter composed with the held screen anchor (AURORA_STEREO_ROUTE_ANCHORED_SCREEN), or
  // viewFromCenter itself while no anchor is placed.
  Mat3x4<float> anchoredScreenViewFromCenter{};
};

// The finished eye image for the sink, as ref-counted handles so a frame in
// flight keeps its own textures alive if the targets are recreated.
struct StereoEyeOutput {
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::Extent3D size{};
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  // The eye's array layer of `texture` (both eyes share one texture under
  // multiview, stereo_multiview.hpp).
  uint32_t layer = 0;
};

struct StereoFrameState {
  bool active = false;    // the provider handed over a packet for this frame
  bool immersive = false; // the packet replays per eye (mode, tag and targets all checked at begin)
  // The eye passes are one Vulkan multiview pass each (stereo_multiview.hpp),
  // decided with the targets at begin.
  bool multiview = false;
  // The eye passes render through these foveated views of the eye targets
  // (stereo_foveation.hpp) instead of the targets' own: the multiview array
  // view, or one per eye. Decided with the targets at begin.
  bool foveated = false;
  wgpu::TextureView foveatedMultiviewColorView;
  std::array<wgpu::TextureView, AURORA_STEREO_EYE_COUNT> foveatedEyeColorViews{};
  bool replayed = false;  // at least one EFB pass was sealed with eye passes
  bool uniformsExhausted = false; // the uniform budget ran out: later draws are mono-only (logged once)
  uint64_t frameToken = 0;
  uint64_t contentTag = AURORA_STEREO_CONTENT_TAG_UNKNOWN;
  AuroraStereoFrameMode mode = AURORA_STEREO_FRAME_VIRTUAL_SCREEN;
  // AURORA_STEREO_ROUTE_HEAD_LOCKED draws: scales about the camera origin
  // before the eye offset. Across the view (x, y) sets the angular size, along
  // it (z) the distance, so a camera-space HUD can be shrunk and pulled closer.
  float headLockedScaleXY = 1.0f;
  float headLockedScaleZ = 1.0f;
  // AURORA_STEREO_ROUTE_SCREEN_2D draws: the virtual screen they are laid on
  // (stereo_replay.hpp compose_screen_2d_projection), its half width and its
  // distance ahead in the recorded space's units; its height follows the
  // recorded picture's aspect. Zero: no screen, and the route draws like
  // AURORA_STEREO_ROUTE_FULLSCREEN.
  float screen2DHalfWidth = 0.0f;
  float screen2DDistance = 0.0f;
  std::array<StereoEyeParams, AURORA_STEREO_EYE_COUNT> eyes{};
  std::array<StereoEyeOutput, AURORA_STEREO_EYE_COUNT> outputs{};
};

namespace detail {

// One eye's attachments for a replayed render pass. Views are copied in at
// seal time so the worker never reads the target globals.
struct StereoEyePass {
  wgpu::TextureView colorView; // the pass's attachment: a foveated view when the frame is
  wgpu::TextureView resolveView;
  wgpu::TextureView depthView;
  // The eye's own, sampleable view of the same image, which the copies taken
  // from this eye read (a foveated view is a render attachment only).
  wgpu::TextureView copySourceView;
  wgpu::Extent3D size{};

  const wgpu::TextureView& copy_view(bool depth) const noexcept {
    // Retail fog copies Z16, then samples its converted IA8 texture. It must
    // read this eye's depth attachment, never the mono EFB or colour view.
    if (depth) return depthView;
    if (resolveView) return resolveView;
    return copySourceView ? copySourceView : colorView;
  }
};

struct StereoPassReplay {
  bool enabled = false;
  // Never replayed into the eyes: the pass ends in a mono-only EFB copy (made
  // under AURORA_STEREO_ROUTE_SKIP) that clears the whole EFB, so what it drew
  // reaches the eyes only through that copy (recording.cpp resolve_pass_into).
  bool monoOnly = false;
  // Only the eyes render this pass: it is an immersive frame's final pass (no EFB
  // copy follows it) on a headset that owns the display, so nothing reads its mono
  // image (recording.cpp finish(), stereo_host::headset_owns_display).
  bool skipMono = false;
  // The eye passes run under a fragment density map (stereo_foveation.hpp).
  bool foveated = false;
  // Both eyes in one Vulkan multiview render pass, into these views of the eye
  // targets' two layers (stereo_multiview.hpp); `eyes` then holds each layer's
  // own view, which the copies taken from an eye read.
  bool multiview = false;
  wgpu::TextureView multiviewColorView;
  wgpu::TextureView multiviewDepthView;
  std::array<StereoEyePass, AURORA_STEREO_EYE_COUNT> eyes{};
  // This pass's EFB copy, taken again from each eye (stereo_shadow.hpp).
  std::array<TextureHandle, AURORA_STEREO_EYE_COUNT> copyTargets{};
  // Palette conversions of eye copies, run with the pass's own conversions.
  std::array<std::vector<tex_palette_conv::ConvRequest>, AURORA_STEREO_EYE_COUNT> paletteConvs{};
  // The UV transform each eye's copy samples with when the copy was made
  // under AURORA_STEREO_ROUTE_HEAD_LOCKED_2D (stereo_replay.hpp
  // HeadLockedPlane); empty, an eye copies the pass's own rectangle.
  std::array<Range, AURORA_STEREO_EYE_COUNT> copyUniformRanges{};
};

} // namespace detail

} // namespace aurora::gfx
