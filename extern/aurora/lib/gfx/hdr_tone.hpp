#pragma once

// The HDR scene target (gfx::set_scene_hdr) stores exposed linear light: what the room's tone curve
// (GX_AURORA_SET_PBR_TONE) takes in. A writer whose colour is display-referred (retail TEV, a debug
// view, a display fog colour) goes back through the curve, so one tone pass after the scene draws it
// as before: the sRGB decode, then the curve's inverse. Without tone data (row 1 x <= 0) the curve is
// Remastered's static straight line at exposure 1: the decode alone.
//
// The inverse is the one bloom.cpp had where that one is exact (shoulder offset t2.z = -t2.y * t1.w and
// shoulder base t2.w = the line's end, as port_room_env.cpp writes them), in its general form
// otherwise (the shoulder from where the line ends, through its own offset t2.z), and kept finite for
// an RGBA16F target:
// slopes below MinSlope count as MinSlope, the shoulder's asymptote is approached but never divided
// by zero, and the reconstructed level is clamped to [0, HdrMax].
// Display white sits at the shoulder's asymptote, so it comes back as a large bounded level: that is
// what an opaque white write needs to post-tone as white, not a claim that blends of it match LDR's.
// Only this reconstruction is capped; genuine HDR writers (VFX's exposed tail) keep their values.
//
// The WGSL is appended to a shader only when it draws into the HDR scene, so LDR sources keep their
// bytes. The C++ twins below are the reference the tests hold the WGSL to.

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace aurora::gfx::hdr {

inline constexpr std::string_view ToneWgsl = R"""(
// The room's tone curve, forwards, for one channel (t: its three rows).
fn hdr_tone_fwd(x: f32, t0: vec4f, t1: vec4f, t2: vec4f) -> f32 {
  if (x < t1.z) {
    return ((t0.x * x + t0.y) * x + t0.z) * x;
  }
  if (x < t1.w) {
    return t1.x * x + t1.y;
  }
  let st = max(t2.y * x + t2.z, 0.0);
  return t2.x * st / (1.0 + st) + t2.w;
}

const hdr_min_slope = 1e-6;
const hdr_f16_max = 65504.0;

// And back: the exposed level that drew as y (linear display), nonnegative and finite in f32.
fn hdr_untone(y: f32, t0: vec4f, t1: vec4f, t2: vec4f) -> f32 {
  let mid = max(t1.z, 0.0);
  let line_start = t1.x * mid + t1.y;
  if (y < line_start) {
    var x = y / max(line_start, 1e-4) * mid;
    for (var n = 0; n < 4; n++) {
      let slope = (3.0 * t0.x * x + 2.0 * t0.y) * x + t0.z;
      x = clamp(x - (hdr_tone_fwd(x, t0, t1, t2) - y) / max(slope, 1e-4), 0.0, mid);
    }
    return x;
  }
  if (y < t1.x * t1.w + t1.y || t2.y <= hdr_min_slope) {
    return max((y - t1.y) / max(t1.x, hdr_min_slope), 0.0);
  }
  let u = clamp((y - t2.w) / max(t2.x, hdr_min_slope), 0.0, 1.0);
  let st = u / max(1.0 - u, hdr_min_slope);
  return max((st - t2.z) / t2.y, 0.0);
}

fn hdr_srgb_dec(c: vec3f) -> vec3f {
  let e = clamp(c, vec3f(0.0), vec3f(1.0));
  return select(pow((e + 0.055) / 1.055, vec3f(2.4)), e / 12.92, e <= vec3f(0.04045));
}

// A display-encoded colour as the exposed linear light the HDR scene holds.
fn hdr_exposed(c: vec3f, t0: vec4f, t1: vec4f, t2: vec4f) -> vec3f {
  let y = hdr_srgb_dec(c);
  if (t1.x <= 0.0) {
    return y;
  }
  let x = vec3f(hdr_untone(y.r, t0, t1, t2), hdr_untone(y.g, t0, t1, t2), hdr_untone(y.b, t0, t1, t2));
  return clamp(x, vec3f(0.0), vec3f(hdr_f16_max));
}
)""";

// Legacy GX SRCALPHA/ONE and SRCALPHA/INVSRCALPHA (retail particles' and glass's alpha blend): the
// source term is a * c in both, so weight it in display space BEFORE the nonlinear inverse. The plain
// inverse sent an alpha-blended white to the shoulder's asymptote, and a * that stayed in the hundreds
// (Elite Quarters' tank glass drew flat saturated blue). ONE/INVSRCALPHA needs no conversion: its source
// is premultiplied, so the plain inverse already gives the source term exactly.
// With local a = clamp(alpha,0,1), post_tone(a * H_add(c,alpha)) equals clamp(c,0,1) * a,
// within inverse/target precision. Hardware multiplies by the preserved TEV alpha once, so
// this over-black display identity assumes normalized alpha in [0,1] and an isolated,
// unfogged contribution: TEV alpha can slightly exceed 1, and later volumetric fog attenuates light.
// Over a nonblack background an alpha-over layer draws darker than LDR's display blend (mid grey at
// alpha 0.5 over itself: about 0.43 for 0.5), where the plain inverse drew it brighter over dark ones.
// Other legacy SRCALPHA/ONE effects intentionally share this policy; this is not full visual
// certification, exact LDR compositing over nonblack backgrounds (the curve is nonlinear),
// or Remastered parity.
// Separate from ToneWgsl so PBR and native HDR writers keep their existing shader source.
inline constexpr std::string_view TevBlendWgsl = R"""(
fn hdr_tev_srcalpha_add(c: vec3f, alpha: f32, t0: vec4f, t1: vec4f, t2: vec4f) -> vec3f {
  let a = clamp(alpha, 0.0, 1.0);
  if (a <= 0.0) {
    return vec3f(0.0);
  }
  return hdr_exposed(clamp(c, vec3f(0.0), vec3f(1.0)) * a, t0, t1, t2) / a;
}

// Legacy GX blends where the source colour scales the destination (hdr_tev_blend.y): 1 is
// dst * src (ZERO/SRCCLR, DSTCLR/ZERO), 2 is dst * (1 + src) (DSTCLR/ONE). The source is a factor,
// not light: a display factor k on the encoded picture is about k^2.2 on its linear light, and the
// exposed scene is that light scaled, so the factor goes over linear, not through the inverse curve
// (which made it depend on the room's exposure and takes white to the shoulder's asymptote).
fn hdr_tev_dst_factor(c: vec3f, mode: f32) -> vec3f {
  let k = clamp(c, vec3f(0.0), vec3f(1.0));
  if (mode > 1.5) {
    return pow(vec3f(1.0) + k, vec3f(2.2)) - vec3f(1.0);
  }
  return hdr_srgb_dec(k);
}
)""";

// The single post-tone (post_tone below) for a shader that turns exposed scene light into display
// colour, as an EFB copy of the HDR scene does. Needs ToneWgsl before it. A separate string so the
// HDR writers that only need ToneWgsl keep their bytes.
inline constexpr std::string_view PostToneWgsl = R"""(
fn hdr_srgb_enc(l: vec3f) -> vec3f {
  let c = clamp(l, vec3f(0.0), vec3f(1.0));
  return select(1.055 * pow(c, vec3f(1.0 / 2.4)) - 0.055, 12.92 * c, c <= vec3f(0.0031308));
}

// Exposed scene light as linear display light in [0, 1]: the curve, or without tone data its static
// line at exposure 1 (the identity, clamped to 1). The input is held to the finite RGBA16F range first,
// signed: Remastered's tonemap (matglsl/00718a4) does not clamp its input, so a negative level (X-ray
// pass A's sky) takes the toe cubic and only the output is clamped (kb topic/xray-post-shader-00089f0.md).
fn hdr_display_linear(x: vec3f, t0: vec4f, t1: vec4f, t2: vec4f) -> vec3f {
  let v = clamp(x, vec3f(-hdr_f16_max), vec3f(hdr_f16_max));
  if (t1.x <= 0.0) {
    return clamp(v, vec3f(0.0), vec3f(1.0));
  }
  let y = vec3f(hdr_tone_fwd(v.r, t0, t1, t2), hdr_tone_fwd(v.g, t0, t1, t2), hdr_tone_fwd(v.b, t0, t1, t2));
  return clamp(y, vec3f(0.0), vec3f(1.0));
}

fn hdr_post_tone(x: vec3f, t0: vec4f, t1: vec4f, t2: vec4f) -> vec3f {
  return hdr_srgb_enc(hdr_display_linear(x, t0, t1, t2));
}
)""";

using Tone = std::array<std::array<float, 4>, 3>;

inline constexpr float MinSlope = 1e-6f;
inline constexpr float HdrMax = 65504.f; // the largest finite RGBA16F value

inline float tone_fwd(float x, const Tone& t) noexcept {
  if (x < t[1][2]) {
    return ((t[0][0] * x + t[0][1]) * x + t[0][2]) * x;
  }
  if (x < t[1][3]) {
    return t[1][0] * x + t[1][1];
  }
  const float st = std::max(t[2][1] * x + t[2][2], 0.f);
  return t[2][0] * st / (1.f + st) + t[2][3];
}

inline float untone(float y, const Tone& t) noexcept {
  const float mid = std::max(t[1][2], 0.f);
  const float lineStart = t[1][0] * mid + t[1][1];
  if (y < lineStart) {
    float x = y / std::max(lineStart, 1e-4f) * mid;
    for (int n = 0; n < 4; ++n) {
      const float slope = (3.f * t[0][0] * x + 2.f * t[0][1]) * x + t[0][2];
      x = std::clamp(x - (tone_fwd(x, t) - y) / std::max(slope, 1e-4f), 0.f, mid);
    }
    return x;
  }
  if (y < t[1][0] * t[1][3] + t[1][1] || t[2][1] <= MinSlope) {
    return std::max((y - t[1][1]) / std::max(t[1][0], MinSlope), 0.f);
  }
  const float u = std::clamp((y - t[2][3]) / std::max(t[2][0], MinSlope), 0.f, 1.f);
  const float st = u / std::max(1.f - u, MinSlope);
  return std::max((st - t[2][2]) / t[2][1], 0.f);
}

inline float srgb_dec(float c) noexcept {
  const float e = std::clamp(c, 0.f, 1.f);
  return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f);
}

inline float srgb_enc(float l) noexcept {
  const float c = std::clamp(l, 0.f, 1.f);
  return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

inline float exposed(float c, const Tone& t) noexcept {
  const float y = srgb_dec(c);
  return t[1][0] <= 0.f ? y : std::clamp(untone(y, t), 0.f, HdrMax);
}

// CPU reference for hdr_tev_srcalpha_add; alpha itself is never rewritten by the writer.
inline float tev_srcalpha_add(float c, float alpha, const Tone& t) noexcept {
  const float a = std::clamp(alpha, 0.f, 1.f);
  if (a <= 0.f) {
    return 0.f;
  }
  return exposed(std::clamp(c, 0.f, 1.f) * a, t) / a;
}

// CPU reference for hdr_tev_dst_factor (mode 1: dst * src, 2: dst * (1 + src)).
inline float tev_dst_factor(float c, int mode) noexcept {
  const float k = std::clamp(c, 0.f, 1.f);
  return mode == 2 ? std::pow(1.f + k, 2.2f) - 1.f : srgb_dec(k);
}

// What the single post-tone after the HDR scene draws for an exposed level: the LDR PBR encode
// (tone, clamp, sRGB) applied once, there.
// Exposed light as linear display light (hdr_display_linear): the curve, or the identity line, in [0, 1].
inline float display_linear(float x, const Tone& t) noexcept {
  const float v = std::clamp(x, -HdrMax, HdrMax);
  return std::clamp(t[1][0] > 0.f ? tone_fwd(v, t) : v, 0.f, 1.f);
}

inline float post_tone(float x, const Tone& t) noexcept { return srgb_enc(display_linear(x, t)); }

} // namespace aurora::gfx::hdr
