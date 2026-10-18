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

// The doors of a doorway that are loaded take the blue look and weaknesses now;
// the ones in rooms not loaded get them from their room patch when they load.
void Restyle(CStateManager& mgr, int bit) {
  PortApWorld::DoorStyle style;
  if (!PortApWorld::BlueDoorStyle(style))
    return;
  const std::vector< uint8_t > weakness = PortApWorld::BlueDoorVulnerability();
  for (const PortApWorld::DoorChange& door : PortApWorld::BeamDoorway(bit)) {
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
  // The map icons of the loaded map areas.
  if (const CWorld* world = mgr.GetWorld()) {
    if (CMapWorld* map = world->GetMapWorld())
      map->PortRefreshDoors();
  }
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
