// Remastered's visor goo splat manager maths (include/MetroidPrime/PortVisorGoo.hpp):
// CRandom16::RangeF, CalculateRandomVisorGooPosition, and AddVisorGooSplat's eviction,
// size budget and ring.
#include "MetroidPrime/PortVisorGoo.hpp"

#include "port_test.h"

#include <cmath>
#include <cstdint>

using namespace PortVisorGoo;

namespace {

bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

float Mag(const CVector3f& v) {
  return std::sqrt(v.GetX() * v.GetX() + v.GetY() * v.GetY() + v.GetZ() * v.GetZ());
}

Ring RingOf(std::initializer_list< Entry > entries) {
  Ring ring;
  for (const Entry& e : entries) {
    Push(ring, e.budget, e.pos);
  }
  return ring;
}

void TestRangeF() {
  CRandom16 rng(1234);
  const uint32_t seed = 1234u * 0x41c64e6du + 0x3039u;
  const float u = static_cast< float >(seed & 0xfffffff) * 3.7252902984619140625e-09f;
  const float expected = (5.f - 2.f) * u + 2.f;
  CHECK_EQ(RangeF(rng, 2.f, 5.f), expected);
  CHECK_EQ(rng.GetSeed(), seed);
  CRandom16 low(7);
  for (int i = 0; i < 1000; ++i) {
    const float v = RangeF(low, -1.f, 1.f);
    CHECK(v >= -1.f && v < 1.f);
  }
}

void TestFirstSplatPosition() {
  // An empty ring: the raw draw, radius [0.4, 0.4 + w/2), in the HUD plane, two draws.
  for (uint32_t s = 1; s < 200; ++s) {
    const float w = (s % 5) * 0.25f;
    CRandom16 rng(s);
    CRandom16 ref(s);
    const float a = RangeF(ref, 0.f, PortParticleMath::BitsFloat(0x40c90fdb));
    const float r =
        RangeF(ref, 0.4f, w * PortParticleMath::BitsFloat(0x3effffff) + 0.4f);
    const CVector3f p = CalcPosition(Ring(), rng, w);
    CHECK_EQ(rng.GetSeed(), ref.GetSeed());
    CHECK_EQ(p.GetX(), r * PortParticleMath::FastSinR(a));
    CHECK_EQ(p.GetY(), r * PortParticleMath::FastCosR(a));
    CHECK_EQ(p.GetZ(), 0.f);
    CHECK(Mag(p) > 0.399f && Mag(p) < 0.4f + w * 0.5f + 1e-3f);
  }
}

// The point the draw gives before any overlap handling.
CVector3f RawPoint(uint32_t seed, float w) {
  CRandom16 rng(seed);
  return CalcPosition(Ring(), rng, w);
}

void TestOverlapMovesAway() {
  // A splat 0.1 to the right of the drawn point: moved 0.33 left of that splat, and the far
  // splat is ignored.
  const uint32_t seed = 42;
  const CVector3f p = RawPoint(seed, 1.f);
  const CVector3f e(p.GetX() + 0.1f, p.GetY(), 0.f);
  const Ring ring = RingOf({{10, CVector3f(5.f, 5.f, 0.f)}, {20, e}});
  CRandom16 rng(seed);
  const CVector3f q = CalcPosition(ring, rng, 1.f);
  if (Mag(q) >= 0.4f) {
    CHECK(Near(q.GetX(), e.GetX() - 0.33f));
    CHECK(Near(q.GetY(), e.GetY()));
  } else {
    CHECK(Near(Mag(q), 0.4f, 1e-4f));
  }
}

void TestCoincidentSplatMovesDown() {
  // On top of a splat (no direction): moved by (0, 1, 0) * -0.33.
  const uint32_t seed = 9;
  const CVector3f p = RawPoint(seed, 1.f);
  const Ring ring = RingOf({{1, p}});
  CRandom16 rng(seed);
  const CVector3f q = CalcPosition(ring, rng, 1.f);
  const CVector3f expected(p.GetX(), p.GetY() - 0.33f, 0.f);
  if (Mag(expected) >= 0.4f) {
    CHECK(Near(q.GetX(), expected.GetX()));
    CHECK(Near(q.GetY(), expected.GetY()));
  } else {
    CHECK(Near(Mag(q), 0.4f, 1e-4f));
  }
}

void TestMinimumRadius() {
  // A splat pushes the point into the centre: it ends on the 0.4 circle, mirrored through it.
  int checked = 0;
  for (uint32_t seed = 1; seed < 400 && checked < 20; ++seed) {
    const CVector3f p = RawPoint(seed, 0.f);
    const float m = Mag(p);
    // A splat just outside p: p moves 0.33 inwards, to radius m - 0.33 + 0.01.
    const float k = (m + 0.01f) / m;
    const CVector3f e(p.GetX() * k, p.GetY() * k, 0.f);
    CRandom16 rng(seed);
    const CVector3f q = CalcPosition(RingOf({{1, e}}), rng, 0.f);
    const float inner = m + 0.01f - 0.33f;
    if (inner <= 0.f || inner > 0.3f) {
      continue;
    }
    ++checked;
    CHECK(Near(Mag(q), 0.4f, 1e-4f));
    // Opposite the splat.
    CHECK(q.GetX() * e.GetX() + q.GetY() * e.GetY() < 0.f);
  }
  CHECK(checked > 0);
}

void TestEvict() {
  Ring ring = RingOf({{1, {}}, {2, {}}, {4, {}}, {8, {}}, {16, {}}});
  CHECK_EQ(ring.count, 5);
  CHECK_EQ(ring.sum, 31u);
  Evict(ring, 3);
  CHECK_EQ(ring.count, 3);
  CHECK_EQ(ring.read, 2);
  CHECK_EQ(ring.write, 5);
  CHECK_EQ(ring.sum, 28u);
  Evict(ring, 7);
  CHECK_EQ(ring.count, 3);
  Evict(ring, 0);
  CHECK_EQ(ring.count, 0);
  CHECK_EQ(ring.sum, 0u);
  CHECK_EQ(ring.read, ring.write);
}

void TestRingWraps() {
  Ring ring;
  for (int i = 0; i < 6; ++i) {
    Push(ring, 1, {});
  }
  Evict(ring, 0);
  CHECK_EQ(ring.read, 6);
  for (int i = 0; i < 8; ++i) {
    Push(ring, 1u << i, CVector3f(float(i), 0.f, 0.f));
  }
  CHECK_EQ(ring.count, 8);
  CHECK_EQ(ring.write, 6);
  CHECK_EQ(ring.read, 6);
  CHECK_EQ(ring.slots[6].pos.GetX(), 0.f);
  CHECK_EQ(ring.slots[5].pos.GetX(), 7.f);
  // Full: the next push overwrites the oldest and read follows write.
  Push(ring, 0, CVector3f(8.f, 0.f, 0.f));
  CHECK_EQ(ring.count, 8);
  CHECK_EQ(ring.write, 7);
  CHECK_EQ(ring.read, 7);
  // Evicting from a full ring starts at read and wraps.
  Evict(ring, 6);
  CHECK_EQ(ring.read, 1);
}

void TestSizeRange() {
  float lo, hi;
  SizeRange(true, 1.1f, 2.1f, lo, hi);
  CHECK_EQ(lo, 1.1f);
  CHECK_EQ(hi, 2.1f);
  SizeRange(true, 3.f, 2.1f, lo, hi);
  CHECK_EQ(lo, 2.1f);
  CHECK_EQ(hi, 3.f);
  SizeRange(false, 1.4f, 2.1f, lo, hi);
  CHECK_EQ(lo, 0.f);
}

void TestToU32() {
  CHECK_EQ(ToU32(-3.f), 0u);
  CHECK_EQ(ToU32(NAN), 0u);
  CHECK_EQ(ToU32(2.9f), 2u);
  CHECK_EQ(ToU32(1e20f), 0xffffffffu);
}

void TestPlan() {
  const float W = 1920.f;
  const float H = 1080.f;
  const float area25 = W * H * 0.25f;
  // First splat: size in [1.1, 2.1), budget = area/4 * size.
  {
    Ring ring;
    CRandom16 rng(5);
    Splat s;
    REQUIRE(Plan(ring, rng, 0, true, 1.1f, 2.1f, W, H, s));
    CHECK(s.size >= 1.1f && s.size < 2.1f);
    CHECK_EQ(s.budget, ToU32(area25 * s.size));
    CHECK(Mag(s.pos) >= 0.399f);
  }
  // More than 7 live particles, no visor data, or a too small lower bound: nothing.
  {
    Ring ring;
    CRandom16 rng(5);
    Splat s;
    CHECK(!Plan(ring, rng, 8, true, 1.1f, 2.1f, W, H, s));
    CHECK(!Plan(ring, rng, 0, false, 1.1f, 2.1f, W, H, s));
    CHECK(!Plan(ring, rng, 0, true, 0.005f, 2.1f, W, H, s));
    CHECK_EQ(rng.GetSeed(), 5u);
  }
  // The budget is 1.5 screens, 6 sizes of a quarter screen: 4.8 used leaves 1.2 for [1.1, 2.1).
  {
    Ring ring = RingOf({{ToU32(area25 * 4.8f), {}}});
    CRandom16 rng(11);
    Splat s;
    REQUIRE(Plan(ring, rng, 1, true, 1.1f, 2.1f, W, H, s));
    CHECK(s.size >= 1.1f && s.size <= 1.2f + 1e-5f);
  }
  // No room left for the lower bound.
  {
    Ring ring = RingOf({{ToU32(area25 * 5.5f), {}}});
    CRandom16 rng(11);
    Splat s;
    CHECK(!Plan(ring, rng, 1, true, 1.1f, 2.1f, W, H, s));
    CHECK_EQ(rng.GetSeed(), 11u);
  }
  // A dead particle drops the oldest splat first, which frees its budget.
  {
    Ring ring = RingOf({{ToU32(area25 * 5.5f), {}}, {ToU32(area25 * 0.1f), {}}});
    CRandom16 rng(3);
    Splat s;
    REQUIRE(Plan(ring, rng, 1, true, 1.1f, 2.1f, W, H, s));
    CHECK_EQ(ring.count, 1);
    CHECK_EQ(ring.sum, ToU32(area25 * 0.1f));
  }
}

void TestAsNormalized() {
  const CVector3f n = AsNormalized(CVector3f(0.03f, -0.2f, 0.f));
  CHECK(Near(Mag(n), 1.f, 1e-6f));
  CHECK(n.GetY() < 0.f);
}

} // namespace

int main() {
  TestRangeF();
  TestFirstSplatPosition();
  TestOverlapMovesAway();
  TestCoincidentSplatMovesDown();
  TestMinimumRadius();
  TestEvict();
  TestRingWraps();
  TestSizeRange();
  TestToU32();
  TestPlan();
  TestAsNormalized();
  return PORT_TEST_RESULT();
}
