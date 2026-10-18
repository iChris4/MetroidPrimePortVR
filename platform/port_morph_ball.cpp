// CMorphBall's port-only members (Remastered's particle variable bindings of the
// ball and the hull flash), moved out of src/MetroidPrime/Player/CMorphBall.cpp
// so the decomp file keeps only its hooks (see docs/NATIVE_PORT.md, "Where port
// code goes"). They are declared under TARGET_PC in
// include/MetroidPrime/Player/CMorphBall.hpp.
//
// Built as part of mp_game, with the same flags and includes as the file it
// came from; the code was moved verbatim.

#include "MetroidPrime/Player/CMorphBall.hpp"

#include "Collision/CMaterialList.hpp"
#include "Collision/CollisionUtil.hpp"
#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Basics/CCast.hpp"
#include "Kyoto/CDependencyGroup.hpp"
#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Input/CFinalInput.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/CActorLights.hpp"
#include "MetroidPrime/CAnimRes.hpp"
#include "MetroidPrime/CAxisAngle.hpp"
#include "MetroidPrime/CControlMapper.hpp"
#include "MetroidPrime/CDamageInfo.hpp"
#include "MetroidPrime/CFluidPlaneCPU.hpp"
#include "MetroidPrime/CGameCollision.hpp"
#include "MetroidPrime/CGameLight.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CRainSplashGenerator.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/CWorldShadow.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/Enemies/CMetroidBeta.hpp"
#include "MetroidPrime/Player/CMorphBallShadow.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/SFX/IceWorld.h"
#include "MetroidPrime/SFX/LavaWorld.h"
#include "MetroidPrime/SFX/MiscSamus.h"
#include "MetroidPrime/ScriptObjects/CScriptAreaAttributes.hpp"
#include "MetroidPrime/ScriptObjects/CScriptSpiderBallAttractionSurface.hpp"
#include "MetroidPrime/ScriptObjects/CScriptSpiderBallWaypoint.hpp"
#include "MetroidPrime/ScriptObjects/CScriptWater.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include "MetroidPrime/TGameTypes.hpp"

#include "Kyoto/Math/CMath.hpp"
#include "port_apclient.h"
#include "port_debug.h"
#include "port_model_variant.h"
#include "port_remastered_ball_light.h"
#include "port_synthetic_effect.h"
#include "Kyoto/Math/CPlane.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CloseEnough.hpp"
#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/Particles/CParticleSwoosh.hpp"
#include "MetroidPrime/Tweaks/CTweakBall.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayer.hpp"

#include "rstl/math.hpp"

#ifdef TARGET_PC
// Remastered's particle variable bindings of the ball (CMorphBallMP1::UpdateEffects, kb
// topic/particle-variable-bindings). Its tables hold the five original suits (idx 0-4); the
// port-only suits leave the variables at their defaults.
namespace {
PortGuid PortBallGuid(const char* uuid) {
  // Textual UUID -> bytes_le, as the PVRT stores them.
  u8 raw[16];
  int n = 0;
  for (const char* p = uuid; *p != '\0' && n < 16; ++p) {
    if (*p == '-') {
      continue;
    }
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    raw[n++] = static_cast< u8 >(nib(p[0]) << 4 | nib(p[1]));
    ++p;
  }
  PortGuid g;
  static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  for (int i = 0; i < 16; ++i) {
    g.bytes[i] = raw[order[i]];
  }
  return g;
}

const PortGuid& PortSwooshVarGuid() {
  static const PortGuid g = PortBallGuid("8f30e325-09bb-4e64-8359-2127d3b65e75");
  return g;
}

const PortGuid& PortFlashColorGuid() {
  static const PortGuid g = PortBallGuid("256a3183-6c29-4058-be0c-2edcc99ebc25");
  return g;
}

// The glow generator's variables per suit, in the slot order IntensityPrimary, LightIntensity,
// LightRadius, LightInnerRadius, LightInnerAngle, LightOuterAngle. Suits 2-4 share the first three.
const PortGuid& PortGlowVarGuid(uint suit, int slot) {
  static const char* const kGuids[5][6] = {
      {"723297df-8127-4b72-863e-66e539a4dbd2", "96e76d63-4e1f-43a3-8656-1b39ac7a8d91",
       "fb99334f-7db3-4a15-b22b-b6a5470b99ea", "119c5e85-80ef-477e-a972-75f743fdc08c",
       "020dab60-3038-4d31-81fd-4cd1e215e6d6", "cb9d9df9-975a-4729-aecb-2bc52def7cf4"},
      {"534dd796-f123-4733-b3b8-df5f6bc3b85a", "8cd1e310-51b2-45d4-a2d2-4bf3e642e894",
       "c141076b-aab4-4b09-843c-5650632c1b25", "d183ecaf-ac35-4a04-89d3-13c9d9957547",
       "842f038a-f6e1-45a0-b009-c35dc5f4abf3", "c7ffed01-49f6-45a9-b686-5f68d64085e5"},
      {"723297df-8127-4b72-863e-66e539a4dbd2", "96e76d63-4e1f-43a3-8656-1b39ac7a8d91",
       "fb99334f-7db3-4a15-b22b-b6a5470b99ea", "d9771a86-0bdb-43ca-80d0-4129875282f6",
       "279c68c0-5004-4b14-bab4-a54ed6200fa1", "ecb7a357-19c1-4cdb-9ca2-f8f4c66d5da3"},
      {"723297df-8127-4b72-863e-66e539a4dbd2", "96e76d63-4e1f-43a3-8656-1b39ac7a8d91",
       "fb99334f-7db3-4a15-b22b-b6a5470b99ea", "4cfa5130-4b1a-4fb4-8df7-eb0cddd9d3aa",
       "65e4e98e-5ffd-4563-ad61-81a53f178f81", "ac3e2c85-40ad-4217-b009-00fdbb57cb43"},
      {"723297df-8127-4b72-863e-66e539a4dbd2", "96e76d63-4e1f-43a3-8656-1b39ac7a8d91",
       "fb99334f-7db3-4a15-b22b-b6a5470b99ea", "1b0fc716-944f-47f9-a549-65ff19847542",
       "31615223-8bd0-4799-9fbc-3faed583c1d3", "7a75e448-5b7d-4907-8bc5-560980ce0a61"},
  };
  static PortGuid cache[5][6];
  static bool init = false;
  if (!init) {
    for (int s = 0; s < 5; ++s) {
      for (int k = 0; k < 6; ++k) {
        cache[s][k] = PortBallGuid(kGuids[s][k]);
      }
    }
    init = true;
  }
  return cache[suit][slot];
}

// Remastered's rodata (skBallNormalSwooshIntensity 0x1d252d4 ... skInnerGlowBoostedLightOuterAngle
// 0x1d254b4): per suit { first value, second value } = { dry, in water }.
constexpr float kSwooshNormal[5] = {1.5f, 1.5f, 1.5f, 1.5f, 1.5f};
constexpr float kSwooshBoosted[5] = {13.f, 13.f, 13.f, 13.f, 40.f};
constexpr float kGlowNormal[6][10] = {
    {15.f, 15.f, 15.f, 15.f, 15.f, 15.f, 15.f, 15.f, 15.f, 15.f},
    {30.f, 60.f, 30.f, 60.f, 30.f, 60.f, 30.f, 60.f, 30.f, 90.f},
    {2.3f, 2.3f, 2.3f, 2.3f, 2.3f, 2.3f, 2.3f, 2.3f, 2.3f, 2.3f},
    {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {90.f, 90.f, 90.f, 90.f, 90.f, 90.f, 90.f, 90.f, 90.f, 90.f},
};
constexpr float kGlowBoosted[6][10] = {
    {300.f, 300.f, 600.f, 600.f, 300.f, 300.f, 300.f, 300.f, 300.f, 300.f},
    {450.f, 450.f, 450.f, 450.f, 450.f, 450.f, 450.f, 450.f, 450.f, 450.f},
    {2.3f, 2.3f, 2.3f, 2.3f, 2.3f, 2.3f, 2.3f, 2.3f, 2.3f, 2.3f},
    {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
    {90.f, 90.f, 90.f, 90.f, 90.f, 90.f, 90.f, 90.f, 90.f, 90.f},
};
} // namespace

void CMorphBall::PortBindSwooshVars() {
  CParticleSwoosh* gens[2] = {x19b8_slowBlueTailSwooshGen.get(), x19bc_slowBlueTailSwooshGen2.get()};
  if (x8_ballGlowColorIdx > 4) {
    return;
  }
  // Boost drain only, scaled by the player's speed 25..40.
  float s = 0.f;
  if (x1df4_boostDrainTime > 0.f) {
    s = rstl::min_val(1.f, rstl::max_val(0.f, (x0_player.GetVelocityWR().Magnitude() - 25.f) / 15.f));
  }
  const float n = kSwooshNormal[x8_ballGlowColorIdx];
  const float value = n + (kSwooshBoosted[x8_ballGlowColorIdx] - n) * s;
  for (int i = 0; i < 2; ++i) {
    if (gens[i] == nullptr) {
      continue;
    }
    if (xPortSwooshVarGen[i] != gens[i]) {
      xPortSwooshVarGen[i] = gens[i];
      xPortSwooshVar[i] = gens[i]->PortGetRealHandle(PortSwooshVarGuid());
    }
    gens[i]->PortBindReal(xPortSwooshVar[i], value);
  }
}

void CMorphBall::PortUpdateWaterFactor(float dt, const CStateManager& mgr) {
  const float radius = GetBallRadius();
  float depth = 0.f;
  if (x0_player.IsInsideFluid()) {
    depth = 2.f * radius;
  } else if (x0_player.IsInFluid()) {
    if (const CScriptWater* water =
            TCastToConstPtr< CScriptWater >(mgr.GetObjectById(x0_player.InFluidId()))) {
      if (water->GetFluidPlane().GetFluidType() == CFluidPlane::kFT_NormalWater) {
        depth = x0_player.GetDistanceUnderWater();
      }
    }
  }
  if (depth <= 0.f) {
    xPortWaterFactor = rstl::min_val(1.f, rstl::max_val(0.f, xPortWaterFactor - 4.f * dt));
  } else {
    const float rise = rstl::min_val(1.f, rstl::max_val(0.f, xPortWaterFactor + 2.f * dt));
    const float target = rstl::min_val(1.f, rstl::max_val(0.f, depth * 0.5f / radius));
    xPortWaterFactor = rstl::min_val(rise, target);
  }
}

void CMorphBall::PortBindGlowVars() {
  CElementGen* gen = PortGlowGen();
  if (gen == nullptr || x8_ballGlowColorIdx > 4) {
    return;
  }
  const uint idx = x8_ballGlowColorIdx;
  if (xPortGlowVarGen != gen || xPortGlowVarIdx != idx) {
    xPortGlowVarGen = gen;
    xPortGlowVarIdx = idx;
    for (int k = 0; k < 6; ++k) {
      xPortGlowVars[k] = gen->PortGetRealHandle(PortGlowVarGuid(idx, k));
    }
  }
  float boost = 0.f;
  if (x1df4_boostDrainTime != 0.f) {
    boost = 1.f - x1df4_boostDrainTime / gpTweakBall->GetBoostBallDrainTime();
  } else if (x1de8_boostChargeTime != 0.f) {
    boost = x1de8_boostChargeTime / gpTweakBall->GetBoostBallMaxChargeTime();
  }
  for (int k = 0; k < 6; ++k) {
    const float n0 = kGlowNormal[k][idx * 2];
    const float n1 = kGlowNormal[k][idx * 2 + 1];
    const float a = n0 + boost * (kGlowBoosted[k][idx * 2] - n0);
    const float b = n1 + boost * (kGlowBoosted[k][idx * 2 + 1] - n1);
    gen->PortBindReal(xPortGlowVars[k], a + xPortWaterFactor * (b - a));
  }
}

// Remastered's CMorphBallMP1::Render (0xcea524) draws the hull with a model colour-add: the
// damage flash (dmg,0,0)*Damaged[idx] plus the boost glow lerp(0, Glow[idx]*Intensity[idx], t).
// The glow table is stored linear (SRGB_To_Linear of the bytes) in Remastered.
CModelFlags CMorphBall::PortHullFlashFlags() const {
  static const float kBoostedHullGlowIntensities[5] = {0.8f, 0.9f, 0.35f, 0.8f, 0.15f};
  static const float kDamagedHullFlashIntensities[5] = {1.f, 1.f, 1.f, 1.f, 1.f};
  static const float kBoostedHullGlow[5][3] = {{160.f, 25.f, 25.f},
                                               {67.f, 159.f, 217.f},
                                               {60.f, 150.f, 30.f},
                                               {96.f, 51.f, 255.f},
                                               {255.f, 60.f, 0.f}};
  const float damage = x1e44_damageEffect;
  const float charge = x1de8_boostChargeTime;
  const float drain = x1df4_boostDrainTime;
  if (damage <= 0.f && charge <= 0.f && drain <= 0.f) {
    return CModelFlags::Normal();
  }
  const uint idx = rstl::min_val(4u, static_cast< uint >(x8_ballGlowColorIdx));
  float add[3] = {0.f, 0.f, 0.f};
  if (damage > 0.f) {
    add[0] += damage * kDamagedHullFlashIntensities[idx];
  }
  if (charge > 0.f || drain > 0.f) {
    const float t = drain == 0.f ? charge / gpTweakBall->GetBoostBallMaxChargeTime()
                                 : 1.f - drain / gpTweakBall->GetBoostBallDrainTime();
    for (int i = 0; i < 3; ++i) {
      const float c = kBoostedHullGlow[idx][i] / 255.f;
      const float lin = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
      add[i] += lin * kBoostedHullGlowIntensities[idx] * t;
    }
  }
  return CModelFlags(CModelFlags::kT_Two, CColor(rstl::min_val(1.f, add[0]), rstl::min_val(1.f, add[1]),
                                                 rstl::min_val(1.f, add[2]), 1.f));
}

void CMorphBall::PortBindFlashColor() {
  CElementGen* gen = x19dc_morphBallTransitionFlashGen.get();
  if (gen == nullptr) {
    return;
  }
  if (xPortFlashVarGen != gen) {
    xPortFlashVarGen = gen;
    xPortFlashVar = gen->PortGetColorHandle(PortFlashColorGuid());
  }
  const SColorRgb& c = skBallLightModulationColors[x8_ballGlowColorIdx];
  const CColor base(c.x0_r / 255.f, c.x1_g / 255.f, c.x2_b / 255.f, 1.f);
  const CColor black(0.f, 0.f, 0.f, 1.f);
  const CColor white(1.f, 1.f, 1.f, 1.f);
  const CPlayer::EPlayerMorphBallState state = x0_player.GetMorphballTransitionState();
  const float t = rstl::min_val(1.f, rstl::max_val(0.f, x0_player.GetMorphBallTransitionFactor()));
  CColor value = base;
  if (state == CPlayer::kMS_Morphing) {
    if (t < 0.5f) {
      value = CColor::Lerp(black, base, rstl::min_val(2.f * t, 1.f));
    }
  } else if (state == CPlayer::kMS_Unmorphing) {
    value = CColor::Lerp(base, black, t);
  } else {
    value = CColor::Lerp(base, white, x1c34_boostLightFactor);
  }
  gen->PortBindColor(xPortFlashVar, value);
}
#endif
