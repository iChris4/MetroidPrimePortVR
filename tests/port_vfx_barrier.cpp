// VFX_BarrierShield's fragment maths: feature 1u << 29 of extern/aurora/lib/gfx/vfx.cpp, from the
// Parasite Queen ring barrier's f197ec38 material (FS perm 002, the one the MATI flag MFC0 selects).
// A scalar reference for the pieces build/s-queenbarrier-20261024/SHADER-IMPLEMENTATION.md names as
// checks: the breakup at CCH0.x 0, .5 and 1, the motion at time 0 and 1, the .45 sawtooth's boundary,
// the three chromatic scene samples, and the split between the mesh's vertex colour and the instance
// colour.
//
// The formulas below are a hand copy of the WGSL branch, as tests/port_vfx_opacity.cpp keeps one for
// the fade: an edit on either side must keep the two equal. The shader's arithmetic is f32 over the
// folded literals of f197-002-lits.txt — c1[0] = (-.45, .008, -.0225, .3), c1[1] = (.03, -.1, .33,
// .66), c1[2] = (.05, .1, .5, 2.8), c1[3] = (5.95, 0, 0, 0) — so .45f is c1[0].x, .008f is c1[0].y,
// 2.22222233f is 1 / .45, .33f is c1[1].z and 5.95000029f is c1[3].x. A multiply and an add may fuse
// in either, so comparisons that are "zero" use kTiny.

#include "aurora/vfx.hpp"
#include "port_test.h"

#include <cmath>
#include <cstdio>

namespace {

using aurora::gfx::vfx::Feature;

constexpr float kTiny = 1e-6f;
constexpr float kSawPeriod = 0.45f;      // c1[0].x, negated in the shader's fma
constexpr float kSawScale = 2.22222233f; // 1 / .45, the shader's folded reciprocal
constexpr float kStep = 0.00800000038f;  // c1[0].y

float Sat(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

// The motion's sawtooth: w - floor(w / .45) x .45. At the boundary the product is exact (0.45f x
// 2.22222233f is 1 in f32), so w = .45 wraps to 0; 3 x .45 leaves a sub-ulp residual instead.
float Sawtooth(float w) { return w - std::floor(w * kSawScale) * kSawPeriod; }

float Close(float a, float b) { return std::fabs(a - b) <= kTiny * (1.f + std::fabs(b)); }

// The pattern's scroll offsets: 2 x (pattern.rg - .5) + time, sawtoothed, x .008 (no near/depth scale).
struct Motion {
  float x, y;
};
Motion MotionOf(float patternR, float patternG, float time) {
  return {Sawtooth(2.f * (patternR - 0.5f) + time) * kStep, Sawtooth(2.f * (patternG - 0.5f) + time) * kStep};
}

// The three scene samples' offsets from the fragment's screen uv: +0, -.002 and +.002 on u, and the
// same on v negated (the copy's v runs down).
struct SampleOffsets {
  float u[3], v[3];
};
SampleOffsets OffsetsOf(const Motion& m) {
  return {{m.x, m.x - 0.002f, m.x + 0.002f}, {-m.y, 0.002f - m.y, -0.002f - m.y}};
}

// The scene colour the three reads compose: R = third.r x 1.165, G = first.r + .33 (1.5 first.g -
// first.r), B = second.r + .33 (1.5 second.b - second.r).
float SceneR(float thirdR) { return thirdR + 0.33f * (1.5f * thirdR - thirdR); }
float SceneG(const float first[4]) { return first[0] + 0.33f * (1.5f * first[1] - first[0]); }
float SceneB(const float second[4]) { return second[0] + 0.33f * (1.5f * second[2] - second[0]); }

// The breakup mask, the fade it reveals and the alpha the shader writes. x carries the fade and the
// mesh gain (the tail multiplies the instance alpha in), so WithInstance below is the fragment's alpha.
struct Breakup {
  float mask, edge, fade, gain, alpha;
  bool discarded;
};
Breakup BreakupOf(float patternA, float noise0, float noise1, float cch0x, float cch0y, float meshG) {
  const float mask = patternA * Sat(noise1 - 0.1f) * Sat(noise0 - 0.1f);
  const float edge = Sat(mask * 10.f);
  const float fade = Sat(edge + 1.f - 2.f * cch0x);
  const bool discarded = fade < 0.33f;
  return {mask, edge, fade, 10.f - 9.f * fade, fade * (1.f + meshG * (cch0y - 1.f)) * 0.75f, discarded};
}

// 1 - dot(normalize(view position), -normalize(view normal)): the fragment's f, from the Remastered
// view-space vectors the port feeds as (x, y, -z) of its camera-local ones.
float Fresnel(float vpx, float vpy, float vpz, float nx, float ny, float nz) {
  const float vl = std::sqrt(vpx * vpx + vpy * vpy + vpz * vpz);
  const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
  return 1.f + (vpx * nx + vpy * ny + vpz * nz) / (vl * nl);
}

// The colour before the tail: x instance.rgb x PCMD (PCMD 1 here).
struct Colour {
  float r, g, b;
};
Colour ColourOf(const Breakup& t, float f, float sceneR, float sceneG, float sceneB, const float mesh[4]) {
  const float rim = t.edge * std::fmin(std::fmax(f * f, 0.05f), 0.66f);
  const float high = std::pow(std::fabs(f), 8.f);
  const float rg = mesh[0] + mesh[1];
  return {t.gain * sceneR,
          t.gain * (sceneG + 40.f * rim * (t.edge * (2.f * t.mask - 0.1f) + 0.1f) + 10.5f * rg + 2.8f * mesh[2] +
                    3.f * high) +
              0.5f,
          t.gain * (sceneB + 40.f * rim * (t.edge * (10.f * t.mask - 0.5f) + 0.5f) + 28.f * rg +
                    5.95000029f * mesh[2] + 12.f * high) +
              0.9f};
}

// The fragment's alpha: x times the instance alpha (the tail's x * vc.w), saturated.
float WithInstance(float alpha, float instanceA) { return Sat(alpha * instanceA); }

// The mesh colour, the pattern texel and the noise reads the tests share.
constexpr float kMesh[4] = {0.2f, 0.4f, 0.6f, 1.f};

// The mask at a.a = .2 and both noise reads .6 is .2 x .25 x .25 = .05, so edge is .5 and the fade is
// sat(1.5 - 2 x CCH0.x): 1, .5 and 0 at CCH0.x = 0, .5 and 1.
void TestBreakupReveal() {
  const Breakup intact = BreakupOf(0.2f, 0.6f, 0.6f, 0.f, 1.f, 0.f);
  const Breakup half = BreakupOf(0.2f, 0.6f, 0.6f, 0.5f, 1.f, 0.f);
  const Breakup gone = BreakupOf(0.2f, 0.6f, 0.6f, 1.f, 1.f, 0.f);
  CHECK_MSG(Close(intact.mask, 0.05f) && Close(intact.edge, 0.5f), "the mask is a.a x sat(n1 - .1) x sat(n0 - .1), x10");
  CHECK_MSG(Close(intact.fade, 1.f) && Close(intact.gain, 1.f) && !intact.discarded,
            "CCH0.x 0: nothing broken up, gain 1");
  CHECK_MSG(Close(half.fade, 0.5f) && Close(half.gain, 5.5f) && !half.discarded,
            "CCH0.x .5: half broken up, gain 5.5");
  CHECK_MSG(Close(gone.fade, 0.f) && gone.discarded, "CCH0.x 1: the fade is under .33 and the fragment is discarded");
  // The .33 boundary is the shader's discard: a fade of .3302 stays, .3298 goes.
  CHECK_MSG(BreakupOf(0.2f, 0.6f, 0.6f, 0.5849f, 1.f, 0.f).discarded == false,
            "a fade just over .33 is kept");
  CHECK_MSG(BreakupOf(0.2f, 0.6f, 0.6f, 0.5851f, 1.f, 0.f).discarded,
            "the fade reaches the .33 discard");
  // No noise over the threshold means no mask, and then nothing breaks up whatever CCH0.x says.
  const Breakup noNoise = BreakupOf(0.2f, 0.1f, 0.1f, 0.f, 1.f, 0.f);
  CHECK_MSG(noNoise.mask == 0.f && Close(noNoise.fade, 1.f), "a mask of 0 leaves the fade at 1");
  // The alpha of the intact pattern at CCH0.y 1 is the fade x .75 alone.
  CHECK_MSG(Close(intact.alpha, 0.75f) && Close(half.alpha, 0.375f),
            "alpha = fade x (1 + mesh.g (CCH0.y - 1)) x .75: .75 and .375 at CCH0.y 1");
  CHECK_MSG(Close(WithInstance(intact.alpha, 0.5f), 0.375f), "the instance alpha multiplies in");
}

// Time 0 and time 1 on the two pattern lanes, including a negative sawtooth input.
void TestMotion() {
  const Motion still = MotionOf(0.5f, 0.5f, 0.f);
  CHECK_MSG(still.x == 0.f && still.y == 0.f, "time 0 at pattern .5: no offset");
  const Motion one = MotionOf(0.5f, 0.5f, 1.f);
  CHECK_MSG(Close(one.x, 0.00080000002f) && Close(one.y, 0.00080000002f),
            "time 1: w = 1 wraps to .1, x .008 = .0008");
  const Motion lanes = MotionOf(0.6f, 0.4f, 0.f);
  CHECK_MSG(Close(lanes.x, 0.0016f), "pattern .6: w = .2, x .008 = .0016");
  CHECK_MSG(Close(lanes.y, 0.002f), "pattern .4: w = -.2 wraps to .25, x .008 = .002");
}

// The sawtooth's period and its behaviour at the boundary: .45f x 2.22222233f is exactly 1 in f32.
void TestSawtoothBoundary() {
  CHECK_MSG(Sawtooth(0.45f) == 0.f && Sawtooth(0.9f) == 0.f, "the period's own multiples wrap to 0");
  CHECK_MSG(Sawtooth(0.4499999f) > 0.44f, "just under the period does not wrap");
  CHECK_MSG(Close(Sawtooth(1.f), 0.1f) && Close(Sawtooth(2.f), 0.2f), "w = 1 and 2 keep w mod .45");
  CHECK_MSG(std::fabs(Sawtooth(1.35f)) <= kTiny, "w = 1.35 is the period x 3 (a sub-ulp residual)");
  CHECK_MSG(Close(Sawtooth(0.1f), 0.1f) && Close(Sawtooth(-0.2f), 0.25f), "inside a period, and from below zero");
}

// The three scene samples: +0, -.002 and +.002 on u, negated on v, and the colour they compose.
void TestChromaticSamples() {
  const SampleOffsets o = OffsetsOf({0.01f, 0.02f});
  CHECK_MSG(Close(o.u[0], 0.01f) && Close(o.u[1], 0.008f) && Close(o.u[2], 0.012f),
            "the u samples are +0, -.002 and +.002");
  CHECK_MSG(Close(o.v[0], -0.02f) && Close(o.v[1], -0.018f) && Close(o.v[2], -0.022f),
            "the v samples negate, +.002 and -.002");
  const float first[4] = {0.2f, 0.4f, 0.f, 1.f};
  const float second[4] = {0.1f, 0.f, 0.3f, 1.f};
  CHECK_MSG(Close(SceneR(0.5f), 0.5825f), "R is the third sample's .r x 1.165");
  CHECK_MSG(Close(SceneG(first), 0.332f), "G is the first sample's .r toward 1.5 x its .g");
  CHECK_MSG(Close(SceneB(second), 0.2155f), "B is the second sample's .r toward 1.5 x its .b");
}

// The mesh's vertex colour and the instance colour are separate inputs: only the instance colour
// scales the result, only the mesh colour reaches the alpha gain and the glow's rg/mesh.b terms, and
// the base (gain x scene) term has neither of them.
void TestMeshColourSeparate() {
  const Breakup t = BreakupOf(0.2f, 0.6f, 0.6f, 0.f, 1.f, kMesh[1]);
  const Colour lit = ColourOf(t, 1.f, 0.5f, 0.4f, 0.3f, kMesh);
  const float noMesh[4] = {0.f, 0.f, 0.f, 1.f};
  const Colour plain = ColourOf(t, 1.f, 0.5f, 0.4f, 0.3f, noMesh);
  CHECK_MSG(Close(lit.r, plain.r) && Close(lit.r, t.gain * 0.5f),
            "the base term is gain x scene and does not see the mesh colour");
  const float rg = kMesh[0] + kMesh[1];
  CHECK_MSG(Close(lit.g - plain.g, t.gain * (10.5f * rg + 2.8f * kMesh[2])),
            "the mesh colour's r + g and its b add into G");
  CHECK_MSG(Close(lit.b - plain.b, t.gain * (28.f * rg + 5.95000029f * kMesh[2])),
            "the mesh colour's r + g and its b add into B, at their own weights");
  // Only the fade and the mesh colour reach the alpha; the instance colour is not an input to it.
  const Breakup otherMesh = BreakupOf(0.2f, 0.6f, 0.6f, 0.f, 1.f, 0.9f);
  CHECK_MSG(Close(t.alpha, otherMesh.alpha) && Close(t.alpha, 0.75f * t.fade),
            "CCH0.y 1: the mesh colour does not scale the alpha");
  const Breakup gain = BreakupOf(0.2f, 0.6f, 0.6f, 0.f, 0.f, kMesh[1]);
  CHECK_MSG(Close(gain.alpha, 0.75f * t.fade * (1.f - kMesh[1])),
            "CCH0.y 0: the alpha keeps 1 + mesh.g x -1, i.e. the mesh colour alone attenuates it");
  // The instance colour scales the whole colour and the instance alpha the alpha, independently.
  const float scale[3] = {2.f, 0.5f, 1.f};
  CHECK_MSG(Close(lit.r * scale[0], 2.f * lit.r) && Close(lit.g * scale[1], 0.5f * lit.g),
            "instance.rgb scales the colour");
  CHECK_MSG(Close(WithInstance(t.alpha, 1.f), t.alpha) && WithInstance(t.alpha, 0.f) == 0.f,
            "instance.a alone scales the alpha");
}

// The fresnel term's inputs and its two clamps.
void TestFresnel() {
  CHECK_MSG(std::fabs(Fresnel(0.f, 0.f, -2.f, 0.f, 0.f, 1.f)) <= kTiny, "a normal at the eye gives f = 0");
  CHECK_MSG(Close(Fresnel(0.f, 0.f, -2.f, 1.f, 0.f, 0.f), 1.f), "a normal across the view gives f = 1");
  CHECK_MSG(Close(Fresnel(0.f, 0.f, -2.f, 0.f, 0.f, -1.f), 2.f), "a normal pointing away gives f = 2");
  const Breakup t = BreakupOf(0.2f, 0.6f, 0.6f, 0.f, 1.f, kMesh[1]);
  const Colour facing = ColourOf(t, 0.f, 0.f, 0.f, 0.f, kMesh);
  const Colour grazing = ColourOf(t, 1.f, 0.f, 0.f, 0.f, kMesh);
  // f = 0: rim is edge x the .05 floor and high is 0; f = 1: rim is edge x the .66 ceiling and high 1.
  CHECK_MSG(Close(facing.g, t.gain * (40.f * t.edge * 0.05f * (t.edge * (2.f * t.mask - 0.1f) + 0.1f) +
                                      10.5f * (kMesh[0] + kMesh[1]) + 2.8f * kMesh[2]) +
                          0.5f),
            "f = 0 leaves the rim floor's glow and no high term");
  CHECK_MSG(grazing.g > facing.g + 2.f, "f = 1 raises the rim to its ceiling and adds the high term");
}

// ---- VFX_BarrierGlow (b1eb2b67, FS/VS perm 000: MATI ba0acbd6 carries neither MFC0 nor MFC1;
// GLOW-IMPLEMENTATION.md) ----
//
// The VS's literals are the folded pool at 0xe00 of b1eb's MTRL (b1eb-vs.txt), -.330000013 and
// .0649999976, not a guessed +-1 clamp; the second displacement's weight is .0350000001. The vertex
// displacement runs in the port's mesh feed (CPortVfxMeshBatch::Add -> AddBarrierGlow) in the model
// coordinates, before the instance transform.
constexpr float kAmpBase = 0.0649999976f;  // c1[0].y
constexpr float kFloor = -0.33f;           // c1[0].x, the max()'s floor
constexpr float kLiftBase = 0.0350000001f;
// MSVC has no M_PI (AGENTS.md); pi/40 is a quarter of the 20-phase sine's period.
constexpr float kPi = 3.14159265358979323846f;

// The displacement: along the model normal by amp, and by lift on the port's model Z, which is
// Remastered's +Y (the mesh data is R2G). phase = time - 10 x mesh.b; amp = .065 (1 + 3 mesh.r)
// max(sin 20 phase, -.33); lift = .035 max(sin 20 (phase + 114), -.33).
struct Displaced {
  float x, y, z;
};
Displaced DisplacedOf(const float pos[3], const float nrm[3], const float mesh[4], float time) {
  const float phase = time - 10.f * mesh[2];
  const float amp = kAmpBase * (1.f + 3.f * mesh[0]) * std::fmax(std::sin(20.f * phase), kFloor);
  const float lift = kLiftBase * std::fmax(std::sin(20.f * (phase + 114.f)), kFloor);
  return {pos[0] + nrm[0] * amp, pos[1] + nrm[1] * amp, pos[2] + nrm[2] * amp + lift};
}

// The amplitude alone (the normal term), for the clamp's own checks.
float AmpOf(const float mesh[4], float time) {
  const float phase = time - 10.f * mesh[2];
  return kAmpBase * (1.f + 3.f * mesh[0]) * std::fmax(std::sin(20.f * phase), kFloor);
}

// The literal values: the extracted -.33 floor and .065 base, not the +/-1 the lits dumper's garbage
// would suggest, and a .035 push on the other axis.
void TestGlowConstants() {
  CHECK_MSG(Close(kFloor, -0.33f) && std::fabs(kFloor) < 0.5f, "the clamp is -.33, not -1");
  CHECK_MSG(Close(kAmpBase, 0.065f) && std::fabs(kAmpBase - 1.f) > 0.5f, "the amplitude base is .065, not 1");
  CHECK_MSG(Close(kLiftBase, 0.035f), "the vertical push is .035");
  CHECK_MSG(uint32_t(Feature::BarrierGlow) == (1u << 30), "BarrierGlow is bit 30");
  CHECK_MSG(uint32_t(Feature::BarrierGlow) != uint32_t(Feature::BarrierShield),
            "the two barrier perms have their own bits");
}

void TestGlowDisplacement() {
  const float pos[3] = {1.f, 2.f, 3.f};
  const float up[3] = {0.f, 0.f, 1.f};
  const float none[3] = {0.f, 0.f, 0.f};
  const float mesh0[4] = {0.f, 0.f, 0.f, 1.f};
  // time 0 at mesh.b 0: phase 0, sin 0, and the lift's sin(2280) is not zero, so test the amplitude
  // term on its own and the whole displacement at a phase both terms are known at.
  CHECK_MSG(AmpOf(mesh0, 0.f) == 0.f, "phase 0: the normal term is 0");
  // sin(20 phase) = 1 at phase = pi/40: the amplitude is the base alone at mesh.r 0.
  CHECK_MSG(Close(AmpOf(mesh0, kPi / 40.f), kAmpBase), "an amplitude of .065, not 1");
  const float meshThird[4] = {1.f / 3.f, 0.f, 0.f, 1.f};
  CHECK_MSG(Close(AmpOf(meshThird, kPi / 40.f), 2.f * kAmpBase), "mesh.r 1/3 doubles 1 + 3 mesh.r");
  const float meshOne[4] = {1.f, 0.f, 0.f, 1.f};
  CHECK_MSG(Close(AmpOf(meshOne, kPi / 40.f), 4.f * kAmpBase), "mesh.r 1: 1 + 3 x 1 = 4");
  // sin(20 phase) = -1 is clamped at -.33: phase = -pi/40.
  CHECK_MSG(Close(AmpOf(meshOne, -kPi / 40.f), 4.f * kAmpBase * kFloor),
            "a negative lobe clamps at -.33, not -1");
  // The mesh colour's b, not the instance's, is the phase's offset: b 1 shifts time by 10.
  const float meshB[4] = {0.f, 0.f, 1.f, 1.f};
  CHECK_MSG(Close(AmpOf(meshB, 10.f + kPi / 40.f), kAmpBase), "phase = time - 10 mesh.b");
  // Its g is not an input.
  const float meshG[4] = {0.f, 0.8f, 0.f, 1.f};
  CHECK_MSG(AmpOf(meshG, 1.234f) == AmpOf(mesh0, 1.234f), "mesh.g does not move the amplitude");
  // The lift lands on the port's model Z (Remastered +Y, R2G), not on port model Y: with no normal,
  // a phase of pi/40 - 114 makes the lift .035 exactly.
  const float liftTime = kPi / 40.f - 114.f;
  const Displaced lift = DisplacedOf(pos, none, mesh0, liftTime);
  CHECK_MSG(Close(lift.x, pos[0]) && Close(lift.y, pos[1]), "the lift does not move model x or y");
  CHECK_MSG(Close(lift.z, pos[2] + kLiftBase), "the lift is .035 on the port's model z");
  // ... and its lobe clamps at -.33 too: phase + 114 = -pi/40.
  const Displaced down = DisplacedOf(pos, none, mesh0, -kPi / 40.f - 114.f);
  CHECK_MSG(Close(down.z, pos[2] + kLiftBase * kFloor), "the lift's negative lobe clamps at -.33");
  // Along the normal: phase pi/40, mesh.r 0 gives .065 on the unit normal's own axis.
  const Displaced along = DisplacedOf(pos, up, mesh0, kPi / 40.f);
  CHECK_MSG(Close(along.x, pos[0]) && Close(along.y, pos[1]), "a normal-only displacement keeps x and y");
}

// The glow's colour maths: k from the pattern's .a times the mesh's ceil(r) gain, the fade/gain/rim,
// and the channel terms. Only the instance colour scales the result (the caller's vc.rgb x PCMD).
struct Glow {
  float k, fade, rim, gain;
  Colour rgb;
};
Glow GlowOf(float patternB, float patternD, float phase, float cch0x, float f, const float mesh[4]) {
  const float k = patternD * (std::ceil(mesh[0]) * 0.200000018f + 0.100000001f);
  const float fade = Sat(patternD + 1.f - 2.f * cch0x);
  const float rim = Sat(f * f) * fade;
  const float gain = 10.f - 9.f * fade;
  const float p = Sat(patternB * (1.f - 0.5f * phase));
  return {k, fade, rim, gain,
          {rim * gain * (k * 1.125f),
           rim * gain * (k * 7.5f - 0.5f * mesh[0] + 1.4f * p + 0.5f),
           rim * gain * (k * 30.f - 0.899999976f * mesh[0] + 3.f * p + 0.9f)}};
}

void TestGlowBreakupAndMeshColour() {
  const float mesh[4] = {0.2f, 0.f, 0.f, 1.f};
  const Glow full = GlowOf(0.3f, 1.f, 0.f, 0.f, 1.f, mesh);
  CHECK_MSG(Close(full.fade, 1.f) && Close(full.gain, 1.f), "CCH0.x 0: no breakup, gain 1");
  CHECK_MSG(Close(full.rim, 1.f), "a facing normal (f = 1) and a full fade leave the rim at 1");
  const Glow half = GlowOf(0.3f, 1.f, 0.f, 0.75f, 1.f, mesh);
  CHECK_MSG(Close(half.fade, 0.5f) && Close(half.gain, 5.5f) && Close(half.rim, 0.5f),
            "CCH0.x .75 (fade = d + 1 - 2 CCH0.x): half faded, gain 5.5");
  const Glow gone = GlowOf(0.3f, 1.f, 0.f, 1.f, 1.f, mesh);
  CHECK_MSG(gone.fade == 0.f && Close(gone.gain, 10.f) && gone.rim == 0.f,
            "CCH0.x 1: faded out, the gain floors at 10 (the pattern .a alone, no discard in this perm)");
  // k = d x (ceil(mesh.r) x .2 + .1): the mesh colour's r is used raw through ceil.
  const float meshZero[4] = {0.f, 0.f, 0.f, 1.f};
  const float meshJust[4] = {0.2f, 0.f, 0.f, 1.f};
  const float meshOne[4] = {1.f, 0.f, 0.f, 1.f};
  const float meshAbove[4] = {1.2f, 0.f, 0.f, 1.f};
  CHECK_MSG(Close(GlowOf(0.f, 0.5f, 0.f, 1.f, 1.f, meshZero).k, 0.5f * 0.100000001f), "ceil(0) x .2 + .1 = .1");
  CHECK_MSG(Close(GlowOf(0.f, 0.5f, 0.f, 1.f, 1.f, meshJust).k, 0.5f * 0.300000019f), "ceil(.2) takes it to 1");
  CHECK_MSG(Close(GlowOf(0.f, 0.5f, 0.f, 1.f, 1.f, meshOne).k, 0.5f * 0.300000019f), "ceil(1) stays 1");
  CHECK_MSG(Close(GlowOf(0.f, 0.5f, 0.f, 1.f, 1.f, meshAbove).k, 0.5f * 0.5f), "ceil(1.2) takes it to 2");
  // The instance colour is not an input here; the mesh colour's b is not either (it only shifts the
  // phase in the VS and the second read's texel on the CPU/GPU side of the pattern).
  const float meshB[4] = {0.2f, 0.f, 0.9f, 1.f};
  const Glow otherB = GlowOf(0.3f, 1.f, 0.f, 0.f, 1.f, meshB);
  CHECK_MSG(Close(otherB.rgb.r, full.rgb.r) && Close(otherB.rgb.g, full.rgb.g) && Close(otherB.rgb.b, full.rgb.b),
            "mesh.b does not enter the colour");
  // mesh.r's continuous -.5 / -.9 pull, at two r values inside one ceil bucket (same k).
  const float meshHigh[4] = {0.9f, 0.f, 0.f, 1.f};
  const Glow high = GlowOf(0.3f, 1.f, 0.f, 0.f, 1.f, meshHigh);
  const float dr = 0.9f - 0.2f;
  CHECK_MSG(Close(high.rgb.r, full.rgb.r), "R is the k term alone");
  CHECK_MSG(Close(high.rgb.g - full.rgb.g, full.gain * (-0.5f * dr)), "mesh.r pulls G down at .5");
  CHECK_MSG(Close(high.rgb.b - full.rgb.b, full.gain * (-0.899999976f * dr)), "mesh.r pulls B down at .9");
  // The pattern's .b enters through p = sat(b (1 - .5 phase)): phase .5 halves it, phase > 2 kills it.
  const Glow zeroB = GlowOf(0.f, 1.f, 0.f, 0.f, 1.f, mesh);
  CHECK_MSG(Close(GlowOf(0.f, 1.f, 2.f, 0.f, 1.f, mesh).rgb.g, zeroB.rgb.g),
            "with pattern .b 0, p is 0 whatever the phase");
  const Glow halfPhase = GlowOf(0.3f, 1.f, 0.5f, 0.f, 1.f, mesh);
  CHECK_MSG(Close(halfPhase.rgb.g - full.rgb.g, full.gain * 1.4f * (0.225f - 0.3f)),
            "phase .5 halves p = sat(b (1 - .5 phase)), and G follows");
  CHECK_MSG(halfPhase.rgb.g < full.rgb.g, "a rising phase lowers p, and G with it");
}

// The alpha: the instance alpha alone (x stays 1), discarded at or below 0 by the shared tail.
void TestGlowAlpha() {
  CHECK_MSG(WithInstance(1.f, 1.f) == 1.f && Close(WithInstance(1.f, 0.5f), 0.5f),
            "alpha = the instance alpha");
  CHECK_MSG(WithInstance(1.f, 0.f) == 0.f && WithInstance(1.f, -0.5f) == 0.f,
            "a non-positive instance alpha is discarded (a <= 0)");
}

} // namespace

int main() {
  CHECK_MSG(uint32_t(Feature::BarrierShield) == (1u << 29), "BarrierShield is bit 29");
  TestBreakupReveal();
  TestMotion();
  TestSawtoothBoundary();
  TestChromaticSamples();
  TestMeshColourSeparate();
  TestFresnel();
  TestGlowConstants();
  TestGlowDisplacement();
  TestGlowBreakupAndMeshColour();
  TestGlowAlpha();
  if (port_test::Failures() == 0) {
    std::printf("port_vfx_barrier: ok\n");
  }
  return PORT_TEST_RESULT();
}
