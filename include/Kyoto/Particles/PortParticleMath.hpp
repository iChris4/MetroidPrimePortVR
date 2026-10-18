#pragma once

// Port-only particle element math that needs no engine state, so it can be tested alone.
// Both follow Remastered (Switch 1.0.0): DistanceFromCameraBlend::CalculateBlendValue 0x298f44 and
// CCEGradient::GetValue 0x2c8d48.
#ifdef TARGET_PC

#include <cmath>

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

} // namespace PortParticleMath

#endif
