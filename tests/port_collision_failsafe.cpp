// Remastered collision failsafe decisions (platform/port_collision_failsafe.cpp) on
// hand-built contacts: core push selection 0xc1fc54, wrapper 0xc1fa68, tunneling
// 0xc20214, safe offset 0xc217a0, box-box impacts 0xbf61b4, the AABox ray 0xbf7e50 and
// the sphere ray NCollisionRayCast::Sphere 0xbf6ac0 (wrapper 0xc0289c).
#include "port_collision_failsafe.h"
#include "port_test.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

using namespace PortCollisionFailsafe;

static bool Near(float a, float b, float e = 1e-5f) { return std::fabs(a - b) <= e; }
static bool Near(const Vec3& a, const Vec3& b, float e = 1e-5f) {
  return Near(a.x, b.x, e) && Near(a.y, b.y, e) && Near(a.z, b.z, e);
}
static Vec3 Mul_(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

static Impact Terrain(float depth, Vec3 n) {
  Impact i;
  i.depth = depth;
  i.otherNormal = n;
  return i;
}

static void PushSelection() {
  // Within a contact the shallowest impact; across contacts the deepest of those.
  std::vector< Contact > list(2);
  list[0].impacts = {Terrain(.3f, {0, 0, 1}), Terrain(.1f, {1, 0, 0})};
  list[1].impacts = {Terrain(.2f, {0, 1, 0})};
  Vec3 push;
  CHECK_MSG(SelectPush(list, {}, 1.f / 60.f, push), "push: moved");
  CHECK_MSG(Near(push, {0, .2f + 1e-4f, 0}), "push: deepest contact's shallowest impact");

  // Ties keep the first (strict compares both ways).
  list[1].impacts = {Terrain(.1f, {0, -1, 0})};
  SelectPush(list, {}, 1.f / 60.f, push);
  CHECK_MSG(Near(push, {1e-4f + .1f, 0, 0}), "push: tie keeps the earlier contact");

  // Zero depth still pushes by EPS4 (best starts at -FLT_EPSILON).
  list.resize(1);
  list[0].impacts = {Terrain(0.f, {0, 0, 1})};
  SelectPush(list, {}, 1.f / 60.f, push);
  CHECK_MSG(Near(push, {0, 0, 1e-4f}, 1e-9f), "push: zero depth");

  // Character contact: horizontal, away from the other's centre seen from `from`,
  // capped at worldDt * 50.
  Impact c;
  c.depth = 2.f;
  c.otherCharacter = true;
  c.otherCenter = {3, 4, 10};
  list[0].impacts = {c};
  SelectPush(list, {0, 0, 0}, 1.f / 60.f, push);
  const float cap = 50.f / 60.f;
  CHECK_MSG(Near(push, {-.6f * cap, -.8f * cap, 0}), "push: character horizontal capped");
  list[0].impacts[0].depth = .1f;
  SelectPush(list, {0, 0, 0}, 1.f / 60.f, push);
  CHECK_MSG(Near(push, {-.6f * (.1f + 1e-4f), -.8f * (.1f + 1e-4f), 0}), "push: character depth");

  list.clear();
  CHECK_MSG(!SelectPush(list, {}, 1.f / 60.f, push), "push: empty");
}

static void CoreLoop() {
  // A floor plane at z = 0 overlapping a box whose bottom is at pos.z: one push.
  CoreHooks hooks;
  int calls = 0;
  hooks.collide = [&](const Vec3& pos, std::vector< Contact >& out) {
    ++calls;
    if (pos.z >= 0.f) return false;
    out.resize(1);
    out[0].impacts = {Terrain(-pos.z, {0, 0, 1})};
    return true;
  };
  bool tunnelCalled = false;
  hooks.tunnel = [&](const Vec3& from, const Vec3& to, Vec3&) {
    tunnelCalled = true;
    CHECK_MSG(Near(from, {0, 0, -.25f}) && Near(to, {0, 0, 1e-4f}), "core: tunnel from state to out");
    return false;
  };
  Vec3 out;
  CHECK_MSG(Core({0, 0, -.25f}, 1.f / 60.f, hooks, out), "core: moved");
  CHECK_MSG(Near(out, {0, 0, 1e-4f}), "core: depth + EPS4");
  CHECK_MSG(calls == 2 && tunnelCalled, "core: stops on the first free test");

  // Never resolves: five pushes at most.
  calls = 0;
  hooks.collide = [&](const Vec3&, std::vector< Contact >& out) {
    ++calls;
    out.resize(1);
    out[0].impacts = {Terrain(1.f, {1, 0, 0})};
    return true;
  };
  hooks.tunnel = [](const Vec3&, const Vec3&, Vec3&) { return false; };
  Core({}, 1.f / 60.f, hooks, out);
  CHECK_MSG(calls == 5 && Near(out, {5 * (1.f + 1e-4f), 0, 0}, 1e-4f), "core: five iterations");

  // A tunneling revert replaces the result and reports moved.
  calls = 0;
  hooks.collide = [&](const Vec3&, std::vector< Contact >&) { return false; };
  hooks.tunnel = [](const Vec3& from, const Vec3&, Vec3& o) {
    o = from;
    return true;
  };
  CHECK_MSG(Core({1, 2, 3}, 1.f / 60.f, hooks, out) && Near(out, {1, 2, 3}), "core: tunnel result");

  // Separate object-state origin (0xc1fc54 tunnels from the state transform, the pushes
  // reference `from`): the tunnel hook sees state -> pushedOut, the push is still from `from`.
  hooks.collide = [](const Vec3& pos, std::vector< Contact >& list) {
    if (pos.z >= 0.f) return false;
    list.resize(1);
    list[0].impacts = {Terrain(-pos.z, {0, 0, 1})};
    return true;
  };
  Vec3 seenFrom, seenTo;
  hooks.tunnel = [&](const Vec3& f, const Vec3& t, Vec3&) {
    seenFrom = f;
    seenTo = t;
    return false;
  };
  const Vec3 from = {0, 0, -.25f}, state = {5, 6, 7};
  CHECK_MSG(Core(from, state, 1.f / 60.f, hooks, out), "core-state: moved");
  CHECK_MSG(Near(seenFrom, state) && Near(seenTo, {0, 0, 1e-4f}) && Near(out, {0, 0, 1e-4f}),
            "core-state: tunnel state -> pushed-out, push reference from `from`");
  // The default overload is the airborne case: state == from.
  Core(from, 1.f / 60.f, hooks, out);
  CHECK_MSG(Near(seenFrom, from), "core-state: default overload tunnels from `from`");
}

static void Wrapper() {
  const Vec3 cur{1, 1, 1};
  Vec3 out;
  int coreCalls = 0;
  auto core = [&](Vec3 a, Vec3 b, bool ra, bool rb) {
    return [=, &coreCalls](bool failsafe, Vec3& o) {
      ++coreCalls;
      o = failsafe ? b : a;
      return failsafe ? rb : ra;
    };
  };
  auto blocked = [](bool a, bool b) {
    return [=](bool failsafe, const Vec3&) { return failsafe ? b : a; };
  };
  // No failsafe filter: the core decides.
  CHECK_MSG(DoCollisionFailsafe(cur, false, core({2, 2, 2}, {}, true, false), blocked(true, true), out) &&
                Near(out, {2, 2, 2}),
            "wrapper: no F1290, core result");
  CHECK_MSG(!DoCollisionFailsafe(cur, false, core({2, 2, 2}, {}, false, false), blocked(true, true), out) &&
                Near(out, cur),
            "wrapper: no F1290, core failed");
  // First core free: kept.
  coreCalls = 0;
  CHECK_MSG(DoCollisionFailsafe(cur, true, core({2, 2, 2}, {3, 3, 3}, true, true), blocked(false, false), out) &&
                Near(out, {2, 2, 2}) && coreCalls == 1,
            "wrapper: first result free");
  // First core failed: not moved, false.
  CHECK_MSG(!DoCollisionFailsafe(cur, true, core({2, 2, 2}, {3, 3, 3}, false, true), blocked(false, false), out) &&
                Near(out, cur),
            "wrapper: first core failed");
  // First blocked, second free: the failsafe-filter result.
  CHECK_MSG(DoCollisionFailsafe(cur, true, core({2, 2, 2}, {3, 3, 3}, true, true), blocked(true, false), out) &&
                Near(out, {3, 3, 3}),
            "wrapper: failsafe filter result");
  // First blocked, second core failed: false, not moved.
  CHECK_MSG(!DoCollisionFailsafe(cur, true, core({2, 2, 2}, {3, 3, 3}, true, false), blocked(true, false), out) &&
                Near(out, cur),
            "wrapper: second core failed");
  // Both blocked: true, not moved.
  CHECK_MSG(DoCollisionFailsafe(cur, true, core({2, 2, 2}, {3, 3, 3}, true, true), blocked(true, true), out) &&
                Near(out, cur),
            "wrapper: still stuck reports resolved");
}

static void Tunneling() {
  // Player-like box: 1 x 1 x 2.7, position at the feet.
  const Vec3 pos{0, 0, 0};
  const Box bounds{{-.5f, -.5f, 0}, {.5f, .5f, 2.7f}};
  TunnelHooks hooks;
  int rays = 0;
  RayHit hit;
  Vec3 rayStart, rayDir;
  float rayLen = 0.f;
  hooks.ray = [&](const Vec3& s, const Vec3& d, float l) {
    ++rays;
    rayStart = s;
    rayDir = d;
    rayLen = l;
    return hit;
  };
  int safeCalls = 0;
  hooks.safeOffset = [&](const Vec3&, float maxDist, Vec3& o) {
    ++safeCalls;
    CHECK_MSG(Near(maxDist, .5f + FLT_EPSILON), "tunnel: safe radius = min half + eps");
    o = {0, 0, .01f};
    return true;
  };
  Vec3 out;
  // Short move (<= min half extent): never checked.
  CHECK_MSG(!DoTunnelingFailsafe(bounds, pos, {0, 0, 0}, {.5f, 0, 0}, hooks, out) && rays == 0 &&
                Near(out, {.5f, 0, 0}),
            "tunnel: short move");
  // Miss.
  hit = RayHit{};
  CHECK_MSG(!DoTunnelingFailsafe(bounds, pos, {0, 0, 0}, {2, 0, 0}, hooks, out) && rays == 1,
            "tunnel: miss");
  CHECK_MSG(Near(rayStart, {0, 0, 1.35f}) && Near(rayDir, {1, 0, 0}) && Near(rayLen, 2.f),
            "tunnel: ray from the bounds-centre height");
  // Code 1: revert.
  hit.code = kRay_Inside;
  CHECK_MSG(DoTunnelingFailsafe(bounds, pos, {0, 0, 0}, {2, 0, 0}, hooks, out) && Near(out, {0, 0, 0}),
            "tunnel: start inside reverts");
  // Hit closer than the half width: revert.
  hit.code = kRay_Hit;
  hit.t = .49f;
  hit.normal = {-1, 0, 0};
  hit.orientation = 2;
  CHECK_MSG(DoTunnelingFailsafe(bounds, pos, {0, 0, 0}, {2, 0, 0}, hooks, out) && Near(out, {0, 0, 0}),
            "tunnel: near hit reverts");
  // Wall hit: placed half a width short, raised by cy, no safe offset.
  hit.t = 1.5f;
  safeCalls = 0;
  CHECK_MSG(DoTunnelingFailsafe(bounds, pos, {0, 0, 0}, {2, 0, 0}, hooks, out) &&
                Near(out, {1.f, 0, 1.35f}) && safeCalls == 0,
            "tunnel: wall placement");
  // Floor hit: the safe offset is added.
  hit.normal = {0, 0, 1};
  hit.orientation = -1;
  CHECK_MSG(DoTunnelingFailsafe(bounds, pos, {0, 0, 0}, {2, 0, 0}, hooks, out) &&
                Near(out, {1.f, 0, 1.36f}) && safeCalls == 1,
            "tunnel: floor placement + safe offset");
  // Feet below the position shift the result by the same amount.
  const Box low{{-.5f, -.5f, -.2f}, {.5f, .5f, 2.5f}};
  hit.normal = {-1, 0, 0};
  hit.orientation = 2;
  DoTunnelingFailsafe(low, pos, {0, 0, 0}, {2, 0, 0}, hooks, out);
  CHECK_MSG(Near(out, {1.f, 0, 1.15f + .2f}), "tunnel: foot offset");
}

static void Floor() {
  CHECK_MSG(IsFloor(.86f, 2), "is_floor: steep up");
  CHECK_MSG(IsFloor(-1.f, 0), "is_floor: orientation 0");
  CHECK_MSG(IsFloor(.8f, -1), "is_floor: unknown orientation passes 0.7071");
  CHECK_MSG(IsFloor(.8f, 3), "is_floor: orientation 3");
  CHECK_MSG(!IsFloor(.8f, 1) && !IsFloor(.8f, 2), "is_floor: ceiling/wall need 0.85");
  CHECK_MSG(!IsFloor(.7f, -1), "is_floor: below 0.7071");
}

static void SafeOffset() {
  const Box box{{-.5f, -.5f, 0}, {.5f, .5f, 2.7f}};
  SafeHooks hooks;
  int rays = 0;
  hooks.rayBlocked = [&](const Vec3&, const Vec3&, float) {
    ++rays;
    return false;
  };
  Vec3 off;
  hooks.collidesAt = [](const Vec3&) { return false; };
  CHECK_MSG(CalculateOffsetToSafeCollisionPoint(box, {}, .5f, hooks, off) && Near(off, {}),
            "safe: already free");
  // Free only above z >= 0.03: radius order first, then direction order (+Z is #4).
  hooks.collidesAt = [](const Vec3& p) { return p.z < .03f - 1e-6f; };
  CHECK_MSG(CalculateOffsetToSafeCollisionPoint(box, {}, .5f + FLT_EPSILON, hooks, off) &&
                Near(off, {0, 0, .03f}) && rays == 0,
            "safe: smallest radius, first direction");
  // Free only along +Y (#0): wins over every later direction at the same radius.
  hooks.collidesAt = [](const Vec3& p) { return !(p.y > 0.f); };
  CalculateOffsetToSafeCollisionPoint(box, {}, .5f, hooks, off);
  CHECK_MSG(Near(off, {0, .01f, 0}), "safe: direction order");
  // Never free: no valid offset; radii stop below maxDist.
  hooks.collidesAt = [](const Vec3&) { return true; };
  CHECK_MSG(!CalculateOffsetToSafeCollisionPoint(box, {}, .5f, hooks, off), "safe: none");
  // Rays only from the min half extent on.
  rays = 0;
  CalculateOffsetToSafeCollisionPoint(box, {}, .8f, hooks, off);
  CHECK_MSG(rays == 26, "safe: ray branch at d >= min half (only 0.705)");
  // Direction table: unit vectors, port = (-remX, remZ, remY) of the SPEC order.
  for (const Vec3& d : kSafeDirections) {
    CHECK_MSG(Near(d.x * d.x + d.y * d.y + d.z * d.z, 1.f, 1e-5f), "safe: unit directions");
  }
  CHECK_MSG(Near(kSafeDirections[18], {-.57735026f, .57735026f, .57735026f}), "safe: dir 18");
  CHECK_MSG(kSafeRadii[0] == .01f && kSafeRadii[20] == 8.f, "safe: radii");
}

static void BoxBox() {
  std::vector< Impact > imp;
  // Self sinks 0.1 into the top of a wide slab: smallest impact pushes up.
  const Box slab{{-5, -5, -1}, {5, 5, 0}};
  CHECK_MSG(BoxBoxImpacts({{-.5f, -.5f, -.1f}, {.5f, .5f, 2.6f}}, slab, imp), "boxbox: hit");
  float best = FLT_MAX;
  Vec3 n;
  for (const Impact& i : imp) {
    if (i.depth < best) {
      best = i.depth;
      n = i.otherNormal;
    }
  }
  CHECK_MSG(Near(best, .1f) && Near(n, {0, 0, 1}), "boxbox: vertical push out of the top");
  // Side overlap: +X push from a box on the -X side.
  imp.clear();
  BoxBoxImpacts({{.8f, -.5f, 0}, {1.8f, .5f, 2.7f}}, {{-1, -1, -1}, {1, 1, 5}}, imp);
  CHECK_MSG(imp.size() == 1 && Near(imp[0].depth, .2f) && Near(imp[0].otherNormal, {1, 0, 0}),
            "boxbox: side impact");
  // Self inside other on every axis: fallback picks the smallest difference.
  imp.clear();
  BoxBoxImpacts({{-.5f, -.5f, 0}, {.5f, .5f, 1}}, {{-3, -3, -.25f}, {3, 3, 4}}, imp);
  CHECK_MSG(imp.size() == 1 && Near(imp[0].depth, 1.25f) && Near(imp[0].otherNormal, {0, 0, -1}),
            "boxbox: contained fallback");
  // Touching: no positive depth, no contact.
  imp.clear();
  CHECK_MSG(!BoxBoxImpacts({{1, 0, 0}, {2, 1, 1}}, {{0, 0, 0}, {1, 1, 1}}, imp), "boxbox: touching");
  // Apart.
  CHECK_MSG(!BoxBoxImpacts({{2, 0, 0}, {3, 1, 1}}, {{0, 0, 0}, {1, 1, 1}}, imp), "boxbox: apart");
}

// ---- AABox x AABox: paired geometry (0xbf61b4 / 0xbf6650) -----------------------------
// Test-local Remastered frame (rem = (-x, z, y)) and a verbatim copy of the pre-change
// BoxBoxImpacts. The copy is a legitimate REGRESSION oracle for the refactor (the legacy
// output must stay bit-identical, zero signs included); it is not an oracle for 0xbf61b4.
struct RemV {
  float v[3];
};
static void RemBox(const Box& b, RemV& mn, RemV& mx) {
  mn = {{-b.max.x, b.min.z, b.min.y}};
  mx = {{-b.min.x, b.max.z, b.max.y}};
}
static Vec3 RemToPort(const RemV& r) { return {-r.v[0], r.v[2], r.v[1]}; }

static bool PrechangeBoxBoxImpacts(const Box& self, const Box& other, std::vector< Impact >& out) {
  // A = self, B = other; Remastered axes x, y (up), z.
  RemV aMin, aMax, bMin, bMax;
  RemBox(self, aMin, aMax);
  RemBox(other, bMin, bMax);
  // DoBoundsOverlap 0x98234 (inclusive) gates the whole routine.
  for (int k = 0; k < 3; ++k) {
    if (aMax.v[k] < bMin.v[k] || bMax.v[k] < aMin.v[k]) return false;
  }
  const std::size_t first = out.size();
  // 0xbf6650: an impact is added only with depth > 0; both halves carry the depth and
  // their owner's outward face normal: A's +axis*sign, B's -axis*sign.
  auto add = [&](int axis, float depth, float sign) {
    if (!(depth > 0.f)) return;
    RemV n{{0.f, 0.f, 0.f}};
    n.v[axis] = -sign; // the other (B) half's normal
    Impact i;
    i.depth = depth;
    i.otherNormal = RemToPort(n);
    out.push_back(i);
  };
  for (int k = 0; k < 3; ++k) {
    const float extB = bMax.v[k] - bMin.v[k];
    const float extA = aMax.v[k] - aMin.v[k];
    const float s0 = aMax.v[k] - bMin.v[k];
    const float s8 = bMax.v[k] - aMin.v[k];
    if (s0 < extB && s0 < extA) add(k, s0, 1.f);
    if (s8 < extA && s8 < extB) add(k, s8, -1.f);
  }
  if (out.size() == first) {
    // No axis partly overlaps: the smallest non-negative difference, earlier axis on ties.
    const float d[6] = {aMax.v[0] - bMin.v[0], bMax.v[0] - aMin.v[0], aMax.v[1] - bMin.v[1],
                        bMax.v[1] - aMin.v[1], aMax.v[2] - bMin.v[2], bMax.v[2] - aMin.v[2]};
    float best = (d[0] < FLT_MAX && d[0] >= 0.f) ? d[0] : FLT_MAX;
    int axis = 0;
    for (int k = 1; k < 6; ++k) {
      if (d[k] < best && d[k] >= 0.f) {
        best = d[k];
        axis = k / 2;
      }
    }
    const float p = aMax.v[axis] - bMin.v[axis];
    const float n = bMax.v[axis] - aMin.v[axis];
    if (p < n) {
      add(axis, p, 1.f);
    } else {
      add(axis, n, -1.f);
    }
  }
  return out.size() != first;
}

static bool Same(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }
static bool Same(const Vec3& a, const Vec3& b) {
  return Same(a.x, b.x) && Same(a.y, b.y) && Same(a.z, b.z);
}
static bool Eq(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

// Independent restatement of the paired API: candidates are listed first, then filtered.
// CAUTION: this shares this file's frame helpers (character-for-character BoxToRem/
// FromRem) and the same operation order as the implementation (corner assignment,
// ((a+b)+c)+d, the fallback scan), so the random agreement below means "the port matches
// this reading of the SPEC" - an independent-formulation consistency check - NOT "the port
// matches 0xbf61b4". A misread axis mapping, a p8/p9 swap, a wrong fallback gate or a wrong
// rounding order would be duplicated here and would pass. The executable expectations for
// the source are the hand-derived deterministic cases; the exact tie and zero-sign rules
// are pinned by BoxBoxPairedBits().
static bool RefPaired(const Box& self, const Box& other, bool (*passes)(const Vec3&),
                      std::vector< PairedImpact >& out) {
  RemV aN, aX, bN, bX;
  RemBox(self, aN, aX);
  RemBox(other, bN, bX);
  for (int k = 0; k < 3; ++k)
    if (aX.v[k] < bN.v[k] || bX.v[k] < aN.v[k]) return false;
  RemV iN, iX;
  for (int k = 0; k < 3; ++k) {
    // 0x98800: iN (max of the two minima) keeps SELF on an exact tie - fcsel a,a,b,ge;
    // iX (min of the two maxima) keeps OTHER - fcsel a,b,a,ge. fmax/fmin would leave the
    // zero-sign choice to the library.
    iN.v[k] = aN.v[k] >= bN.v[k] ? aN.v[k] : bN.v[k];
    iX.v[k] = aX.v[k] < bX.v[k] ? aX.v[k] : bX.v[k];
  }
  struct Cand {
    int axis;
    float depth, sign;
  };
  std::vector< Cand > prim;
  for (int k = 0; k < 3; ++k) {
    const float wa = aX.v[k] - aN.v[k], wb = bX.v[k] - bN.v[k];
    const float up = aX.v[k] - bN.v[k], dn = bX.v[k] - aN.v[k];
    if (up < wb && up < wa) prim.push_back({k, up, 1.f});
    if (dn < wa && dn < wb) prim.push_back({k, dn, -1.f});
  }
  auto build = [&](const Cand& c, PairedImpact& pi) {
    if (!(c.depth > 0.f)) return false;
    RemV sn{{0, 0, 0}}, on{{0, 0, 0}};
    sn.v[c.axis] = c.sign;
    // 0xbf66b8-0xbf66c0 negate all three components of the skZeroVector-based normal, so
    // the other half's zero components are negated zeros, not fresh +0.0.
    on.v[0] = -sn.v[0];
    on.v[1] = -sn.v[1];
    on.v[2] = -sn.v[2];
    if (passes && !passes(RemToPort(on))) return false;
    const int a1 = (c.axis + 1) % 3, a2 = (c.axis + 2) % 3;
    const float* faceSelf = c.sign > 0.f ? iX.v : iN.v;
    const float* faceOther = c.sign > 0.f ? iN.v : iX.v;
    // corners (a1,a2): (min,min) (min,max) (max,min) (max,max), summed in that order.
    const float lo1 = iN.v[a1], hi1 = iX.v[a1], lo2 = iN.v[a2], hi2 = iX.v[a2];
    RemV mid{{0, 0, 0}};
    mid.v[c.axis] = ((faceSelf[c.axis] + faceSelf[c.axis]) + faceSelf[c.axis] + faceSelf[c.axis]) * 0.25f;
    mid.v[a1] = (((lo1 + lo1) + hi1) + hi1) * 0.25f;
    mid.v[a2] = (((lo2 + hi2) + lo2) + hi2) * 0.25f;
    RemV op = mid;
    op.v[c.axis] = faceOther[c.axis];
    pi.selfDepth = pi.otherDepth = c.depth;
    pi.selfNormal = RemToPort(sn);
    pi.otherNormal = RemToPort(on);
    pi.selfPoint = RemToPort(mid);
    pi.otherPoint = RemToPort(op);
    return true;
  };
  bool any = false;
  for (const Cand& c : prim) {
    PairedImpact pi;
    if (build(c, pi)) {
      out.push_back(pi);
      any = true;
    }
  }
  if (any) return true;
  float diff[6];
  for (int k = 0; k < 3; ++k) {
    diff[2 * k] = aX.v[k] - bN.v[k];
    diff[2 * k + 1] = bX.v[k] - aN.v[k];
  }
  float best = (diff[0] < FLT_MAX && diff[0] >= 0.f) ? diff[0] : FLT_MAX;
  int axis = 0;
  for (int i = 1; i < 6; ++i)
    if (diff[i] >= 0.f && diff[i] < best) best = diff[i], axis = i / 2;
  const float up = diff[2 * axis], dn = diff[2 * axis + 1];
  const Cand c = up < dn ? Cand{axis, up, 1.f} : Cand{axis, dn, -1.f};
  PairedImpact pi;
  if (!build(c, pi)) return false;
  out.push_back(pi);
  return true;
}

static bool NotMinusX(const Vec3& n) { return !(n.x == -1.f); }
static bool RejectAll(const Vec3&) { return false; }
static bool HashPass(const Vec3& n) {
  return ((int)(n.x * 3 + n.y * 5 + n.z * 7 + 100) & 1) != 0;
}

static bool OnePaired(const Box& s, const Box& o, const NormalPredicate& p,
                      std::vector< PairedImpact >& v) {
  return BoxBoxPairedImpacts(s, o, p, v);
}

static void BoxBoxPaired() {
  std::vector< PairedImpact > v;
  // Partial overlap on all three axes: three impacts in Remastered order x, up, z.
  CHECK_MSG(OnePaired({{0, 0, 0}, {2, 2, 2}}, {{1, 1, 1}, {3, 3, 3}}, NormalPredicate(), v) &&
                v.size() == 3,
            "paired: three-axis partial overlap");
  if (v.size() == 3) {
    CHECK_MSG(Eq(v[0].selfNormal, {1, 0, 0}) && Eq(v[0].otherNormal, {-1, 0, 0}) &&
                  Eq(v[0].selfPoint, {2, 1.5f, 1.5f}) && Eq(v[0].otherPoint, {1, 1.5f, 1.5f}) &&
                  v[0].selfDepth == 1.f && v[0].otherDepth == 1.f,
              "paired: x impact normals, depth and points");
    CHECK_MSG(Eq(v[1].selfNormal, {0, 0, 1}) && Eq(v[1].otherNormal, {0, 0, -1}) &&
                  Eq(v[1].selfPoint, {1.5f, 1.5f, 2}) && Eq(v[1].otherPoint, {1.5f, 1.5f, 1}),
              "paired: up impact second");
    CHECK_MSG(Eq(v[2].selfNormal, {0, 1, 0}) && Eq(v[2].otherNormal, {0, -1, 0}) &&
                  Eq(v[2].selfPoint, {1.5f, 2, 1.5f}) && Eq(v[2].otherPoint, {1.5f, 1, 1.5f}),
              "paired: port-y impact third");
  }
  // Mirror: self on the + side of other flips every sign and face plane.
  v.clear();
  OnePaired({{1, 1, 1}, {3, 3, 3}}, {{0, 0, 0}, {2, 2, 2}}, NormalPredicate(), v);
  CHECK_MSG(v.size() == 3 && Eq(v[0].selfNormal, {-1, 0, 0}) && Eq(v[0].selfPoint, {1, 1.5f, 1.5f}) &&
                Eq(v[0].otherPoint, {2, 1.5f, 1.5f}) && Eq(v[1].otherNormal, {0, 0, 1}),
            "paired: mirrored signs and planes");
  // Equal minimum edge on x: neither +1 nor -1 holds, only y and z partial overlaps emit.
  v.clear();
  OnePaired({{0, 0, 0}, {2, 2, 2}}, {{0, 1, 1}, {3, 3, 3}}, NormalPredicate(), v);
  CHECK_MSG(v.size() == 2 && Eq(v[0].selfNormal, {0, 0, 1}) && Eq(v[1].selfNormal, {0, 1, 0}),
            "paired: equal-edge axis emits nothing");
  // Equal maximum edge on x.
  v.clear();
  OnePaired({{0, 0, 0}, {2, 2, 2}}, {{1, 1, 1}, {2, 3, 3}}, NormalPredicate(), v);
  CHECK_MSG(v.size() == 2, "paired: equal-max edge emits nothing on that axis");
  // Touching on x (depth 0) with partial overlaps on y and z: the pair still overlaps.
  v.clear();
  OnePaired({{2, 0, 0}, {3, 2, 2}}, {{0, 1, 1}, {2, 3, 3}}, NormalPredicate(), v);
  CHECK_MSG(v.size() == 2 && Eq(v[0].selfNormal, {0, 0, 1}), "paired: touching axis, others emit");
  // Touching on every axis candidate: no contact, nothing appended.
  v.clear();
  CHECK_MSG(!OnePaired({{2, 0, 0}, {3, 1, 1}}, {{0, 0, 0}, {2, 1, 1}}, NormalPredicate(), v) &&
                v.empty(),
            "paired: touching only -> none");
  // Apart.
  CHECK_MSG(!OnePaired({{5, 0, 0}, {6, 1, 1}}, {{0, 0, 0}, {2, 1, 1}}, NormalPredicate(), v) &&
                v.empty(),
            "paired: apart");
  // Containment: single fallback impact, tie A == B takes the -1 side, points on the faces.
  v.clear();
  CHECK_MSG(OnePaired({{1, 1, 1}, {2, 2, 2}}, {{0, 0, 0}, {3, 3, 3}}, NormalPredicate(), v) &&
                v.size() == 1,
            "paired: containment fallback");
  if (v.size() == 1) {
    CHECK_MSG(v[0].selfDepth == 2.f && v[0].otherDepth == 2.f && Eq(v[0].selfNormal, {1, 0, 0}) &&
                  Eq(v[0].otherNormal, {-1, 0, 0}) && Eq(v[0].selfPoint, {2, 1.5f, 1.5f}) &&
                  Eq(v[0].otherPoint, {1, 1.5f, 1.5f}),
              "paired: fallback depth, tie sign, normals and points");
  }
  // Fallback axis choice: contained in x and y, nearer the other's top in z.
  v.clear();
  OnePaired({{1, 1, 2.5f}, {2, 2, 2.9f}}, {{0, 0, 0}, {3, 3, 3}}, NormalPredicate(), v);
  CHECK_MSG(v.size() == 1 && Near(v[0].selfDepth, .5f) && Eq(v[0].selfNormal, {0, 0, -1}),
            "paired: fallback picks the minimum difference axis");
  // Predicate rejects the only primary impact: the fallback candidate is tried.
  const Box s{{0, 1, 2.5f}, {2, 1.5f, 2.9f}};
  const Box o{{1, 0, 0}, {3, 3, 3}};
  v.clear();
  OnePaired(s, o, NormalPredicate(), v);
  CHECK_MSG(v.size() == 1 && Eq(v[0].otherNormal, {-1, 0, 0}) && v[0].selfDepth == 1.f,
            "paired: single primary without predicate");
  v.clear();
  int calls = 0;
  Vec3 seen;
  OnePaired(s, o, [&](const Vec3& n) { ++calls; seen = n; return true; }, v);
  CHECK_MSG(calls == 1 && Eq(seen, {-1, 0, 0}), "paired: predicate asked once with the other normal");
  v.clear();
  CHECK_MSG(OnePaired(s, o, NotMinusX, v) && v.size() == 1 && Near(v[0].selfDepth, .5f) &&
                Eq(v[0].otherNormal, {0, 0, 1}),
            "paired: predicate rejection runs the fallback");
  v.clear();
  CHECK_MSG(!OnePaired(s, o, RejectAll, v) && v.empty(), "paired: everything rejected -> none");
  // Prefix append keeps earlier entries; a miss leaves the vector untouched.
  v.clear();
  PairedImpact sentinel;
  sentinel.selfDepth = 7.f;
  v.push_back(sentinel);
  OnePaired({{0, 0, 0}, {2, 2, 2}}, {{1, 1, 1}, {3, 3, 3}}, NormalPredicate(), v);
  CHECK_MSG(v.size() == 4 && v[0].selfDepth == 7.f, "paired: appends after a prefix");
  OnePaired({{9, 9, 9}, {10, 10, 10}}, {{0, 0, 0}, {1, 1, 1}}, NormalPredicate(), v);
  CHECK_MSG(v.size() == 4, "paired: no hit leaves the prefix");
}

static unsigned gSeed = 12345u;
static unsigned Rnd() {
  gSeed = gSeed * 1664525u + 1013904223u;
  return gSeed >> 8;
}
static float RndCoord(bool grid) {
  return grid ? (float)(Rnd() % 17) * 0.25f - 1.f : (float)(Rnd() % 100000) / 25000.f - 1.f;
}
static Box RndBox(bool grid) {
  Box b;
  float a[3], c[3];
  for (int k = 0; k < 3; ++k) {
    a[k] = RndCoord(grid);
    c[k] = RndCoord(grid);
    if (a[k] > c[k]) std::swap(a[k], c[k]);
  }
  b.min = {a[0], a[1], a[2]};
  b.max = {c[0], c[1], c[2]};
  return b;
}

static void BoxBoxRandom() {
  int hits = 0, fallbacks = 0, mismatches = 0;
  bool (*preds[3])(const Vec3&) = {nullptr, NotMinusX, HashPass};
  for (int it = 0; it < 200000; ++it) {
    const bool grid = (it & 1) == 0;
    const Box s = RndBox(grid), o = RndBox(grid);
    // Legacy API vs the verbatim pre-change implementation.
    std::vector< Impact > a, b;
    const bool ra = BoxBoxImpacts(s, o, a), rb = PrechangeBoxBoxImpacts(s, o, b);
    bool ok = ra == rb && a.size() == b.size();
    for (std::size_t i = 0; ok && i < a.size(); ++i)
      ok = Same(a[i].depth, b[i].depth) && Same(a[i].otherNormal, b[i].otherNormal) &&
           !a[i].otherCharacter && Same(a[i].otherCenter, Vec3());
    // Paired API vs the independent reference, for each predicate; no predicate also
    // agrees with the legacy depth/normal sequence.
    for (int pi = 0; pi < 3; ++pi) {
      std::vector< PairedImpact > got, ref;
      const bool rg = BoxBoxPairedImpacts(
          s, o, preds[pi] ? NormalPredicate(preds[pi]) : NormalPredicate(), got);
      const bool rr = RefPaired(s, o, preds[pi], ref);
      ok = ok && rg == rr && got.size() == ref.size();
      for (std::size_t i = 0; ok && i < got.size(); ++i) {
        ok = Same(got[i].selfDepth, ref[i].selfDepth) && Same(got[i].otherDepth, ref[i].otherDepth) &&
             Same(got[i].selfNormal, ref[i].selfNormal) &&
             Same(got[i].otherNormal, ref[i].otherNormal) &&
             Same(got[i].selfPoint, ref[i].selfPoint) && Same(got[i].otherPoint, ref[i].otherPoint);
      }
      if (pi == 0) {
        ok = ok && got.size() == a.size();
        // The paired other normal is the source's component-wise negation, so its zero
        // components carry the opposite zero signs to the legacy helper's +0.0 zeros:
        // values (and depths) agree, the bits of the zeros need not.
        for (std::size_t i = 0; ok && i < got.size(); ++i)
          ok = Same(got[i].selfDepth, a[i].depth) && Eq(got[i].otherNormal, a[i].otherNormal);
        if (rg) ++hits;
        if (rg && got.size() == 1) ++fallbacks;
      }
    }
    if (!ok && ++mismatches <= 3) std::printf("box pair mismatch it=%d\n", it);
  }
  CHECK_MSG(mismatches == 0,
            "boxbox random: legacy == prechange regression oracle, paired == restatement");
  CHECK_MSG(hits > 20000 && fallbacks > 1000, "boxbox random: coverage");
}

// ---- directed bit patterns: signed zeros, filter calls, source ties -------------------
// Every expectation here is hand-derived from 0x98800 + 0xbf6650 and compared bitwise
// (Same/memcmp), because the F1/F2 corrections can differ from the pre-change answers
// only in zero signs. `Rem3`/`PortFromRem` build test boxes from Remastered-frame extents
// (the exact inverse of BoxToRem, negating the port x axis), signed zeros included.
static RemV Rem3(float x, float y, float z) { return {{x, y, z}}; }
static Box PortFromRem(const RemV& mn, const RemV& mx) {
  Box b;
  b.min.x = -mx.v[0];
  b.max.x = -mn.v[0];
  b.min.y = mn.v[2];
  b.max.y = mx.v[2];
  b.min.z = mn.v[1];
  b.max.z = mx.v[1];
  return b;
}

static void BoxBoxPairedBits() {
  CHECK_MSG(Same(0.f, 0.f) && !Same(0.f, -0.0f), "paired bits: memcmp separates zero signs");

  // F1 - the other half's normal is the self normal negated COMPONENT BY COMPONENT
  // (three fneg on the skZeroVector-based normal, 0xbf66b8-0xbf66c0) and the filter
  // (0xbf66d0) is handed that exact value; the self half keeps skZeroVector's +0.0 zeros.
  // Self rem [-1,1]^3 against the same box shifted by +-0.5 on one axis has exactly one
  // candidate: sign +1 for +0.5 (depth 1.5), sign -1 for -0.5 (depth 1.5).
  const Vec3 selfBits[3][2] = { // [rem axis][0: sign -1, 1: sign +1]
      {{1, 0, 0}, {-1, 0, 0}},          // port x <- rem x (negated)
      {{-0.0f, 0, -1}, {-0.0f, 0, 1}},  // port z <- rem y
      {{-0.0f, -1, 0}, {-0.0f, 1, 0}},  // port y <- rem z
  };
  const Vec3 otherBits[3][2] = {
      {{-1, -0.0f, -0.0f}, {1, -0.0f, -0.0f}},
      {{0, -0.0f, 1}, {0, -0.0f, -1}},
      {{0, 1, -0.0f}, {0, -1, -0.0f}},
  };
  for (int axis = 0; axis < 3; ++axis) {
    for (int s = 0; s < 2; ++s) {
      RemV mn = Rem3(-1, -1, -1), mx = Rem3(1, 1, 1);
      const float shift = s == 0 ? -0.5f : 0.5f;
      mn.v[axis] += shift;
      mx.v[axis] += shift;
      std::vector< PairedImpact > v;
      int calls = 0;
      Vec3 seen;
      const bool r = BoxBoxPairedImpacts(
          PortFromRem(Rem3(-1, -1, -1), Rem3(1, 1, 1)), PortFromRem(mn, mx),
          [&](const Vec3& n) { ++calls; seen = n; return true; }, v);
      CHECK_MSG(r && v.size() == 1 && calls == 1, "paired bits: one candidate, one filter call");
      if (v.size() != 1) continue;
      CHECK_MSG(Same(v[0].selfNormal, selfBits[axis][s]), "paired bits: self normal bits");
      CHECK_MSG(Same(v[0].otherNormal, otherBits[axis][s]) && Same(seen, otherBits[axis][s]),
                "paired bits: other normal bits and filter input");
      CHECK_MSG(Same(v[0].otherNormal.x, -v[0].selfNormal.x) &&
                    Same(v[0].otherNormal.y, -v[0].selfNormal.y) &&
                    Same(v[0].otherNormal.z, -v[0].selfNormal.z),
                "paired bits: other normal is the component-wise negation");
      CHECK_MSG(v[0].selfDepth == 1.5f && v[0].otherDepth == 1.5f, "paired bits: depth");
    }
  }

  // F2 - 0x98800 keeps SELF on a tie of the two minima (fcsel s0,s0,s2,ge at
  // 0x98814/0x98820/0x98830) and OTHER on a tie of the two maxima (fcsel s4,s6,s4,ge at
  // 0x9883c/0x98858/0x98864). Only zero signs can distinguish the two answers, and the
  // face midpoint on a tangential axis is twice the intersection minimum plus twice the
  // maximum, summed in corner order and scaled by 0.25, so the tie's zero sign survives
  // when the other end of the interval is -0.0. Port z is rem y.
  {
    // Minima tie: self's rem-y minimum is +0.0, other's is -0.0; both maxima are -0.0,
    // so ((+0.0 + +0.0) + -0.0) + -0.0 -> +0.0. Keeping other would give -0.0.
    const Box self = PortFromRem(Rem3(-1, 0.f, -1), Rem3(1, -0.0f, 1));
    const Box other = PortFromRem(Rem3(-0.5f, -0.0f, -1), Rem3(2, -0.0f, 1));
    std::vector< PairedImpact > v;
    CHECK_MSG(OnePaired(self, other, NormalPredicate(), v) && v.size() == 1 &&
                  v[0].selfDepth == 1.5f,
              "paired bits: min-tie case is one rem-x impact");
    if (v.size() == 1) {
      CHECK_MSG(Same(v[0].selfNormal, Vec3{-1, 0, 0}) &&
                    Same(v[0].otherNormal, Vec3{1, -0.0f, -0.0f}),
                "paired bits: min-tie normals");
      CHECK_MSG(Same(v[0].selfPoint.z, 0.f) && Same(v[0].otherPoint.z, 0.f),
                "paired bits: min tie keeps self (+0.0)");
    }
  }
  {
    // Maxima tie: both minima are -0.0, self's rem-y maximum is +0.0 and other's is -0.0,
    // so (((-0.0 + -0.0) + -0.0) + -0.0) -> -0.0. Keeping self would give +0.0.
    const Box self = PortFromRem(Rem3(-1, -0.0f, -1), Rem3(1, 0.f, 1));
    const Box other = PortFromRem(Rem3(-0.5f, -0.0f, -1), Rem3(2, -0.0f, 1));
    std::vector< PairedImpact > v;
    CHECK_MSG(OnePaired(self, other, NormalPredicate(), v) && v.size() == 1,
              "paired bits: max-tie case is one rem-x impact");
    if (v.size() == 1) {
      CHECK_MSG(Same(v[0].selfNormal, Vec3{-1, 0, 0}) &&
                    Same(v[0].otherNormal, Vec3{1, -0.0f, -0.0f}),
                "paired bits: max-tie normals");
      CHECK_MSG(Same(v[0].selfPoint.z, -0.0f) && Same(v[0].otherPoint.z, -0.0f),
                "paired bits: max tie keeps other (-0.0)");
    }
  }

  // A candidate whose depth is 0 is dropped by the 0xbf6650 gate BEFORE the 0xbf66d0
  // filter runs: it is neither appended nor handed to the filter. Self rem [0,2]^3,
  // other rem x[2,3] y[1,3] z[1,3] gives a depth-0 +1 candidate on rem x (dropped) and
  // depth-1 +1 candidates on rem y then rem z (filter calls 2, appends 2, no fallback).
  {
    const Box self = PortFromRem(Rem3(0, 0, 0), Rem3(2, 2, 2));
    const Box other = PortFromRem(Rem3(2, 1, 1), Rem3(3, 3, 3));
    std::vector< PairedImpact > v;
    std::vector< Vec3 > seen;
    const bool r = BoxBoxPairedImpacts(
        self, other, [&](const Vec3& n) { seen.push_back(n); return true; }, v);
    CHECK_MSG(r && v.size() == 2 && seen.size() == 2,
              "paired bits: zero-depth candidate never reaches the filter");
    if (v.size() == 2 && seen.size() == 2) {
      CHECK_MSG(Same(seen[0], Vec3{0, -0.0f, -1}) && Same(seen[1], Vec3{0, -1, -0.0f}),
                "paired bits: filter order is axis order, with the other-half normals");
      CHECK_MSG(Same(v[0].otherNormal, seen[0]) && Same(v[1].otherNormal, seen[1]),
                "paired bits: filter input is the appended other normal");
    }
  }

  // Several candidates: the filter is asked once per candidate in source order (rem x, rem
  // y, rem z; +1 before -1) and a rejection in the middle does not trigger the fallback.
  {
    const Box self = PortFromRem(Rem3(0, 0, 0), Rem3(2, 2, 2));
    const Box other = PortFromRem(Rem3(1, 1, 1), Rem3(3, 3, 3));
    std::vector< PairedImpact > v;
    std::vector< Vec3 > seen;
    const bool r = BoxBoxPairedImpacts(
        self, other, [&](const Vec3& n) { seen.push_back(n); return true; }, v);
    CHECK_MSG(r && v.size() == 3 && seen.size() == 3, "paired bits: three candidates, three calls");
    if (v.size() == 3 && seen.size() == 3) {
      CHECK_MSG(Same(seen[0], Vec3{1, -0.0f, -0.0f}) && Same(seen[1], Vec3{0, -0.0f, -1}) &&
                    Same(seen[2], Vec3{0, -1, -0.0f}),
                "paired bits: candidate call order x, y, z");
      CHECK_MSG(Same(v[0].otherNormal, seen[0]) && Same(v[1].otherNormal, seen[1]) &&
                    Same(v[2].otherNormal, seen[2]),
                "paired bits: appended normals follow the call order");
    }
    v.clear();
    seen.clear();
    int calls = 0;
    const bool rm = BoxBoxPairedImpacts(
        self, other, [&](const Vec3& n) { ++calls; seen.push_back(n); return calls != 2; }, v);
    CHECK_MSG(rm && calls == 3 && v.size() == 2 && Same(seen[1], Vec3{0, -0.0f, -1}),
              "paired bits: middle candidate rejected, others kept, no fallback");
    if (v.size() == 2)
      CHECK_MSG(Same(v[0].otherNormal, Vec3{1, -0.0f, -0.0f}) &&
                    Same(v[1].otherNormal, Vec3{0, -1, -0.0f}),
                "paired bits: rejection removes only the middle impact");
  }

  // Every primary candidate rejected: the fallback candidate (0xbf6460ff) is built AND
  // filtered, so the filter sees it as the last call. Self rem [0,2]^3, other rem
  // x[1,3] y[1,3] z[0,2] has two +1 candidates (rem x, rem y, depth 1) and no rem-z
  // candidate; the smallest difference is rem x (1 against 3), sign +1, depth 1.
  {
    const Box self = PortFromRem(Rem3(0, 0, 0), Rem3(2, 2, 2));
    const Box other = PortFromRem(Rem3(1, 1, 0), Rem3(3, 3, 2));
    std::vector< PairedImpact > v;
    int calls = 0;
    const bool ra =
        BoxBoxPairedImpacts(self, other, [&](const Vec3&) { ++calls; return false; }, v);
    CHECK_MSG(!ra && v.empty() && calls == 3,
              "paired bits: filter-all-reject consults the fallback and appends nothing");
    v.clear();
    calls = 0;
    const bool rb = BoxBoxPairedImpacts(self, other, [&](const Vec3&) { return ++calls > 2; }, v);
    CHECK_MSG(rb && calls == 3 && v.size() == 1 && v[0].selfDepth == 1.f &&
                  Same(v[0].otherNormal, Vec3{1, -0.0f, -0.0f}),
              "paired bits: the fallback impact is the one the third call accepts");
  }

  // Asymmetric, non-dyadic face centre: rem self x[0,1.3] y[0.1,0.512] z[0.1,0.56] against
  // other x[0.4,1.9] y/z unchanged, so the single candidate is rem x +1 with depth
  // 1.3f - 0.4f = 0.8999999761581421f and the intersection box is x[0.4,1.3] y[0.1,0.512]
  // z[0.1,0.56]. The corner sums round, and the two corner patterns differ in this case:
  // rem y (a1 pattern: min,min,max,max) gives ((0.1f+0.1f)+0.512f+0.512f)*0.25f =
  // 0.3059999942779541f, rem z (a2: min,max,min,max) gives ((0.1f+0.56f)+0.1f+0.56f)*
  // 0.25f = 0.33000001311302185f; the exact average would be different in both. The port
  // point is (-rem.x, rem.z, rem.y), so selfPoint = (-1.2999999523162842f,
  // 0.33000001311302185f, 0.3059999942779541f) and otherPoint takes iMin[0] = 0.4f.
  {
    const Box self = PortFromRem(Rem3(0, 0.1f, 0.1f), Rem3(1.3f, 0.512f, 0.56f));
    const Box other = PortFromRem(Rem3(0.4f, 0.1f, 0.1f), Rem3(1.9f, 0.512f, 0.56f));
    std::vector< PairedImpact > v;
    CHECK_MSG(OnePaired(self, other, NormalPredicate(), v) && v.size() == 1,
              "paired bits: asymmetric case is one rem-x impact");
    if (v.size() == 1) {
      CHECK_MSG(Same(v[0].selfDepth, 0.8999999761581421f) &&
                    Same(v[0].selfNormal, Vec3{-1, 0, 0}) &&
                    Same(v[0].otherNormal, Vec3{1, -0.0f, -0.0f}),
                "paired bits: asymmetric depth and normals");
      CHECK_MSG(Same(v[0].selfPoint, Vec3{-1.2999999523162842f, 0.33000001311302185f,
                                          0.3059999942779541f}) &&
                    Same(v[0].otherPoint, Vec3{-0.4000000059604645f, 0.33000001311302185f,
                                               0.3059999942779541f}),
                "paired bits: asymmetric non-dyadic face centres");
    }
  }
}

static void Ray() {
  const Box box{{1, -1, -1}, {3, 1, 1}};
  RayHit r = RayAABox(box, {0, 0, 0}, {1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Hit && Near(r.t, 1.f) && Near(r.normal, {-1, 0, 0}), "ray: hit");
  r = RayAABox(box, {0, 0, 0}, {1, 0, 0}, .5f);
  CHECK_MSG(r.code == kRay_Miss, "ray: too short");
  r = RayAABox(box, {2, 0, 0}, {1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Inside && r.t == 0.f, "ray: start inside");
  r = RayAABox(box, {0, 2, 0}, {1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Miss, "ray: parallel outside");
  r = RayAABox(box, {2, 0, 5}, {0, 0, -1}, 10.f);
  CHECK_MSG(r.code == kRay_Hit && Near(r.t, 4.f) && Near(r.normal, {0, 0, 1}), "ray: down onto top");
  r = RayAABox(box, {5, 0, 0}, {1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Miss, "ray: behind");
}

// ---------------------------------------------------------------------------
// CCollidableSphere::AABox_Sphere 0xc02d70 (dispatcher 0xc02cec). Box-sphere pairs
// below are built in PORT axes; the routine converts to Remastered's Y-up frame, so
// every expected normal is stated in port axes with the frame mapping in mind.
// ---------------------------------------------------------------------------

// Player-like box: 1 x 1 x 2.7, position at the feet.
static const Box kPlayerBox{{-.5f, -.5f, 0}, {.5f, .5f, 2.7f}};

static bool OneSphere(const Box& self, Vec3 center, float radius, Impact& out) {
  std::vector< Impact > v;
  if (!BoxSphereImpacts(self, center, radius, v) || v.size() != 1) return false;
  out = v[0];
  return true;
}

static void BoxSphereFaces() {
  // Six exterior face contacts, gap g = 0.25 and r = 1: the sphere half's normal is
  // radial toward the box, i.e. the port axis pointing from the sphere at the box.
  const float g = .25f, r = 1.f;
  const struct { Vec3 c; Vec3 n; } cases[6] = {
      {{.5f + g, 0, 1.35f}, {-1, 0, 0}},  // +X port face is the -X Rem face
      {{-.5f - g, 0, 1.35f}, {1, 0, 0}},  // -X port face is the +X Rem face
      {{0, .5f + g, 1.35f}, {0, -1, 0}},  // port +Y is Rem +Z
      {{0, -.5f - g, 1.35f}, {0, 1, 0}},
      {{0, 0, 2.7f + g}, {0, 0, -1}},  // port +Z is Rem +Y
      {{0, 0, -g}, {0, 0, 1}},
  };
  const char* names[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
  for (int i = 0; i < 6; ++i) {
    Impact im;
    char what[64];
    std::snprintf(what, sizeof what, "boxsphere: face %s hit", names[i]);
    CHECK_MSG(OneSphere(kPlayerBox, cases[i].c, r, im), what);
    std::snprintf(what, sizeof what, "boxsphere: face %s radial normal", names[i]);
    CHECK_MSG(Near(im.otherNormal, cases[i].n, 1e-5f), what);
    std::snprintf(what, sizeof what, "boxsphere: face %s depth r-g", names[i]);
    CHECK_MSG(Near(im.depth, r - g, 1e-5f), what);
    CHECK_MSG(Near(im.otherNormal.x * im.otherNormal.x + im.otherNormal.y * im.otherNormal.y +
                       im.otherNormal.z * im.otherNormal.z,
                   1.f, 1e-5f),
              "boxsphere: unit normal");
    CHECK_MSG(!im.otherCharacter && Near(im.otherCenter, {}), "boxsphere: flags left to the caller");
  }
  // The exterior distance is the ARM estimate refined ONCE (0xc032f0-0xc03330), not a
  // correctly-rounded root: for d2 = 0.25^2 it reads 0.249998569, so the depth is
  // 0.750001431 and not 0.75. A two-refinement or sqrt-based distance would give
  // exactly 0.75 here, which is what the second check excludes.
  Impact im;
  CHECK_MSG(OneSphere(kPlayerBox, {.75f, 0, 1.35f}, r, im), "boxsphere: one-refinement hit");
  CHECK_MSG(Near(im.depth, 1.f - 0.249998569f, 5e-8f), "boxsphere: exterior depth, one refinement");
  CHECK_MSG(!Near(im.depth, .75f, 1e-7f), "boxsphere: exterior depth is not the exact distance");
  // Appends like BoxBoxImpacts and leaves earlier impacts alone.
  std::vector< Impact > v;
  v.push_back(Terrain(.5f, {0, 1, 0}));
  CHECK_MSG(BoxSphereImpacts(kPlayerBox, {.75f, 0, 1.35f}, r, v) && v.size() == 2 &&
                Near(v[0].depth, .5f) && Near(v[0].otherNormal, {0, 1, 0}),
            "boxsphere: appends one impact");
}

static void BoxSphereEdgeCorner() {
  // Edge: outside the port +X and +Y faces. The sphere normal is radial, not a box
  // axis, and the depth is r - sqrt(2 * 0.25^2).
  Impact im;
  CHECK_MSG(OneSphere(kPlayerBox, {.75f, .75f, 1.35f}, 1.f, im), "boxsphere: edge hit");
  CHECK_MSG(Near(im.otherNormal, {-0.707106829f, -0.707106829f, 0.f}, 1e-5f),
            "boxsphere: edge radial normal");
  CHECK_MSG(std::fabs(im.otherNormal.x) > .5f && std::fabs(im.otherNormal.y) > .5f &&
                im.otherNormal.z == 0.f,
            "boxsphere: edge normal is not a box axis");
  // d2 = 0.125: the one-refinement distance is 0.353549033, the exact one 0.353553391.
  CHECK_MSG(Near(im.depth, 1.f - 0.353549033f, 5e-8f), "boxsphere: edge depth, one refinement");
  CHECK_MSG(!Near(im.depth, 1.f - .353553391f, 1e-7f),
            "boxsphere: edge depth is not the exact distance");

  // Corner: outside three faces of a cube ({-1..1}^3), gaps 0.5, r = 2. All three
  // components are radial (Rem -X, -Y, -Z map to port +X, -Z, -Y).
  const Box cube{{-1, -1, -1}, {1, 1, 1}};
  CHECK_MSG(OneSphere(cube, {1.5f, 1.5f, 1.5f}, 2.f, im), "boxsphere: corner hit");
  CHECK_MSG(Near(im.otherNormal, {-0.577350259f, -0.577350259f, -0.577350259f}, 1e-5f),
            "boxsphere: corner radial normal");
  // d2 = 0.75: one refinement 0.866020024 against 0.866025404 exact.
  CHECK_MSG(Near(im.depth, 2.f - 0.866020024f, 5e-8f), "boxsphere: corner depth, one refinement");
  CHECK_MSG(!Near(im.depth, 2.f - .866025404f, 1e-6f),
            "boxsphere: corner depth is not the exact distance");
  // The edge normal's bits: two refinements on the Rem vector (0.25, 0, -0.25) give
  // 0.707106829 while a sqrt normalization gives 0.707106769, so this pins the
  // CUnitVector3f path (0x71164 -> 0x6b6b8) rather than an invented unit vector.
  CHECK_MSG(OneSphere(kPlayerBox, {.75f, .75f, 1.35f}, 1.f, im) && im.otherNormal.x == -0.707106829f &&
                im.otherNormal.y == -0.707106829f,
            "boxsphere: edge normal uses the source normalization");

  // Tangency and misses: d2 >= r*r is rejected, exact tangency included.
  std::vector< Impact > v;
  CHECK_MSG(!BoxSphereImpacts(kPlayerBox, {.75f, 0, 1.35f}, .25f, v) && v.empty(),
            "boxsphere: exact tangency is a miss");
  CHECK_MSG(!BoxSphereImpacts(kPlayerBox, {.75f, 0, 1.35f}, .2f, v) && v.empty(),
            "boxsphere: r below the gap is a miss");
  CHECK_MSG(!BoxSphereImpacts(kPlayerBox, {5.f, 0, 1.35f}, 1.f, v) && v.empty(),
            "boxsphere: far miss");
  CHECK_MSG(BoxSphereImpacts(kPlayerBox, {.75f, 0, 1.35f}, .250001f, v) && v.size() == 1 &&
                Near(v[0].depth, .250001f - .249998569f, 5e-8f),
            "boxsphere: a hair more than the gap overlaps");
  v.clear();
}

static void BoxSphereInterior() {
  Impact im;
  // Interior, off centre: nearest signed face distance in Rem order. 0.2 below the
  // port +Z (Rem +Y) face while every other face is at least 0.5 away, so Rem +Y wins.
  CHECK_MSG(OneSphere(kPlayerBox, {0, 0, 2.5f}, 1.f, im), "boxsphere: interior hit");
  CHECK_MSG(Near(im.otherNormal, {0, 0, -1}), "boxsphere: interior normal is -nearest face");
  CHECK_MSG(Near(im.depth, 1.f + (2.7f - 2.5f), 1e-6f), "boxsphere: interior depth r + D");
  // Exact face: the centre on the port +X face (the Rem -X face), D = 0, depth exactly r
  // and the returned normal is +X in Rem, i.e. -X in port axes.
  CHECK_MSG(OneSphere(kPlayerBox, {.5f, 0, 1.35f}, 1.f, im), "boxsphere: centre on a face");
  CHECK_MSG(Near(im.depth, 1.f, 1e-7f) && Near(im.otherNormal, {-1, 0, 0}),
            "boxsphere: on-face depth r and the port -X normal");

  // Coincident centre: ties keep the Rem order -X, -Y, -Z, +X, +Y, +Z, so what comes
  // back is the negative of the winning Rem face, mapped into port axes.
  const Box cube{{-1, -1, -1}, {1, 1, 1}};
  CHECK_MSG(OneSphere(cube, {}, 2.f, im), "boxsphere: coincident hit");
  // All six faces are 1 away: -X keeps the tie against -Y and every later face, so the
  // winner is Rem -X (port +X) and the returned normal is its negative in port axes.
  CHECK_MSG(Near(im.otherNormal, {-1, 0, 0}) && Near(im.depth, 3.f), "boxsphere: tie keeps Rem -X");
  const Box wide{{-2, -1, -1}, {2, 1, 1}};
  CHECK_MSG(OneSphere(wide, {}, 2.f, im), "boxsphere: tie hit 2");
  CHECK_MSG(Near(im.otherNormal, {0, 0, 1}) && Near(im.depth, 3.f),
            "boxsphere: tie keeps Rem -Y (port +Z)");
  const Box slab{{-2, -.5f, -2}, {2, .5f, 2}};
  CHECK_MSG(OneSphere(slab, {}, 2.f, im), "boxsphere: tie hit 3");
  CHECK_MSG(Near(im.otherNormal, {0, 1, 0}) && Near(im.depth, 2.5f),
            "boxsphere: tie keeps Rem -Z (port +Y)");

  // Near/interior is |d2| < 1e-5 and is NOT an inside test: a centre just outside two
  // faces (d2 = 8e-6) still takes the face branch, whose normal is a pure axis where
  // the exterior branch would give a 45 degree radial normal.
  CHECK_MSG(OneSphere(kPlayerBox, {.502f, .502f, 1.35f}, 1.f, im), "boxsphere: near-outside hit");
  CHECK_MSG(Near(im.otherNormal, {-1, 0, 0}, 1e-6f), "boxsphere: below 1e-5 uses the face normal");
  CHECK_MSG(Near(im.depth, 1.f - .002f, 1e-5f), "boxsphere: below 1e-5 depth r - gap");
  // Just above the threshold (d2 = 1.8e-5) the exterior branch runs: radial normal,
  // depth r - d2*rsqrt_est(d2) with the one-step refinement.
  CHECK_MSG(OneSphere(kPlayerBox, {.503f, .503f, 1.35f}, 1.f, im), "boxsphere: above-threshold hit");
  CHECK_MSG(Near(im.otherNormal, {-0.707106829f, -0.707106829f, 0.f}, 1e-5f),
            "boxsphere: above 1e-5 uses the radial normal");
  CHECK_MSG(Near(im.depth, 1.f - 0.0042426406f, 1e-6f), "boxsphere: above 1e-5 depth r - L");
}

static void BoxSpherePush() {
  // The sphere contact feeds the failsafe core like any other: one contact with the
  // sphere's depth and normal, then SelectPush 0xc1fc54 decides the translation.
  std::vector< Impact > v;
  CHECK_MSG(BoxSphereImpacts(kPlayerBox, {.75f, 0, 1.35f}, 1.f, v) && v.size() == 1,
            "bs-push: producer");
  const Impact& i = v[0];
  Vec3 push;
  std::vector< Contact > list(1);
  list[0].impacts = v;
  CHECK_MSG(SelectPush(list, {}, 1.f / 60.f, push) && Near(push, Mul_(i.otherNormal, i.depth + 1e-4f)),
            "bs-push: Solid pushes along the sphere normal by depth + eps");
  // Character / CollisionActor (0x41 flags): horizontal, away from the sphere centre
  // seen from `from`, capped at worldDt * 50.
  list[0].impacts[0].otherCharacter = true;
  list[0].impacts[0].otherCenter = {.75f, 0, 1.35f};
  CHECK_MSG(SelectPush(list, {}, 1.f / 60.f, push) && Near(push, {-i.depth - 1e-4f, 0, 0}, 1e-6f),
            "bs-push: Character pushes horizontally by depth + eps");
  // A deeper overlap hits the worldDt * 50 cap (0.8333 at 60 Hz).
  std::vector< Impact > deep;
  CHECK_MSG(BoxSphereImpacts(kPlayerBox, {.75f, 0, 1.35f}, 2.f, deep) && deep.size() == 1,
            "bs-push: deep producer");
  std::vector< Contact > deepList(1);
  deepList[0].impacts = deep;
  deepList[0].impacts[0].otherCharacter = true;
  deepList[0].impacts[0].otherCenter = {.75f, 0, 1.35f};
  CHECK_MSG(deep[0].depth > 50.f / 60.f, "bs-push: deep overlap exceeds the cap");
  CHECK_MSG(SelectPush(deepList, {}, 1.f / 60.f, push) && Near(push, {-50.f / 60.f, 0, 0}),
            "bs-push: Character push is capped at worldDt * 50");
  // Centre directly above `from`: no horizontal direction, zero push, still moved.
  CHECK_MSG(SelectPush(deepList, {.75f, 0, 0}, 1.f / 60.f, push) && Near(push, {}),
            "bs-push: Character with no horizontal offset pushes nowhere");
  // The core loop (0xc1fc54) must consume the real contact and stop on the first free
  // test, exactly as it does for terrain and box actors.
  CoreHooks hooks;
  int calls = 0;
  hooks.collide = [&](const Vec3&, std::vector< Contact >& out) {
    if (++calls > 1) return false;
    out = deepList;
    return true;
  };
  bool tunneled = false;
  hooks.tunnel = [&](const Vec3& from, const Vec3&, Vec3& out) {
    tunneled = true;
    out = from;
    return false;
  };
  Vec3 out2;
  CHECK_MSG(Core({}, 1.f / 60.f, hooks, out2) && Near(out2, {-50.f / 60.f, 0, 0}) && calls == 2 &&
                tunneled,
            "bs-push: core consumes the sphere contact");

  // First pass versus fallback: the material filter (0x1270) sees the sphere, the
  // failsafe filter (0x1290) excludes Character/CollisionActor, so a position that is
  // still blocked there cannot be resolved by the second pass.
  const Vec3 cur{1, 1, 1};
  const Vec3 first = {2, 2, 2}, second = {3, 3, 3};
  Vec3 out;
  int pass[2] = {0, 0};
  auto core = [&](bool failsafe, Vec3& o) {
    ++pass[failsafe ? 1 : 0];
    o = failsafe ? second : first;
    return !failsafe; // the sphere is not in the 0x1290 filter
  };
  auto blocked = [](bool failsafe, const Vec3&) { return !failsafe; };
  CHECK_MSG(!DoCollisionFailsafe(cur, true, core, blocked, out) && Near(out, cur) && pass[0] == 1 &&
                pass[1] == 1,
            "bs-push: Character sphere is a first-pass contact only");
  auto core2 = [&](bool failsafe, Vec3& o) {
    ++pass[failsafe ? 1 : 0];
    o = failsafe ? second : first;
    return true;
  };
  CHECK_MSG(DoCollisionFailsafe(cur, true, core2, blocked, out) && Near(out, second),
            "bs-push: fallback-safe contact uses the failsafe-filter result");
}

static void SphereRay() {
  // Strictly inside: code 1, t = 0 and the wrapper's untouched skUpVector normal,
  // which is port +Z (Remastered's (0,1,0) under ToRem/FromRem).
  RayHit r = RaySphere({0, 0, 0}, 1.f, {0, 0, 0}, {1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Inside && r.t == 0.f && Near(r.normal, {0, 0, 1}),
            "sphereray: from the centre");
  r = RaySphere({0, 0, 0}, 1.f, {.25f, 0, .25f}, {0, 1, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Inside && r.t == 0.f && Near(r.normal, {0, 0, 1}),
            "sphereray: inside, off centre");
  // A translated centre is measured from the ray start, and the inside arm never
  // reads `length`.
  r = RaySphere({0, 0, 10}, 2.f, {0, 0, 9}, {0, 0, 1}, 5.f);
  CHECK_MSG(r.code == kRay_Inside && r.t == 0.f, "sphereray: inside a translated sphere");
  r = RaySphere({0, 0, 0}, 1.f, {.5f, 0, 0}, {1, 0, 0}, 0.f);
  CHECK_MSG(r.code == kRay_Inside && r.t == 0.f, "sphereray: inside with zero length");

  // The boundary is NOT inside (r2 <= d2): heading in reports the entry at t = 0,
  // heading out misses (B < 0).
  r = RaySphere({0, 0, 0}, 1.f, {1, 0, 0}, {-1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Hit && Near(r.t, 0.f, 1e-6f) && Near(r.normal, {1, 0, 0}),
            "sphereray: on the boundary heading in");
  r = RaySphere({0, 0, 0}, 1.f, {1, 0, 0}, {1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Miss, "sphereray: on the boundary heading out");

  // A straight entry along port +X: the entry point is the near side (port x = -1)
  // and the normal is radial, point - centre.
  r = RaySphere({0, 0, 0}, 1.f, {-2, 0, 0}, {1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Hit && Near(r.t, 1.f, 1e-5f) && Near(r.normal, {-1, 0, 0}, 1e-5f),
            "sphereray: entry along an axis");
  // Translated centre, down port +Z: t = 10 - 2 at (0,0,8), normal -Z.
  r = RaySphere({0, 0, 10}, 2.f, {0, 0, 0}, {0, 0, 1}, 20.f);
  CHECK_MSG(r.code == kRay_Hit && Near(r.t, 8.f, 1e-5f) && Near(r.normal, {0, 0, -1}, 1e-5f),
            "sphereray: entry into a translated sphere");
  // Off centre: the chord sits at |y| = 0.5, so t = 2 - sqrt(0.75) and the normal is
  // the radial (sqrt(0.75), 0.5, 0) in Remastered, i.e. (-0.8660254, 0.5, 0) in port.
  r = RaySphere({0, 0, 0}, 1.f, {-2, .5f, 0}, {1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Hit && Near(r.t, 1.1339746f, 1e-5f), "sphereray: off-centre entry t");
  CHECK_MSG(Near(r.normal, {-0.8660254f, .5f, 0.f}, 1e-5f), "sphereray: off-centre radial normal");
  CHECK_MSG(Near(r.normal.x * r.normal.x + r.normal.y * r.normal.y + r.normal.z * r.normal.z, 1.f,
                 1e-5f),
            "sphereray: unit normal");

  // The entry distance uses the source's root (CMath::SqrtF 0x65f70: the ARM
  // reciprocal-sqrt estimate refined TWICE, times the value), not a correctly rounded
  // sqrt. r = 1.0150001 (0x3f81eb86) is a witness: its entry t reads 0.984999776
  // (0x3f7c28f2) where a host root of the same discriminant gives 0.984999895
  // (0x3f7c28f4), so this pins the emulation rather than a plain sqrt.
  const float witnessR = 1.0150001f;
  r = RaySphere({0, 0, 0}, witnessR, {-2, 0, 0}, {1, 0, 0}, 10.f);
  const float hostRoot = 2.f - std::sqrt(witnessR * witnessR);
  CHECK_MSG(r.code == kRay_Hit && r.t == 0.984999776f, "sphereray: source sqrt of the discriminant");
  CHECK_MSG(r.t != hostRoot, "sphereray: not a correctly rounded host sqrt");

  // Misses. `t < length` is strict, so the endpoint itself misses.
  r = RaySphere({0, 0, 0}, 1.f, {-2, 0, 0}, {1, 0, 0}, 1.f);
  CHECK_MSG(r.code == kRay_Miss, "sphereray: endpoint is a miss");
  r = RaySphere({0, 0, 0}, 1.f, {-2, 0, 0}, {1, 0, 0}, 1.0001f);
  CHECK_MSG(r.code == kRay_Hit && Near(r.t, 1.f, 1e-5f), "sphereray: just past the endpoint");
  r = RaySphere({0, 0, 0}, 1.f, {-2, 0, 0}, {1, 0, 0}, .5f);
  CHECK_MSG(r.code == kRay_Miss, "sphereray: too short");
  r = RaySphere({0, 0, 0}, 1.f, {-2, 0, 0}, {1, 0, 0}, 0.f);
  CHECK_MSG(r.code == kRay_Miss, "sphereray: zero length");
  r = RaySphere({0, 0, 0}, 1.f, {-2, 0, 0}, {-1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Miss, "sphereray: pointing away");
  r = RaySphere({0, 0, 0}, 1.f, {-2, 1.5f, 0}, {1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Miss, "sphereray: past the side");
  // Exact tangency: disc == 0 is excluded (0.0 < disc is strict).
  r = RaySphere({0, 0, 0}, 1.f, {-2, 1.f, 0}, {1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Miss, "sphereray: tangent");
  // A degenerate zero radius is never strictly inside and never tangent-positive.
  r = RaySphere({0, 0, 0}, 0.f, {0, 0, 0}, {1, 0, 0}, 5.f);
  CHECK_MSG(r.code == kRay_Miss, "sphereray: zero radius at the centre");
}

int main() {
  PushSelection();
  CoreLoop();
  Wrapper();
  Tunneling();
  Floor();
  SafeOffset();
  BoxBox();
  BoxBoxPaired();
  BoxBoxRandom();
  BoxBoxPairedBits();
  Ray();
  SphereRay();
  BoxSphereFaces();
  BoxSphereEdgeCorner();
  BoxSphereInterior();
  BoxSpherePush();
  if (port_test::Failures() == 0) {
    std::printf("port_collision_failsafe: ok\n");
  }
  return PORT_TEST_RESULT();
}
