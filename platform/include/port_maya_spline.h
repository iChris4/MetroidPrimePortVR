#pragma once

// Remastered's CMayaSpline (build/mpr/volfog/D-mayaspline.md): keys with Maya tangent types,
// evaluated as a cubic Hermite in time, then clamped or wrapped into the spline's range.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "port_bytes.h"

class PortMayaSpline {
public:
  // `d` is the raw spline: u32 keys, per key f32 t, f32 v, u8 in, u8 out (+8 bytes per FIXED
  // side), then f32 min, f32 max, u8 pre, u8 post, s8 clamp mode. `used`, when given, gets
  // how many bytes it took.
  bool Load(const uint8_t* d, size_t len, size_t* used = nullptr) {
    if (len < 4) {
      return false;
    }
    const size_t n = port::ReadLE32(d);
    size_t o = 4;
    if (n > 4096) {
      return false;
    }
    for (size_t i = 0; i < n; ++i) {
      if (o + 10 > len) {
        return false;
      }
      Knot k;
      k.t = port::ReadLEFloat(d + o);
      k.v = port::ReadLEFloat(d + o + 4);
      k.in = d[o + 8];
      k.out = d[o + 9];
      o += 10;
      if (k.in > kFixed || k.out > kFixed || !std::isfinite(k.t) || !std::isfinite(k.v)) {
        return false;
      }
      for (int side = 0; side < 2; ++side) {
        if ((side == 0 ? k.in : k.out) == kFixed) {
          if (o + 8 > len) {
            return false;
          }
          float* tan = side == 0 ? k.itan : k.otan;
          tan[0] = port::ReadLEFloat(d + o);
          tan[1] = port::ReadLEFloat(d + o + 4);
          o += 8;
        }
      }
      m_k.push_back(k);
    }
    if (o + 11 > len) {
      return false;
    }
    m_min = port::ReadLEFloat(d + o);
    m_max = port::ReadLEFloat(d + o + 4);
    m_pre = d[o + 8];
    m_post = d[o + 9];
    m_clamp = int8_t(d[o + 10]);
    if (used != nullptr) {
      *used = o + 11;
    }
    for (size_t i = 0; i + 1 < m_k.size(); ++i) {  // RepairInvalidKnots
      if (m_k[i + 1].t - m_k[i].t < 0.002f) {
        m_k[i].out = kStep;
        while (i + 2 < m_k.size() && m_k[i + 2].t - m_k[i + 1].t < 0.002f) {
          m_k.erase(m_k.begin() + long(i) + 1);
        }
      }
    }
    for (size_t i = 0; i < m_k.size(); ++i) {
      Tangents(i);
    }
    return true;
  }
  size_t Size() const { return m_k.size(); }
  // GetMinTime / GetMaxTime: the first and last key's time, 0 without keys.
  float FirstTime() const { return m_k.empty() ? 0.f : m_k.front().t; }
  float LastTime() const { return m_k.empty() ? 0.f : m_k.back().t; }
  float Eval(float t) const { return Clamp(Unclamped(t)); }

private:
  enum { kLinear, kFlat, kSpline, kStep, kClamped, kFixed };
  enum { kConst, kLin, kCycle, kCycleRel, kOsc };
  static constexpr float kEps = 1.7453e-7f, kVertX = 1e-4f, kVertY = 572.957f, kSlopeCap = 5729578.f;
  struct Knot {
    float t = 0, v = 0;
    uint8_t in = 0, out = 0;
    float itan[2] = {0, 0}, otan[2] = {0, 0};
  };

  void Tangents(size_t i) {
    Knot& k = m_k[i];
    const Knot* p = i > 0 ? &m_k[i - 1] : nullptr;
    const Knot* nx = i + 1 < m_k.size() ? &m_k[i + 1] : nullptr;
    int tin = k.in, tout = k.out;
    if (p != nullptr && tin == kClamped) {
      const float d0 = std::fabs(p->v - k.v), d1 = nx != nullptr ? std::fabs(nx->v - k.v) : d0;
      if (!(d0 > 0.05f && d1 > 0.05f)) {
        tin = kFlat;
      }
    }
    bool pend = false;
    if (tin == kLinear) {
      if (p != nullptr) {
        k.itan[0] = k.t - p->t, k.itan[1] = k.v - p->v;
      } else {
        k.itan[0] = 1, k.itan[1] = 0;
      }
    } else if (tin == kFlat) {
      const Knot* a = p != nullptr ? &k : nx;
      const Knot* b = p != nullptr ? p : &k;
      if (p == nullptr && nx == nullptr) {
        a = nullptr;
      }
      k.itan[0] = a != nullptr ? a->t - b->t : 0.f, k.itan[1] = 0;
    } else if (tin == kSpline || tin == kClamped) {
      tin = kSpline;
      pend = true;
    } else if (tin == kStep) {
      k.itan[0] = 1, k.itan[1] = 0;
    }
    if (nx != nullptr && tout == kClamped) {
      const float d0 = std::fabs(nx->v - k.v), d1 = p != nullptr ? std::fabs(p->v - k.v) : d0;
      if (!(d0 > 0.05f && d1 > 0.05f)) {
        tout = kFlat;
      }
    }
    bool spl = false;
    if (tout == kLinear) {
      if (nx != nullptr) {
        k.otan[0] = nx->t - k.t, k.otan[1] = nx->v - k.v;
      } else {
        k.otan[0] = 1, k.otan[1] = 0;
      }
      spl = pend;
    } else if (tout == kFlat) {
      if (p == nullptr && nx == nullptr) {
        k.otan[0] = 0, k.otan[1] = 0;
      } else {
        const Knot* a = nx != nullptr ? nx : &k;
        const Knot* b = nx != nullptr ? &k : p;
        k.otan[0] = a->t - b->t, k.otan[1] = 0;
      }
      spl = pend;
    } else if (tout == kStep) {
      k.otan[0] = 1, k.otan[1] = 0;
      spl = pend;
    } else if (tout == kSpline || tout == kClamped) {
      tout = kSpline;
      spl = true;
    } else {
      spl = pend;
    }
    if (spl) {
      float a[2], b[2];
      if (p == nullptr && nx != nullptr) {
        a[0] = b[0] = nx->t - k.t, a[1] = b[1] = nx->v - k.v;
      } else if (p != nullptr && nx == nullptr) {
        a[0] = b[0] = k.t - p->t, a[1] = b[1] = k.v - p->v;
      } else if (p == nullptr && nx == nullptr) {
        a[0] = b[0] = 1, a[1] = b[1] = 0;
      } else {
        const float dt = nx->t - p->t, dv = nx->v - p->v;
        const float slope = dt < 1e-4f ? (dv > 0 ? kSlopeCap : -kSlopeCap) : dv / dt;
        const float dn = nx->t - k.t, dp = k.t - p->t;
        a[0] = dn < 1e-4f ? 0.f : dn, a[1] = dn < 1e-4f ? slope : slope * dn;
        b[0] = dp < 1e-4f ? 0.f : dp, b[1] = dp < 1e-4f ? slope : slope * dp;
      }
      if (tin == kSpline) {
        k.itan[0] = a[0], k.itan[1] = a[1];
      }
      if (tout == kSpline) {
        k.otan[0] = b[0], k.otan[1] = b[1];
      }
    }
    for (float* tg : {k.itan, k.otan}) {
      if (tg[0] < 0) {
        tg[0] = 0;
      }
      const float m = std::hypot(tg[0], tg[1]);
      if (std::fabs(m) >= kEps) {
        tg[0] /= m, tg[1] /= m;
      }
      if (std::fabs(tg[0]) < kEps && std::fabs(tg[1]) >= kEps) {
        tg[0] = kVertX, tg[1] = tg[1] < 0 ? -kVertY : kVertY;
      }
    }
  }

  static float Slope(const float* tg) {
    if (std::fabs(tg[0]) < kEps) {
      return tg[1] > 0 ? kSlopeCap : -kSlopeCap;
    }
    return tg[1] / tg[0];
  }

  float Unclamped(float t) const {
    const size_t n = m_k.size();
    if (n == 0) {
      return 0.f;
    }
    if (t < m_k[0].t) {
      return m_pre == kConst ? m_k[0].v : Infinity(t, true);
    }
    if (t > m_k[n - 1].t) {
      return m_post == kConst ? m_k[n - 1].v : Infinity(t, false);
    }
    long lo = 0, hi = long(n) - 1, exact = -1;
    while (lo <= hi) {
      const long mid = (lo + hi) >> 1;
      if (m_k[size_t(mid)].t > t) {
        hi = mid - 1;
      } else if (m_k[size_t(mid)].t < t) {
        lo = mid + 1;
      } else {
        exact = mid;
        break;
      }
    }
    long seg;
    if (exact >= 0) {
      if (exact == 0) {
        return m_k[0].v;
      }
      seg = exact - 1;
    } else {
      seg = lo - 1;
    }
    if (seg < 0 || size_t(seg) + 1 >= n) {
      return m_k[size_t(std::max(seg, 0L))].v;
    }
    const Knot& a = m_k[size_t(seg)];
    const Knot& b = m_k[size_t(seg) + 1];
    if (a.out == kStep) {
      return a.v;
    }
    const double m1 = Slope(a.otan), m2 = Slope(b.itan);
    const double D = double(b.t) - a.t, H = double(b.v) - a.v;
    const double A = (D * m1 + D * m2 - 2 * H) / (D * D * D);
    const double B = (3 * H - 2 * D * m1 - D * m2) / (D * D);
    const double u = double(t) - a.t;
    return float(a.v + u * (m1 + u * (B + u * A)));
  }

  float Infinity(float t, bool pre) const {
    const float first = m_k.front().t, last = m_k.back().t, span = last - first;
    const int typ = pre ? m_pre : m_post;
    if (std::fabs(span) < 1e-5f) {
      return m_k[0].v;
    }
    const float ref = last < t ? last : first;
    const float q = std::fabs(t - ref) / span;
    const float ip = q >= 0 ? std::floor(q) : std::ceil(q);
    const float frac = q - ip;
    const float ns = std::fabs(ip) + 1;
    float tm = span * std::fabs(frac);
    if (typ == kCycle || typ == kCycleRel) {
      tm = pre ? last - tm : first + tm;
    } else if (typ == kOsc) {
      const bool even = std::fabs(std::fmod(ns, 2.f)) < 1e-5f;
      tm = pre ? (even ? last - tm : first + tm) : (even ? first + tm : last - tm);
    } else if (typ == kLin) {
      if (pre) {
        const Knot& k = m_k.front();
        return std::fabs(k.itan[0]) < kEps ? k.v : k.v - (first - t) * k.itan[1] / k.itan[0];
      }
      const Knot& k = m_k.back();
      return std::fabs(k.otan[0]) < kEps ? k.v : k.v + (t - last) * k.otan[1] / k.otan[0];
    }
    float v = Clamp(Unclamped(tm));
    if (typ == kCycleRel) {
      const float off = ns * (m_k.back().v - m_k.front().v);
      v = pre ? v - off : v + off;
    }
    return v;
  }

  float Clamp(float v) const {
    if (m_clamp == 1) {
      v = m_min - v >= 0 ? m_min : v;
      v = v - m_max >= 0 ? m_max : v;
    } else if (m_clamp == 2) {
      const float rng = m_max - m_min;
      if (rng > 0) {
        if (v > m_max + 1.1920929e-07f) {
          v -= rng * float(int((v - m_max) / rng) + 1);
        } else if (v < m_min - 1.1920929e-07f) {
          v += rng * float(std::abs(int((v - m_min) / rng)) + 1);
        }
      } else {
        v = m_min;
      }
    }
    return v;
  }

  std::vector<Knot> m_k;
  float m_min = 0, m_max = 0;
  int m_pre = kConst, m_post = kConst, m_clamp = 0;
};
