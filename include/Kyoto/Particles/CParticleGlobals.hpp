#ifndef _CPARTICLEGLOBALS
#define _CPARTICLEGLOBALS

#include "types.h"

#include "Kyoto/SObjectTag.hpp"
#include "Kyoto/Particles/CElementGen.hpp"

class CParticleGlobals {
public:
  struct SParticleSystem {
    FourCC x0_type;
    CElementGen* x4_system;
    SParticleSystem* x8_prev;

    SParticleSystem(FourCC type, CElementGen* system)
    : x0_type(type)
    , x4_system(system)
    , x8_prev(mCurrentParticleSystem) {
      mCurrentParticleSystem = this;
    }
    ~SParticleSystem() { mCurrentParticleSystem = x8_prev; }
  };

  static void SetEmitterTime(int time);
  static void SetParticleLifetime(int lifetime);
  static void UpdateParticleLifetimeTweenValues(int time);

  static int GetParticleLifetime() { return mParticleLifetime; }
  static float GetParticleLifetimeReal() { return mParticleLifetimeReal; }
  static int GetEmitterTime() { return mEmitterTime; }
  static float GetEmitterTimeReal() { return mEmitterTimeReal; }
  static int GetParticleLifetimePercentage() { return mParticleLifetimePercentage; }
  static float GetParticleLifetimePercentageReal() { return mParticleLifetimePercentageReal; }
  static float GetParticleLifetimePercentageRemainder() {
    return mParticleLifetimePercentageRemainder;
  }
  static CElementGen::CParticle* GetCurrentParticle() { return mCurrentParticle; }
#ifdef TARGET_PC
  // GC reads address 0 harmlessly when no ADV values are bound; read zeros instead.
  static float* GetParticleAccessParameters() {
    static float sZeros[8] = {};
    return mParticleAccessParameters != nullptr ? mParticleAccessParameters : sZeros;
  }
#else
  static float* GetParticleAccessParameters() { return mParticleAccessParameters; }
#endif
  static SParticleSystem* GetCurrentParticleSystem() { return mCurrentParticleSystem; }
  static void SetCurrentParticleSystem(SParticleSystem* system) { mCurrentParticleSystem = system; }

public:
  static int mParticleLifetime;
  static float mParticleLifetimeReal;
  static int mEmitterTime;
  static float mEmitterTimeReal;
  static int mParticleLifetimePercentage;
  static float mParticleLifetimePercentageReal;
  static float mParticleLifetimePercentageRemainder;
  static CElementGen::CParticle* mCurrentParticle;
  static float* mParticleAccessParameters;
  static SParticleSystem* mCurrentParticleSystem;
#ifdef TARGET_PC
  // port-only: the particle a per-particle GetValueUV call is for (null = none)
  static CElementGen::CParticle* xPortUVParticle;
  // port-only: the particle the elements are being evaluated for, set only while a generator with
  // PIRN is evaluating per-particle elements (null otherwise). IRND elements then give a value
  // fixed per (particle, element) instead of relying on being read at frame 0.
  static const CElementGen::CParticle* xPortIrndParticle;
  // port-only: what Remastered's DFCS/DFCP (0x299134 / 0x299064) read from a swoosh's context: the
  // swoosh's local->world transform (T(global) * R(global orientation) * S(global scale), the
  // BuildTransform4f of its registers), the swoosh's local translation (DFCS) and the position of
  // the segment being evaluated (DFCP, segment +0xc). Null outside a swoosh evaluation.
  struct SPortSwooshEval {
    CTransform4f worldFromLocal;
    CVector3f systemLocal;
    const CVector3f* segmentPos;
    // The point's swoosh node key (KEWS, Remastered 0x24f00c), when the point has one.
    bool hasNode = false;
    int nodePercentage = 0;
    float nodeRemainder = 0.f;
  };
  static const SPortSwooshEval* xPortSwooshEval;
  struct PortSwooshEvalScope {
    const SPortSwooshEval* prev;
    explicit PortSwooshEvalScope(const SPortSwooshEval* eval) : prev(xPortSwooshEval) {
      xPortSwooshEval = eval;
    }
    ~PortSwooshEvalScope() { xPortSwooshEval = prev; }
  };
  // A deterministic uniform in [0, 1) from the particle's seed and the element's address, and the
  // raw 32-bit hash it comes from.
  static u32 PortIrndBits(const CElementGen::CParticle* particle, const void* element);
  static float PortIrndUnit(const CElementGen::CParticle* particle, const void* element);
#endif
};

#endif // _CPARTICLEGLOBALS
