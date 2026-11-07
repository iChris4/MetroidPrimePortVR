#pragma once

// Pure helpers that turn a staged GX uniform into one eye's copy. No GPU, no
// globals: the recording side calls them with the bytes it just built, and the
// tests call them with synthetic uniforms.
//
// The GX uniform (gx/shader_info.cpp fill_uniform) begins with the viewport
// sizes, then the 4x4 projection, then the ten position matrices, the ten
// texture matrices, the ten normal matrices, the loaded TEV registers and,
// when the shader lights, the eight lights. The offsets of the parts that
// carry the camera are fixed by the shader's line mode and TEV register use,
// so the builder hands them over as a StereoUniformLayout.

#include "stereo_replay.hpp"

#include <aurora/math.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aurora::gfx {

// The uniform's fixed counts (gx/gx.hpp: MaxPnMtx, MaxTexMtx, GX::MaxLights and
// sizeof(gx::Light)); recording.cpp asserts them against the live definitions.
inline constexpr uint32_t kStereoPositionMatrices = 10;
inline constexpr uint32_t kStereoTextureMatrices = 10;
inline constexpr uint32_t kStereoLights = 8;
inline constexpr uint32_t kStereoLightBytes = 80; // pos vec4, dir vec4, colour, cosAtt, distAtt

struct StereoUniformLayout {
  uint32_t projectionOffset = 0; // the Mat4x4 projection
  uint32_t positionOffset = 0;   // kStereoPositionMatrices Mat3x4 position matrices
  uint32_t normalOffset = 0;     // kStereoPositionMatrices Mat3x4 normal matrices
  uint32_t lightsOffset = 0;     // kStereoLights lights, 0 when the shader has none
  uint32_t size = 0;             // the whole uniform
  uint32_t clipPlaneOffset = 0;  // optional view-space covector; 0 when absent
  // The one position (and normal) matrix slot the draw can index, when its
  // vertices carry no matrix index (gx/shader.cpp: in_pnmtxidx is the current
  // one); -1 composes all of them.
  int32_t fixedPositionSlot = -1;

  // The layout fill_uniform produces for a shader: `lineMode` != 0 adds one
  // 16-byte block before the projection; `lightingEnabled` places the lights
  // after the `tevRegCount` loaded TEV registers that follow the normal matrices.
  static constexpr StereoUniformLayout for_gx(uint8_t lineMode, bool lightingEnabled, uint32_t tevRegCount,
                                              uint32_t size) noexcept {
    StereoUniformLayout layout{};
    layout.projectionOffset = 16u + (lineMode != 0 ? 16u : 0u);
    layout.positionOffset = layout.projectionOffset + static_cast<uint32_t>(sizeof(Mat4x4<float>));
    layout.normalOffset = layout.positionOffset + (kStereoPositionMatrices + kStereoTextureMatrices) *
                                                      static_cast<uint32_t>(sizeof(Mat3x4<float>));
    if (lightingEnabled) {
      layout.lightsOffset = layout.normalOffset +
                            kStereoPositionMatrices * static_cast<uint32_t>(sizeof(Mat3x4<float>)) +
                            tevRegCount * static_cast<uint32_t>(sizeof(Vec4<float>));
    }
    layout.size = size;
    return layout;
  }

  [[nodiscard]] constexpr bool valid() const noexcept {
    const uint32_t normalEnd = normalOffset + kStereoPositionMatrices * static_cast<uint32_t>(sizeof(Mat3x4<float>));
    const uint32_t lightsEnd = lightsOffset != 0 ? lightsOffset + kStereoLights * kStereoLightBytes : 0;
    return size >= 16 && projectionOffset + static_cast<uint32_t>(sizeof(Mat4x4<float>)) <= size &&
           normalEnd <= size && lightsEnd <= size &&
           (clipPlaneOffset == 0 || clipPlaneOffset + sizeof(Vec4<float>) <= size);
  }
};

// Whether the staged projection is a perspective one (only those carry the
// game camera; an orthographic draw is 2D content).
inline bool stereo_uniform_is_perspective(const uint8_t* uniform, const StereoUniformLayout& layout) noexcept {
  Mat4x4<float> projection;
  std::memcpy(&projection, uniform + layout.projectionOffset, sizeof(projection));
  return !stereo_replay::is_orthographic_projection(projection);
}

struct StereoEyeCompose {
  const Mat4x4<float>* projection = nullptr;     // the eye frustum
  const Mat3x4<float>* viewFromCenter = nullptr; // the eye pose, from the space the draw was recorded in
  // AURORA_STEREO_ROUTE_HEAD_LOCKED: scales about the camera origin before the
  // eye pose. Across the view (x, y) changes the angular size; along it (z,
  // the depth) changes the distance.
  float positionScaleXY = 1.0f;
  float positionScaleZ = 1.0f;
  float renderScaleX = 1.0f; // eye target size over the recorded render target size
  float renderScaleY = 1.0f;
};

// Rewrites `uniform` (a copy of the mono uniform) in place for one eye of a
// perspective draw:
//  - the projection keeps its depth rows and takes the eye frustum's four
//    perspective terms (stereo_replay::compose_projection);
//  - every position matrix is composed with the eye pose, after the optional
//    head-locked scales, so vertices land in the eye's view space;
//  - every normal matrix takes the pose's rotation (rigid: no inverse transpose);
//  - the lights follow the same pose, so lighting stays attached to the world
//    rather than to the head: GX lights live in view space, and the game
//    placed them in the centre (game camera) space the draw was recorded in;
//  - the render size the shader uses for point/line expansion and the GX pixel
//    centre correction becomes the eye target's.
inline void compose_stereo_uniform(uint8_t* uniform, const StereoUniformLayout& layout,
                                   const StereoEyeCompose& eye) noexcept {
  Mat4x4<float> projection;
  std::memcpy(&projection, uniform + layout.projectionOffset, sizeof(projection));
  projection = stereo_replay::compose_projection(*eye.projection, projection);
  std::memcpy(uniform + layout.projectionOffset, &projection, sizeof(projection));

  if (layout.clipPlaneOffset != 0 && eye.positionScaleXY != 0.f && eye.positionScaleZ != 0.f) {
    // Position is transformed by R*S then t. The clip-plane covector takes its
    // inverse transpose, including eye translation; copying the mono equation
    // unchanged clips the same weapon at different places in the two eyes.
    Vec4<float> plane;
    std::memcpy(&plane, uniform + layout.clipPlaneOffset, sizeof(plane));
    const float x = plane[0] / eye.positionScaleXY;
    const float y = plane[1] / eye.positionScaleXY;
    const float z = plane[2] / eye.positionScaleZ;
    const auto& view = *eye.viewFromCenter;
    const Vec4<float> transformed{
        view.m0[0]*x + view.m0[1]*y + view.m0[2]*z,
        view.m1[0]*x + view.m1[1]*y + view.m1[2]*z,
        view.m2[0]*x + view.m2[1]*y + view.m2[2]*z, 0.f};
    const float constant = plane[3];
    plane = transformed;
    plane[3] = constant - (plane[0]*view.m0[3] + plane[1]*view.m1[3] + plane[2]*view.m2[3]);
    std::memcpy(uniform + layout.clipPlaneOffset, &plane, sizeof(plane));
  }

  const bool oneSlot = layout.fixedPositionSlot >= 0 &&
                       static_cast<uint32_t>(layout.fixedPositionSlot) < kStereoPositionMatrices;
  const uint32_t firstSlot = oneSlot ? static_cast<uint32_t>(layout.fixedPositionSlot) : 0;
  const uint32_t endSlot = oneSlot ? firstSlot + 1 : kStereoPositionMatrices;
  for (uint32_t i = firstSlot; i < endSlot; ++i) {
    const size_t offset = layout.positionOffset + i * sizeof(Mat3x4<float>);
    Mat3x4<float> source;
    std::memcpy(&source, uniform + offset, sizeof(source));
    if (eye.positionScaleXY != 1.0f || eye.positionScaleZ != 1.0f) {
      for (size_t row = 0; row < 3; ++row) {
        auto& values = *(&source.m0 + row);
        const float scale = row == 2 ? eye.positionScaleZ : eye.positionScaleXY;
        for (size_t c = 0; c < 4; ++c) {
          values[c] *= scale;
        }
      }
    }
    const auto transformed = stereo_replay::compose_affine(*eye.viewFromCenter, source);
    std::memcpy(uniform + offset, &transformed, sizeof(transformed));
  }
  for (uint32_t i = firstSlot; i < endSlot; ++i) {
    const size_t offset = layout.normalOffset + i * sizeof(Mat3x4<float>);
    Mat3x4<float> source;
    std::memcpy(&source, uniform + offset, sizeof(source));
    const auto transformed = stereo_replay::compose_normal(*eye.viewFromCenter, source);
    std::memcpy(uniform + offset, &transformed, sizeof(transformed));
  }
  if (layout.lightsOffset != 0) {
    const auto& view = *eye.viewFromCenter;
    for (uint32_t i = 0; i < kStereoLights; ++i) {
      const size_t offset = layout.lightsOffset + i * kStereoLightBytes;
      float pos[4];
      float dir[4];
      std::memcpy(pos, uniform + offset, sizeof(pos));
      std::memcpy(dir, uniform + offset + sizeof(pos), sizeof(dir));
      float outPos[4] = {0.0f, 0.0f, 0.0f, pos[3]};
      float outDir[4] = {0.0f, 0.0f, 0.0f, dir[3]};
      for (size_t row = 0; row < 3; ++row) {
        const auto& v = *(&view.m0 + row);
        outPos[row] = v[0] * pos[0] + v[1] * pos[1] + v[2] * pos[2] + v[3];
        outDir[row] = v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2];
      }
      std::memcpy(uniform + offset, outPos, sizeof(outPos));
      std::memcpy(uniform + offset + sizeof(outPos), outDir, sizeof(outDir));
    }
  }

  float renderSize[2];
  std::memcpy(renderSize, uniform, sizeof(renderSize));
  renderSize[0] *= eye.renderScaleX;
  renderSize[1] *= eye.renderScaleY;
  std::memcpy(uniform, renderSize, sizeof(renderSize));
}

// Multiview (stereo_multiview.hpp): the one matrix that takes a draw's mono
// view-space position (its unchanged position matrices applied) to an eye's clip
// space, i.e. what compose_stereo_uniform spreads over the projection and the
// position matrices: the eye's composed projection, after the eye pose, after
// the head-locked scales. Rows of the column-vector matrix, as Aurora stores
// them (the shader computes vec4 * mat).
inline Mat4x4<float> compose_eye_clip(const Mat4x4<float>& eyeProjection, const Mat3x4<float>& viewFromCenter,
                                      float positionScaleXY, float positionScaleZ) noexcept {
  // The pose with the scales applied on its right (columns), as a 4x4.
  const float scales[3] = {positionScaleXY, positionScaleXY, positionScaleZ};
  float pose[4][4] = {};
  for (size_t row = 0; row < 3; ++row) {
    const auto& values = *(&viewFromCenter.m0 + row);
    for (size_t column = 0; column < 3; ++column) {
      pose[row][column] = values[column] * scales[column];
    }
    pose[row][3] = values[3];
  }
  pose[3][3] = 1.0f;
  Mat4x4<float> out{};
  for (size_t row = 0; row < 4; ++row) {
    const auto& projectionRow = *(&eyeProjection.m0 + row);
    auto& dst = *(&out.m0 + row);
    for (size_t column = 0; column < 4; ++column) {
      dst[column] = projectionRow[0] * pose[0][column] + projectionRow[1] * pose[1][column] +
                    projectionRow[2] * pose[2][column] + projectionRow[3] * pose[3][column];
    }
  }
  return out;
}

// AuroraSetStereoScreenTexMtx: after compose_stereo_uniform, derives texture
// matrix `texSlot` again from the eye's composed projection and position
// matrix `pnSlot` (stereo_replay::screen_tex_mtx), so a draw that samples the
// viewport's EFB copy through it reads what lies behind it in the eye's own
// copy. A slot out of range leaves the uniform alone.
inline void compose_stereo_screen_tex_mtx(uint8_t* uniform, const StereoUniformLayout& layout, uint32_t texSlot,
                                          uint32_t pnSlot) noexcept {
  if (texSlot >= kStereoTextureMatrices || pnSlot >= kStereoPositionMatrices) {
    return;
  }
  Mat4x4<float> projection;
  std::memcpy(&projection, uniform + layout.projectionOffset, sizeof(projection));
  Mat3x4<float> position;
  std::memcpy(&position, uniform + layout.positionOffset + pnSlot * sizeof(Mat3x4<float>), sizeof(position));
  const auto texture = stereo_replay::screen_tex_mtx(projection, position);
  std::memcpy(uniform + layout.positionOffset + (kStereoPositionMatrices + texSlot) * sizeof(Mat3x4<float>), &texture,
              sizeof(texture));
}

// AURORA_STEREO_ROUTE_HEAD_LOCKED_2D: an orthographic draw laid on the
// head-locked plane (stereo_replay.hpp HeadLockedPlane).
struct StereoEye2DCompose {
  const Mat4x4<float>* projection = nullptr;               // the eye frustum
  const Mat3x4<float>* headLockedViewFromCenter = nullptr; // the eye's offset from the head centre
  stereo_replay::HudScreen plane{};                        // the plane, after the head-locked scales
  float renderScaleX = 1.0f; // eye target size over the recorded render target size
  float renderScaleY = 1.0f;
};

namespace detail {
// The eye's projection in place of the mono one, and the eye target's render
// size, as compose_stereo_uniform makes it.
inline void set_stereo_projection(uint8_t* uniform, const StereoUniformLayout& layout,
                                  const Mat4x4<float>& projection, float renderScaleX, float renderScaleY) noexcept {
  std::memcpy(uniform + layout.projectionOffset, &projection, sizeof(projection));

  float renderSize[2];
  std::memcpy(renderSize, uniform, sizeof(renderSize));
  renderSize[0] *= renderScaleX;
  renderSize[1] *= renderScaleY;
  std::memcpy(uniform, renderSize, sizeof(renderSize));
}
} // namespace detail

// Rewrites `uniform` (a copy of the mono uniform) in place for one eye of an
// orthographic draw on the head-locked plane: the projection becomes
// stereo_replay::compose_head_locked_2d_projection's (the draw's position
// matrices already give its flat-screen NDC through the mono one, so they
// stay), and the render size becomes the eye target's as for a perspective
// draw.
inline void compose_stereo_2d_uniform(uint8_t* uniform, const StereoUniformLayout& layout,
                                      const StereoEye2DCompose& eye) noexcept {
  Mat4x4<float> projection;
  std::memcpy(&projection, uniform + layout.projectionOffset, sizeof(projection));
  projection = stereo_replay::compose_head_locked_2d_projection(*eye.projection, *eye.headLockedViewFromCenter,
                                                                eye.plane, projection);
  detail::set_stereo_projection(uniform, layout, projection, eye.renderScaleX, eye.renderScaleY);
}

// AURORA_STEREO_ROUTE_SCREEN_2D: a draw, perspective or orthographic, laid on
// the virtual screen (stereo_replay.hpp compose_screen_2d_projection).
struct StereoEyeScreenCompose {
  const Mat4x4<float>* projection = nullptr;     // the eye frustum
  const Mat3x4<float>* viewFromCenter = nullptr; // the eye pose, from the space the draw was recorded in
  stereo_replay::HudScreen screen{};
  stereo_replay::HudNdcRemap ndcRemap{}; // the draw's viewport within the recorded picture
  float renderScaleX = 1.0f; // eye target size over the recorded render target size
  float renderScaleY = 1.0f;
};

// Rewrites `uniform` (a copy of the mono uniform) in place for one eye of a
// draw on the virtual screen: only the projection changes (the screen takes
// the mono picture, so the position matrices and the lights stay the mono
// view's), and the render size becomes the eye target's.
inline void compose_stereo_screen_2d_uniform(uint8_t* uniform, const StereoUniformLayout& layout,
                                             const StereoEyeScreenCompose& eye) noexcept {
  Mat4x4<float> projection;
  std::memcpy(&projection, uniform + layout.projectionOffset, sizeof(projection));
  projection = stereo_replay::compose_screen_2d_projection(*eye.projection, *eye.viewFromCenter, eye.screen,
                                                           projection, eye.ndcRemap);
  detail::set_stereo_projection(uniform, layout, projection, eye.renderScaleX, eye.renderScaleY);
}

} // namespace aurora::gfx
