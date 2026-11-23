#pragma once

#include <aurora/math.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace aurora::gfx::stereo_replay {

// The alpha an eye re-issues a composite's source draws with (Mario Kart Wii's ghost kart).
// The game's own composite blends its copy of the ghost at about half strength; the constant
// stands in for the alpha that quad carries in its vertex colour.
constexpr float kCompositeSourceAlpha = 0.5f;

struct SubviewRect {
  float left = 0.f;
  float top = 0.f;
  float width = 0.f;
  float height = 0.f;
};

// MKW uses a top/bottom split for two screens and quadrants for three/four.
// Coordinates belong to the displayed EFB region, including its crop origin.
inline SubviewRect player_one_region(SubviewRect display, uint32_t players) noexcept {
  if (players >= 2 && players <= 4) {
    display.height *= 0.5f;
    if (players >= 3) {
      display.width *= 0.5f;
    }
  }
  return display;
}

inline bool subviews_overlap(SubviewRect a, SubviewRect b) noexcept {
  return a.width > 0.f && a.height > 0.f && b.width > 0.f && b.height > 0.f && a.left < b.left + b.width &&
         a.left + a.width > b.left && a.top < b.top + b.height && a.top + a.height > b.top;
}

inline bool subview_contains(SubviewRect outer, SubviewRect inner) noexcept {
  // GX viewport jitter and rounding can extend a pane by a fraction of a pixel.
  constexpr float tolerance = 1.f;
  return inner.width > 0.f && inner.height > 0.f && inner.left >= outer.left - tolerance &&
         inner.top >= outer.top - tolerance && inner.left + inner.width <= outer.left + outer.width + tolerance &&
         inner.top + inner.height <= outer.top + outer.height + tolerance;
}

// Shared orthographic overlays may span the display. World geometry must belong
// wholly to P1; otherwise another camera could be expanded into the same eye.
// EFB effects sample the desktop's multi-camera image, so they cannot be reused.
inline bool replay_player_one_draw(SubviewRect viewport, SubviewRect playerRegion, bool perspective,
                                   bool nativeEfbEffect) noexcept {
  return !nativeEfbEffect &&
         (perspective ? subview_contains(playerRegion, viewport) : subviews_overlap(playerRegion, viewport));
}

// Split-screen furniture is often geometry in a full-display orthographic
// viewport, not a separate viewport. MKW draws its partition as the
// partition_line layout: yoko_line (800x1) and tate_line (1x800) picture panes
// centred on the display and sampling a pattern texture. Recognize only
// rectangles/lines at the split boundaries and complete masks of other
// players' panes, whether textured or not. Full-frame fades and small HUD
// backgrounds must remain visible.
inline bool is_split_screen_furniture(SubviewRect bounds, SubviewRect display, uint32_t players) noexcept {
  if (players < 2 || players > 4 || display.width <= 0.f || display.height <= 0.f)
    return false;
  const float x = (bounds.left - display.left) / display.width;
  const float y = (bounds.top - display.top) / display.height;
  const float w = bounds.width / display.width;
  const float h = bounds.height / display.height;
  constexpr float tolerance = 0.008f; // Up to a few native EFB pixels of inset/jitter.
  const auto near = [](float a, float b) { return std::abs(a - b) <= tolerance; };
  if (h <= tolerance && w >= 0.45f && near(y + h * 0.5f, 0.5f))
    return true;
  if (players >= 3 && w <= tolerance && h >= 0.45f && near(x + w * 0.5f, 0.5f))
    return true;
  if (players == 2)
    return near(x, 0.f) && near(y, 0.5f) && near(w, 1.f) && near(h, 0.5f);
  return near(w, 0.5f) && near(h, 0.5f) &&
         ((near(x, 0.5f) && (near(y, 0.f) || near(y, 0.5f))) || (near(x, 0.f) && near(y, 0.5f)));
}

// An OpenXR eye supplies the shape of its asymmetric frustum, but the sealed
// GX draw already contains the depth mapping adjusted for that draw's GX
// viewport and Aurora's reversed-Z convention. Replacing the complete matrix
// would pair an unrelated depth range with the original pipeline compare and
// clear state, which can reject the entire eye. Replace only the four
// perspective-frustum coefficients and preserve every depth-related element.
inline Mat4x4<float> compose_projection(const Mat4x4<float>& eyeFrustum, const Mat4x4<float>& gameProjection) noexcept {
  Mat4x4<float> out = gameProjection;
  out.m0[0] = eyeFrustum.m0[0];
  out.m0[2] = eyeFrustum.m0[2];
  out.m1[1] = eyeFrustum.m1[1];
  out.m1[2] = eyeFrustum.m1[2];
  return out;
}

// Mario Kart Wii's mirror mode negates the X scale of its projection matrix and
// reverses its cull mode to match the winding that flip produces. compose_projection
// replaces that coefficient with the headset frustum's always-positive X scale, so
// the eye would draw normal winding against a reversed cull mode: every surface
// inside out.
//
// The flip has to survive, but it cannot simply be re-applied to the eye's clip
// position. The eyes are placed by the per-eye view delta, so a reflection taken
// after it mirrors each eye about its own axis and swaps the stereo pair. The
// reflection S = diag(-1, 1, 1) belongs between the delta and the game camera, i.e.
// in the anchored camera's space, which the two helpers below reach by splitting it
// in half around the delta V:
//
//   clip = (P . S) . (S . V . S) . A . p  =  P . V . S . A . p
//
// mirror_projection_x supplies (P . S), mirror_view_delta_x supplies (S . V . S), and
// S . S cancels. Keeping the reflection out of the staged position and normal
// matrices leaves lighting in the game's own unmirrored view space, which is the
// space its light positions are already expressed in.
inline bool projection_mirrors_x(const Mat4x4<float>& gameProjection) noexcept {
  return gameProjection.m0[0] < 0.0f;
}

// P . S: post-multiplying by the reflection negates the matrix's X column, which is
// every coefficient the clip position picks up from the vertex's X.
inline Mat4x4<float> mirror_projection_x(const Mat4x4<float>& projection) noexcept {
  Mat4x4<float> out = projection;
  out.m0[0] = -out.m0[0];
  out.m1[0] = -out.m1[0];
  out.m2[0] = -out.m2[0];
  out.m3[0] = -out.m3[0];
  return out;
}

// S . V . S: the mirror image of the headset's eye delta, i.e. the pose the eye
// would have if it were reflected along with the world. Conjugating by a reflection
// negates exactly the entries with one X index: the X offset (half the IPD, plus any
// head translation) and the yaw and roll terms that couple X to the other axes, while
// pitch and the Y/Z offsets are left alone. Rendering an eye from this reflected pose
// and flipping the result horizontally - which is what mirror_projection_x does - is
// what that eye should see of the mirrored world, with the stereo pair the right way
// round and head tracking still unmirrored.
inline Mat3x4<float> mirror_view_delta_x(const Mat3x4<float>& viewFromCenter) noexcept {
  Mat3x4<float> out = viewFromCenter;
  out.m0[1] = -out.m0[1];
  out.m0[2] = -out.m0[2];
  out.m0[3] = -out.m0[3];
  out.m1[0] = -out.m1[0];
  out.m2[0] = -out.m2[0];
  return out;
}

// Aurora stores the GX 3x4 matrices row-major. The vertex shader consumes
// them as vec4 * mat3x4, which is equivalent to the original column-vector
// affine transform. Applying an eye-space delta therefore composes delta *
// objectToCenter in the ordinary row-major notation used below.
inline Mat3x4<float> compose_affine(const Mat3x4<float>& viewFromCenter, const Mat3x4<float>& objectToCenter) noexcept {
  Mat3x4<float> out{};
  for (size_t row = 0; row < 3; ++row) {
    auto& dst = *(&out.m0 + row);
    const auto& view = *(&viewFromCenter.m0 + row);
    for (size_t column = 0; column < 3; ++column) {
      dst[column] = view[0] * objectToCenter.m0[column] + view[1] * objectToCenter.m1[column] +
                    view[2] * objectToCenter.m2[column];
    }
    dst[3] = view[3] + view[0] * objectToCenter.m0[3] + view[1] * objectToCenter.m1[3] + view[2] * objectToCenter.m2[3];
  }
  return out;
}

// Normals receive only the eye transform's linear part. OpenXR view deltas
// are rigid transforms, so no inverse-transpose correction is needed here.
inline Mat3x4<float> compose_normal(const Mat3x4<float>& viewFromCenter, const Mat3x4<float>& objectToCenter) noexcept {
  Mat3x4<float> out{};
  for (size_t row = 0; row < 3; ++row) {
    auto& dst = *(&out.m0 + row);
    const auto& view = *(&viewFromCenter.m0 + row);
    for (size_t column = 0; column < 3; ++column) {
      dst[column] = view[0] * objectToCenter.m0[column] + view[1] * objectToCenter.m1[column] +
                    view[2] * objectToCenter.m2[column];
    }
    dst[3] = 0.0f;
  }
  return out;
}

// The pose's rotation alone, the eye at the centre (AURORA_STEREO_ROUTE_SKY):
// what an eye sees of something infinitely far is set by where it looks, not
// by where it stands.
inline Mat3x4<float> without_translation(const Mat3x4<float>& viewFromCenter) noexcept {
  Mat3x4<float> out = viewFromCenter;
  out.m0[3] = 0.0f;
  out.m1[3] = 0.0f;
  out.m2[3] = 0.0f;
  return out;
}

// The pose with its translation scaled (AURORA_STEREO_ROUTE_VISOR, from the
// head-locked pose: the eye's offset from the head centre). A point at depth d
// then lands in the eye where the same direction at depth d / scale would with
// the whole offset, while its own depth stays d: it reads that much farther.
inline Mat3x4<float> scaled_translation(const Mat3x4<float>& viewFromCenter, float scale) noexcept {
  Mat3x4<float> out = viewFromCenter;
  out.m0[3] *= scale;
  out.m1[3] *= scale;
  out.m2[3] *= scale;
  return out;
}

// A texture matrix that projects a vertex onto the screen: the (s, t, q) rows
// a GX_TG_MTX3x4 texgen from GX_TG_POS applies to the object-space position,
// with s / q and t / q the vertex's own texture coordinates in a copy of the
// viewport (u to the right and v down, both from 0): s = (x + w) / 2,
// t = (w - y) / 2 and q = w of the clip position `projection` gives the
// view-space point `objectToView` makes of it. Only the projection's x, y and
// w rows take part, so the same sum serves the game's own projection and
// modelview (the mono draw builds it on the CPU) and an eye's composed ones
// (AuroraSetStereoScreenTexMtx): a draw that samples the viewport's EFB copy
// through it then reads what lies behind it in that eye's own copy, where
// coordinates worked out for the mono camera would point at the wrong place
// (Metroid Prime's refracting particles).
inline Mat3x4<float> screen_tex_mtx(const Mat4x4<float>& projection, const Mat3x4<float>& objectToView) noexcept {
  // s, t and q as functionals of the view-space point (x, y, z, 1).
  float rows[3][4];
  for (size_t i = 0; i < 4; ++i) {
    rows[0][i] = 0.5f * (projection.m0[i] + projection.m3[i]);
    rows[1][i] = 0.5f * (projection.m3[i] - projection.m1[i]);
    rows[2][i] = projection.m3[i];
  }
  // Then of the object-space point, through the affine transform (whose
  // implicit fourth row is (0, 0, 0, 1)).
  Mat3x4<float> out{};
  for (size_t row = 0; row < 3; ++row) {
    auto& dst = *(&out.m0 + row);
    const auto& r = rows[row];
    for (size_t column = 0; column < 4; ++column) {
      dst[column] = r[0] * objectToView.m0[column] + r[1] * objectToView.m1[column] + r[2] * objectToView.m2[column];
    }
    dst[3] += r[3];
  }
  return out;
}

// The size, along one axis, of an eye's version of a texture made from the
// mono EFB: `monoSize` texels in mono, taken from an EFB `monoReference`
// pixels across, when the eye's image is `eyeReference` pixels across. It
// keeps the mono texture's share of its source, so an effect built from an
// eye's view (an EFB copy, its palette conversion) has the eye's resolution
// rather than the desktop window's, and a copy the game asked for at half
// size stays half the eye's. Kept within 1..`maxDimension`; the mono size
// when there is no reference to scale by.
inline uint32_t eye_copy_extent(uint32_t monoSize, uint32_t monoReference, uint32_t eyeReference,
                                uint32_t maxDimension) noexcept {
  if (monoReference == 0 || eyeReference == 0) {
    return monoSize;
  }
  const double scaled =
      static_cast<double>(monoSize) * static_cast<double>(eyeReference) / static_cast<double>(monoReference);
  const double rounded = std::floor(scaled + 0.5);
  const double limit = maxDimension != 0 ? static_cast<double>(maxDimension) : rounded;
  return static_cast<uint32_t>(std::clamp(rounded, 1.0, std::max(limit, 1.0)));
}

// A fixed virtual screen for the game's 2D content, sized and placed in the
// recorded center-eye view space: a rectangle `distance` units straight ahead
// of the game camera, `halfWidth` by `halfHeight` units across. It stays where
// the camera puts it, so turning the head looks around it rather than dragging
// it along.
struct HudScreen {
  float halfWidth = 0.0f;
  float halfHeight = 0.0f;
  float distance = 0.0f;
  float offsetX = 0.0f;
  float offsetY = 0.0f;

  [[nodiscard]] bool valid() const noexcept { return halfWidth > 0.0f && halfHeight > 0.0f && distance > 0.0f; }
};

// Converts a draw's viewport-local NDC into the NDC of the complete displayed
// frame. It is identity for a full-frame viewport. Virtual-screen replay uses a
// full-eye host viewport, so this keeps sub-pane HUD elements in their original
// part of the 2D screen instead of applying their viewport twice.
struct HudNdcRemap {
  float scaleX = 1.0f;
  float scaleY = 1.0f;
  float offsetX = 0.0f;
  float offsetY = 0.0f;
};

inline HudNdcRemap make_hud_ndc_remap(float viewportLeft, float viewportTop, float viewportWidth, float viewportHeight,
                                      float frameLeft, float frameTop, float frameWidth, float frameHeight) noexcept {
  if (!(frameWidth > 0.0f) || !(frameHeight > 0.0f)) {
    return {};
  }
  return {
      .scaleX = viewportWidth / frameWidth,
      .scaleY = viewportHeight / frameHeight,
      .offsetX = (2.0f * (viewportLeft - frameLeft) + viewportWidth) / frameWidth - 1.0f,
      .offsetY = 1.0f - (2.0f * (viewportTop - frameTop) + viewportHeight) / frameHeight,
  };
}

inline Mat4x4<float> remap_hud_ndc(const Mat4x4<float>& projection, const HudNdcRemap& remap) noexcept {
  Mat4x4<float> out = projection;
  for (size_t i = 0; i < 4; ++i) {
    out.m0[i] = projection.m0[i] * remap.scaleX + projection.m3[i] * remap.offsetX;
    out.m1[i] = projection.m1[i] * remap.scaleY + projection.m3[i] * remap.offsetY;
  }
  return out;
}

// A GX orthographic projection is affine: apply_xf_projection writes exactly
// (0, 0, 0, 1) into its w row, and the renderer's depth-window flip only ever
// touches the z row. An orthographic draw's clip position is therefore already
// its NDC position, which is what compose_hud_screen_projection relies on.
inline bool is_orthographic_projection(const Mat4x4<float>& projection) noexcept {
  return projection.m3[0] == 0.0f && projection.m3[1] == 0.0f && projection.m3[2] == 0.0f && projection.m3[3] == 1.0f;
}

// The Z row that reproduces the backend NDC depth the original orthographic draw
// would have produced. The virtual-screen shader captures it before replacing
// raster depth with a stable midrange value.
//
// This is a straight pass-through of the stored Z row, and deliberately does not
// depend on the reversed-Z setting. The projection reaching here is the one staged
// into the draw's own uniform, i.e. effective_projection()'s output, which since
// the reverse-Z fix carries the near/far depth correction already applied - exactly
// once, in the matrix - and the vertex shader now adds nothing on top of it. So
// dot(v, projection.m2) IS the depth the unmodified draw would have written.
//
// It previously re-applied a correction here (negating the row under reversed Z, or
// folding m3 in under forward Z). That was correct only while the vertex shader
// still applied its own redundant per-vertex correction for this one to cancel
// against. With that per-vertex step gone, any correction here is a double
// application: it would invert the virtual screen's depth ordering, so the 2D
// layers meant to sit on top would lose the depth test to the ones behind them.
inline Vec4<float> backend_ndc_depth_row(const Mat4x4<float>& projection) noexcept { return projection.m2; }

// Replaces an orthographic draw's projection so its 2D output lands on the
// fixed virtual screen instead of being stretched across the whole eye.
//
// The GX vertex shader computes `vec4(mv_pos, 1) * proj`, reading m0..m3 as the
// x/y/z/w rows of that product, so for an orthographic draw m0 and m1 already
// yield the game's NDC x/y and m2 its NDC depth. This composes three more steps
// into the same matrix:
//
//   1. NDC to a point on the screen rectangle in the recorded center-eye view
//      space: (ndc.x * halfWidth, ndc.y * halfHeight, -distance).
//   2. That space into this eye's view space, through viewFromCenter.
//   3. Eye view space into clip space, through the OpenXR frustum's four terms.
//
// Each step is affine in the vertex position, so the whole chain collapses into
// one projection matrix and the draw's own position matrices stay untouched.
//
// The composed Z row carries the original flat-screen NDC depth. The exact-depth
// vertex variant captures it, then parks clip depth in the middle of the volume
// for stable rasterization; the fragment variant exports the captured value.
// Keeping original depth out of the VR perspective divide is what makes
// equal-depth 2D layers deterministic under head rotation and translation.
inline Mat4x4<float> compose_hud_screen_projection(const Mat4x4<float>& eyeFrustum, const Mat3x4<float>& viewFromCenter,
                                                   const HudScreen& screen, const Mat4x4<float>& gameProjection,
                                                   const HudNdcRemap& ndcRemap = {}) noexcept {
  const Mat4x4<float> frameProjection = remap_hud_ndc(gameProjection, ndcRemap);
  // The screen point's three coordinates, each as a functional of (mv_pos, 1).
  Mat3x4<float> screenPoint{};
  for (size_t i = 0; i < 4; ++i) {
    screenPoint.m0[i] = frameProjection.m0[i] * screen.halfWidth;
    screenPoint.m1[i] = frameProjection.m1[i] * screen.halfHeight;
    screenPoint.m2[i] = 0.0f;
  }
  screenPoint.m0[3] += screen.offsetX;
  screenPoint.m1[3] += screen.offsetY;
  screenPoint.m2[3] = -screen.distance;

  // The same functionals carried into eye view space. viewFromCenter's own
  // translation column joins the constant term, the one place the implicit 1 of
  // the homogeneous screen point contributes.
  Mat3x4<float> eyePoint{};
  for (size_t row = 0; row < 3; ++row) {
    auto& dst = *(&eyePoint.m0 + row);
    const auto& view = *(&viewFromCenter.m0 + row);
    for (size_t i = 0; i < 4; ++i) {
      dst[i] = view[0] * screenPoint.m0[i] + view[1] * screenPoint.m1[i] + view[2] * screenPoint.m2[i];
    }
    dst[3] += view[3];
  }

  const Vec4<float> exactDepthRow = backend_ndc_depth_row(gameProjection);
  Mat4x4<float> out{};
  for (size_t i = 0; i < 4; ++i) {
    out.m0[i] = eyeFrustum.m0[0] * eyePoint.m0[i] + eyeFrustum.m0[2] * eyePoint.m2[i];
    out.m1[i] = eyeFrustum.m1[1] * eyePoint.m1[i] + eyeFrustum.m1[2] * eyePoint.m2[i];
    out.m3[i] = -eyePoint.m2[i];
    // The exact-depth shader captures this original flat-screen value before
    // parking the geometry at 0.5 for rasterization.
    out.m2[i] = exactDepthRow[i];
  }
  return out;
}

// --- the head-locked plane (AURORA_STEREO_ROUTE_HEAD_LOCKED_2D) ------------
//
// Metroid Prime's scan visor draws its window as 2D content: it copies the
// centre of the framebuffer, stretches that copy over an orthographic pane
// and frames it (CPlayerVisor::DrawScanEffect). Replayed per eye as it is,
// the pane lands identically in both eyes (at infinity, at each eye image's
// own centre rather than straight ahead) and each eye's copy is taken from
// its own image centre, which with asymmetric eye frustums is not where the
// head points. The route lays such 2D draws on a plane in front of the head
// instead, and takes each eye's copy from the part of that eye's view behind
// the same rectangle of the plane, so the window magnifies what is straight
// ahead of the head, with the eye's own view inside it.
//
// The plane maps the mono viewport like the game's HUD camera does: NDC
// (x, y) is the point (x * tanHalfWidth, y * tanHalfHeight, -1) * distance in
// the recorded (game camera) space, before the head-locked scales; the same
// scales as a head-locked perspective draw then apply, so the window and the
// HUD keep their relative size and depth whatever the settings.
struct HeadLockedPlane {
  float tanHalfWidth = 0.0f;  // tan of half the view angle the viewport spans across
  float tanHalfHeight = 0.0f; // tan of half the view angle it spans up
  float distance = 0.0f;      // along the view, in the recorded space's units
  float offsetX = 0.0f;
  float offsetY = 0.0f;

  [[nodiscard]] bool valid() const noexcept {
    return tanHalfWidth > 0.0f && tanHalfHeight > 0.0f && distance > 0.0f;
  }
};

// The plane as the screen compose_hud_screen_projection lays 2D content on,
// after the head-locked scales (stereo_frame.hpp: across the view for the
// angular size, along it for the distance).
inline HudScreen head_locked_plane_screen(const HeadLockedPlane& plane, float scaleXY, float scaleZ) noexcept {
  if (!plane.valid() || !(scaleXY > 0.0f) || !(scaleZ > 0.0f)) {
    return {};
  }
  return {
      .halfWidth = plane.tanHalfWidth * plane.distance * scaleXY,
      .halfHeight = plane.tanHalfHeight * plane.distance * scaleXY,
      .distance = plane.distance * scaleZ,
      .offsetX = plane.offsetX * scaleXY,
      .offsetY = plane.offsetY * scaleXY,
  };
}

// One eye's projection for an orthographic draw laid on the head-locked
// plane: compose_hud_screen_projection's chain through the eye's head-locked
// pose, then the depth row made fit for the ordinary GX shader. That shader
// divides every clip coordinate by w; the composed w is the same for every
// vertex (the plane faces the view and the head-locked pose is a
// translation), so scaling the exact-depth row by it hands the divide the
// depth the draw would have had on the flat screen. The draw then keeps its
// depth relation to the eye's world and to the other 2D draws just as in
// mono (the scan window's shield masks the scan icons behind it).
inline Mat4x4<float> compose_head_locked_2d_projection(const Mat4x4<float>& eyeFrustum,
                                                       const Mat3x4<float>& headLockedViewFromCenter,
                                                       const HudScreen& plane, const Mat4x4<float>& gameProjection,
                                                       const HudNdcRemap& ndcRemap = {}) noexcept {
  Mat4x4<float> out =
      compose_hud_screen_projection(eyeFrustum, headLockedViewFromCenter, plane, gameProjection, ndcRemap);
  const float w = out.m3[3];
  if (w > 0.0f) {
    for (size_t i = 0; i < 4; ++i) {
      out.m2[i] *= w;
    }
  }
  return out;
}

// Where a rectangle of the mono viewport lands in one eye's image when laid
// on the head-locked plane, as the UV transform tex_copy_conv takes: the
// eye's texture coordinates of the rectangle's left/top corner and its size.
// The rectangle is given as the NDC of its left, top, right and bottom edges
// (y up). An EFB copy made under the route is taken from each eye through
// this, so the copy holds what that eye sees behind the rectangle rather
// than the same part of each eye's image. Empty when the plane is invalid or
// behind the eye.
struct EyeUvRect {
  float u = 0.0f;
  float v = 0.0f;
  float width = 0.0f;
  float height = 0.0f;

  [[nodiscard]] bool valid() const noexcept { return width > 0.0f && height > 0.0f; }
};

inline EyeUvRect head_locked_plane_uv_rect(const Mat4x4<float>& eyeFrustum,
                                           const Mat3x4<float>& headLockedViewFromCenter, const HudScreen& plane,
                                           float ndcLeft, float ndcTop, float ndcRight, float ndcBottom) noexcept {
  if (!plane.valid()) {
    return {};
  }
  // A corner of the rectangle on the plane, into the eye's view space, then
  // through the frustum's four terms into the eye's texture coordinates.
  const auto project = [&](float nx, float ny, float& u, float& v) {
    const float p[3] = {nx * plane.halfWidth + plane.offsetX, ny * plane.halfHeight + plane.offsetY,
                        -plane.distance};
    float e[3];
    for (size_t row = 0; row < 3; ++row) {
      const auto& m = *(&headLockedViewFromCenter.m0 + row);
      e[row] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    }
    const float w = -e[2];
    if (!(w > 0.0f)) {
      return false;
    }
    const float cx = eyeFrustum.m0[0] * e[0] + eyeFrustum.m0[2] * e[2];
    const float cy = eyeFrustum.m1[1] * e[1] + eyeFrustum.m1[2] * e[2];
    u = 0.5f * (cx / w + 1.0f);
    v = 0.5f * (1.0f - cy / w);
    return true;
  };
  float u0 = 0.0f;
  float v0 = 0.0f;
  float u1 = 0.0f;
  float v1 = 0.0f;
  if (!project(ndcLeft, ndcTop, u0, v0) || !project(ndcRight, ndcBottom, u1, v1)) {
    return {};
  }
  return {.u = u0, .v = v0, .width = u1 - u0, .height = v1 - v0};
}

// --- the virtual screen (AURORA_STEREO_ROUTE_SCREEN_2D) ---------------------
//
// A 2D layout drawn for the whole picture, such as Metroid Prime's morph ball
// HUD, keeps its pieces in the corners of the mono view. Replayed per eye as
// it is, an orthographic draw covers each eye's whole image, so those pieces
// land at the edges of the lenses, out of sight. The route lays the draw's
// mono picture on the virtual screen instead (HudScreen, in the recorded game
// camera space), which keeps the layout's shape and keeps it in front of the
// player while the head looks around it.
//
// Unlike compose_hud_screen_projection the draw may be a perspective one as
// well (a HUD model drawn through its frame's camera): the screen point is
// built from the draw's mono clip coordinates, (x * halfWidth, y * halfHeight,
// -distance * w, w), which is the point for its NDC scaled by its w, so the
// chain stays linear.
//
// Depth: the eye's clip w is then the point's eye depth times the mono w, so
// the mono clip z times a constant hands the divide the draw's own NDC depth
// times that constant over the eye depth. With the screen's nearest corner's
// eye depth as the constant, the factor is at most one on the screen, so the
// depth stays in the volume (and in the draw's depth range, which the viewport
// applies), and it is the same for every draw at a point of the screen, so the
// layout keeps its own depth order. A screen that faces the eye squarely
// keeps the exact depth.
//
// `ndcRemap` places the draw's viewport in the recorded picture (identity for
// a full-picture viewport). The eye pass applies the same viewport, scaled to
// the eye, after this projection, so the eye coordinates are taken back into
// that viewport too.
inline constexpr float kScreen2DMinDepthFraction = 0.05f; // of the distance, for a corner beside or behind the eye

inline Mat4x4<float> compose_screen_2d_projection(const Mat4x4<float>& eyeFrustum, const Mat3x4<float>& viewFromCenter,
                                                  const HudScreen& screen, const Mat4x4<float>& gameProjection,
                                                  const HudNdcRemap& ndcRemap = {}) noexcept {
  const Mat4x4<float> frameProjection = remap_hud_ndc(gameProjection, ndcRemap);
  // The point's eye-space coordinates, each as a functional of (mv_pos, 1).
  Mat3x4<float> eyePoint{};
  for (size_t row = 0; row < 3; ++row) {
    const auto& view = *(&viewFromCenter.m0 + row);
    auto& dst = *(&eyePoint.m0 + row);
    for (size_t i = 0; i < 4; ++i) {
      dst[i] = view[0] * screen.halfWidth * frameProjection.m0[i] +
               view[1] * screen.halfHeight * frameProjection.m1[i] +
               (view[3] - view[2] * screen.distance) * frameProjection.m3[i];
    }
  }
  const auto& depthRow = viewFromCenter.m2;
  float nearest = 0.0f;
  bool first = true;
  for (const float x : {-screen.halfWidth, screen.halfWidth}) {
    for (const float y : {-screen.halfHeight, screen.halfHeight}) {
      const float depth = -(depthRow[0] * x + depthRow[1] * y - depthRow[2] * screen.distance + depthRow[3]);
      nearest = first ? depth : std::min(nearest, depth);
      first = false;
    }
  }
  nearest = std::max(nearest, kScreen2DMinDepthFraction * screen.distance);

  const float inverseScaleX = ndcRemap.scaleX != 0.0f ? 1.0f / ndcRemap.scaleX : 1.0f;
  const float inverseScaleY = ndcRemap.scaleY != 0.0f ? 1.0f / ndcRemap.scaleY : 1.0f;
  Mat4x4<float> out{};
  for (size_t i = 0; i < 4; ++i) {
    out.m3[i] = -eyePoint.m2[i];
    const float x = eyeFrustum.m0[0] * eyePoint.m0[i] + eyeFrustum.m0[2] * eyePoint.m2[i];
    const float y = eyeFrustum.m1[1] * eyePoint.m1[i] + eyeFrustum.m1[2] * eyePoint.m2[i];
    out.m0[i] = (x - ndcRemap.offsetX * out.m3[i]) * inverseScaleX;
    out.m1[i] = (y - ndcRemap.offsetY * out.m3[i]) * inverseScaleY;
    out.m2[i] = backend_ndc_depth_row(frameProjection)[i] * nearest;
  }
  return out;
}

// The headset settings panel (aurora_imgui_set_stereo_overlay): a rectangle
// centred on the virtual screen, in the same world units as the screen.
struct OverlayPanel {
  float halfWidth = 0.0f;
  float halfHeight = 0.0f;
  float distance = 0.0f;

  [[nodiscard]] bool valid() const noexcept { return halfWidth > 0.0f && halfHeight > 0.0f && distance > 0.0f; }
};

// `widthFraction` of a virtual screen `screenWidth` across and `screenDistance`
// ahead, its height following the panel's own aspect.
inline OverlayPanel overlay_panel_on_screen(float screenWidth, float screenDistance, float widthFraction,
                                            float panelAspect) noexcept {
  if (!(panelAspect > 0.0f)) {
    return {};
  }
  const float halfWidth = 0.5f * screenWidth * widthFraction;
  return {.halfWidth = halfWidth, .halfHeight = halfWidth / panelAspect, .distance = screenDistance};
}

// Clip position of a point on the panel for one eye of an immersive frame, as a
// matrix applied to (x, y, 0, 1) with x and y running -1..1 across the panel,
// +y up. It is the 2D layer's chain with the panel's corners in place of a
// game draw's NDC, so the panel sits exactly where the race HUD's screen does.
// The panel is drawn over the finished eye with no depth test, so its depth is
// simply parked mid-volume.
inline Mat4x4<float> compose_overlay_panel_projection(const Mat4x4<float>& eyeFrustum,
                                                      const Mat3x4<float>& viewFromCenter,
                                                      const OverlayPanel& panel) noexcept {
  Mat4x4<float> corners{};
  corners.m0 = {1.0f, 0.0f, 0.0f, 0.0f};
  corners.m1 = {0.0f, 1.0f, 0.0f, 0.0f};
  corners.m3 = {0.0f, 0.0f, 0.0f, 1.0f};
  const HudScreen screen{.halfWidth = panel.halfWidth, .halfHeight = panel.halfHeight, .distance = panel.distance};
  auto out = compose_hud_screen_projection(eyeFrustum, viewFromCenter, screen, corners);
  for (size_t i = 0; i < 4; ++i) {
    out.m2[i] = 0.5f * out.m3[i];
  }
  return out;
}

// The same panel on a virtual-screen eye image, which the runtime shows as a
// quad layer the screen's width across: a centred rectangle `widthFraction` of
// the image's width, with the panel's aspect.
inline Mat4x4<float> overlay_panel_flat_projection(float widthFraction, float panelAspect,
                                                   float imageAspect) noexcept {
  Mat4x4<float> out{};
  if (!(panelAspect > 0.0f) || !(imageAspect > 0.0f)) {
    return out;
  }
  out.m0 = {widthFraction, 0.0f, 0.0f, 0.0f};
  out.m1 = {0.0f, widthFraction * imageAspect / panelAspect, 0.0f, 0.0f};
  out.m2 = {0.0f, 0.0f, 0.0f, 0.5f};
  out.m3 = {0.0f, 0.0f, 0.0f, 1.0f};
  return out;
}

// The immersive window (AuroraStereoFrame::window): the 2D layer's screen as an
// opening each eye sees the race through, the rest of the eye left transparent.
//
// Each row, applied to (x, y, 1) for a point of the eye image at NDC (x, y),
// gives one of the homogeneous coordinates (u, v, w) of where that pixel's ray
// meets the screen's plane: the point (u / w, v / w), in units of the screen's
// half extents, so the screen covers -1..1 on both axes, lying in front of the
// eye exactly when w > 0. The rows are linear in NDC, so a full-screen triangle
// carrying their values at its corners interpolates them exactly. An eye on or
// behind the screen's plane gets all-zero rows and sees nothing through it.
struct WindowMask {
  Vec3<float> u;
  Vec3<float> v;
  Vec3<float> w;

  [[nodiscard]] Vec3<float> at(float x, float y) const noexcept {
    return {u.x * x + u.y * y + u.z, v.x * x + v.y * y + v.z, w.x * x + w.y * y + w.z};
  }
};

// The screen is the one compose_hud_screen_projection places the 2D layer on:
// halfWidth by halfHeight, `distance` straight ahead in the recorded center-eye
// view space, reached through viewFromCenter and the eye frustum's four terms.
inline WindowMask window_mask(const Mat4x4<float>& eyeFrustum, const Mat3x4<float>& viewFromCenter,
                              const HudScreen& screen) noexcept {
  const float sx = eyeFrustum.m0[0];
  const float sy = eyeFrustum.m1[1];
  if (!screen.valid() || sx == 0.0f || sy == 0.0f) {
    return {};
  }
  // The inverse of viewFromCenter's linear part L, by its adjugate.
  const auto& r0 = viewFromCenter.m0;
  const auto& r1 = viewFromCenter.m1;
  const auto& r2 = viewFromCenter.m2;
  float inverse[3][3] = {
      {r1[1] * r2[2] - r1[2] * r2[1], r0[2] * r2[1] - r0[1] * r2[2], r0[1] * r1[2] - r0[2] * r1[1]},
      {r1[2] * r2[0] - r1[0] * r2[2], r0[0] * r2[2] - r0[2] * r2[0], r0[2] * r1[0] - r0[0] * r1[2]},
      {r1[0] * r2[1] - r1[1] * r2[0], r0[1] * r2[0] - r0[0] * r2[1], r0[0] * r1[1] - r0[1] * r1[0]},
  };
  const float determinant = r0[0] * inverse[0][0] + r0[1] * inverse[1][0] + r0[2] * inverse[2][0];
  if (determinant == 0.0f) {
    return {};
  }
  for (auto& row : inverse) {
    for (float& value : row) {
      value /= determinant;
    }
  }
  // Window coordinates of an eye-space point p are q = A p + b: back into the
  // center-eye space, moved to the screen's centre and divided by its half
  // extents. The screen's plane is q.z = 0, its front facing the camera.
  const float scale[3] = {1.0f / screen.halfWidth, 1.0f / screen.halfHeight, 1.0f};
  const float t[3] = {r0[3], r1[3], r2[3]};
  float A[3][3];
  float b[3];
  for (int row = 0; row < 3; ++row) {
    float back = 0.0f;
    for (int column = 0; column < 3; ++column) {
      A[row][column] = inverse[row][column] * scale[row];
      back += inverse[row][column] * t[column];
    }
    b[row] = (-back + (row == 2 ? screen.distance : 0.0f)) * scale[row];
  }
  if (!(b[2] > 0.0f)) {
    return {};
  }
  // The pixel's ray is d = K (x, y, 1) with d.z = -1: the frustum maps an eye
  // point to clip x = sx * x + m0[2] * z, y = sy * y + m1[2] * z, w = -z.
  const float K[3][3] = {
      {1.0f / sx, 0.0f, eyeFrustum.m0[2] / sx},
      {0.0f, 1.0f / sy, eyeFrustum.m1[2] / sy},
      {0.0f, 0.0f, -1.0f},
  };
  // a = M (x, y, 1) is the ray in window coordinates. It meets the plane at
  // q = b + s a with s = -b.z / a.z, in front of the eye when s > 0, i.e. when
  // a.z < 0, so (u, v, w) = (b.z a.x - b.x a.z, b.z a.y - b.y a.z, -a.z).
  float M[3][3];
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      M[row][column] = A[row][0] * K[0][column] + A[row][1] * K[1][column] + A[row][2] * K[2][column];
    }
  }
  float rows[3][3];
  float largest = 0.0f;
  for (int column = 0; column < 3; ++column) {
    rows[0][column] = b[2] * M[0][column] - b[0] * M[2][column];
    rows[1][column] = b[2] * M[1][column] - b[1] * M[2][column];
    rows[2][column] = -M[2][column];
    for (const auto& row : rows) {
      largest = std::max(largest, std::abs(row[column]));
    }
  }
  if (!(largest > 0.0f)) {
    return {};
  }
  // Only the ratios matter; a positive scale keeps the values near one whatever
  // the world units are.
  const float normalize = 1.0f / largest;
  const auto out = [&](const float (&row)[3]) {
    return Vec3<float>{row[0] * normalize, row[1] * normalize, row[2] * normalize};
  };
  return {.u = out(rows[0]), .v = out(rows[1]), .w = out(rows[2])};
}

} // namespace aurora::gfx::stereo_replay
