#pragma once

// Port-only: Remastered's visor goo splat manager (CVisorEffectManagerMP1, Switch 1.0.0), the
// state of managed effect type 4 and its splat maths. Pure, so it can be tested alone.
// AddVisorGooSplat 0xd1078c, CalculateRandomVisorGooPosition 0xd10bcc, CRandom16::RangeF 0xb9f90,
// CVector3f::AsNormalized 0x6b238, CVector3f::Magnitude 0x6b218.
#ifdef TARGET_PC

#include <cmath>
#include <cstdint>

#include "Kyoto/CRandom16.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Particles/PortParticleMath.hpp"

namespace PortVisorGoo {

// One splat: its share of the screen-size budget and its HUD position.
struct Entry {
  uint32_t budget = 0;
  CVector3f pos = CVector3f(0.f, 0.f, 0.f);
};

// The ring of the last 8 splats, oldest at `read`; `sum` is the budgets of the live entries.
struct Ring {
  Entry slots[8];
  int read = 0;
  int write = 0;
  int count = 0;
  uint32_t sum = 0;

  void Reset() {
    read = 0;
    write = 0;
    count = 0;
    sum = 0;
  }
};

// FCVTZU: truncate toward zero, saturating to [0, 2^32 - 1]; NaN gives 0.
inline uint32_t ToU32(float v) {
  if (!(v > 0.f)) {
    return 0;
  }
  if (v >= 4294967296.f) {
    return 0xffffffffu;
  }
  return static_cast< uint32_t >(v);
}

// CRandom16::RangeF 0xb9f90: a + (b - a) * (28-bit draw * 2^-28).
inline float RangeF(CRandom16& rng, float a, float b) {
  const float d = b - a;
  const float u = rng.PortFloat28();
  const float m = d * u;
  return m + a;
}

// CVector3f::AsNormalized 0x6b238: v times twice-refined FRSQRTE of (z*z + x*x) + y*y.
inline CVector3f AsNormalized(const CVector3f& v) {
  const float zz = v.GetZ() * v.GetZ();
  const float xx = v.GetX() * v.GetX();
  const float yy = v.GetY() * v.GetY();
  const float zx = zz + xx;
  const float s = zx + yy;
  float e = PortParticleMath::ReciprocalSqrtEstimate(s);
  e *= PortParticleMath::ReciprocalSqrtStep(s, e * e);
  e *= PortParticleMath::ReciprocalSqrtStep(s, e * e);
  return CVector3f(v.GetX() * e, v.GetY() * e, v.GetZ() * e);
}

// CVector3f::Magnitude 0x6b218: SqrtF((x*x + y*y) + z*z).
inline float Magnitude(const CVector3f& v) {
  const float xx = v.GetX() * v.GetX();
  const float yy = v.GetY() * v.GetY();
  const float zz = v.GetZ() * v.GetZ();
  const float xy = xx + yy;
  return PortParticleMath::SqrtF(xy + zz);
}

// CalculateRandomVisorGooPosition 0xd10bcc. A point in the HUD plane at radius [0.4, 0.4 + w/2),
// moved off the ring's splats it lands near (up to three laps), then kept out of the centre.
inline CVector3f CalcPosition(const Ring& ring, CRandom16& rng, float w) {
  using PortParticleMath::BitsFloat;
  const float minRadius = BitsFloat(0x3ecccccd); // 0.4
  const float tiny = BitsFloat(0x3727c5ac);      // 1e-5
  const float a = RangeF(rng, 0.f, BitsFloat(0x40c90fdb));
  const float rw = w * BitsFloat(0x3effffff);
  const float r = RangeF(rng, minRadius, rw + minRadius);
  const float s = PortParticleMath::FastSinR(a);
  const float c = PortParticleMath::FastCosR(a);
  float px = r * s;
  float py = r * c;
  float pz = 0.f;
  if (ring.count == 0) {
    return CVector3f(px, py, pz);
  }
  for (int pass = 0;; ++pass) {
    bool noOverlap = true;
    int i = ring.read;
    for (int k = 0; k < ring.count; ++k) {
      const Entry& e = ring.slots[i];
      const float dx = e.pos.GetX() - px;
      const float dy = e.pos.GetY() - py;
      const float dz = e.pos.GetZ() - pz;
      const float dxx = dx * dx;
      const float dyy = dy * dy;
      const float dzz = dz * dz;
      const float dxy = dxx + dyy;
      const float d2 = dxy + dzz;
      if (d2 < BitsFloat(0x3ddf06f8)) {
        const float len = std::fabs(d2) < tiny ? 0.f : PortParticleMath::SqrtF(d2);
        float ux = 0.f;
        float uy = 1.f;
        float uz = 0.f;
        if (!(std::fabs(len) < tiny)) {
          const float inv = 1.f / len;
          ux = dx * inv;
          uy = dy * inv;
          uz = dz * inv;
        }
        const float k33 = BitsFloat(0x3ea8f5c3); // 0.33
        const float ox = ux * k33;
        const float oy = uy * k33;
        const float oz = uz * k33;
        px = e.pos.GetX() - ox;
        py = e.pos.GetY() - oy;
        pz = e.pos.GetZ() - oz;
        noOverlap = false;
      }
      i = i + 1 == 8 ? 0 : i + 1;
    }
    if (pass > 1 || noOverlap) {
      break;
    }
  }
  const CVector3f p(px, py, pz);
  const float m = Magnitude(p);
  if (m >= minRadius) {
    return p;
  }
  const CVector3f n = std::fabs(m) < tiny ? CVector3f(0.f, 1.f, 0.f) : AsNormalized(p);
  const float t = minRadius - m;
  const float cx = t * n.GetX();
  const float cy = t * n.GetY();
  const float cz = t * n.GetZ();
  const float qx = cx + px;
  const float qy = cy + py;
  const float qz = cz + pz;
  return CVector3f(-qx, -qy, -qz);
}

// Drops the oldest splats while the ring holds more than the effect's live particles.
inline void Evict(Ring& ring, int live) {
  while (ring.count > live) {
    ring.sum -= ring.slots[ring.read].budget;
    ring.read = ring.read + 1 == 8 ? 0 : ring.read + 1;
    --ring.count;
  }
}

inline void Push(Ring& ring, uint32_t budget, const CVector3f& pos) {
  const int oldCount = ring.count;
  ring.slots[ring.write].budget = budget;
  ring.slots[ring.write].pos = pos;
  ring.write = ring.write + 1 == 8 ? 0 : ring.write + 1;
  if (oldCount == 8) {
    ring.read = ring.write;
  } else {
    ++ring.count;
  }
  ring.sum += budget;
}

// The size range from the root PART's visor effect data (VESN/VESM, defaults 1.4 and 2.1),
// ordered. Without visor data lo is 0, which AddVisorGooSplat rejects.
inline void SizeRange(bool visorData, float minScale, float maxScale, float& lo, float& hi) {
  if (!visorData) {
    lo = 0.f;
    hi = 0.f;
    return;
  }
  if (minScale - maxScale >= 0.f) {
    lo = maxScale;
    hi = minScale;
  } else {
    lo = minScale;
    hi = maxScale;
  }
}

struct Splat {
  CVector3f pos;
  float size;
  uint32_t budget;
};

// AddVisorGooSplat after the managed-effect check: evicts, then decides whether a splat fits
// (live particles <= 7, room in the 1.5 * W*H/4 size budget) and draws its position and size.
// The caller binds `size`, spawns at `pos` and pushes the entry.
inline bool Plan(Ring& ring, CRandom16& rng, int live, bool visorData, float minScale,
                 float maxScale, float width, float height, Splat& out) {
  Evict(ring, live);
  if (live > 7) {
    return false;
  }
  float lo;
  float hi;
  SizeRange(visorData, minScale, maxScale, lo, hi);
  if (lo < PortParticleMath::BitsFloat(0x3c23d70a)) { // 0.01
    return false;
  }
  const float area = static_cast< float >(ToU32(width * height));
  const float area15 = area * 1.5f;
  const float area25 = area * 0.25f;
  const float sum = static_cast< float >(ring.sum);
  const float room = (area15 - sum) / area25;
  if (room < lo) {
    return false;
  }
  float w = (area25 + sum) / area15;
  if (w + -1.f >= 0.f) {
    w = 1.f;
  }
  out.pos = CalcPosition(ring, rng, w);
  const float hi2 = room - hi >= 0.f ? hi : room;
  out.size = RangeF(rng, lo, hi2);
  out.budget = ToU32(area25 * out.size);
  return true;
}

} // namespace PortVisorGoo

#endif // TARGET_PC
