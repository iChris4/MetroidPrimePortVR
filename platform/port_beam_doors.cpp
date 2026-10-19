#include "port_beam_doors.h"

#include <set>

#include "port_ap_world.h"
#include "port_apclient.h"
#include "port_debug.h"
#include "port_log.h"

#include "Kyoto/Streams/CMemoryInStream.hpp"
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CDamageVulnerability.hpp"
#include "MetroidPrime/CMapWorld.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/ScriptObjects/CScriptActor.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDamageableTrigger.hpp"
#include "MetroidPrime/TCastTo.hpp"

namespace PortBeamDoors {

bool Enabled() { return PortDebug::BeamDoorsStayUnlocked(); }

const uint32_t* Unlocked(uint32_t* copy) {
  if (!Enabled() || gpGameState == nullptr)
    return nullptr;
  const uint* bits = gpGameState->PortBeamDoorBits();
  for (int i = 0; i < kWords; ++i)
    copy[i] = bits[i];
  return copy;
}

namespace {

// Doorways broken for real whose restyle waits for the shield to finish fading.
std::set< int > sPending;

// The doors of a doorway that are loaded take the look and weaknesses `styleOf`
// gives them now (false: leave that door); the ones in rooms not loaded get
// theirs from their room patch when they load.
template < typename StyleOf >
void RestyleWith(CStateManager& mgr, int bit, StyleOf styleOf) {
  for (const PortApWorld::DoorChange& door : PortApWorld::BeamDoorway(bit)) {
    PortApWorld::DoorStyle style;
    std::vector< uint8_t > weakness;
    if (!styleOf(door, style, weakness))
      continue;
    for (int k = 0; k < 2; ++k) {
      if (door.forces[k] != 0) {
        const TUniqueId uid = mgr.GetIdForScript(TEditorId(door.forces[k]));
        if (uid != kInvalidUniqueId) {
          if (CScriptDamageableTrigger* trigger =
                  dynamic_cast< CScriptDamageableTrigger* >(mgr.ObjectById(uid))) {
            CMemoryInStream in(weakness.data(), weakness.size());
            trigger->PortRestyle(CDamageVulnerability(in), style.pattern0, style.pattern1,
                                 style.color);
          }
        }
      }
      if (door.shieldActors[k] != 0) {
        const TUniqueId uid = mgr.GetIdForScript(TEditorId(door.shieldActors[k]));
        if (uid != kInvalidUniqueId) {
          CScriptActor* actor = TCastToPtr< CScriptActor >(mgr.ObjectById(uid));
          if (actor != nullptr && actor->HasModelData()) {
            const CVector3f scale = actor->GetModelData()->GetScale();
            actor->SetModelData(CModelData(
                CStaticRes(door.vertical ? style.cmdlVertical : style.cmdl, scale)));
          }
        }
      }
    }
  }
}

// The map icons of the loaded map areas.
void RefreshMap(CStateManager& mgr) {
  if (const CWorld* world = mgr.GetWorld()) {
    if (CMapWorld* map = world->GetMapWorld())
      map->PortRefreshDoors();
  }
}

// Doorway `bit` goes blue (`onlyUnlockable`: just the doors the setting may
// unlock, for a doorway that was recorded while the setting was off).
void Restyle(CStateManager& mgr, int bit, bool onlyUnlockable = false) {
  PortApWorld::DoorStyle blue;
  if (!PortApWorld::BlueDoorStyle(blue))
    return;
  const std::vector< uint8_t > weakness = PortApWorld::BlueDoorVulnerability();
  RestyleWith(mgr, bit,
              [&](const PortApWorld::DoorChange& door, PortApWorld::DoorStyle& style,
                  std::vector< uint8_t >& vulnerability) {
                if (onlyUnlockable && !PortAp::BeamDoorUnlockable(door.mrea, door.index))
                  return false;
                style = blue;
                vulnerability = weakness;
                return true;
              });
}

// Doorway `bit` goes back to the locks its doors have without the setting; a
// door whose blast shield still stands never turned blue and is left alone.
void Relock(CStateManager& mgr, int bit) {
  RestyleWith(mgr, bit, [](const PortApWorld::DoorChange& door, PortApWorld::DoorStyle& style,
                           std::vector< uint8_t >& vulnerability) {
    return PortAp::BeamDoorUnlockable(door.mrea, door.index) &&
           PortAp::LockedDoorStyle(door.index, style, vulnerability);
  });
}

} // namespace

bool Unlock(CStateManager& mgr, int bit) {
  if (gpGameState == nullptr || bit < 0 || bit >= PortApWorld::BeamDoorwayCount() ||
      static_cast< size_t >(bit) >= kMaxBits)
    return false;
  uint* bits = gpGameState->PortBeamDoorBits();
  if (Test(bits, bit))
    return false;
  Set(bits, bit);
  Restyle(mgr, bit);
  RefreshMap(mgr);
  PortLog::Write("beam doors: doorway %d stays unlocked\n", bit);
  return true;
}

void OnForceBroken(CStateManager& mgr, uint32_t mrea, uint32_t editorId) {
  if (!Enabled() || gpGameState == nullptr)
    return;
  const int door = PortApWorld::FindDoorByForce(mrea, editorId);
  const int bit = PortApWorld::BeamDoorwayBit(door);
  if (bit < 0 || Test(gpGameState->PortBeamDoorBits(), bit))
    return;
  if (!PortAp::BeamDoorUnlockable(mrea, door))
    return;
  // Recorded now (a save or a room change must keep it); the loaded objects are
  // restyled when the shield has faded, not while it is still drawn.
  Set(gpGameState->PortBeamDoorBits(), bit);
  sPending.insert(bit);
  PortLog::Write("beam doors: doorway %d stays unlocked\n", bit);
}

void OnForceFaded(CStateManager& mgr, uint32_t mrea, uint32_t editorId) {
  if (sPending.empty() || gpGameState == nullptr)
    return;
  const int bit = PortApWorld::BeamDoorwayBit(PortApWorld::FindDoorByForce(mrea, editorId));
  if (bit < 0 || sPending.erase(bit) == 0 || !Test(gpGameState->PortBeamDoorBits(), bit))
    return;
  Restyle(mgr, bit);
  RefreshMap(mgr);
}

void Tick(CStateManager& mgr) {
  static bool seen = false;
  static bool last = false;
  const bool now = Enabled();
  if (seen && now == last)
    return;
  const bool first = !seen;
  seen = true;
  last = now;
  if (first || gpGameState == nullptr)
    return;
  if (!now)
    sPending.clear();
  const uint* bits = gpGameState->PortBeamDoorBits();
  for (int bit = 0; bit < PortApWorld::BeamDoorwayCount(); ++bit) {
    if (!Test(bits, bit))
      continue;
    if (now)
      Restyle(mgr, bit, true);
    else
      Relock(mgr, bit);
  }
  RefreshMap(mgr);
}

bool Reset() {
  if (gpGameState == nullptr)
    return false;
  uint* bits = gpGameState->PortBeamDoorBits();
  for (int i = 0; i < kWords; ++i)
    bits[i] = 0;
  sPending.clear();
  return true;
}

} // namespace PortBeamDoors
