#pragma once

#include "port_collision_failsafe.h" // PortCollisionFailsafe::Vec3

// NCollisionRayCast::Capsule 0xbf6f10 with its two source dependencies,
// CMath::SolveQuadratic 0x699b0 and NCollisionRayCast::InfiniteCylinder 0xbf73c0.
// Pure geometry: no materials, contact lists, traversal or game state, and no caller.
// Kernel spec and control-flow decode (addresses below are from it):
// build/s-movement-next-20261016/ball-capsule-kernel/{SPEC.md,raw/DECODE.md}, authoritative
// dumps in .../ball-producer/raw/spec/. Every output here is pinned bit-for-bit against the
// AArch64 ISA fixture by tests/port_remastered_capsule_ray.cpp (see
// tests/fixtures/port_remastered_capsule_ray_isa.h for its provenance).
//
// The kernel is axis-selective (per-axis crosses and span tests), so it runs in the
// Remastered (Y up) frame. The public entry converts its port-frame inputs once, before any
// arithmetic, exactly as platform/port_remastered_ground_world.cpp:22-24 does:
// rem = (-port.x, port.z, port.y), its own inverse. RayCapsule's output is a scalar, so
// nothing is converted back, and the conversion permutes components only: the source's
// |dir| = 1 assumption carries over to within one rounding.
//
// Contract (the source adds no guards; do not "fix" it):
//  - `unitDirection` must be a unit vector: the inlined sphere tests hard-code
//    disc = r^2 - (m^2 - b^2), which is the tangent discriminant only for |dir| = 1.
//  - All inputs finite, `radius` nonnegative, `length` nonnegative, no intermediate
//    overflow. IEEE binary32 round-to-nearest, gradual underflow, no fast-math or implicit
//    contraction. Explicit fused operations model the ISA reciprocal/reciprocal-square-root
//    refinement steps. NaN and overflow behavior is outside this contract.
//
// Output quirks (all preserved):
//  - `distance` is output-only: no path reads it.
//  - Return 1 either writes 0 (the inside arms: perpendicular max(t_near, 0), cap-inside and
//    the degenerate-axis sphere test) or leaves the output untouched (the general branch's
//    tiny-clipped arm), as below. Return 0 leaves `distance` untouched and return 2 always
//    writes the hit parameter t.
//  - The perpendicular branch (|u.dir| < 1e-5) tests only the axial span: an accepted hit or
//    inside return there skips the `length` test and both caps, so its t can exceed `length`.
//    A span-test rejection is not such a return: it falls through to the caps.
//  - The general branch returns 1 WITHOUT writing the output when the clipped cylinder
//    parameter is |t| < 1e-5 (0xbf71a0-0xbf71b0).
//  - Both capsule epsilons are 0x3727c5ac = 1e-5; the quadratic solver's threshold is the
//    different constant 0x34000000, which IS FLT_EPSILON. Strictness: `s <= tmin` and
//    `t_near >= tmax` reject, the tangent discriminant rejects at disc <= 0, and a cap hit
//    needs t < best.
//  - The cap "inside" arm is applied only when best > 0, so with length == 0 a start inside
//    a cap sphere is not reported at all (best is the running value, initialised to length).
namespace PortRemasteredCapsuleRay {

using Vec3 = PortCollisionFailsafe::Vec3;

// CCapsule: +0 a, +0xc b, +0x18 radius.
struct Capsule {
  Vec3 a, b; // Segment endpoints, port frame (Z up). b == a is the degenerate case.
  float radius = 0.f;
};

// NCollisionRayCast::Capsule 0xbf6f10. Port frame (Z up): `origin`, `unitDirection` and
// `length` are CFixedLengthRay's origin, dir and len. `distance` is output-only.
// Returns 0 = miss, 1 = start inside, 2 = hit.
int RayCapsule(const Capsule& capsule, Vec3 origin, Vec3 unitDirection, float length,
               float& distance);

// 0xbf6f10's callees, exposed for independent boundary/ISA regression tests. Unlike the
// entry above, every vector here is already REMASTERED frame (Y up): RayCapsule converts
// once and these functions must not convert again.
namespace Source {

// The source's reciprocal for 1/(u.dir), 0xbf70c4-0xbf70e8: ARM FRECPE followed by two
// fused FRECPS refinements (r = r * (2 - r*x)), the estimate being the architecture's
// integer algorithm - Switch's 8-fraction-bit variant, as FEAT_RPRES is not present.
// Not host 1/x: FRECPE(1.0f) is 0.998046875 (511/512), so the refinements matter.
float Reciprocal(float value);

// ARM FPRecipEstimate (FRECPE) alone, without the two refinements above: the estimate the
// reciprocal starts from. Exposed for the bare-instruction rows of
// tests/fixtures/port_remastered_capsule_ray_isa.h, which the refined sequence cannot pin (its
// Newton steps can mask estimate differences). Infinity gives a zero of the same sign, zero
// gives infinity of the same sign, |value| < 2^-128 gives infinity of the operand's sign, and a
// NaN is quieted with its payload kept. The kernel calls it only through `Reciprocal`.
float ReciprocalEstimate(float value);

// CMath::SolveQuadratic 0x699b0. Threshold 0x34000000 = FLT_EPSILON (1.1920929e-07), not the
// capsule's 1e-5.
// Returns 0 (roots untouched), 1 (linear, roots[0] = -c/b, roots[1] untouched) or 2 (two
// roots, ascending: the source orders them by the sign of `a`). disc == 0 is not rejected
// here; the discriminant is b*b + (a * -4.0f) * c in that exact order.
int SolveQuadratic(float a, float b, float c, float (&roots)[2]);

// NCollisionRayCast::InfiniteCylinder 0xbf73c0: the ray against the infinite cylinder of
// `radius` around the line through `axisPoint` with unit `unitAxis`. `nearDistance <=
// farDistance` always. A miss returns false and leaves both outputs untouched (the source
// zeroes its root buffer but never writes the outputs on that path). The parameters are
// named for the source's tNear/tFar without using the bare `near`/`far` identifiers, which
// MSVC's legacy <windows.h> macros expand.
bool InfiniteCylinder(Vec3 axisPoint, Vec3 unitAxis, float radius, Vec3 rayOrigin,
                      Vec3 unitDirection, float& nearDistance, float& farDistance);

} // namespace Source
} // namespace PortRemasteredCapsuleRay
