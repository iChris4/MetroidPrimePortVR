// The stereo replay's uniform composition (extern/aurora/lib/gfx/stereo_uniform.hpp):
// the GX uniform layout the recorder assumes, what one eye's copy of a
// perspective draw's uniform must contain, and the head-locked plane that
// AURORA_STEREO_ROUTE_HEAD_LOCKED_2D lays 2D draws on and takes EFB copies
// through (stereo_replay.hpp HeadLockedPlane: the scan visor's window).

#include "gfx/stereo_uniform.hpp"
#include "gfx/floating_hud.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "vr stereo regression failed: %s\n", what);
    std::abort();
  }
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }

aurora::Mat3x4<float> Affine(float m00, float m01, float m02, float tx, float m10, float m11, float m12, float ty,
                             float m20, float m21, float m22, float tz) {
  aurora::Mat3x4<float> out{};
  out.m0 = aurora::Vec4<float>{m00, m01, m02, tx};
  out.m1 = aurora::Vec4<float>{m10, m11, m12, ty};
  out.m2 = aurora::Vec4<float>{m20, m21, m22, tz};
  return out;
}
} // namespace

int main() {
  using namespace aurora;
  using namespace aurora::gfx;

  // The layout of a lit shader with two TEV registers loaded, no line mode.
  const auto layout = StereoUniformLayout::for_gx(0, true, 2, 2400);
  Check(layout.projectionOffset == 16, "projection follows the four viewport sizes");
  Check(layout.positionOffset == 80, "position matrices follow the projection");
  Check(layout.normalOffset == 80 + 20 * 48, "normal matrices follow ten position and ten texture matrices");
  Check(layout.lightsOffset == 1040 + 10 * 48 + 2 * 16, "lights follow the normal matrices and the TEV registers");
  Check(layout.valid(), "layout fits the uniform");
  const auto line = StereoUniformLayout::for_gx(1, false, 0, 1600);
  Check(line.projectionOffset == 32 && line.lightsOffset == 0 && line.valid(), "line mode shifts the projection");
  Check(!StereoUniformLayout::for_gx(1, false, 0, 1200).valid(), "a short uniform is rejected");

  // A synthetic uniform: sizes, a perspective projection, position matrix 0 at
  // (1, 2, 3), identity normal matrix 0, a marker in texture matrix 0, and a
  // light five units ahead pointing forward.
  std::vector<uint8_t> uniform(2400, 0);
  const float sizes[4] = {1920.f, 1080.f, 640.f, 480.f};
  std::memcpy(uniform.data(), sizes, sizeof(sizes));
  Mat4x4<float> projection{};
  projection.m0 = Vec4<float>{1.5f, 0.f, 0.1f, 0.f};
  projection.m1 = Vec4<float>{0.f, 2.f, 0.2f, 0.f};
  projection.m2 = Vec4<float>{0.f, 0.f, -1.1f, -0.3f};
  projection.m3 = Vec4<float>{0.f, 0.f, -1.f, 0.f};
  std::memcpy(uniform.data() + layout.projectionOffset, &projection, sizeof(projection));
  Check(stereo_uniform_is_perspective(uniform.data(), layout), "a perspective projection is recognised");
  const auto position0 = Affine(1, 0, 0, 1, 0, 1, 0, 2, 0, 0, 1, 3);
  std::memcpy(uniform.data() + layout.positionOffset, &position0, sizeof(position0));
  const auto identity = Affine(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0);
  std::memcpy(uniform.data() + layout.normalOffset, &identity, sizeof(identity));
  const auto marker = Affine(7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7);
  const size_t textureMatrix0 = layout.positionOffset + kStereoPositionMatrices * sizeof(Mat3x4<float>);
  std::memcpy(uniform.data() + textureMatrix0, &marker, sizeof(marker));
  const float light[20] = {0.f, 0.f, -5.f, 0.f, 0.f, 0.f, -1.f, 0.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
  std::memcpy(uniform.data() + layout.lightsOffset, light, sizeof(light));

  // The eye: its own frustum terms, a quarter turn about Y (x' = -z, z' = x)
  // and a 32 mm offset to the right.
  Mat4x4<float> eyeProjection{};
  eyeProjection.m0 = Vec4<float>{0.8f, 0.f, 0.05f, 0.f};
  eyeProjection.m1 = Vec4<float>{0.f, 0.9f, -0.04f, 0.f};
  const auto view = Affine(0, 0, -1, 0.032f, 0, 1, 0, 0, 1, 0, 0, 0);

  auto eye = uniform;
  compose_stereo_uniform(eye.data(), layout, StereoEyeCompose{&eyeProjection, &view, 1.f, 1.f, 2.f, 0.5f});

  Mat4x4<float> outProjection;
  std::memcpy(&outProjection, eye.data() + layout.projectionOffset, sizeof(outProjection));
  Check(Near(outProjection.m0[0], 0.8f) && Near(outProjection.m0[2], 0.05f) && Near(outProjection.m1[1], 0.9f) &&
            Near(outProjection.m1[2], -0.04f),
        "the four frustum terms come from the eye");
  Check(Near(outProjection.m2[2], -1.1f) && Near(outProjection.m2[3], -0.3f) && Near(outProjection.m3[2], -1.f),
        "the depth rows are the game's");

  Mat3x4<float> outPosition;
  std::memcpy(&outPosition, eye.data() + layout.positionOffset, sizeof(outPosition));
  Check(Near(outPosition.m0[3], -3.f + 0.032f) && Near(outPosition.m1[3], 2.f) && Near(outPosition.m2[3], 1.f),
        "position matrix 0 is rotated and offset into the eye");
  Check(Near(outPosition.m0[2], -1.f) && Near(outPosition.m2[0], 1.f) && Near(outPosition.m0[0], 0.f),
        "position matrix 0 carries the eye rotation");

  Mat3x4<float> outNormal;
  std::memcpy(&outNormal, eye.data() + layout.normalOffset, sizeof(outNormal));
  Check(Near(outNormal.m0[2], -1.f) && Near(outNormal.m2[0], 1.f) && Near(outNormal.m0[3], 0.f),
        "normal matrix 0 is rotated without translation");

  Check(std::memcmp(eye.data() + textureMatrix0, uniform.data() + textureMatrix0, sizeof(marker)) == 0,
        "texture matrices are untouched");

  // A screen-projecting texture matrix (AuroraSetStereoScreenTexMtx): derived
  // again from the eye's composed projection and position matrix, so s / q
  // and t / q are the vertex's own place in the eye image, u to the right
  // and v down from the top left corner.
  auto screenUniform = eye;
  compose_stereo_screen_tex_mtx(screenUniform.data(), layout, 0, 0);
  Mat3x4<float> outTexture;
  std::memcpy(&outTexture, screenUniform.data() + textureMatrix0, sizeof(outTexture));
  {
    const float object[4] = {0.3f, -0.2f, -4.f, 1.f};
    const auto dot4 = [](const Vec4<float>& row, const float (&v)[4]) {
      return row[0] * v[0] + row[1] * v[1] + row[2] * v[2] + row[3] * v[3];
    };
    const float viewPoint[4] = {dot4(outPosition.m0, object), dot4(outPosition.m1, object),
                                dot4(outPosition.m2, object), 1.f};
    const float clipW = dot4(outProjection.m3, viewPoint);
    const float u = 0.5f * (dot4(outProjection.m0, viewPoint) / clipW + 1.f);
    const float v = 0.5f * (1.f - dot4(outProjection.m1, viewPoint) / clipW);
    const float s = dot4(outTexture.m0, object);
    const float t = dot4(outTexture.m1, object);
    const float q = dot4(outTexture.m2, object);
    Check(Near(q, clipW) && Near(s / q, u) && Near(t / q, v),
          "the screen texture matrix lands a vertex at its own place in the eye image");
  }
  Check(std::memcmp(screenUniform.data() + layout.positionOffset, eye.data() + layout.positionOffset,
                    kStereoPositionMatrices * sizeof(Mat3x4<float>)) == 0,
        "the screen texture matrix leaves the position matrices alone");
  auto ignored = eye;
  compose_stereo_screen_tex_mtx(ignored.data(), layout, kStereoTextureMatrices, 0);
  Check(ignored == eye, "a texture matrix slot out of range is ignored");
  {
    // Straight ahead of a symmetric frustum is the middle of the image; a
    // point to the right and up lands right of and above it.
    Mat4x4<float> symmetric{};
    symmetric.m0 = Vec4<float>{2.f, 0.f, 0.f, 0.f};
    symmetric.m1 = Vec4<float>{0.f, 2.f, 0.f, 0.f};
    symmetric.m3 = Vec4<float>{0.f, 0.f, -1.f, 0.f};
    const auto ahead = stereo_replay::screen_tex_mtx(symmetric, identity);
    Check(Near(ahead.m0[2], -0.5f) && Near(ahead.m1[2], -0.5f) && Near(ahead.m2[2], -1.f) &&
              Near(ahead.m0[0], 1.f) && Near(ahead.m1[1], -1.f),
          "the screen texture matrix is the halved and shifted clip rows");
    // (0.25, 0.25, -1): s = 0.75, t = 0.25, q = 1.
    Check(Near(ahead.m0[0] * 0.25f + ahead.m0[2] * -1.f, 0.75f) &&
              Near(ahead.m1[1] * 0.25f + ahead.m1[2] * -1.f, 0.25f),
          "right and up is right of and above the middle of the image");
  }

  float outLight[20];
  std::memcpy(outLight, eye.data() + layout.lightsOffset, sizeof(outLight));
  Check(Near(outLight[0], 5.032f) && Near(outLight[1], 0.f) && Near(outLight[2], 0.f),
        "the light position follows the eye");
  Check(Near(outLight[4], 1.f) && Near(outLight[5], 0.f) && Near(outLight[6], 0.f),
        "the light direction is rotated only");
  Check(Near(outLight[8], 1.f) && Near(outLight[11], 1.f), "the light colour is untouched");

  float outSizes[4];
  std::memcpy(outSizes, eye.data(), sizeof(outSizes));
  Check(Near(outSizes[0], 3840.f) && Near(outSizes[1], 540.f) && Near(outSizes[2], 640.f) &&
            Near(outSizes[3], 480.f),
        "the render size becomes the eye's, the logical size stays");

  // Head-locked: the scales apply before the pose, across and along the view.
  auto headLocked = uniform;
  compose_stereo_uniform(headLocked.data(), layout,
                         StereoEyeCompose{&eyeProjection, &identity, 0.5f, 2.f, 1.f, 1.f});
  std::memcpy(&outPosition, headLocked.data() + layout.positionOffset, sizeof(outPosition));
  Check(Near(outPosition.m0[3], 0.5f) && Near(outPosition.m1[3], 1.f) && Near(outPosition.m2[3], 6.f) &&
            Near(outPosition.m0[0], 0.5f) && Near(outPosition.m2[2], 2.f),
        "head-locked scales shrink across the view and push along it");

  // The sky (AURORA_STEREO_ROUTE_SKY): the eye's rotation without its offset,
  // so a dome centred on the camera has no disparity and stays put when the
  // head moves.
  const auto skyView = stereo_replay::without_translation(view);
  Check(Near(skyView.m0[3], 0.f) && Near(skyView.m1[3], 0.f) && Near(skyView.m2[3], 0.f) &&
            Near(skyView.m0[2], -1.f) && Near(skyView.m2[0], 1.f),
        "the sky view keeps the rotation and drops the translation");
  auto sky = uniform;
  compose_stereo_uniform(sky.data(), layout, StereoEyeCompose{&eyeProjection, &skyView, 1.f, 1.f, 1.f, 1.f});
  std::memcpy(&outPosition, sky.data() + layout.positionOffset, sizeof(outPosition));
  Check(Near(outPosition.m0[3], -3.f) && Near(outPosition.m1[3], 2.f) && Near(outPosition.m2[3], 1.f) &&
            Near(outPosition.m0[2], -1.f),
        "a sky draw is rotated into the eye without the eye's offset");

  // A visor draw (AURORA_STEREO_ROUTE_VISOR): head-locked, with the eye's
  // offset (half a 64 mm IPD at 1.5 units a metre) scaled by near / far. A
  // drop 0.21 units ahead lands in the eye where the same direction 2 units
  // ahead (the HUD's distance) would with the whole offset, at its own depth.
  {
    const auto headLockedEye = Affine(1, 0, 0, 0.048f, 0, 1, 0, 0, 0, 0, 1, 0);
    const float nearDepth = 0.21f;
    const float hudDepth = 2.f;
    const auto visorEye = stereo_replay::scaled_translation(headLockedEye, nearDepth / hudDepth);
    Check(Near(visorEye.m0[3], 0.048f * nearDepth / hudDepth) && Near(visorEye.m0[0], 1.f) &&
              Near(visorEye.m2[3], 0.f),
          "the visor view scales the eye offset and keeps the rotation");
    // Ahead is -z; the eye's x in clip over w.
    const auto ndcX = [&](const Mat3x4<float>& eyeView, float depth) {
      const float x = eyeView.m0[3];
      const float z = -depth + eyeView.m2[3];
      return (eyeProjection.m0[0] * x + eyeProjection.m0[2] * z) / -z;
    };
    Check(Near(ndcX(visorEye, nearDepth), ndcX(headLockedEye, hudDepth)),
          "a visor draw at the near plane shows the disparity of the HUD's distance");
    auto visor = uniform;
    const auto drop = Affine(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -nearDepth);
    std::memcpy(visor.data() + layout.positionOffset, &drop, sizeof(drop));
    compose_stereo_uniform(visor.data(), layout, StereoEyeCompose{&eyeProjection, &visorEye, 1.f, 1.f, 1.f, 1.f});
    std::memcpy(&outPosition, visor.data() + layout.positionOffset, sizeof(outPosition));
    Check(Near(outPosition.m2[3], -nearDepth) && Near(outPosition.m0[3], 0.048f * nearDepth / hudDepth),
          "a visor draw keeps its depth (in front of the world) and takes the scaled offset");
  }

  // An orthographic projection is 2D content: identical in both eyes.
  auto ortho = uniform;
  Mat4x4<float> orthoProjection{};
  orthoProjection.m0 = Vec4<float>{0.1f, 0.f, 0.f, -1.f};
  orthoProjection.m1 = Vec4<float>{0.f, 0.2f, 0.f, 1.f};
  orthoProjection.m2 = Vec4<float>{0.f, 0.f, -0.5f, 0.f};
  orthoProjection.m3 = Vec4<float>{0.f, 0.f, 0.f, 1.f};
  std::memcpy(ortho.data() + layout.projectionOffset, &orthoProjection, sizeof(orthoProjection));
  Check(!stereo_uniform_is_perspective(ortho.data(), layout), "an orthographic projection is 2D content");

  // The head-locked plane: the HUD camera's half-angle tangents and a depth,
  // scaled like a head-locked perspective draw (across by size times
  // distance, along by distance).
  const stereo_replay::HeadLockedPlane plane{1.2f, 0.6f, 20.f};
  Check(plane.valid() && !stereo_replay::HeadLockedPlane{}.valid(), "a plane needs its three extents");
  const auto screen = stereo_replay::head_locked_plane_screen(plane, 0.5625f, 0.75f);
  Check(Near(screen.halfWidth, 13.5f) && Near(screen.halfHeight, 6.75f) && Near(screen.distance, 15.f),
        "the plane scales across the view by the size and along it by the distance");
  Check(!stereo_replay::head_locked_plane_screen({}, 1.f, 1.f).valid(), "no plane, no screen");

  // An orthographic draw on the plane: its NDC lands on the plane, offset by
  // the eye, and its flat-screen depth survives the perspective divide. A
  // 90 degree symmetric eye frustum, an eye a tenth of a unit to the right,
  // a plane two by one half-units across at distance four.
  Mat4x4<float> flatFrustum{};
  flatFrustum.m0 = Vec4<float>{1.f, 0.f, 0.f, 0.f};
  flatFrustum.m1 = Vec4<float>{0.f, 1.f, 0.f, 0.f};
  flatFrustum.m3 = Vec4<float>{0.f, 0.f, -1.f, 0.f};
  const auto rightEye = Affine(1, 0, 0, 0.1f, 0, 1, 0, 0, 0, 0, 1, 0);
  const stereo_replay::HudScreen paneScreen{2.f, 1.f, 4.f};
  // ndc x = 0.1 x, ndc y = 0.2 y, depth = -0.5 z + 0.25
  Mat4x4<float> paneOrtho{};
  paneOrtho.m0 = Vec4<float>{0.1f, 0.f, 0.f, 0.f};
  paneOrtho.m1 = Vec4<float>{0.f, 0.2f, 0.f, 0.f};
  paneOrtho.m2 = Vec4<float>{0.f, 0.f, -0.5f, 0.25f};
  paneOrtho.m3 = Vec4<float>{0.f, 0.f, 0.f, 1.f};
  const auto onPlane =
      stereo_replay::compose_head_locked_2d_projection(flatFrustum, rightEye, paneScreen, paneOrtho);
  // The vertex (5, 0, 1): ndc x 0.5, so plane x 1, plus the eye's 0.1, at distance 4.
  const float vertex[4] = {5.f, 0.f, 1.f, 1.f};
  const auto clip = [&](const Vec4<float>& row) {
    return row[0] * vertex[0] + row[1] * vertex[1] + row[2] * vertex[2] + row[3] * vertex[3];
  };
  const float w = clip(onPlane.m3);
  Check(Near(w, 4.f), "the plane's distance is the clip w");
  Check(Near(clip(onPlane.m0) / w, 0.275f) && Near(clip(onPlane.m1) / w, 0.f),
        "the vertex lands on the plane, offset by the eye");
  Check(Near(clip(onPlane.m2) / w, -0.25f), "the flat-screen depth survives the divide");
  auto paneUniform = ortho;
  std::memcpy(paneUniform.data() + layout.projectionOffset, &paneOrtho, sizeof(paneOrtho));
  compose_stereo_2d_uniform(paneUniform.data(), layout,
                            StereoEye2DCompose{&flatFrustum, &rightEye, paneScreen, 2.f, 0.5f});
  Mat4x4<float> staged;
  std::memcpy(&staged, paneUniform.data() + layout.projectionOffset, sizeof(staged));
  Check(std::memcmp(&staged, &onPlane, sizeof(staged)) == 0, "the 2D eye uniform carries the plane projection");
  Check(std::memcmp(paneUniform.data() + layout.positionOffset, ortho.data() + layout.positionOffset,
                    sizeof(Mat3x4<float>)) == 0,
        "a 2D draw's position matrices are untouched");
  std::memcpy(outSizes, paneUniform.data(), sizeof(outSizes));
  Check(Near(outSizes[0], 3840.f) && Near(outSizes[1], 540.f), "the 2D eye uniform takes the eye's render size");

  // A copy through the plane: the part of the eye's view behind the plane's
  // rectangle, in the eye's texture coordinates.
  const auto uv =
      stereo_replay::head_locked_plane_uv_rect(flatFrustum, rightEye, paneScreen, -0.5f, 0.5f, 0.5f, -0.5f);
  Check(uv.valid() && Near(uv.u, 0.3875f) && Near(uv.v, 0.4375f) && Near(uv.width, 0.25f) &&
            Near(uv.height, 0.125f),
        "the copy follows the plane's rectangle, offset by the eye");
  // An asymmetric frustum: straight ahead is off the eye image's centre, and so is the copy.
  Mat4x4<float> cantedFrustum = flatFrustum;
  cantedFrustum.m0[2] = 0.5f;
  const auto canted =
      stereo_replay::head_locked_plane_uv_rect(cantedFrustum, identity, paneScreen, -0.5f, 0.5f, 0.5f, -0.5f);
  Check(canted.valid() && Near(canted.u + 0.5f * canted.width, 0.25f),
        "the copy is centred where the head points, not at the image's centre");
  Check(!stereo_replay::head_locked_plane_uv_rect(flatFrustum, rightEye, {}, -0.5f, 0.5f, 0.5f, -0.5f).valid(),
        "no plane, no copy rectangle");

  // The virtual screen (AURORA_STEREO_ROUTE_SCREEN_2D: the morph ball's HUD).
  // A screen that faces the eye squarely is the head-locked plane: same
  // place, same exact depth.
  {
    const auto onScreen =
        stereo_replay::compose_screen_2d_projection(flatFrustum, rightEye, paneScreen, paneOrtho);
    bool same = true;
    for (size_t row = 0; row < 4; ++row) {
      for (size_t i = 0; i < 4; ++i) {
        same = same && Near((*(&onScreen.m0 + row))[i], (*(&onPlane.m0 + row))[i]);
      }
    }
    Check(same, "a screen facing the eye lays an orthographic draw like the head-locked plane");
  }
  // A perspective draw: its mono NDC lands on the screen, and with the screen
  // facing the eye its NDC depth survives the divide.
  const auto apply = [](const Mat4x4<float>& m, const float (&v)[4], float (&out)[4]) {
    for (size_t row = 0; row < 4; ++row) {
      const auto& r = *(&m.m0 + row);
      out[row] = r[0] * v[0] + r[1] * v[1] + r[2] * v[2] + r[3] * v[3];
    }
  };
  // Where the point of the screen at frame NDC (x, y) lands in an eye's NDC.
  const auto screenPointNdc = [](const Mat4x4<float>& frustum, const Mat3x4<float>& view,
                                 const stereo_replay::HudScreen& s, float x, float y, float& outX, float& outY) {
    const float p[4] = {x * s.halfWidth, y * s.halfHeight, -s.distance, 1.f};
    float e[3];
    for (size_t row = 0; row < 3; ++row) {
      const auto& r = *(&view.m0 + row);
      e[row] = r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3] * p[3];
    }
    outX = (frustum.m0[0] * e[0] + frustum.m0[2] * e[2]) / -e[2];
    outY = (frustum.m1[1] * e[1] + frustum.m1[2] * e[2]) / -e[2];
  };
  {
    const float hudVertex[4] = {0.3f, -0.2f, -4.f, 1.f};
    float mono[4];
    apply(projection, hudVertex, mono);
    const auto composed =
        stereo_replay::compose_screen_2d_projection(cantedFrustum, rightEye, paneScreen, projection);
    float eyeClip[4];
    apply(composed, hudVertex, eyeClip);
    float x = 0.f;
    float y = 0.f;
    screenPointNdc(cantedFrustum, rightEye, paneScreen, mono[0] / mono[3], mono[1] / mono[3], x, y);
    Check(eyeClip[3] > 0.f && Near(eyeClip[0] / eyeClip[3], x) && Near(eyeClip[1] / eyeClip[3], y),
          "a perspective draw's mono picture lands on the screen");
    Check(Near(eyeClip[2] / eyeClip[3], mono[2] / mono[3]), "a screen facing the eye keeps the draw's depth");
  }
  // The head turned 30 degrees and moved: the screen stays where it hangs,
  // and depth only ever shrinks, by the same factor for every draw at a
  // point of the screen, so the layout keeps its depth order.
  {
    const float c = std::cos(0.5235988f);
    const float s = std::sin(0.5235988f);
    const auto turned = Affine(c, 0, -s, 0.2f, 0, 1, 0, -0.1f, s, 0, c, 0.3f);
    const auto composed = stereo_replay::compose_screen_2d_projection(flatFrustum, turned, paneScreen, paneOrtho);
    // ndc (0.5, 0.4) at mono depths 0.75 and 0.25.
    const float front[4] = {5.f, 2.f, -1.f, 1.f};
    const float back[4] = {5.f, 2.f, 0.f, 1.f};
    float frontClip[4];
    float backClip[4];
    apply(composed, front, frontClip);
    apply(composed, back, backClip);
    float x = 0.f;
    float y = 0.f;
    screenPointNdc(flatFrustum, turned, paneScreen, 0.5f, 0.4f, x, y);
    Check(Near(frontClip[0] / frontClip[3], x) && Near(frontClip[1] / frontClip[3], y),
          "the screen stays where it hangs when the head turns");
    const float frontDepth = frontClip[2] / frontClip[3];
    const float backDepth = backClip[2] / backClip[3];
    Check(frontDepth > backDepth && backDepth > 0.f && frontDepth <= 0.75f && backDepth <= 0.25f,
          "depth shrinks on a turned screen, keeping its order");
    Check(Near(frontDepth / 0.75f, backDepth / 0.25f), "every draw at a point of the screen shrinks alike");
    float largest = 0.f;
    for (const float cornerX : {-1.f, 1.f}) {
      for (const float cornerY : {-1.f, 1.f}) {
        const float corner[4] = {10.f * cornerX, 5.f * cornerY, -1.f, 1.f};
        float clip[4];
        apply(composed, corner, clip);
        largest = std::fmax(largest, clip[2] / clip[3] / 0.75f);
      }
    }
    Check(Near(largest, 1.f), "the nearest corner keeps the exact depth");
  }
  // A draw in a sub-viewport (the picture's top-left quarter) keeps its place
  // in the picture: the eye pass applies the same viewport, scaled to the eye,
  // after the projection.
  {
    const auto quarter = stereo_replay::make_hud_ndc_remap(0.f, 0.f, 320.f, 240.f, 0.f, 0.f, 640.f, 480.f);
    const auto composed =
        stereo_replay::compose_screen_2d_projection(flatFrustum, rightEye, paneScreen, paneOrtho, quarter);
    float clip[4];
    apply(composed, vertex, clip); // ndc (0.5, 0) of the quarter: (-0.25, 0.5) of the picture
    float x = 0.f;
    float y = 0.f;
    screenPointNdc(flatFrustum, rightEye, paneScreen, -0.25f, 0.5f, x, y);
    Check(Near(clip[0] / clip[3] * quarter.scaleX + quarter.offsetX, x) &&
              Near(clip[1] / clip[3] * quarter.scaleY + quarter.offsetY, y),
          "a sub-viewport draw keeps its place in the picture on the screen");
  }
  {
    auto screenUniform = ortho;
    std::memcpy(screenUniform.data() + layout.projectionOffset, &paneOrtho, sizeof(paneOrtho));
    compose_stereo_screen_2d_uniform(screenUniform.data(), layout,
                                     StereoEyeScreenCompose{&flatFrustum, &rightEye, paneScreen, {}, 2.f, 0.5f});
    Mat4x4<float> stagedScreen;
    std::memcpy(&stagedScreen, screenUniform.data() + layout.projectionOffset, sizeof(stagedScreen));
    const auto expected = stereo_replay::compose_screen_2d_projection(flatFrustum, rightEye, paneScreen, paneOrtho);
    Check(std::memcmp(&stagedScreen, &expected, sizeof(expected)) == 0,
          "the screen eye uniform carries the screen projection");
    Check(std::memcmp(screenUniform.data() + layout.positionOffset, ortho.data() + layout.positionOffset,
                      kStereoPositionMatrices * sizeof(Mat3x4<float>)) == 0,
          "a screen draw's position matrices are untouched");
    std::memcpy(outSizes, screenUniform.data(), sizeof(outSizes));
    Check(Near(outSizes[0], 3840.f) && Near(outSizes[1], 540.f), "the screen eye uniform takes the eye's render size");
  }

  // An eye's version of an EFB copy has the eye's resolution: a full copy
  // of an 800 x 450 desktop EFB in a 4808 x 4904 eye, a half-size copy
  // stays half the eye's, a part of the view keeps its share.
  Check(stereo_replay::eye_copy_extent(800, 800, 4808, 16384) == 4808 &&
            stereo_replay::eye_copy_extent(450, 450, 4904, 16384) == 4904,
        "a full EFB copy takes the eye's size");
  Check(stereo_replay::eye_copy_extent(400, 800, 4808, 16384) == 2404,
        "a half-size copy stays half the eye's size");
  Check(stereo_replay::eye_copy_extent(192, 640, 2688, 16384) == 806, "a part of the view keeps its share");
  Check(stereo_replay::eye_copy_extent(1000, 100, 5000, 8192) == 8192, "the device's limit caps the size");
  Check(stereo_replay::eye_copy_extent(1, 4808, 100, 16384) == 1, "a copy never shrinks to nothing");
  Check(stereo_replay::eye_copy_extent(256, 0, 4808, 16384) == 256 &&
            stereo_replay::eye_copy_extent(256, 640, 0, 16384) == 256,
        "without a reference the mono size stays");

  {
    // A draw whose vertices carry no matrix index uses one position matrix: only
    // that slot (and its normal matrix) is composed for the eye, the others stay.
    auto fixed = StereoUniformLayout::for_gx(0, false, 0, 2400);
    fixed.fixedPositionSlot = 1;
    std::vector<uint8_t> uniform(fixed.size, 0);
    const auto slot0 = Affine(1, 0, 0, 1, 0, 1, 0, 2, 0, 0, 1, 3);
    const auto slot1 = Affine(1, 0, 0, 4, 0, 1, 0, 5, 0, 0, 1, 6);
    std::memcpy(uniform.data() + fixed.positionOffset, &slot0, sizeof(slot0));
    std::memcpy(uniform.data() + fixed.positionOffset + sizeof(Mat3x4<float>), &slot1, sizeof(slot1));
    const auto rotation = Affine(0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0);
    std::memcpy(uniform.data() + fixed.normalOffset, &rotation, sizeof(rotation));
    std::memcpy(uniform.data() + fixed.normalOffset + sizeof(Mat3x4<float>), &rotation, sizeof(rotation));
    Mat4x4<float> projection{};
    projection.m0 = Vec4<float>{1.f, 0.f, 0.f, 0.f};
    projection.m1 = Vec4<float>{0.f, 1.f, 0.f, 0.f};
    projection.m2 = Vec4<float>{0.f, 0.f, -1.f, -1.f};
    projection.m3 = Vec4<float>{0.f, 0.f, -1.f, 0.f};
    std::memcpy(uniform.data() + fixed.projectionOffset, &projection, sizeof(projection));
    const auto view = Affine(1, 0, 0, 0.032f, 0, 1, 0, 0, 0, 0, 1, 0);
    auto eye = uniform;
    compose_stereo_uniform(eye.data(), fixed, StereoEyeCompose{&projection, &view, 1.f, 1.f, 1.f, 1.f});
    Mat3x4<float> out0;
    Mat3x4<float> out1;
    std::memcpy(&out0, eye.data() + fixed.positionOffset, sizeof(out0));
    std::memcpy(&out1, eye.data() + fixed.positionOffset + sizeof(Mat3x4<float>), sizeof(out1));
    Check(Near(out1.m0[3], 4.032f) && Near(out0.m0[3], 1.f), "only the fixed position slot takes the eye offset");
    Mat3x4<float> normal0;
    Mat3x4<float> normal1;
    std::memcpy(&normal0, eye.data() + fixed.normalOffset, sizeof(normal0));
    std::memcpy(&normal1, eye.data() + fixed.normalOffset + sizeof(Mat3x4<float>), sizeof(normal1));
    Check(std::memcmp(&normal0, &rotation, sizeof(rotation)) == 0, "the other slot's normal matrix stays");
    fixed.fixedPositionSlot = -1;
    auto all = uniform;
    compose_stereo_uniform(all.data(), fixed, StereoEyeCompose{&projection, &view, 1.f, 1.f, 1.f, 1.f});
    std::memcpy(&out0, all.data() + fixed.positionOffset, sizeof(out0));
    Check(Near(out0.m0[3], 1.032f), "without a fixed slot every position matrix takes it");
  }

  // A held weapon must use the world's compensated room-scale view. Stereo
  // adds head translation to both draw passes; a raw simulation view would
  // leave the weapon behind by the distance already walked by the body.
  {
    for (float walk : {-1.5f, 0.f, 1.5f}) {
      for (float body : {walk, 0.f}) { // free walking and a collision-blocked step
        const float handFromHead = 0.3f;
        const float gunWorld = body + handFromHead;
        const float renderBase = body - walk;
        const auto gunInBase = Affine(1, 0, 0, gunWorld - renderBase,
                                     0, 1, 0, 0, 0, 0, 1, -0.6f);
        for (float eyeOffset : {-0.032f, 0.032f}) {
          auto weapon = uniform;
          std::memcpy(weapon.data() + layout.positionOffset, &gunInBase, sizeof(gunInBase));
          const auto trackedEye = Affine(1, 0, 0, -walk - eyeOffset,
                                        0, 1, 0, 0, 0, 0, 1, 0);
          compose_stereo_uniform(weapon.data(), layout,
                                StereoEyeCompose{&projection, &trackedEye, 1.f, 1.f, 1.f, 1.f});
          Mat3x4<float> result;
          std::memcpy(&result, weapon.data() + layout.positionOffset, sizeof(result));
          Check(Near(result.m0[3], handFromHead - eyeOffset),
                "room-scale weapon stays at the hand after walking or wall blocking");
        }
      }
    }
  }

  // Floating morph-ball HUD: full rotation and translation, with smooth lag
  // independent of headset refresh rate. Large turns take the short path.
  {
    using namespace stereo_replay;
    const auto pose = [](int axis, float angle, float x) {
      auto m = Affine(1, 0, 0, x, 0, 1, 0, 0.4f, 0, 0, 1, -0.2f);
      const int j = (axis + 1) % 3;
      const int k = (axis + 2) % 3;
      (*(&m.m0 + j))[j] = (*(&m.m0 + k))[k] = std::cos(angle);
      (*(&m.m0 + j))[k] = -std::sin(angle);
      (*(&m.m0 + k))[j] = std::sin(angle);
      return m;
    };
    for (int axis = 0; axis < 3; ++axis) {
      FloatingHud hud;
      const auto start = pose(axis, 0.f, 0.f);
      const auto target = pose(axis, 3.0f, 1.f);
      hud.Update(start, 1.f / 90.f);
      auto floating = hud.Update(target, 1.f / 90.f);
      Check(floating.m0[3] > 0.f && floating.m0[3] < 0.2f, "floating HUD eases toward head translation");
      const int j = (axis + 1) % 3;
      Check((*(&floating.m0 + j))[j] > 0.9f, "floating HUD lags yaw, pitch and roll initially");
      for (int frame = 0; frame < 180; ++frame) floating = hud.Update(target, 1.f / 90.f);
      for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
          Check(std::fabs((*(&floating.m0 + row))[column] - (*(&target.m0 + row))[column]) < 0.001f,
                "floating HUD catches up after looking left, right, up, down or behind");
        }
      }
      // The filtered plane retains the two actual eye positions.
      const auto headView = inverse_rigid(target);
      const auto leftView = compose_affine(Affine(1,0,0,0.032f, 0,1,0,0, 0,0,1,0), headView);
      const auto rightView = compose_affine(Affine(1,0,0,-0.032f, 0,1,0,0, 0,0,1,0), headView);
      const auto left = compose_affine(leftView, floating);
      const auto right = compose_affine(rightView, floating);
      Check(Near(left.m0[3] - right.m0[3], 0.064f), "floating HUD keeps stereo eye separation");
      hud.Reset();
      floating = hud.Update(start, 1.f / 90.f);
      Check(Near(floating.m0[3], start.m0[3]), "menu exit resets the floating HUD to the current head");
    }
    FloatingHud slow, fast;
    const auto start = pose(1, 0.f, 0.f);
    const auto target = pose(1, 2.f, 1.f);
    slow.Update(start, 0.f);
    fast.Update(start, 0.f);
    Mat3x4<float> a{}, b{};
    for (int frame = 0; frame < 30; ++frame) a = slow.Update(target, 1.f / 60.f);
    for (int frame = 0; frame < 60; ++frame) b = fast.Update(target, 1.f / 120.f);
    Check(Near(a.m0[3], b.m0[3]) && Near(a.m0[0], b.m0[0]), "floating HUD follows at the same speed at 60 and 120 Hz");

    {
      // The held screen (AURORA_STEREO_ROUTE_ANCHORED_SCREEN): upright, ahead of the head's facing, at its position.
      const auto headAt = [](float yaw, float pitch, float roll) {
        // R = Ry(yaw) * Rx(pitch) * Rz(roll), the head at (0.3, 1.6, -0.4); the head looks along -Z.
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        const float cr = std::cos(roll), sr = std::sin(roll);
        const float ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
        const float rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
        const float rz[3][3] = {{cr, -sr, 0}, {sr, cr, 0}, {0, 0, 1}};
        float t[3][3]{}, r[3][3]{};
        for (int i = 0; i < 3; ++i)
          for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) t[i][j] += ry[i][k] * rx[k][j];
        for (int i = 0; i < 3; ++i)
          for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[i][j] += t[i][k] * rz[k][j];
        return Affine(r[0][0], r[0][1], r[0][2], 0.3f, r[1][0], r[1][1], r[1][2], 1.6f, r[2][0], r[2][1], r[2][2], -0.4f);
      };
      const auto forwardOf = [](const Mat3x4<float>& m) {
        return std::array<float, 3>{-m.m0[2], -m.m1[2], -m.m2[2]};
      };
      for (const float yaw : {0.f, 1.2f, -2.5f, 3.1f}) {
        const auto anchor = upright_screen_anchor(headAt(yaw, 0.6f, 0.4f));
        const auto level = forwardOf(headAt(yaw, 0.f, 0.f));
        const auto f = forwardOf(anchor);
        Check(Near(f[0], level[0]) && Near(f[1], 0.f) && Near(f[2], level[2]),
              "the held screen faces the head's yaw, without its pitch");
        Check(Near(anchor.m0[1], 0.f) && Near(anchor.m1[1], 1.f) && Near(anchor.m2[1], 0.f),
              "the held screen stays upright whatever the head's roll");
        Check(Near(anchor.m0[3], 0.3f) && Near(anchor.m1[3], 1.6f) && Near(anchor.m2[3], -0.4f),
              "the held screen is placed from the head's position");
        const auto right = std::array<float, 3>{anchor.m0[0], anchor.m1[0], anchor.m2[0]};
        const auto back = std::array<float, 3>{anchor.m0[2], anchor.m1[2], anchor.m2[2]};
        Check(Near(right[0] * back[0] + right[2] * back[2], 0.f) &&
                  Near(right[0] * right[0] + right[2] * right[2], 1.f),
              "the held screen's axes stay orthonormal");
      }
      // Looking straight down or up, the head's up (or down) gives the facing.
      const auto down = upright_screen_anchor(headAt(0.9f, -1.5707963f, 0.f));
      const auto level = forwardOf(headAt(0.9f, 0.f, 0.f));
      Check(Near(forwardOf(down)[0], level[0]) && Near(forwardOf(down)[2], level[2]),
            "looking straight down, the held screen goes ahead of the body's facing");
      const auto up = upright_screen_anchor(headAt(0.9f, 1.5707963f, 0.f));
      Check(Near(forwardOf(up)[0], level[0]) && Near(forwardOf(up)[2], level[2]),
            "looking straight up, the held screen goes ahead too");
      // A point of the head's own space (the head-locked HUD's minimap) found in the held anchor's
      // space after the head turned away from where the anchor was placed (AuroraSetStereoPlacement3DFollowHead).
      const auto anchor = upright_screen_anchor(headAt(0.f, 0.f, 0.f));
      const auto turned = headAt(1.1f, 0.3f, 0.2f);
      const auto eyeFromHead = Affine(1, 0, 0, -0.032f, 0, 1, 0, 0, 0, 0, 1, 0);
      const auto eyeFromAnchor = compose_affine(compose_affine(eyeFromHead, inverse_rigid(turned)), anchor);
      const std::array<float, 3> point{0.5f, 0.3f, -2.f};
      const auto anchorFromHead = compose_affine(inverse_rigid(anchor), turned);
      const auto apply = [](const Mat3x4<float>& m, const std::array<float, 3>& p) {
        std::array<float, 3> out{};
        for (int row = 0; row < 3; ++row) {
          const auto& r = *(&m.m0 + row);
          out[row] = r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3];
        }
        return out;
      };
      const auto near3 = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::fabs(a[0] - b[0]) < 0.001f && std::fabs(a[1] - b[1]) < 0.001f && std::fabs(a[2] - b[2]) < 0.001f;
      };
      const std::array<float, 3> other{-0.4f, 0.9f, -1.3f};
      const auto all = follow_head(eyeFromAnchor, eyeFromHead, point, 1.f);
      Check(near3(apply(all, point), apply(anchorFromHead, point)) &&
                near3(apply(all, other), apply(anchorFromHead, other)),
            "followed all the way, a draw stands as the head-locked route has it");
      const auto none = follow_head(eyeFromAnchor, eyeFromHead, point, 0.f);
      Check(near3(apply(none, other), other), "not followed, a draw stays in the held anchor's space");
      const auto half = follow_head(eyeFromAnchor, eyeFromHead, point, 0.5f);
      const auto there = apply(anchorFromHead, point);
      const auto mid = apply(half, point);
      Check(near3(mid, {0.5f * (point[0] + there[0]), 0.5f * (point[1] + there[1]), 0.5f * (point[2] + there[2])}),
            "half followed, the point is half way to where the head has it");
      const auto twice = compose_affine(half, half);
      const auto fullTrace = anchorFromHead.m0[0] + anchorFromHead.m1[1] + anchorFromHead.m2[2];
      Check(std::fabs(twice.m0[0] + twice.m1[1] + twice.m2[2] - fullTrace) < 0.001f,
            "half followed, the draw has turned half of the head's turn");
    }
    const auto before = pose(1, 3.1f, 0.f);
    const auto after = pose(1, -3.1f, 0.f);
    slow.Reset();
    slow.Update(before, 0.f);
    a = slow.Update(after, 1.f / 60.f);
    Check(a.m0[0] < -0.99f, "crossing the rear heading does not spin the HUD around");
    const float cant = 0.12f;
    const auto head = pose(1, 1.f, 2.f);
    const auto leftPose = compose_affine(head, pose(1, cant, -0.032f));
    const auto rightPose = compose_affine(head, pose(1, -cant, 0.032f));
    const auto centre = center_head_pose(inverse_rigid(leftPose), inverse_rigid(rightPose));
    Check(Near(centre.m0[0], head.m0[0]) && Near(centre.m0[2], head.m0[2]),
          "canted eye displays do not bias the HUD's facing toward one eye");
    const auto expectedCentre = compose_affine(head, pose(1, 0.f, 0.f));
    Check(Near(centre.m0[3], expectedCentre.m0[3]) && Near(centre.m2[3], expectedCentre.m2[3]),
          "canted eye displays preserve the actual head centre");
  }

  // A beam-change split must classify the same point identically in both eyes,
  // including a rotated headset, IPD/translation and head-locked scaling.
  {
    auto clipped = layout;
    clipped.clipPlaneOffset = 2304;
    Check(clipped.valid(), "optional clip-plane uniform fits");
    auto invalid = clipped;
    invalid.clipPlaneOffset = invalid.size - 4;
    Check(!invalid.valid(), "clip plane past the uniform is rejected");
    const Vec4<float> plane{.6f, .8f, .3f, -.1f};
    for (float eyeX : {-.032f, .032f}) {
      const auto eyeView = Affine(0, 0, -1, eyeX, 0, 1, 0, .17f, 1, 0, 0, -.23f);
      for (float scale : {1.f, 1.4f}) {
        auto bytes = uniform;
        std::memcpy(bytes.data()+clipped.clipPlaneOffset, &plane, sizeof(plane));
        compose_stereo_uniform(bytes.data(), clipped,
                              StereoEyeCompose{&eyeProjection, &eyeView, scale, .8f, 1.f, 1.f});
        Vec4<float> eyePlane;
        std::memcpy(&eyePlane, bytes.data()+clipped.clipPlaneOffset, sizeof(eyePlane));
        for (const Vec4<float> p : {Vec4<float>{.05f,.02f,-.5f},
                                   Vec4<float>{.3f,.2f,-.2f},
                                   Vec4<float>{.1f,.05f,-.7f}}) {
          const float x=p[0]*scale, y=p[1]*scale, z=p[2]*.8f;
          const Vec4<float> q{eyeView.m0[0]*x+eyeView.m0[1]*y+eyeView.m0[2]*z+eyeView.m0[3],
                               eyeView.m1[0]*x+eyeView.m1[1]*y+eyeView.m1[2]*z+eyeView.m1[3],
                               eyeView.m2[0]*x+eyeView.m2[1]*y+eyeView.m2[2]*z+eyeView.m2[3]};
          const float expected=plane[0]*p[0]+plane[1]*p[1]+plane[2]*p[2]+plane[3];
          const float actual=eyePlane[0]*q[0]+eyePlane[1]*q[1]+eyePlane[2]*q[2]+eyePlane[3];
          Check(Near(expected,actual), "solid/hologram split distance is eye invariant");
        }
      }
    }
  }
  std::puts("port_vr_stereo_tests: ok");
  return 0;
}
