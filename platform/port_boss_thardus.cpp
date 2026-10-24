#include "port_savestate_boss.h"

#include "MetroidPrime/CGameLight.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Enemies/CThardus.hpp"
#include "MetroidPrime/CCollisionActor.hpp"
#include "MetroidPrime/CCollisionActorManager.hpp"

// The fight is progress through the destroyable rocks. Rocks are made at kSM_Registered with
// no editor id, so the room restore knows nothing of them. Each rock is stone (hit it until
// its collider's health runs out), then exposed as phazon (own pool, hit again), then gone.
const uint8_t PortBossState::kThardusVersion = 1;

namespace {
enum RockState : uint8_t { kRockStone, kRockPhazon, kRockGone };
} // namespace

void PortBossState::PutThardus(CStateManager& mgr, CThardus& b, PortSaveState::BlockWriter& w) {
  const uint count = b.x610_destroyableRocks.size();
  w.U8(static_cast< uint8_t >(count));
  w.U32(b.x648_currentRock);
  // heard the player / Reset arrived / enraged / Suck done / memo count / boss bar set
  w.U8(b.x5c8_heardPlayer);
  w.U8(b.x95c_doCodeTrigger);
  w.U8(b.x8f0_);
  w.U8(b.x689_);
  w.U8(b.x95d_);
  w.U8(b.x95e_);
  for (uint i = 0; i < count; ++i) {
    uint8_t state = kRockStone;
    float hp = b.x90c_rockHealths[i];
    if (i < b.x648_currentRock) {
      state = kRockGone;
      hp = 0.f;
    } else {
      if (b.x6b0_destroyedRocks[i])
        state = kRockPhazon;
      // The live pool is the rock's own; the collider copies it every frame.
      if (CEntity* rock = mgr.ObjectById(b.x610_destroyableRocks[i])) {
        if (CHealthInfo* health = static_cast< CDestroyableRock* >(rock)->HealthInfo(mgr))
          hp = health->GetHP();
      }
    }
    w.U8(state);
    w.F32(hp);
  }
}

void PortBossState::RestoreThardus(CStateManager& mgr, CThardus& b,
                                   PortSaveState::BlockReader* r) {
  // No block (older save): the health is the sum of the rocks' pools, which can't tell
  // which rocks are gone, so leave the fight as the room restore left it.
  if (r == nullptr)
    return;
  const uint count = b.x610_destroyableRocks.size();
  const uint savedCount = r->U8();
  const uint current = r->U32();
  const bool heard = r->U8() != 0;
  const bool codeTrigger = r->U8() != 0;
  const bool enraged = r->U8() != 0;
  const bool sucked = r->U8() != 0;
  const uint8_t memos = r->U8();
  const bool barSet = r->U8() != 0;
  uint8_t states[16];
  float hps[16];
  if (!r->ok || savedCount != count || count == 0 || count > 16 || current > count)
    return;
  for (uint i = 0; i < count; ++i) {
    states[i] = r->U8();
    hps[i] = r->F32();
  }
  if (!r->ok)
    return;
  for (uint i = 0; i < count; ++i) {
    // NaN fails the comparison; a pool is never negative.
    if (states[i] > kRockGone || !(hps[i] >= 0.f) || hps[i] > 1.0e6f)
      return;
    // Rocks before the current one are gone, the rest are not.
    if ((states[i] == kRockGone) != (i < current))
      return;
  }
  if (b.x5f0_rockColliders.get() == nullptr)
    return;

  for (uint i = 0; i < count; ++i) {
    CDestroyableRock* rock =
        static_cast< CDestroyableRock* >(mgr.ObjectById(b.x610_destroyableRocks[i]));
    CGameLight* light = TCastToPtr< CGameLight >(mgr.ObjectById(b.x6c0_rockLights[i]));
    CCollisionActor* collider = TCastToPtr< CCollisionActor >(mgr.ObjectById(
        b.x5f0_rockColliders->GetCollisionDescFromIndex(i).GetCollisionActorId()));
    if (rock == nullptr)
      continue;
    if (states[i] != kRockStone) {
      // What ExposeDestroyableRock does, minus its sound, particles and thermal flash.
      if (!rock->IsUsingPhazonModel())
        rock->UsePhazonModel();
      b.x6b0_destroyedRocks[i] = true;
      rock->SetThermalMag(1.5f);
    }
    if (states[i] == kRockGone) {
      rock->SetActive(false);
      if (collider != nullptr)
        collider->SetActive(false);
      if (light != nullptr)
        light->SetActive(false);
    } else {
      if (light != nullptr)
        light->SetActive(states[i] == kRockPhazon);
      if (CHealthInfo* health = rock->HealthInfo(mgr))
        *health = CHealthInfo(hps[i], 0.f);
      if (collider != nullptr) {
        if (CHealthInfo* health = collider->HealthInfo(mgr))
          *health = CHealthInfo(hps[i], 0.f);
      }
    }
    b.x90c_rockHealths[i] = hps[i];
  }
  b.x648_currentRock = current;
  b.x5c8_heardPlayer = heard;
  b.x95c_doCodeTrigger = codeTrigger;
  b.x8f0_ = enraged;
  b.x689_ = sucked;
  b.x95d_ = memos;
  // kSM_Reset (the fight's start) also drops the invulnerable flag the class starts with.
  if (codeTrigger)
    b.x93d_ = false;
  // The bar is full-scale at the fight's start total (every rock's stone and phazon pools,
  // the last doubled); TargetPatrol sets it from the current health, which is wrong on a
  // restored fight, so set it here and mark it done.
  if (barSet) {
    float total = 0.f;
    for (uint i = 0; i < count; ++i)
      total += (i == count - 1 ? 2.f : 1.f) * (b.x6a8_ + b.x6a4_);
    b.UpdateTotalHealth(mgr);
    mgr.SetBossParams(b.GetUniqueId(), total, 88);
    b.x95e_ = true;
  }
}
