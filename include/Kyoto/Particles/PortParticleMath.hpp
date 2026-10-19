#pragma once

// Port-only particle element math that needs no engine state, so it can be tested alone.
// All follow Remastered (Switch 1.0.0): DistanceFromCameraBlend::CalculateBlendValue 0x298f44,
// CCEGradient::GetValue 0x2c8d48 and CVERandomVector::GetValue 0x29e954.
#ifdef TARGET_PC

#include <cmath>
#include <cstdint>
#include <cstring>

#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"

namespace PortParticleMath {

// XFMD3 particles already carry the emitter's global orientation in their positions.
// Their center/sort transform therefore removes only the camera rotation; the legacy
// system-camera transform remains in use for XFMD2/default positions and directions.
inline CVector3f ParticlePositionForSort(const CTransform4f& cameraRotationInverse,
                                         const CTransform4f& systemCameraCopy,
                                         bool followsEmitter, const CVector3f& current,
                                         const CVector3f& previous, float deltaScale) {
  const CVector3f pos = (current - previous) * deltaScale + previous;
  const CTransform4f& positionCameraCopy = followsEmitter ? cameraRotationInverse : systemCameraCopy;
  return positionCameraCopy * pos;
}

inline CVector3f ParticleDirectionInCamera(const CTransform4f& systemCameraCopy,
                                           const CVector3f& direction) {
  return systemCameraCopy.Rotate(direction);
}

// DFCS/DFCP: 1 at distance a, 0 at distance b, clamped; 0 when a and b coincide.
inline float DistanceBlend(float d, float a, float b) {
  if (std::fabs(a - b) < 1e-5f) {
    return 0.f;
  }
  const float t = (d - b) / (a - b);
  return t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
}

struct GradientStop {
  float color[4];
  float position;
};

// GRAD: `frame` is the integer age in frames, `period` the int element's value (below 2 it is
// 1). The position is min(frame, period) / period, or (frame mod period) / period when it
// repeats; stops hold at both ends and every channel (alpha too) lerps linearly. Returns false
// with out untouched-to-zero when no stop brackets the position (Remastered returns 0 then).
inline bool EvalGradient(const GradientStop* stops, int count, int period, bool repeat, int frame, float out[4]) {
  if (period < 2) {
    period = 1;
  }
  float t;
  if (repeat) {
    int m = frame % period;
    if (m < 0) {
      m += period;
    }
    t = float(m) / float(period);
  } else {
    t = float(frame < period ? frame : period) / float(period);
  }
  for (int c = 0; c < 4; ++c) {
    out[c] = 0.f;
  }
  if (count <= 0) {
    return false;
  }
  if (t <= stops[0].position) {
    for (int c = 0; c < 4; ++c) {
      out[c] = stops[0].color[c];
    }
    return true;
  }
  for (int i = 1; i < count; ++i) {
    if (t <= stops[i].position) {
      const float a = stops[i - 1].position;
      const float b = stops[i].position;
      const float f = b > a ? (t - a) / (b - a) : 1.f;
      for (int c = 0; c < 4; ++c) {
        out[c] = stops[i - 1].color[c] + (stops[i].color[c] - stops[i - 1].color[c]) * f;
      }
      return true;
    }
  }
  for (int c = 0; c < 4; ++c) {
    out[c] = stops[count - 1].color[c];
  }
  return true;
}

inline uint32_t FloatBits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

inline float BitsFloat(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

// FRSQRTE as the Switch runs it: ARM RecipSqrtEstimate's 8-fraction-bit integer construction
// (no FEAT_RPRES), not the host's rsqrt.
inline float ReciprocalSqrtEstimate(float v) {
  const uint32_t bits = FloatBits(v);
  int exponent = int((bits >> 23) & 255);
  uint32_t fraction = bits & 0x7fffff;
  if (exponent == 255) {
    return fraction ? BitsFloat(bits | 0x400000) : 0.f;
  }
  if ((bits & 0x7fffffff) == 0) {
    return BitsFloat((bits & 0x80000000) | 0x7f800000);
  }
  if (bits >> 31) {
    return BitsFloat(0x7fc00000);
  }
  if (exponent == 0) {
    while ((fraction & 0x400000) == 0) {
      fraction <<= 1;
      --exponent;
    }
    fraction = (fraction & 0x3fffff) << 1;
  }
  uint32_t scaled = (exponent & 1) ? (128 | (fraction >> 16)) : (256 | (fraction >> 15));
  scaled = scaled < 256 ? scaled * 2 + 1 : ((scaled & ~1u) + 1) * 2;
  uint32_t b = 512;
  while (scaled * (b + 1) * (b + 1) < (1u << 28)) {
    ++b;
  }
  return BitsFloat((uint32_t((380 - exponent) / 2) << 23) | (((b + 1) / 2 & 255) << 15));
}

// FRSQRTS: fused (3 - a*b) / 2; zero times infinity gives 1.5.
inline float ReciprocalSqrtStep(float a, float b) {
  if ((a == 0.f && std::isinf(b)) || (b == 0.f && std::isinf(a))) {
    return 1.5f;
  }
  return std::fma(-a, b, 3.f) * .5f;
}

// RNDV (CVERandomVector::GetValue 0x29e954): three draws in [0, 1) (CRandom16::GetVector3f
// 0xba108) mapped to the cube [-1, 1)^3, scaled to `magnitude` by one refined FRSQRTE of
// (x*x + y*y) + z*z. A cube point, so the directions are not uniform over the sphere.
inline CVector3f RandomVector(float u0, float u1, float u2, float magnitude) {
  const float x = (u0 + u0) + -1.f;
  const float y = (u1 + u1) + -1.f;
  const float z = (u2 + u2) + -1.f;
  // Separate statements: Remastered uses unfused fmul/fadd, and clang contracts
  // within one expression on arm64.
  const float xx = x * x;
  const float yy = y * y;
  const float zz = z * z;
  const float xy = xx + yy;
  const float squared = xy + zz;
  float reciprocal = ReciprocalSqrtEstimate(squared);
  reciprocal *= ReciprocalSqrtStep(squared, reciprocal * reciprocal);
  const float scale = magnitude * reciprocal;
  return CVector3f(x * scale, y * scale, z * scale);
}

// FRECPE as the Switch runs it: ARM RecipEstimate's 8-fraction-bit integer construction (no
// FEAT_RPRES), round to nearest, denormals not flushed.
inline float ReciprocalEstimate(float v) {
  const uint32_t bits = FloatBits(v);
  const uint32_t sign = bits & 0x80000000;
  int exponent = int((bits >> 23) & 255);
  uint32_t fraction = bits & 0x7fffff;
  if (exponent == 255) {
    return fraction ? BitsFloat(bits | 0x400000) : BitsFloat(sign);
  }
  // Zero, and anything below 2^-128, overflows to infinity.
  if ((bits & 0x7fffffff) < 0x200000) {
    return BitsFloat(sign | 0x7f800000);
  }
  if (exponent == 0) {
    if ((fraction & 0x400000) == 0) {
      exponent = -1;
      fraction = (fraction << 2) & 0x7fffff;
    } else {
      fraction = (fraction << 1) & 0x7fffff;
    }
  }
  const uint32_t scaled = 256 | (fraction >> 15);
  const uint32_t estimate = ((1u << 19) / (scaled * 2 + 1) + 1) / 2;
  int resultExponent = 253 - exponent;
  uint32_t resultFraction = (estimate & 255) << 15;
  if (resultExponent == 0) {
    resultFraction = 0x400000 | (resultFraction >> 1);
  } else if (resultExponent == -1) {
    resultFraction = 0x200000 | (resultFraction >> 2);
    resultExponent = 0;
  }
  return BitsFloat(sign | (uint32_t(resultExponent) << 23) | resultFraction);
}

// FRECPS: fused 2 - a*b; zero times infinity gives 2.
inline float ReciprocalStep(float a, float b) {
  if ((a == 0.f && std::isinf(b)) || (b == 0.f && std::isinf(a))) {
    return 2.f;
  }
  return std::fma(-a, b, 2.f);
}

// One component of CCoordsExt::UpdateInverseScale 0x2a6390: FRECPE and two FRECPS refinements.
inline float InverseScale(float s) {
  float e = ReciprocalEstimate(s);
  e *= ReciprocalStep(e, s);
  return e * ReciprocalStep(e, s);
}

// CMath::SqrtF 0x65f70: 0 for v <= 0, else v times twice-refined FRSQRTE of max(v, 1e-35).
inline float SqrtF(float v) {
  if (!(v > 0.f)) {
    return 0.f;
  }
  const float m = std::fmax(v, BitsFloat(0x554ad2e));
  float e = ReciprocalSqrtEstimate(m);
  e *= ReciprocalSqrtStep(m, e * e);
  e *= ReciprocalSqrtStep(m, e * e);
  return v * e;
}

// NORM (CVENormalizeVector::GetValue 0x29e620): scaled by 1/|v| (CVector3f::Magnitude 0x6b218,
// (x*x + y*y) + z*z through SqrtF) when |v| > 2^-23, else unchanged.
inline CVector3f NormalizeVector(const CVector3f& v) {
  const float xx = v.GetX() * v.GetX();
  const float yy = v.GetY() * v.GetY();
  const float zz = v.GetZ() * v.GetZ();
  const float xy = xx + yy;
  const float magnitude = SqrtF(xy + zz);
  if (!(magnitude > BitsFloat(0x34000000))) {
    return v;
  }
  const float inverse = 1.f / magnitude;
  return CVector3f(inverse * v.GetX(), inverse * v.GetY(), inverse * v.GetZ());
}

// CMath::InvSqrtF 0x66090: twice-refined FRSQRTE of max(v, 1e-35).
inline float InvSqrtF(float v) {
  const float m = std::fmax(v, BitsFloat(0x554ad2e));
  float e = ReciprocalSqrtEstimate(m);
  e *= ReciprocalSqrtStep(m, e * e);
  return e * ReciprocalSqrtStep(m, e * e);
}

// CParticleValues::skMinNormalizableMagSqd (0x21109c8).
constexpr float kMinNormalizableMagSqd = 1.4210854715202004e-14f;

// CParticleMath::CrossIntoMatrixRow 0x286d70: a x b scaled by InvSqrtF of its squared length;
// false (out untouched) when that is below kMinNormalizableMagSqd.
inline bool NormalizedCross(const CVector3f& a, const CVector3f& b, CVector3f& out) {
  const float a1b2 = a.GetY() * b.GetZ(), a2b1 = a.GetZ() * b.GetY();
  const float a2b0 = a.GetZ() * b.GetX(), a0b2 = a.GetX() * b.GetZ();
  const float a0b1 = a.GetX() * b.GetY(), a1b0 = a.GetY() * b.GetX();
  const float cx = a1b2 - a2b1, cy = a2b0 - a0b2, cz = a0b1 - a1b0;
  const float xx = cx * cx, yy = cy * cy, zz = cz * cz;
  const float xy = xx + yy;
  const float squared = zz + xy;
  if (squared < kMinNormalizableMagSqd) {
    return false;
  }
  const float s = InvSqrtF(squared);
  out = CVector3f(cx * s, cy * s, cz * s);
  return true;
}

// RAZY (CERotationFromAxis::GetValue 0x29d298 as GetRotationElement 0x25e418 builds it: axis pair
// 1, normalised row 2, second pair 1): Z = the first vector normalised, X = normalize(second x Z),
// Y = normalize(Z x X), each under kMinNormalizableMagSqd or false (the caller's fallback). The
// rotation is conj(FromMatrix(rows X, Y, Z)), the one whose columns are X, Y, Z.
inline bool RotationFromAxisZY(const CVector3f& first, const CVector3f& second, CVector3f& x,
                               CVector3f& y, CVector3f& z) {
  const float xx = first.GetX() * first.GetX();
  const float yy = first.GetY() * first.GetY();
  const float zz = first.GetZ() * first.GetZ();
  const float xy = xx + yy;
  const float squared = xy + zz;
  if (squared < kMinNormalizableMagSqd) {
    return false;
  }
  const float s = InvSqrtF(squared);
  z = CVector3f(s * first.GetX(), s * first.GetY(), s * first.GetZ());
  return NormalizedCross(second, z, x) && NormalizedCross(z, x, y);
}

// CPSS (CVECameraPositionInSystemSpace::GetValue 0x2a24fc): the camera position in the system's
// global frame, conj(q) * (camera - t) * q with q = (w, x, y, z) unnormalised, then times
// InverseScale(s) per axis. Separate statements: Remastered uses unfused fmul/fadd/fsub.
inline CVector3f CameraPositionInSystemSpace(const CVector3f& camera, const CVector3f& t,
                                             float w, float x, float y, float z,
                                             const CVector3f& s) {
  const float d0 = camera.GetX() - t.GetX();
  const float d1 = camera.GetY() - t.GetY();
  const float d2 = camera.GetZ() - t.GetZ();
  const float d0x = d0 * x, d1y = d1 * y, d2z = d2 * z;
  const float dot = (d0x + d1y) + d2z;
  const float d0w = d0 * w, d2y = d2 * y, d1z = d1 * z;
  const float b = d1z + (d0w - d2y);
  const float d1w = d1 * w, d0z = d0 * z, d2x = d2 * x;
  const float c = d2x + (d1w - d0z);
  const float d2w = d2 * w, d1x = d1 * x, d0y = d0 * y;
  const float e = d0y + (d2w - d1x);
  const float wb = w * b, xa = x * dot, zc = z * c, ye = y * e;
  const float ya = y * dot, za = z * dot, zb = z * b, yb = y * b;
  const float we = w * e, wc = w * c, xe = x * e, xc = x * c;
  const float outX = (zc + (xa + wb)) - ye;
  const float outY = (xe + (ya + wc)) - zb;
  const float outZ = (yb + (za + we)) - xc;
  return CVector3f(InverseScale(s.GetX()) * outX, InverseScale(s.GetY()) * outY,
                   InverseScale(s.GetZ()) * outZ);
}

// FCVTZS to 32 bits: toward zero, saturating, NaN to 0.
inline int32_t ConvertToInt32(float v) {
  if (std::isnan(v)) {
    return 0;
  }
  if (v >= 2147483648.f) {
    return INT32_MAX;
  }
  if (v <= -2147483648.f) {
    return INT32_MIN;
  }
  return int32_t(v);
}

// The angle wrap CMath::FastSinR 0x691a0 and FastCosR 0x69280 open with, which
// CVEAngleCone::GetValue 0x29eebc and CVEAngleSphere::GetValue 0x2cb100 inline: t when
// |t| <= pi, else t less its whole turns, then moved by a turn when outside [-pi, pi].
inline float WrapAngle(float t) {
  const float pi = BitsFloat(0x40490fdb);
  const float negPi = BitsFloat(0xc0490fdb);
  const float turn = BitsFloat(0x40c90fdb);
  const float negTurn = BitsFloat(0xc0c90fdb);
  if (std::fabs(t) <= pi) {
    return t;
  }
  const float turns = t * BitsFloat(0x3e22f983);
  const float back = float(ConvertToInt32(turns)) * negTurn;
  const float r = t + back;
  const float minus = r + negTurn;
  const float plus = r + turn;
  const float above = r + negPi;
  const float below = negPi - r;
  const float low = below >= 0.f ? plus : r;
  return above >= 0.f ? minus : low;
}

// CMath::FastSinR 0x691a0: odd degree-7 polynomial of the wrapped angle.
inline float FastSinR(float t) {
  t = WrapAngle(t);
  const float x2 = t * t;
  const float t1 = t * BitsFloat(0x3f7ff347);
  const float x3 = t * x2;
  const float t3 = x3 * BitsFloat(0xbe2a34ae);
  const float x5 = x2 * x3;
  const float s3 = t1 + t3;
  const float t5 = x5 * BitsFloat(0x3c047fca);
  const float x7 = x2 * x5;
  const float s5 = s3 + t5;
  const float t7 = x7 * BitsFloat(0xb9206873);
  return s5 + t7;
}

// CMath::FastCosR 0x69280: even degree-8 polynomial of the wrapped angle.
inline float FastCosR(float t) {
  t = WrapAngle(t);
  const float x2 = t * t;
  const float t2 = x2 * BitsFloat(0xbefffd62);
  const float s2 = t2 + 1.f;
  const float x4 = x2 * x2;
  const float t4 = x4 * BitsFloat(0x3d2a7a18);
  const float s4 = s2 + t4;
  const float x6 = x2 * x4;
  const float t6 = x6 * BitsFloat(0xbab2bb2b);
  const float s6 = s4 + t6;
  const float x8 = x2 * x6;
  const float t8 = x8 * BitsFloat(0x37a93188);
  return s6 + t8;
}

// The parabolic sine CVEAngleCone/CVEAngleSphere inline, of an already wrapped angle:
// h = 4/pi t - 4/pi^2 t|t|, then h + .225 (h|h| - h).
inline float ParabolicSin(float t) {
  const float m = t * BitsFloat(0x3fa2f983);
  const float tb = t * BitsFloat(0x3ecf817a);
  const float n = tb * std::fabs(t);
  const float h = m - n;
  const float ah = std::fabs(h);
  const float hh = h * ah;
  const float d = hh - h;
  const float p = d * BitsFloat(0x3e666666);
  return h + p;
}

struct Quat {
  float w, x, y, z;
};

// CQuaternion::FromMatrix 0xd2de4 of nine row-major floats.
inline Quat QuatFromMatrix(const float m[9]) {
  const float d01 = m[0] + m[4];
  const float trace = d01 + m[8];
  Quat q;
  if (trace > 0.f) {
    const float s = trace + 1.f;
    const float inv = InvSqrtF(s);
    const float root = s * inv;
    const float k = inv * .5f;
    q.w = root * .5f;
    const float dx = m[7] - m[5];
    const float dy = m[2] - m[6];
    const float dz = m[3] - m[1];
    q.x = k * dx;
    q.y = k * dy;
    q.z = k * dz;
    return q;
  }
  int i = m[4] > m[0] ? 1 : 0;
  if (m[8] > m[4 * i]) {
    i = 2;
  }
  static const int kNext[3] = {1, 2, 0};
  const int j = kNext[i];
  const int k = kNext[j];
  const float jk = m[4 * j] + m[4 * k];
  const float d = m[4 * i] - jk;
  const float s = d + 1.f;
  const float inv = InvSqrtF(s);
  const float root = inv * s;
  const float half = inv * .5f;
  float v[3];
  v[i] = root * .5f;
  const float sj = m[3 * j + i] + m[3 * i + j];
  v[j] = half * sj;
  const float sk = m[3 * k + i] + m[3 * i + k];
  v[k] = half * sk;
  const float sw = m[3 * k + j] - m[3 * j + k];
  q.w = half * sw;
  q.x = v[0];
  q.y = v[1];
  q.z = v[2];
  return q;
}

// REUL in axis order 0 (CERotationEuler::GetValue 0x29c944, QuatGeneric 0x29ca18): the angles in
// degrees, each axis's FastSinR/FastCosR pair scaled by one refined FRSQRTE of s^2 + c^2, the
// matrix Rz * Ry * Rx built term by term, then FromMatrix.
inline Quat EulerQuat(float xDegrees, float yDegrees, float zDegrees) {
  const float degrees = BitsFloat(0x3c8efa35);
  const float angles[3] = {xDegrees * degrees, yDegrees * degrees, zDegrees * degrees};
  float s[3], c[3];
  for (int i = 0; i < 3; ++i) {
    s[i] = FastSinR(angles[i]);
    c[i] = FastCosR(angles[i]);
  }
  for (int i = 0; i < 3; ++i) {
    const float ss = s[i] * s[i];
    const float cc = c[i] * c[i];
    const float n = ss + cc;
    const float e = ReciprocalSqrtEstimate(n);
    const float r = e * ReciprocalSqrtStep(n, e * e);
    s[i] = s[i] * r;
    c[i] = c[i] * r;
  }
  const float sx = s[0], sy = s[1], sz = s[2];
  const float cx = c[0], cy = c[1], cz = c[2];
  const float sxsy = sx * sy;
  const float cxsy = cx * sy;
  float m[9];
  m[0] = cy * cz;
  const float sxsycz = sxsy * cz, cxsz = cx * sz;
  m[1] = sxsycz - cxsz;
  const float cxsycz = cxsy * cz, sxsz = sx * sz;
  m[2] = cxsycz + sxsz;
  m[3] = cy * sz;
  const float sxsysz = sxsy * sz, cxcz = cx * cz;
  m[4] = sxsysz + cxcz;
  const float cxsysz = cxsy * sz, sxcz = sx * cz;
  m[5] = cxsysz - sxcz;
  m[6] = -sy;
  m[7] = sx * cy;
  m[8] = cx * cy;
  return QuatFromMatrix(m);
}

// CQuaternion::Transform 0xd3f2c: v turned by q (unnormalised), operation for operation.
inline CVector3f QuatTransform(const Quat& q, const CVector3f& v) {
  const float vx = v.GetX(), vy = v.GetY(), vz = v.GetZ();
  const float vxx = vx * q.x, vyy = vy * q.y, vzz = vz * q.z;
  const float dot2 = vxx + vyy;
  const float dot = dot2 + vzz;
  const float vzy = vz * q.y, vxw = vx * q.w, vyz = vy * q.z;
  const float b2 = vzy + vxw;
  const float b = b2 - vyz;
  const float vxz = vx * q.z, vyw = vy * q.w, vzx = vz * q.x;
  const float c2 = vxz + vyw;
  const float c = c2 - vzx;
  const float vyx = vy * q.x, vzw = vz * q.w, vxy = vx * q.y;
  const float e2 = vyx + vzw;
  const float e = e2 - vxy;
  const float xd = q.x * dot, wb = q.w * b, zc = q.z * c, ye = q.y * e;
  const float x1 = xd + wb;
  const float x2 = x1 - zc;
  const float yd = q.y * dot, wc = q.w * c, xe = q.x * e, zb = q.z * b;
  const float y1 = yd + wc;
  const float y2 = y1 - xe;
  const float zd = q.z * dot, we = q.w * e, yb = q.y * b, xc = q.x * c;
  const float z1 = zd + we;
  const float z2 = z1 - yb;
  return CVector3f(x2 + ye, y2 + zb, z2 + xc);
}

// CQuaternion::Multiply 0xd43d8: a * b, operation for operation.
inline Quat QuatMultiply(const Quat& a, const Quat& b) {
  const float xx = a.x * b.x, yy = a.y * b.y, zz = a.z * b.z;
  const float dot = (xx + yy) + zz;
  Quat q;
  q.w = a.w * b.w - dot;
  const float xw = a.x * b.w, wx = a.w * b.x, yz = a.y * b.z, zy = a.z * b.y;
  q.x = ((xw + wx) + yz) - zy;
  const float yw = a.y * b.w, wy = a.w * b.y, zx = a.z * b.x, xz = a.x * b.z;
  q.y = (zx + (yw + wy)) - xz;
  const float zw = a.z * b.w, wz = a.w * b.z, xy = a.x * b.y, yx = a.y * b.x;
  q.z = (xy + (zw + wz)) - yx;
  return q;
}

// FMULX: a product, except zero times infinity is 2 with the product's sign.
inline float MultiplyExtended(float a, float b) {
  if ((a == 0.f && std::isinf(b)) || (b == 0.f && std::isinf(a))) {
    return std::signbit(a) != std::signbit(b) ? -2.f : 2.f;
  }
  return a * b;
}

// CVector3f::Normalize 0x6b0b0: scaled by twice-refined FRSQRTE of (z*z + x*x) + y*y, no
// guard (a zero vector becomes (2, 2, 2) with its signs).
inline CVector3f NormalizeInPlace(const CVector3f& v) {
  const float xx = v.GetX() * v.GetX();
  const float yy = v.GetY() * v.GetY();
  const float zz = v.GetZ() * v.GetZ();
  const float zx = zz + xx;
  const float n = zx + yy;
  float e = ReciprocalSqrtEstimate(n);
  e = e * ReciprocalSqrtStep(n, e * e);
  e = e * ReciprocalSqrtStep(n, e * e);
  return CVector3f(MultiplyExtended(v.GetX(), e), MultiplyExtended(v.GetY(), e),
                   MultiplyExtended(v.GetZ(), e));
}

// An ANCR/ASPR angle in radians: ((range/2 - range*u) + 0) in degrees, wrapped.
inline float ConeAngle(float range, float u) {
  const float half = range * .5f;
  const float drawn = range * u;
  const float centred = half - drawn;
  const float biased = centred + 0.f;
  return WrapAngle(biased * BitsFloat(0x3c8efa35));
}

// The unrotated ANCR/ASPR direction (sin b cos a, -sin a, cos b cos a) by the parabolic law,
// the cosines as sines of the wrapped angle plus pi/2.
inline CVector3f ConeDirection(float a, float b) {
  const float halfPi = BitsFloat(0x3fc90fdb);
  const float sb = ParabolicSin(b);
  const float cb = ParabolicSin(WrapAngle(b + halfPi));
  const float sa = ParabolicSin(a);
  const float ca = ParabolicSin(WrapAngle(a + halfPi));
  const float x = sb * ca;
  const float z = cb * ca;
  return CVector3f(x, -sa, z);
}

// ANCR (CVEAngleCone::GetValue 0x29eebc): the direction for draws u, v turned by q and scaled
// by the magnitude, not normalised.
inline CVector3f AngleCone(const Quat& q, float xRange, float yRange, float u, float v,
                           float magnitude) {
  const CVector3f d = QuatTransform(q, ConeDirection(ConeAngle(xRange, u), ConeAngle(yRange, v)));
  return CVector3f(d.GetX() * magnitude, d.GetY() * magnitude, d.GetZ() * magnitude);
}

// ASPR's direction (CVEAngleSphere::GetValue 0x2cb100): turned by q, then Normalize 0x6b0b0.
inline CVector3f AngleSphereDirection(const Quat& q, float range1, float range2, float u,
                                      float v) {
  return NormalizeInPlace(
      QuatTransform(q, ConeDirection(ConeAngle(range1, u), ConeAngle(range2, v))));
}

} // namespace PortParticleMath

#endif
