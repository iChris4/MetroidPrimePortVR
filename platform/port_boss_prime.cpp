#include "port_savestate_boss.h"

#include "MetroidPrime/CCollisionActor.hpp"
#include "MetroidPrime/CCollisionActorManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Enemies/CMetroidPrime.hpp"
#include "MetroidPrime/Enemies/CMetroidPrimeRelay.hpp"
#include "MetroidPrime/Enemies/CMetroidPrimeStage2.hpp"
#include "MetroidPrime/ScriptObjects/CScriptWaypoint.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include "Kyoto/Math/CTransform4f.hpp"

#include "port_log.h"

#include <cstring>

const uint8_t PortBossState::kMetroidPrimeVersion = 1;
const uint8_t PortBossState::kMetroidPrimeStage2Version = 1;

namespace {

// The exo's place in its four stages (one per subchamber): waiting in InActive for the
// player (before the fight, or after a retreat until the next room's script sends Start),
// or fighting a stage.
enum ExoStage : uint8_t { kExoWaiting = 0, kExoFighting = 1 };

// What InActive's hand-over gives the boss bar (CMetroidPrime::InActive).
const float kExoBossEnergy = 2860.f;
const uint kExoBossString = 91;

void PutTransform(PortSaveState::BlockWriter& w, const CTransform4f& xf) {
  w.F32(xf.Get00()); w.F32(xf.Get01()); w.F32(xf.Get02()); w.F32(xf.Get03());
  w.F32(xf.Get10()); w.F32(xf.Get11()); w.F32(xf.Get12()); w.F32(xf.Get13());
  w.F32(xf.Get20()); w.F32(xf.Get21()); w.F32(xf.Get22()); w.F32(xf.Get23());
}

bool GetTransform(PortSaveState::BlockReader& r, CTransform4f& out) {
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

bool InState(const CPatterned& b, const char* name) {
  const char* cur = b.GetStateMachineState().GetName();
  return cur != nullptr && std::strcmp(cur, name) == 0;
}

// Before its first frame, when the state machine has not entered Start.
bool NotStarted(const CPatterned& b) {
  return b.GetStateMachineState().GetActorState() == nullptr;
}

CMetroidPrime* FindExo(CStateManager& mgr, TUniqueId uid) {
  if (CMetroidPrime* exo = PATTERNED_CAST_TO(CMetroidPrime, mgr.ObjectById(uid)))
    return exo;
  CObjectList& list = mgr.ObjectListById(kOL_PhysicsActor);
  for (int i = list.GetFirstObjectIndex(); i != -1; i = list.GetNextObjectIndex(i))
    if (CMetroidPrime* exo = PATTERNED_CAST_TO(CMetroidPrime, list[i]))
      return exo;
  return nullptr;
}

} // namespace

// The exo works its health out every frame from its stage (x91c_), the face hits left in
// the stage (x8d0_) and the face's health (its head collision actor), so those are what is
// saved, not its health. x570_/x580_ are the beam the face takes and the beams this stage
// has used. Its AI restarts in InActive; Start (x1444_24_) lets it hand over to the fight.
bool PortBossState::PutMetroidPrime(CStateManager& mgr, CMetroidPrimeRelay& relay,
                                    PortSaveState::BlockWriter& w) {
  CMetroidPrime* exo = PATTERNED_CAST_TO(CMetroidPrime, mgr.ObjectById(relay.x34_mpUid));
  if (exo == nullptr || !exo->IsAlive() || exo->x568_relayId != relay.GetUniqueId())
    return false;
  int stage = exo->x91c_;
  int hits = exo->x8d0_;
  CTransform4f xf = exo->GetTransform();
  ExoStage mode = kExoFighting;
  if (NotStarted(*exo) || InState(*exo, "Start") || InState(*exo, "InActive")) {
    mode = kExoWaiting;
  } else if (InState(*exo, "Retreat") || hits <= 0) {
    // Its last face hit is in: saved as the retreat ends (the stage counted, at the
    // waypoint it jumps to), since the room script already got the retreat's message.
    mode = kExoWaiting;
    ++stage;
    const TUniqueId wp = exo->GetConnectedRelayObject(mgr, kSS_Retreat, kSM_Follow);
    if (const CScriptWaypoint* waypoint = TCastToConstPtr< CScriptWaypoint >(mgr.GetObjectById(wp)))
      xf = waypoint->GetTransform();
  }
  float faceHP = exo->x8c0_.GetHP();
  if (const CCollisionActor* face =
          TCastToConstPtr< CCollisionActor >(mgr.GetObjectById(exo->x8cc_headColActor))) {
    // A broken face is given its full health back on the next frame.
    if (face->GetHealthInfo(mgr)->GetHP() > 0.f && !exo->x8f4_28_)
      faceHP = face->GetHealthInfo(mgr)->GetHP();
  }
  bool started = exo->x1444_24_;
  if (exo->mPortFaceHits > 0 && mode == kExoWaiting && !InState(*exo, "Retreat")) {
    // A restored fight still in InActive's hand-over: what it hands over is the fight.
    // Saved as waiting, a save taken in that second (quick resume) loses the stage's hits.
    mode = kExoFighting;
    hits = exo->mPortFaceHits;
    faceHP = exo->mPortFaceHP;
    started = false;
  }
  w.U8(mode);
  w.U32(static_cast< uint32_t >(stage));
  w.U32(static_cast< uint32_t >(hits));
  w.F32(faceHP);
  w.U32(static_cast< uint32_t >(exo->x570_));
  w.U32(exo->x580_);
  w.U8(started ? 1 : 0);
  w.U8(mgr.GetBossId() == exo->GetUniqueId() ? 1 : 0);
  PutTransform(w, xf);
  // Appended after version 1's payload: a build that predates it reads up to the pose.
  w.U8(exo->GetActive() ? 1 : 0);
  return true;
}

// The relay made a new exo from disc at InitializedInArea, before the room restore runs.
void PortBossState::RestoreMetroidPrime(CStateManager& mgr, CMetroidPrimeRelay& relay,
                                        PortSaveState::BlockReader* r) {
  // An older save has nothing of the exo (it had no record), so it starts from disc.
  if (r == nullptr)
    return;
  const uint8_t mode = r->U8();
  const int stage = static_cast< int >(r->U32());
  const int hits = static_cast< int >(r->U32());
  const float faceHP = r->F32();
  const uint32_t vulnerability = r->U32();
  const uint32_t used = r->U32();
  const bool started = r->U8() != 0;
  const bool bossBar = r->U8() != 0;
  CTransform4f xf = CTransform4f::Identity();
  const bool pose = GetTransform(*r, xf);
  if (!r->ok || !pose)
    return;
  // Saves without the byte: a fighting exo was active.
  const uint8_t activeByte = r->U8();
  const bool active = r->ok ? activeByte != 0 : mode == kExoFighting;
  if (mode > kExoFighting || stage < 0 || stage > 3 || used > 0xF ||
      !(faceHP == faceHP) || faceHP < 0.f || faceHP > 1e6f)
    return;
  if (mode == kExoFighting && (hits < 1 || hits > 64))
    return;

  CMetroidPrime* exo = FindExo(mgr, relay.x34_mpUid);
  if (exo == nullptr || !exo->IsAlive()) {
    PortLog::Write("[savestate] Metroid Prime: no exo to restore\n");
    return;
  }
  if (vulnerability >= static_cast< uint32_t >(exo->x588_.size()))
    return;
  // Another loaded subchamber's relay made it: move it to the saved one's room as
  // UpdateRoomTransition does, and take that relay's stage data.
  if (exo->x568_relayId != relay.GetUniqueId()) {
    const TAreaId area = relay.GetCurrentAreaId();
    if (exo->GetCurrentAreaId() != area) {
      exo->SetActorAreaId(mgr, exo->GetUniqueId(), area);
      for (uint i = 0; i < exo->x56c_collisionManager->GetNumCollisionActors(); ++i)
        exo->SetActorAreaId(
            mgr, exo->x56c_collisionManager->GetCollisionDescFromIndex(i).GetCollisionActorId(),
            area);
      for (int i = 0; i < exo->xb24_plasmaProjectileIds.size(); ++i)
        exo->SetActorAreaId(mgr, exo->xb24_plasmaProjectileIds[i], area);
      exo->SetActorAreaId(mgr, exo->xeac_, area);
    }
    exo->AttachToRelayInRoom(mgr, area);
  }

  exo->x91c_ = stage;
  exo->SetFaceVulnerability(mgr, static_cast< CMetroidPrime::EVulnerabilities >(vulnerability),
                            exo->x8f4_25_);
  exo->x580_ = used;
  exo->SetTransform(xf);
  exo->SetDestPos(xf.GetTranslation());
  // The Start that woke it went through the relay, which keeps no message journal.
  exo->x1444_24_ = started || mode == kExoFighting;
  if (mode == kExoFighting) {
    // Applied when InActive hands over, which sets a new stage's face hits.
    exo->mPortFaceHits = hits;
    exo->mPortFaceHP = rstl::min_val(faceHP, exo->x8c0_.GetHP());
  }
  if (bossBar)
    mgr.SetBossParams(exo->GetUniqueId(), kExoBossEnergy, kExoBossString);
  // The relay made it from its patterned info, inactive; the room script's Activate went
  // through the relay (forwarded), and the restore replays no relay message. Without it
  // the exo never thinks, so InActive never hands the saved stage back.
  if (active && !exo->GetActive())
    mgr.DeliverScriptMsg(exo, relay.GetUniqueId(), kSM_Activate);
}

// The essence's health is restored on its record, but Think copies the health of its
// lock-on collision actor over it every frame, and that actor was made with the spawn
// health: it gets the restored one. x6d8_damagePhase is how many of its summon thresholds
// (75/50/25 %) it has passed; x6dc_ the visor it shows in.
void PortBossState::PutMetroidPrimeStage2(CStateManager&, CMetroidPrimeStage2& b,
                                          PortSaveState::BlockWriter& w) {
  w.U8(static_cast< uint8_t >(b.x6d8_damagePhase));
  w.U8(static_cast< uint8_t >(b.x6dc_currentVisorPhase));
}

void PortBossState::RestoreMetroidPrimeStage2(CStateManager& mgr, CMetroidPrimeStage2& b,
                                              PortSaveState::BlockReader* r) {
  if (!b.IsAlive())
    return;
  b.SetupHealthInfo(mgr);
  int damagePhase = -1;
  int visorPhase = -1;
  if (r != nullptr) {
    damagePhase = r->U8();
    visorPhase = r->U8();
    if (!r->ok || damagePhase > 3 || visorPhase > 2)
      return;
  } else if (b.x6c4_hpFraction > 0.f) {
    // UpdateSummonType's thresholds, passed one per hit.
    const float hp = b.x6c4_hpFraction * b.HealthInfo(mgr)->GetHP();
    damagePhase = hp < 0.25f ? 3 : hp < 0.5f ? 2 : hp < 0.75f ? 1 : 0;
  }
  if (damagePhase >= 0)
    b.x6d8_damagePhase = damagePhase;
  if (visorPhase >= 0) {
    b.x6dc_currentVisorPhase = visorPhase;
    b.x6e0_previousVisorPhase = visorPhase;
  }
}
