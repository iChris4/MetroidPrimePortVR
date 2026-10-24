#include "port_savestate_boss.h"

#include "MetroidPrime/Enemies/CFlaahgra.hpp"

// Flaahgra's fight progress is the stage (one per bomb-slot hit, 0..3), the damage taken
// in the current vulnerable spell (x810_, retreat at GetRetreatHP) and the script's
// Start/Stop and Play switches. Its health already carries the bomb-slot cuts; the
// mirrors and their waypoints are script objects the room restore rebuilds.
const uint8_t PortBossState::kFlaahgraVersion = 1;

static const CFlaahgra* sRestartedFlaahgra = nullptr;

bool PortBossState::ConsumeFlaahgraRestart(const CFlaahgra& b) {
  if (sRestartedFlaahgra != &b)
    return false;
  sRestartedFlaahgra = nullptr;
  return true;
}

void PortBossState::PutFlaahgra(CStateManager&, CFlaahgra& b, PortSaveState::BlockWriter& w) {
  w.U32(static_cast< uint32_t >(b.x788_stage));
  w.F32(b.x810_);
  w.U8(b.x8e4_31_ ? 1 : 0);
  w.U8(b.x8e5_24_ ? 1 : 0);
}

void PortBossState::RestoreFlaahgra(CStateManager& mgr, CFlaahgra& b,
                                    PortSaveState::BlockReader* r) {
  if (r != nullptr) {
    const uint32_t stage = r->U32();
    const float retreatDamage = r->F32();
    const bool stopped = r->U8() != 0;
    const bool specialAttack = r->U8() != 0;
    if (r->ok && stage <= 3 && retreatDamage == retreatDamage && retreatDamage >= 0.f) {
      b.x788_stage = static_cast< int >(stage);
      b.x810_ = retreatDamage;
      b.x8e4_31_ = stopped;
      b.x8e5_24_ = specialAttack;
    }
  }
  // The replayed Close ran ApplyBombSlotDamage, which leaves the boss in its get-up
  // faint; the FSM restarts from its start state, so that must not carry over.
  b.x8e4_29_getup = false;
  b.x7d4_faintTime = 0.f;
  b.x7d8_ = 0.f;
  b.x430_damageColor = CColor(0.5f, 0.5f, 0.f, 1.f);
  b.x7f8_ = 0;
  b.x814_ = 0.f;
  b.x818_curHp = b.HealthInfo(mgr)->GetHP();
  // A fight in progress (saved with a block, awake) already ran FadeIn once; without
  // this its restart replayed Exited/CloseIn, which moved a counter, a relay and a timer.
  sRestartedFlaahgra = (r != nullptr && b.GetActive()) ? &b : nullptr;
}
