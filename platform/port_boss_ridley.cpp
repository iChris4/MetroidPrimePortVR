#include "port_savestate_boss.h"

#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Enemies/CRidley.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"

#include <cmath>

// Ridley's fight is its aiStage plus three health pools (stage 2 xb10_, then
// xb18_ and xb1c_ in stage 3). Activate and the pose restore reset the entity's
// health from those pools, so they have to come back with the saved values.
const uint8_t PortBossState::kRidleyVersion = 1;

namespace {
// Stage 2 hit thresholds (xcbc_/xcb0_) after `passed` of the 0.8..0.2 steps.
float Stage2Threshold(int passed, float spawnHp) {
  return passed == 0 ? 0.8f * spawnHp : 0.2f * static_cast< float >(4 - passed) * spawnHp;
}

} // namespace

void PortBossState::PutRidley(CStateManager&, CRidley& b, PortSaveState::BlockWriter& w) {
  w.U8(static_cast< uint8_t >(b.xc64_aiStage));
  w.F32(b.xb10_);
  w.F32(b.xb18_);
  w.F32(b.xb1c_);
  w.F32(b.xcbc_);
  w.U32(b.xcb0_);
  w.U8(b.xa31_26_ ? 1 : 0);
}

void PortBossState::RestoreRidley(CStateManager& mgr, CRidley& b, PortSaveState::BlockReader* r) {
  // A boss not yet activated has nothing in progress (its health is the pre-Activate value).
  // The collision managers exist from Registered on; a boss without them never ran its setup.
  if (!b.GetActive() || b.x984_bodyCollision.get() == nullptr ||
      b.x980_tailCollision.get() == nullptr)
    return;
  int stage;
  float xb10, xb18, xb1c, xcbc;
  uint32_t xcb0;
  bool xa31_26;
  if (r != nullptr) {
    stage = r->U8();
    xb10 = r->F32();
    xb18 = r->F32();
    xb1c = r->F32();
    xcbc = r->F32();
    xcb0 = r->U32();
    xa31_26 = r->U8() != 0;
    auto sane = [](float f) { return std::isfinite(f) && f >= 0.f && f < 1.0e6f; };
    if (!r->ok || (stage != 2 && stage != 3) || !sane(xb10) || !sane(xb18) || !sane(xb1c) ||
        !sane(xcbc) || xcb0 > 4)
      return;
  } else {
    // No block: split the restored health over the pools the way a fight drains them.
    const float hp = b.HealthInfo(mgr)->GetHP();
    if (!(hp > 0.f))
      return;
    xcbc = b.xcbc_;
    xcb0 = b.xcb0_;
    xa31_26 = b.xa31_26_;
    if (hp > b.xb18_ + b.xb1c_) {
      stage = 2;
      xb10 = hp - b.xb18_ - b.xb1c_;
      xb18 = b.xb18_;
      xb1c = b.xb1c_;
      int passed = 0;
      for (int n = 1; n <= 4; ++n) {
        if (xb10 < 0.2f * static_cast< float >(5 - n) * b.xcb8_)
          passed = n;
      }
      xcb0 = static_cast< uint32_t >(passed);
      xcbc = Stage2Threshold(passed, b.xcb8_);
    } else {
      stage = 3;
      xb10 = 0.f;
      if (hp > b.xb1c_) {
        xb18 = hp - b.xb1c_;
        xb1c = b.xb1c_;
      } else {
        xb18 = 0.f;
        xb1c = hp;
        xa31_26 = true;
      }
      xcbc = (xb18 > 0.f ? 0.6667f * b.xb18_ : 0.6667f * b.xb1c_);
    }
  }
  b.xb10_ = xb10;
  b.xb18_ = xb18;
  b.xb1c_ = xb1c;
  b.xcbc_ = xcbc;
  b.xcb0_ = xcb0;
  b.xa31_26_ = xa31_26;
  if (stage == 3) {
    // What the kSM_Damage stage 2 -> 3 switch sets up besides the fields.
    b.xc64_aiStage = 3;
    b.xa31_25_ = false;
    b.SetStage3Immunity(mgr);
    if (b.x450_bodyController->GetLocomotionType() != pas::kLT_Combat) {
      b.PortStage3WingEffects(mgr);
    }
  }
  // The boss bar's maximum is the spawn health plus the stage 3 pools, as Activate sets it.
  mgr.SetBossParams(b.GetUniqueId(), b.xb1c_ + (b.xcb8_ + b.xb18_), 90);
  b.HealthInfo(mgr)->SetHP(b.xb1c_ + (b.xb10_ + b.xb18_));
}
