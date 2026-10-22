#include "port_savestate.h"
#include "port_log.h"

#include "Kyoto/Math/CTransform4f.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CObjectList.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Cameras/CCinematicCamera.hpp"
#include "MetroidPrime/Enemies/COmegaPirate.hpp"
#include "MetroidPrime/Enemies/CPatterned.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CWorldState.hpp"
#include "MetroidPrime/ScriptObjects/CScriptCounter.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDoor.hpp"
#include "MetroidPrime/ScriptObjects/CScriptEMPulse.hpp"
#include "MetroidPrime/ScriptObjects/CScriptHUDMemo.hpp"
#include "MetroidPrime/ScriptObjects/CScriptMazeNode.hpp"
#include "MetroidPrime/ScriptObjects/CScriptMemoryRelay.hpp"
#include "MetroidPrime/ScriptObjects/CScriptMidi.hpp"
#include "MetroidPrime/ScriptObjects/CScriptPlatform.hpp"
#include "MetroidPrime/ScriptObjects/CScriptSpiderBallWaypoint.hpp"
#include "MetroidPrime/ScriptObjects/CScriptStreamedMusic.hpp"
#include "MetroidPrime/ScriptObjects/CScriptTimer.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

// Block tags of a RoomObject. A reader skips tags it does not know.
enum RoomTag : uint32_t {
  kTagTransform = 1,  // 12 floats, the CActor transform
  kTagMotion = 2,     // velocity, momentum (CPhysicsActor)
  kTagHealth = 3,     // hp, knockback resistance
  // 4 was the AI state: forcing one skips what the enemy class sets up around it
  // (a parasite put into Patrol from Generate deletes itself), so enemies resume
  // through their own Start.
  kTagTimer = 5,      // time, timing
  kTagCounter = 6,    // current
  kTagDoor = 7,       // open
  kTagPlatform = 8,   // current and target waypoint (editor ids), speed, move delay
  kTagGenerated = 9,  // empty: a script generator's copy of the enemy with this editor id
  kTagMessages = 10,  // the script messages the enemy was sent (EncodeScriptMsgs)
};

// Friend of the script classes whose state is private.
struct PortRoomState {
  static void PutTimer(const CScriptTimer& t, PortSaveState::BlockWriter& w) {
    w.F32(t.x34_time);
    w.U8(t.x42_isTiming ? 1 : 0);
  }
  static void GetTimer(CScriptTimer& t, PortSaveState::BlockReader& r) {
    const float time = r.F32();
    const bool timing = r.U8() != 0;
    if (r.ok) {
      t.x34_time = time;
      t.x42_isTiming = timing;
      // Resumed timing starts at the saved time, not after a skipped tick.
#ifdef TARGET_PC
      t.mSkipNextTick = false;
#endif
    }
  }
  static void PutCounter(const CScriptCounter& c, PortSaveState::BlockWriter& w) {
    w.U32(static_cast< uint32_t >(c.x38_current));
  }
  static void GetCounter(CScriptCounter& c, PortSaveState::BlockReader& r) {
    const int current = static_cast< int >(r.U32());
    if (r.ok)
      c.x38_current = current;
  }
  static uint32_t WaypointId(const CStateManager& mgr, TUniqueId uid) {
    return uid == kInvalidUniqueId ? 0xFFFFFFFFu : mgr.GetEditorIdForUniqueId(uid).Value();
  }
  static void PutPlatform(const CStateManager& mgr, const CScriptPlatform& p,
                          PortSaveState::BlockWriter& w) {
    w.U32(WaypointId(mgr, p.x258_currentWaypoint));
    w.U32(WaypointId(mgr, p.x25a_targetWaypoint));
    w.F32(p.x25c_currentSpeed);
    w.F32(p.x260_moveDelay);
  }
  static void GetPlatform(CStateManager& mgr, CScriptPlatform& p, PortSaveState::BlockReader& r) {
    const uint32_t current = r.U32();
    const uint32_t target = r.U32();
    const float speed = r.F32();
    const float delay = r.F32();
    if (!r.ok)
      return;
    p.x258_currentWaypoint =
        current == 0xFFFFFFFFu ? kInvalidUniqueId : mgr.GetIdForScript(TEditorId(current));
    p.x25a_targetWaypoint =
        target == 0xFFFFFFFFu ? kInvalidUniqueId : mgr.GetIdForScript(TEditorId(target));
    p.x25c_currentSpeed = speed;
    p.x260_moveDelay = delay;
  }
  // Applies `move` (the platform's step from its loaded pose to its saved one) to
  // every slave, nested platforms' too, and keeps each slave's offset the way
  // CScriptActorRotate leaves it after a turn (orientation, translation from the
  // platform), so the next turn starts from the restored pose. Returns whether a
  // spider ball waypoint moved.
  static bool CarrySlaves(CStateManager& mgr, CScriptPlatform& p, const CTransform4f& move,
                          CScriptPlatform::TMovedList& moved) {
    bool waypoints = false;
    rstl::vector< SRiders >* lists[] = {&p.x328_slavesStatic, &p.x338_slavesDynamic};
    for (rstl::vector< SRiders >* list : lists) {
      for (rstl::vector< SRiders >::iterator it = list->begin(); it != list->end(); ++it) {
        CActor* act = TCastToPtr< CActor >(mgr.ObjectById(it->x0_uid));
        if (act == nullptr || CScriptPlatform::IsInMovedList(act->GetUniqueId(), moved))
          continue;
        moved.push_back(act->GetUniqueId().Value());
        act->SetTransform(move * act->GetTransform());
        CTransform4f offset = act->GetTransform();
        offset.SetTranslation(act->GetTranslation() - p.GetTranslation());
        it->x8_transform = offset;
        if (TCastToPtr< CScriptSpiderBallWaypoint >(act) != nullptr)
          waypoints = true;
        if (CScriptPlatform* child = TCastToPtr< CScriptPlatform >(act))
          waypoints |= CarrySlaves(mgr, *child, move, moved);
      }
    }
    return waypoints;
  }
  // Puts a platform at its saved pose and carries its slaves with it, turned as well
  // as moved (slaves keep only their pose from the room load, so a bare SetTransform
  // leaves the flame jets of a piston where the room loaded them, and Ore
  // Processing's spider tracks unturned on a turned column, issue #172). A slave
  // with a saved pose of its own gets it after this. Returns whether a spider ball
  // waypoint moved.
  static bool RestorePlatformPose(CStateManager& mgr, CScriptPlatform& p, const CTransform4f& xf) {
    const CTransform4f move = xf * p.GetTransform().GetInverse();
    p.SetTransform(xf);
    CScriptPlatform::TMovedList moved;
    moved.push_back(p.GetUniqueId().Value());
    const bool waypoints = CarrySlaves(mgr, p, move, moved);
    // A delta PreThink worked out from the pose before this one would move the
    // slaves again on the next Think.
    p.x270_dragDelta = CVector3f::Zero();
    return waypoints;
  }
  // What CScriptActorRotate does after turning a waypoint: every waypoint's path and
  // touch bounds are rebuilt from the poses now in place.
  static void RebuildSpiderBallWaypoints(CStateManager& mgr) {
    std::vector< CScriptSpiderBallWaypoint* > waypoints;
    CObjectList& list = mgr.ObjectListById(kOL_All);
    for (int i = list.GetFirstObjectIndex(); i != -1; i = list.GetNextObjectIndex(i))
      if (CScriptSpiderBallWaypoint* wp = TCastToPtr< CScriptSpiderBallWaypoint >(list[i]))
        waypoints.push_back(wp);
    for (CScriptSpiderBallWaypoint* wp : waypoints)
      wp->ClearWaypoints();
    for (CScriptSpiderBallWaypoint* wp : waypoints) {
      wp->BuildWaypointListAndBounds(mgr);
      wp->SetTransformDirty(true);
    }
  }
};

namespace PortSaveState {
namespace {

constexpr uint32_t kNoEditorId = 0xFFFFFFFFu;

void PutTransform(BlockWriter& w, const CTransform4f& xf) {
  w.F32(xf.Get00()); w.F32(xf.Get01()); w.F32(xf.Get02()); w.F32(xf.Get03());
  w.F32(xf.Get10()); w.F32(xf.Get11()); w.F32(xf.Get12()); w.F32(xf.Get13());
  w.F32(xf.Get20()); w.F32(xf.Get21()); w.F32(xf.Get22()); w.F32(xf.Get23());
}

bool GetTransform(BlockReader& r, CTransform4f& out) {
  float m[12];
  for (float& f : m)
    f = r.F32();
  if (!r.ok)
    return false;
  for (float f : m)
    if (!(f == f) || f > 1e7f || f < -1e7f)
      return false;
  out = CTransform4f(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
  return true;
}

void PutVec(BlockWriter& w, const CVector3f& v) {
  w.F32(v.GetX());
  w.F32(v.GetY());
  w.F32(v.GetZ());
}

bool GetVec(BlockReader& r, CVector3f& out) {
  const float x = r.F32();
  const float y = r.F32();
  const float z = r.F32();
  if (!r.ok || !(x == x) || !(y == y) || !(z == z))
    return false;
  out = CVector3f(x, y, z);
  return true;
}

// Every live object of the world, one pass over the all-objects list.
template < typename F >
void ForEachObject(CStateManager& mgr, F&& fn) {
  CObjectList& list = mgr.ObjectListById(kOL_All);
  for (int idx = list.GetFirstObjectIndex(); idx != -1; idx = list.GetNextObjectIndex(idx)) {
    if (CEntity* ent = list[idx])
      fn(*ent);
  }
}

bool AreaReady(const CWorld& world, int area) {
  if (area < 0 || area >= world.GetNumAreas())
    return false;
  const CGameArea& a = world.GetAreaAlways(TAreaId(area));
  return a.IsLoaded() && a.IsValidated();
}

// The room script data a loaded area runs now: the patched copy when a
// cutscene-skip patch swapped one in, or null/0 when there is nothing to read.
void LoadedScriptData(const CWorld& world, int area, const uint8_t*& data, size_t& size) {
  const CGameArea& a = world.GetAreaAlways(TAreaId(area));
  const int n = a.GetScriptingSize();
  const char* bytes = a.GetScriptingData();
  if (n <= 0 || bytes == nullptr) {
    data = nullptr;
    size = 0;
    return;
  }
  data = reinterpret_cast< const uint8_t* >(bytes);
  size = static_cast< size_t >(n);
}

// The object's editor id when it is a scripted object of a captured-or-
// applied area (dynamic objects have none), else kNoEditorId.
uint32_t ScriptId(const CEntity& ent) {
  if (ent.IsInGraveyard() || ent.GetEditorId() == kInvalidEditorId)
    return kNoEditorId;
  return ent.GetEditorId().Value();
}

// What the CStateManager hooks noted per object (raw TUniqueId value), tagged with
// the manager it belongs to so a world being torn down never touches the next one's.
struct Noted {
  const CStateManager* mgr = nullptr;
  bool generated = false;
  std::vector< ScriptMsgEntry > msgs;
};
std::map< uint16_t, Noted > sNoted;

const Noted* FindNoted(const CStateManager& mgr, TUniqueId uid) {
  const auto it = sNoted.find(uid.value);
  return it != sNoted.end() && it->second.mgr == &mgr ? &it->second : nullptr;
}

Noted& NoteFor(const CStateManager& mgr, TUniqueId uid) {
  Noted& n = sNoted[uid.value];
  if (n.mgr != &mgr) {
    n = Noted();
    n.mgr = &mgr;
  }
  return n;
}

TUniqueId RawId(uint16_t value) {
  TUniqueId uid;
  uid.value = value;
  return uid;
}

bool IsGone(const CEntity& ent) {
  const CPatterned* patterned = TCastToConstPtr< CPatterned >(const_cast< CEntity* >(&ent));
  return patterned != nullptr && !patterned->IsAlive();
}

// A script generator's copy carries its template's editor id. Enemy copies are saved
// as copies (fights count their deaths); other copies (pickups, maze walls) are not
// saved at all, as before, but no longer overwrite their template's record.
bool IsGeneratedCopy(const CStateManager& mgr, CEntity& ent) {
  const Noted* n = FindNoted(mgr, ent.GetUniqueId());
  return n != nullptr && n->generated;
}

void CaptureObject(CStateManager& mgr, CEntity& ent, RoomObject& obj) {
  obj.editorId = ent.GetEditorId().Value();
  obj.active = ent.GetActive();
  if (const Noted* n = FindNoted(mgr, ent.GetUniqueId())) {
    if (n->generated)
      obj.blocks.push_back({kTagGenerated, std::string()});
    if (!n->msgs.empty())
      obj.blocks.push_back({kTagMessages, EncodeScriptMsgs(n->msgs)});
  }

  CActor* actor = TCastToPtr< CActor >(&ent);
  if (actor != nullptr) {
    if (const CHealthInfo* health = actor->GetHealthInfo(mgr)) {
      BlockWriter w;
      w.F32(health->GetHP());
      w.F32(health->GetKnockBackResistance());
      obj.blocks.push_back({kTagHealth, w.bytes});
    }
  }
  CPhysicsActor* physics = TCastToPtr< CPhysicsActor >(&ent);
  const bool isDoor = TCastToPtr< CScriptDoor >(&ent) != nullptr;
  if (physics != nullptr && !isDoor) {
    // Inert props keep the card load's pose: a snapshot of every script actor would
    // cost kilobytes (quick resume writes one every 30 s) for nothing. Enemies,
    // platforms and anything moving are saved.
    const bool moving = physics->GetVelocityWR().IsNonZero() ||
                        physics->GetMomentumWR().IsNonZero();
    if (moving || TCastToPtr< CPatterned >(&ent) != nullptr ||
        TCastToPtr< CScriptPlatform >(&ent) != nullptr) {
      BlockWriter xf;
      PutTransform(xf, physics->GetTransform());
      obj.blocks.push_back({kTagTransform, xf.bytes});
      BlockWriter motion;
      PutVec(motion, physics->GetVelocityWR());
      PutVec(motion, physics->GetMomentumWR());
      obj.blocks.push_back({kTagMotion, motion.bytes});
    }
  }
  if (CScriptTimer* timer = TCastToPtr< CScriptTimer >(&ent)) {
    BlockWriter w;
    PortRoomState::PutTimer(*timer, w);
    obj.blocks.push_back({kTagTimer, w.bytes});
  }
  if (CScriptCounter* counter = dynamic_cast< CScriptCounter* >(&ent)) {
    BlockWriter w;
    PortRoomState::PutCounter(*counter, w);
    obj.blocks.push_back({kTagCounter, w.bytes});
  }
  if (CScriptDoor* door = TCastToPtr< CScriptDoor >(&ent)) {
    DoorSaveState state;
    state.open = door->IsOpen();
    // A door that reports open has no close in flight, so its pending bits stay
    // clear ("open with a close in flight" is not a payload this build writes).
    if (!state.open) {
      state.pendingClose = door->PortClosingPending();
      state.refusedClosePending = door->PortRefusedClosePending();
    }
    state.doClose = door->PortDoClosePending();
    obj.blocks.push_back({kTagDoor, EncodeDoorSaveState(state)});
  }
  if (CScriptPlatform* platform = TCastToPtr< CScriptPlatform >(&ent)) {
    BlockWriter w;
    PortRoomState::PutPlatform(mgr, *platform, w);
    obj.blocks.push_back({kTagPlatform, w.bytes});
  }
}

// Delivers `msg` with the entity's links cleared: the handler's own sends (kSS_Active
// and the like) are dropped, since their targets are restored from the save themselves.
void DeliverUnlinked(CStateManager& mgr, CEntity& ent, TUniqueId sender, EScriptObjectMessage msg) {
  const rstl::vector< SConnection > conns = ent.ConnectionList();
  ent.ConnectionList().clear();
  mgr.DeliverScriptMsg(&ent, sender, msg);
  ent.ConnectionList() = conns;
}

// Many classes do their setup in the kSM_Activate/kSM_Deactivate handler (Flaahgra builds
// its body controller there, bosses and turrets switch their collision actors, drones their
// lights), and the script that sent it never runs again after a state load, so a bare
// SetActive leaves them half set up (a mid-fight Flaahgra crashed in Think on a null body
// controller). A cinematic camera's handler starts the cinematic, so it only gets the flag.
// Same for handlers with one-shot side effects: memory relays (mailbox), HUD memos, streamed
// music/MIDI stop, EMP pulses (static interference) and maze nodes (barrier delete).
void RestoreActive(CStateManager& mgr, CEntity& ent, bool active) {
  if (ent.GetActive() == active)
    return;
  if (dynamic_cast< CCinematicCamera* >(&ent) != nullptr ||
      dynamic_cast< CScriptMemoryRelay* >(&ent) != nullptr ||
      dynamic_cast< CScriptHUDMemo* >(&ent) != nullptr ||
      dynamic_cast< CScriptStreamedMusic* >(&ent) != nullptr ||
      dynamic_cast< CScriptMidi* >(&ent) != nullptr ||
      dynamic_cast< CScriptEMPulse* >(&ent) != nullptr ||
      dynamic_cast< CScriptMazeNode* >(&ent) != nullptr) {
    ent.SetActive(active);
    return;
  }
  DeliverUnlinked(mgr, ent, kInvalidUniqueId, active ? kSM_Activate : kSM_Deactivate);
  if (ent.GetActive() != active)
    ent.SetActive(active);
}

// Sends the enemy the script messages it had been sent before the save, in order (what
// woke a boss: a trigger's Action to Thardus), and keeps them for the next save.
// Senders are the objects of the rebuilt room with the saved editor ids.
int ReplayScriptMsgs(CStateManager& mgr, CEntity& ent, const RoomObject& obj) {
  const RoomBlock* b = FindBlock(obj, kTagMessages);
  std::vector< ScriptMsgEntry > journal;
  if (b == nullptr || !DecodeScriptMsgs(b->data, journal) || journal.empty())
    return 0;
  const TUniqueId uid = ent.GetUniqueId();
  for (const ScriptMsgEntry& e : journal) {
    const TUniqueId sender =
        e.sender == kNoEditorId ? kInvalidUniqueId : mgr.GetIdForScript(TEditorId(e.sender));
    DeliverUnlinked(mgr, ent, sender, static_cast< EScriptObjectMessage >(e.msg));
    // A handler may delete the object (an enemy told to go away).
    if (mgr.ObjectById(uid) != &ent || ent.IsInGraveyard())
      break;
  }
  NoteFor(mgr, uid).msgs = journal;
  return static_cast< int >(journal.size());
}

void ApplyObject(CStateManager& mgr, CEntity& ent, const RoomObject& obj, RoomApplyStats& stats) {
  RestoreActive(mgr, ent, obj.active);
  // Before the saved health and pose, which then win over whatever a message reset.
  stats.replayed += ReplayScriptMsgs(mgr, ent, obj);
  if (ent.IsInGraveyard())
    return;

  CActor* actor = TCastToPtr< CActor >(&ent);
  if (actor != nullptr) {
    if (const RoomBlock* b = FindBlock(obj, kTagHealth)) {
      BlockReader r(b->data);
      const float hp = r.F32();
      const float resist = r.F32();
      if (r.ok && hp == hp) {
        if (CHealthInfo* health = actor->HealthInfo(mgr)) {
          health->SetHP(hp);
          health->SetKnockbackResistance(resist);
        }
      }
    }
  }
  if (CPhysicsActor* physics = TCastToPtr< CPhysicsActor >(&ent)) {
    if (const RoomBlock* b = FindBlock(obj, kTagTransform)) {
      BlockReader r(b->data);
      CTransform4f xf = CTransform4f::Identity();
      if (GetTransform(r, xf)) {
        physics->SetTransform(xf);
        // An enemy's path destination is where it stands, not the room origin (the
        // rebuilt object's zero). AI states only move it off that when the player is
        // out of range: Omega Pirate restored inside its attack range searched a path
        // to the origin, failed, and a shagged path keeps it from turning or attacking.
        if (CPatterned* patterned = TCastToPtr< CPatterned >(&ent))
          patterned->SetDestPos(xf.GetTranslation());
      }
    }
    if (const RoomBlock* b = FindBlock(obj, kTagMotion)) {
      BlockReader r(b->data);
      CVector3f velocity;
      CVector3f momentum;
      if (GetVec(r, velocity) && GetVec(r, momentum)) {
        physics->SetVelocityWR(velocity);
        physics->SetMomentumWR(momentum);
      }
    }
  }
  // Omega Pirate keeps its health in a collision actor and needs its intro's Reset later.
  if (COmegaPirate* omega = dynamic_cast< COmegaPirate* >(&ent))
    omega->PortRestoredFromSave(mgr);
  if (CScriptTimer* timer = TCastToPtr< CScriptTimer >(&ent)) {
    if (const RoomBlock* b = FindBlock(obj, kTagTimer)) {
      BlockReader r(b->data);
      PortRoomState::GetTimer(*timer, r);
    }
  }
  if (CScriptCounter* counter = dynamic_cast< CScriptCounter* >(&ent)) {
    if (const RoomBlock* b = FindBlock(obj, kTagCounter)) {
      BlockReader r(b->data);
      PortRoomState::GetCounter(*counter, r);
    }
  }
  if (CScriptDoor* door = TCastToPtr< CScriptDoor >(&ent)) {
    if (const RoomBlock* b = FindBlock(obj, kTagDoor)) {
      DoorSaveState want;
      // No script message here: a door that still owes Closed sends it from Think's
      // own branch for the kind of close it was in (once the rest of the saved room
      // state has been applied). A payload this build did not write is ignored.
      if (DecodeDoorSaveState(b->data, want)) {
        CScriptDoor::PortDoorFlags flags;
        flags.open = want.open;
        flags.normalClosePending = want.pendingClose;
        flags.refusedClosePending = want.refusedClosePending;
        flags.doClose = want.doClose;
        // Applied even when the door already reads closed: that is what restores
        // the message an interrupted close still owes, and the block on opening
        // again a close in flight carries.
        if (flags.normalClosePending || flags.refusedClosePending || flags.doClose ||
            flags.open != door->IsOpen())
          door->PortRestoreDoorState(flags);
      }
    }
  }
  if (CScriptPlatform* platform = TCastToPtr< CScriptPlatform >(&ent)) {
    if (const RoomBlock* b = FindBlock(obj, kTagPlatform)) {
      BlockReader r(b->data);
      PortRoomState::GetPlatform(mgr, *platform, r);
    }
  }
}

// The saved platform poses of the areas applied now, each carrying its slaves,
// parents ahead of the platforms they carry. ApplyObject then sets the same pose
// again (a no-op) and the absolute pose of any saved slave. Returns whether a
// spider ball waypoint moved.
bool RestorePlatforms(CStateManager& mgr, const RoomState& room, const std::set< int >& now) {
  struct Saved {
    CScriptPlatform* platform;
    CTransform4f xf;
  };
  std::vector< Saved > saved;
  for (const RoomObject& obj : room.objects) {
    if (now.count(TEditorId(obj.editorId).AreaNum()) == 0)
      continue;
    const RoomBlock* b = FindBlock(obj, kTagTransform);
    if (b == nullptr)
      continue;
    CEntity* ent = mgr.ObjectById(mgr.GetIdForScript(TEditorId(obj.editorId)));
    CScriptPlatform* platform = ent != nullptr ? TCastToPtr< CScriptPlatform >(ent) : nullptr;
    if (platform == nullptr || platform->IsInGraveyard())
      continue;
    BlockReader r(b->data);
    Saved s = {platform, CTransform4f::Identity()};
    if (GetTransform(r, s.xf))
      saved.push_back(s);
  }
  std::vector< std::vector< size_t > > parents(saved.size());
  for (size_t child = 0; child < saved.size(); ++child) {
    for (size_t parent = 0; parent < saved.size(); ++parent) {
      if (parent != child && saved[parent].platform->IsSlave(saved[child].platform->GetUniqueId()))
        parents[child].push_back(parent);
    }
  }
  std::vector< size_t > order;
  if (!PlatformRestoreOrder(parents, order)) {
    // Each platform still gets its saved pose from ApplyObject, without its slaves.
    PortLog::Write("[savestate] platforms carry each other in a cycle; their slaves stay as loaded\n");
    return false;
  }
  bool waypoints = false;
  for (size_t i : order)
    waypoints |= PortRoomState::RestorePlatformPose(mgr, *saved[i].platform, saved[i].xf);
  return waypoints;
}


// Spawn layers (issue #161). The layer bits each area's script objects were
// last spawned with, keyed by world and area, and the override ArmSpawnLayers
// installed for a state load.
std::map< std::pair< uint32_t, int >, uint64_t > sSpawnMasks;
struct SpawnOverride {
  uint32_t world = 0;
  int area = 0;
  uint64_t spawn = 0;  // bits the area spawns with
  uint64_t live = 0;   // bits to put back afterwards
};
std::vector< SpawnOverride > sOverrides;

uint32_t WorldKey(CStateManager& mgr) {
  const CWorld* world = mgr.GetWorld();
  return world != nullptr ? static_cast< uint32_t >(world->IGetWorldAssetId()) : 0;
}

uint64_t LayerMask(const CScriptLayerManager& layers, int area) {
  return layers.GetAreaLayers()[area].m_layerBits;
}

void SetLayerMask(CScriptLayerManager& layers, int area, uint64_t mask) {
  const int count = layers.GetAreaLayerCount(TAreaId(area));
  for (int l = 0; l < count; ++l)
    layers.SetLayerActive(TAreaId(area), TLayerId(l), ((mask >> l) & 1) != 0);
}

CScriptLayerManager* WorldLayers(uint32_t worldId, int area) {
  if (gpGameState == nullptr)
    return nullptr;
  CScriptLayerManager* layers = gpGameState->StateForWorld(static_cast< CAssetId >(worldId)).GetLayerState().GetPtr();
  if (layers == nullptr || area < 0 || area >= static_cast< int >(layers->GetAreaLayers().size()))
    return nullptr;
  return layers;
}

void RestoreOverride(const SpawnOverride& o) {
  if (CScriptLayerManager* layers = WorldLayers(o.world, o.area))
    SetLayerMask(*layers, o.area, o.live);
}

} // namespace

void NoteScriptMsg(CStateManager& mgr, uint16_t dest, uint16_t sender, int msg) {
  if (!JournalsScriptMsg(msg))
    return;
  CEntity* ent = mgr.ObjectById(RawId(dest));
  if (ent == nullptr || TCastToPtr< CPatterned >(ent) == nullptr || ScriptId(*ent) == kNoEditorId)
    return;
  ScriptMsgEntry e;
  e.msg = static_cast< uint8_t >(msg);
  const TUniqueId from = RawId(sender);
  if (from != kInvalidUniqueId && mgr.ObjectById(from) != nullptr) {
    const TEditorId eid = mgr.GetEditorIdForUniqueId(from);
    if (eid != kInvalidEditorId)
      e.sender = eid.Value();
  }
  AppendScriptMsg(NoteFor(mgr, ent->GetUniqueId()).msgs, e);
}

void NoteGenerated(CStateManager& mgr, uint16_t uid) { NoteFor(mgr, RawId(uid)).generated = true; }

void NoteRemoved(CStateManager& mgr, uint16_t uid) {
  const auto it = sNoted.find(uid);
  if (it != sNoted.end() && it->second.mgr == &mgr)
    sNoted.erase(it);
}

void NoteAreaSpawnBegin(CStateManager& mgr, int area) {
  const CScriptLayerManager* layers = mgr.WorldLayerState().GetPtr();
  if (layers == nullptr || area < 0 || area >= static_cast< int >(layers->GetAreaLayers().size()))
    return;
  sSpawnMasks[{WorldKey(mgr), area}] = LayerMask(*layers, area);
}

void NoteAreaSpawnEnd(CStateManager& mgr, int area) {
  const uint32_t world = WorldKey(mgr);
  for (size_t i = 0; i < sOverrides.size(); ++i) {
    if (sOverrides[i].world == world && sOverrides[i].area == area) {
      RestoreOverride(sOverrides[i]);
      sOverrides.erase(sOverrides.begin() + static_cast< ptrdiff_t >(i));
      return;
    }
  }
}

void ArmSpawnLayers(const RoomState& room, uint32_t worldId) {
  // The game state was just rebuilt: an override left from before belongs to the old one.
  sOverrides.clear();
  if (!room.spawnLayersRecorded)
    return;
  for (const AreaSpawnLayers& sl : room.spawnLayers) {
    if (!sl.known)
      continue;
    CScriptLayerManager* layers = WorldLayers(worldId, sl.area);
    if (layers == nullptr)
      continue;
    SpawnOverride o{worldId, sl.area, sl.mask, LayerMask(*layers, sl.area)};
    if (o.spawn == o.live)
      continue;
    SetLayerMask(*layers, sl.area, sl.mask);
    sOverrides.push_back(o);
  }
}

void DisarmSpawnLayers() {
  for (const SpawnOverride& o : sOverrides)
    RestoreOverride(o);
  sOverrides.clear();
}

void SetSpawnLayersLive(bool live) {
  for (const SpawnOverride& o : sOverrides) {
    if (CScriptLayerManager* layers = WorldLayers(o.world, o.area))
      SetLayerMask(*layers, o.area, live ? o.live : o.spawn);
  }
}

void CaptureRoomState(CStateManager& mgr, RoomState& out) {
  out = {};
  const CWorld* world = mgr.GetWorld();
  if (world == nullptr)
    return;
  // This writer records one fingerprint per captured area; an area whose
  // scripting data is not readable stays without one.
  out.fingerprintsRecorded = true;
  out.spawnLayersRecorded = true;
  std::set< int > areas;
  for (int i = 0; i < world->GetNumAreas(); ++i) {
    if (AreaReady(*world, i)) {
      areas.insert(i);
      out.areas.push_back(i);
      const uint8_t* data = nullptr;
      size_t size = 0;
      LoadedScriptData(*world, i, data, size);
      out.fingerprints.push_back(MakeAreaFingerprint(i, data, size));
      AreaSpawnLayers sl;
      sl.area = i;
      const auto spawned = sSpawnMasks.find({WorldKey(mgr), i});
      if (spawned != sSpawnMasks.end()) {
        sl.known = true;
        sl.mask = spawned->second;
      }
      out.spawnLayers.push_back(sl);
    }
  }
  ForEachObject(mgr, [&](CEntity& ent) {
    const uint32_t id = ScriptId(ent);
    if (id == kNoEditorId || areas.count(ent.GetEditorId().AreaNum()) == 0 || IsGone(ent))
      return;
    const bool generated = IsGeneratedCopy(mgr, ent);
    if (generated && TCastToPtr< CPatterned >(&ent) == nullptr)
      return;
    // A copy's editor id is its template's: the template decides whether that id is present.
    if (!generated)
      out.present.push_back(id);
    RoomObject obj;
    CaptureObject(mgr, ent, obj);
    out.objects.push_back(std::move(obj));
  });
}

int ApplyRoomState(CStateManager& mgr, const RoomState& room, std::vector< int32_t >& done,
                   RoomApplyStats& stats) {
  const CWorld* world = mgr.GetWorld();
  if (world == nullptr)
    return 0;
  std::set< int > now;
  int waiting = 0;
  for (int32_t area : room.areas) {
    if (std::find(done.begin(), done.end(), area) != done.end())
      continue;
    if (!AreaReady(*world, area)) {
      ++waiting;
      continue;
    }
    // A rebuilt area may have loaded a different room script than the snapshot
    // was taken from (a mod reload swaps the patched script under the load).
    // Its saved editor ids and states then describe other objects, so nothing
    // is applied to or deleted from that area. It still counts as handled: the
    // data will not change, so there is nothing to wait for.
    const uint8_t* data = nullptr;
    size_t size = 0;
    LoadedScriptData(*world, area, data, size);
    const RoomAreaRestore decision = DecideAreaRestore(room, area, data, size);
    if (decision != RoomAreaRestore::kRestore) {
      PortLog::Write("[savestate] area %d: %s; its room state is left as loaded\n",
                     static_cast< int >(area), RoomAreaRestoreReason(decision));
      ++stats.skipped;
      done.push_back(area);
      continue;
    }
    now.insert(area);
  }
  if (now.empty())
    return waiting;

  const std::set< uint32_t > present(room.present.begin(), room.present.end());
  std::vector< TUniqueId > doomed;
  ForEachObject(mgr, [&](CEntity& ent) {
    const uint32_t id = ScriptId(ent);
    if (id == kNoEditorId || now.count(ent.GetEditorId().AreaNum()) == 0)
      return;
    if (present.count(id) == 0 || IsGone(ent))
      doomed.push_back(ent.GetUniqueId());
  });
  for (TUniqueId uid : doomed) {
    mgr.DeleteObjectRequest(uid);
    ++stats.removed;
  }
  const bool waypointsMoved = RestorePlatforms(mgr, room, now);
  std::vector< const RoomObject* > copies;
  for (const RoomObject& obj : room.objects) {
    if (now.count(TEditorId(obj.editorId).AreaNum()) == 0)
      continue;
    if (FindBlock(obj, kTagGenerated) != nullptr) {
      copies.push_back(&obj);
      continue;
    }
    CEntity* ent = mgr.ObjectById(mgr.GetIdForScript(TEditorId(obj.editorId)));
    if (ent == nullptr || ent->IsInGraveyard()) {
      ++stats.unmatched;
      continue;
    }
    ApplyObject(mgr, *ent, obj, stats);
    ++stats.applied;
  }
  // The enemies a script generator made (Hive Totem's wasps): the rebuilt room has
  // only their templates, and a fight counting their deaths would wait forever.
  // Made again from the template the way CScriptGenerator does, then given the
  // copy's saved state (ApplyObject sends the Activate the generator sent).
  for (const RoomObject* obj : copies) {
    const bool wasGenerating = mgr.IsGeneratingObject();
    mgr.SetIsGeneratingObject(true);
    const TUniqueId uid = mgr.GenerateObject(TEditorId(obj->editorId)).second;
    mgr.SetIsGeneratingObject(wasGenerating);
    CEntity* ent = uid != kInvalidUniqueId ? mgr.ObjectById(uid) : nullptr;
    if (ent == nullptr) {
      ++stats.unmatched;
      continue;
    }
    ApplyObject(mgr, *ent, *obj, stats);
    ++stats.generated;
  }
  if (waypointsMoved)
    PortRoomState::RebuildSpiderBallWaypoints(mgr);
  done.insert(done.end(), now.begin(), now.end());
  return waiting;
}

} // namespace PortSaveState
