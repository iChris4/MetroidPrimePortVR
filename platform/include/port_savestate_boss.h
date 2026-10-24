#pragma once

// Save-state fight state of the bosses: what a boss keeps beyond its health
// (phase, sub-pools of health, how far through a sequence it is). The room
// restore rebuilds the boss from disc, delivers Activate, replays its script
// messages and sets its saved health and pose; without this, a boss saved
// mid-fight resumed in its first phase (Ridley's next hit put its health back
// to full from its untouched pools).
//
// The block is RoomObject tag kTagBoss: u8 kind, u8 version, then the class's
// fields. A reader that does not know the kind or version ignores the block,
// and a save without one (older builds) gets the restore hook with null data,
// which derives what it can from the restored health.

#include "port_savestate.h"

#include <string>

class CEntity;
class CStateManager;
class CRidley;
class CThardus;
class CFlaahgra;
class CMetroidPrimeRelay;
class CMetroidPrimeStage2;

namespace PortSaveState {
// Appends the boss block of `ent` to `out` and returns true, or returns false
// for an object that has none.
bool CaptureBossState(CStateManager& mgr, CEntity& ent, std::string& out);
// After the generic restore of `ent` (activation, replay, health, pose).
// `data` is the saved block, or null when the save has none.
void RestoreBossState(CStateManager& mgr, CEntity& ent, const std::string* data);
} // namespace PortSaveState

// Friend of the boss classes. Each Put writes the fields after the kind/version
// header; each Restore gets a reader positioned after it, or null.
struct PortBossState {
  enum Kind : uint8_t {
    kRidley = 1,
    kThardus = 2,
    kFlaahgra = 3,
    kMetroidPrime = 4,  // the exo, saved on the record of the relay that made it
    kMetroidPrimeStage2 = 5,
  };

  // port_boss_ridley.cpp
  static const uint8_t kRidleyVersion;
  static void PutRidley(CStateManager& mgr, CRidley& b, PortSaveState::BlockWriter& w);
  static void RestoreRidley(CStateManager& mgr, CRidley& b, PortSaveState::BlockReader* r);
  // port_boss_thardus.cpp
  static const uint8_t kThardusVersion;
  static void PutThardus(CStateManager& mgr, CThardus& b, PortSaveState::BlockWriter& w);
  static void RestoreThardus(CStateManager& mgr, CThardus& b, PortSaveState::BlockReader* r);
  // port_boss_flaahgra.cpp
  static const uint8_t kFlaahgraVersion;
  static void PutFlaahgra(CStateManager& mgr, CFlaahgra& b, PortSaveState::BlockWriter& w);
  static void RestoreFlaahgra(CStateManager& mgr, CFlaahgra& b, PortSaveState::BlockReader* r);
  // True once for a restored Flaahgra whose FSM restarts: the FadeIn start state must not
  // send its Exited/CloseIn again (the script objects already hold their results).
  static bool ConsumeFlaahgraRestart(const CFlaahgra& b);
  // port_boss_prime.cpp. The exo has no editor id (CMetroidPrimeRelay makes it at
  // InitializedInArea), so its state rides on the relay's record; Put returns
  // false when the relay has no live exo.
  static const uint8_t kMetroidPrimeVersion;
  static bool PutMetroidPrime(CStateManager& mgr, CMetroidPrimeRelay& relay,
                              PortSaveState::BlockWriter& w);
  static void RestoreMetroidPrime(CStateManager& mgr, CMetroidPrimeRelay& relay,
                                  PortSaveState::BlockReader* r);
  static const uint8_t kMetroidPrimeStage2Version;
  static void PutMetroidPrimeStage2(CStateManager& mgr, CMetroidPrimeStage2& b,
                                    PortSaveState::BlockWriter& w);
  static void RestoreMetroidPrimeStage2(CStateManager& mgr, CMetroidPrimeStage2& b,
                                        PortSaveState::BlockReader* r);
};
