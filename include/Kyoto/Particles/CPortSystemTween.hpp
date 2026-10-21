#pragma once

// Port-only PSLW (Remastered's SLTW, GetLifetimeTweenElement 0x26a640 on time slot 0): a blend
// from A to B by the system tween, a + tween * (b - a). CREBlendByLifetimeConstant (0x296170, both
// children constant) and CRELifetimeTween (0x2960d4) share the arithmetic; a missing child reads 0.
// Header only, so a test can link it alone.

#include "Kyoto/Particles/CParticleGlobals.hpp"
#include "Kyoto/Particles/CRealElement.hpp"

class CREPortSystemTween : public CRealElement {
  CRealElement* x4_a;
  CRealElement* x8_b;

public:
  CREPortSystemTween(CRealElement* a, CRealElement* b) : x4_a(a), x8_b(b) {}
  ~CREPortSystemTween() override {
    delete x4_a;
    delete x8_b;
  }

  bool GetValue(int frame, float& valOut) const override {
    float a = 0.f, b = 0.f;
    if (x4_a) {
      x4_a->GetValue(frame, a);
    }
    if (x8_b) {
      x8_b->GetValue(frame, b);
    }
    const float tween = CParticleGlobals::xPortSystemTween;
    const float diff = b - a;
    const float scaled = tween * diff;
    valOut = a + scaled;
    return false;
  }
};
