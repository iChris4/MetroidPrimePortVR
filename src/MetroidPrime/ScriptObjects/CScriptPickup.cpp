#include "MetroidPrime/ScriptObjects/CScriptPickup.hpp"

#include "MetroidPrime/CAnimData.hpp"
#include "MetroidPrime/CAnimPlaybackParms.hpp"
#include "MetroidPrime/CArtifactDoll.hpp"
#include "MetroidPrime/CExplosion.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CFirstPersonCamera.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerGun.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

#include "MetroidPrime/HUD/CHUDMemoParms.hpp"
#include "MetroidPrime/HUD/CSamusHud.hpp"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Text/CStringTable.hpp"

#include "port_apclient.h"
#ifdef TARGET_PC
#include "Collision/CMaterialFilter.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "port_map_pickups.h"
#endif
#include "port_randomizer.h"
#include "rstl/math.hpp"

static float skDrawInDistance = 30.f;

CScriptPickup::CScriptPickup(TUniqueId uid, const rstl::string& name, const CEntityInfo& info,
                             const CTransform4f& xf, const CModelData& mData,
                             const CActorParameters& aParams, const CAABox& aabb,
                             CPlayerState::EItemType itemType, int amount, int capacity,
                             CAssetId pickupEffect, float possibility, float lifeTime,
                             float fadeInTime, float startDelay, bool active)
: CPhysicsActor(uid, active, name, info, xf, mData, CMaterialList(), aabb,
                SMoverData(1.f, CVector3f::Zero(), CAxisAngle::Identity(), CVector3f::Zero(),
                           CAxisAngle::Identity()),
                aParams, 0.3f, 0.1f)
, x258_itemType(itemType)
, x25c_amount(amount)
, x260_capacity(capacity)
, x264_possibility(possibility)
, x268_fadeInTime(fadeInTime)
, x26c_lifeTime(lifeTime)
, x270_curTime(0.f)
, x278_delayTimer(startDelay)
, x28c_24_generated(false)
, x28c_25_inTractor(false)
, x28c_26_enableTractorTest(false) {
  if (pickupEffect != kInvalidAssetId) {
    x27c_pickupParticleDesc = gpSimplePool->GetObj(SObjectTag('PART', pickupEffect));
    x27c_pickupParticleDesc->Lock();
  }

  if (HasAnimation()) {
    AnimationData()->SetAnimation(CAnimPlaybackParms(0, -1, 1.f, true), false);
  }

  if (x268_fadeInTime) {
    SetModelFlags(CModelFlags::AlphaBlended(0.f).DepthCompareUpdate(true, false));
  }
}

CScriptPickup::~CScriptPickup() {}

#ifdef TARGET_PC
// Port: map_pickups = Seen. Marks this pickup's map dot once the player has
// looked at it for half a second: within 40 units of the camera, near the middle
// of the view and with a clear line from the eye to the pickup (static geometry
// and solid actors block; see-through surfaces such as gratings, the pickup
// itself and characters do not).
void CScriptPickup::PortMarkSeen(float dt, CStateManager& mgr) {
  if (PortMapPickups::Mode() != PortDebug::kMapPickups_Seen) {
    return;
  }
  xpc_seenStep += dt;
  if ((++xpc_seenTicks % 6) != 0) {
    return;
  }
  const float step = xpc_seenStep;
  xpc_seenStep = 0.f;
  if (!PortInSight(mgr)) {
    xpc_seenDwell = 0.f;
    return;
  }
  xpc_seenDwell += step;
  if (xpc_seenDwell >= 0.5f) {
    PortMapPickups::MarkSeen(static_cast< size_t >(xpc_dotIndex));
  }
}

bool CScriptPickup::PortInSight(CStateManager& mgr) {
  if (xpc_dotIndex == -2) {
    const CWorld* world = mgr.GetWorld();
    xpc_dotIndex = -1;
    if (world != nullptr && !x28c_24_generated) {
      const CGameArea& area = world->GetAreaAlways(GetAreaId());
      const CVector3f pos = GetTranslation();
      const float p[3] = {pos.GetX(), pos.GetY(), pos.GetZ()};
      xpc_dotIndex = PortMapPickups::DotIndex(world->IGetWorldAssetId(), area.GetAreaAssetId(), p);
    }
  }
  if (xpc_dotIndex < 0 || PortMapPickups::Seen(xpc_dotIndex)) {
    return false;
  }
  // Only the first-person camera sees pickups: not the morph ball, cinematic or boss cameras.
  const CFirstPersonCamera* camera = mgr.CameraManager()->FirstPersonCamera();
  if (camera == nullptr || mgr.CameraManager()->GetCurrentCameraId() != camera->GetUniqueId()) {
    return false;
  }
  const CVector3f eye = camera->GetTranslation();
  const CVector3f center = GetBoundingBox().GetCenterPoint();
  const CVector3f delta = center - eye;
  const float distSq = delta.MagSquared();
  if (distSq > 40.f * 40.f || !delta.CanBeNormalized()) {
    return false;
  }
  // In front of the camera and in the middle half of the view, so a pickup at the
  // edge of the screen doesn't count.
  if (CVector3f::Dot(camera->GetTransform().GetColumn(kDY), delta) <= 0.f) {
    return false;
  }
  const CVector3f screen = camera->ConvertToScreenSpace(center);
  if (fabsf(screen.GetX()) > 0.5f || fabsf(screen.GetY()) > 0.5f) {
    return false;
  }
  // As CVisorFlare's occlusion test: see-through surfaces don't block the view.
  static const CMaterialFilter kSightFilter = CMaterialFilter::MakeIncludeExclude(
      CMaterialList(kMT_Solid),
      CMaterialList(kMT_Player, kMT_Character, kMT_CameraPassthrough, kMT_ProjectilePassthrough,
                    kMT_SeeThrough));
  // Stop a little short, so floor and wall hugging pickups don't block themselves.
  const CVector3f end = center - delta.AsNormalized() * 0.5f;
  // True when nothing blocks the line.
  return mgr.RayCollideWorld(eye, end, kSightFilter, this);
}
#endif

void CScriptPickup::Think(float dt, CStateManager& mgr) {
  if (!GetActive()) {
    return;
  }

  if (x278_delayTimer >= 0.f) {
    CPhysicsActor::Stop();
    x278_delayTimer -= dt;
    return;
  }

#ifdef TARGET_PC
  PortMarkSeen(dt, mgr);
#endif

  x270_curTime += dt;
  if (x28c_25_inTractor && (x26c_lifeTime - x270_curTime) < 2.f) {
    x270_curTime = rstl::max_val(x26c_lifeTime - 2.f - FLT_EPSILON, x270_curTime - 2.f * dt);
  }

  CModelFlags drawFlags = CModelFlags::Normal();

  if (x268_fadeInTime) {
    if (x270_curTime < x268_fadeInTime) {
      drawFlags =
          CModelFlags::AlphaBlended(x270_curTime / x268_fadeInTime).DepthCompareUpdate(true, false);
    } else {
      x268_fadeInTime = 0.f;
    }
  } else if (x26c_lifeTime) {
    float alpha = 1.f;
    if (x26c_lifeTime < 2.f) {
      alpha = 1.f - (x26c_lifeTime / x270_curTime);
    } else if ((x26c_lifeTime - x270_curTime) < 2.f) {
      alpha = (x26c_lifeTime - x270_curTime) / 2.f;
    }

    drawFlags = CModelFlags::AlphaBlended(alpha).DepthCompareUpdate(true, false);
  }

  SetModelFlags(drawFlags);

  if (HasAnimation()) {
    CAdvancementDeltas deltas = UpdateAnimation(dt, mgr, true);
    MoveToOR(deltas.GetOffsetDelta(), dt);
    RotateToOR(deltas.GetOrientationDelta(), dt);
  }

  if (x28c_25_inTractor) {
    CVector3f velocity =
        mgr.GetPlayer()->GetTranslation() + (CVector3f::Up() * 2.f) - GetTranslation();
    x274_tractorTime += dt;
    float halfTractorTime = rstl::min_val(x274_tractorTime, 2.f) * 0.5f;
    velocity = velocity.AsNormalized() * (halfTractorTime * 20.f);
    if (x28c_26_enableTractorTest && mgr.GetPlayer()->GetPlayerGun()->GetChargePercentage() <
                                         CPlayerGun::GetTractorBeamFactor()) {
      x28c_26_enableTractorTest = false;
      x28c_25_inTractor = false;
      velocity = CVector3f::Zero();
    }
    SetVelocityWR(velocity);
  } else if (x28c_24_generated) {
    if (mgr.GetPlayer()->GetPlayerGun()->GetChargePercentage() >
        CPlayerGun::GetTractorBeamFactor()) {
      // Charge attraction is a sphere around the collection point, independent
      // of the game camera, headset facing, and cannon aim.
      const CVector3f posDelta =
          GetTranslation() - (mgr.GetPlayer()->GetTranslation() + CVector3f::Up() * 2.f);
      if (posDelta.MagSquared() < skDrawInDistance * skDrawInDistance) {
        x28c_25_inTractor = true;
        x28c_26_enableTractorTest = true;
        x274_tractorTime = 0.f;
      }
    }
  }

  if (x26c_lifeTime && x270_curTime > x26c_lifeTime) {
    mgr.DeleteObjectRequest(GetUniqueId());
  }
}

void CScriptPickup::Touch(CActor& act, CStateManager& mgr) {
  if (GetActive() && !(x278_delayTimer >= 0) && TCastToPtr< CPlayer >(act)) {
    CPlayerState::EItemType itemType = x258_itemType;
    // An Archipelago location: the server gives whatever this pickup holds,
    // so the retail item is not granted here.
    const bool apOwned =
        mgr.GetWorld() != nullptr &&
        PortAp::OwnsPickup(
            static_cast< uint32_t >(mgr.GetWorld()->IGetWorldAssetId()),
            static_cast< uint32_t >(mgr.GetWorld()->IGetAreaAlways(GetAreaId())->IGetAreaAssetId()),
            static_cast< uint32_t >(GetEditorId().Value()));
    if (!apOwned && itemType >= CPlayerState::kIT_Truth && itemType <= CPlayerState::kIT_Newborn) {
      CAssetId id = CArtifactDoll::GetArtifactHeadScanFromItemType(itemType);
      if (id != kInvalidAssetId) {
        mgr.PlayerState()->SetScanTime(id, 0.5f);
      }
    }

    if (x27c_pickupParticleDesc) {
      if (mgr.GetPlayerState()->GetActiveVisor(mgr) != CPlayerState::kPV_Thermal) {
        mgr.AddObject(rs_new CExplosion(
            TLockedToken< CGenDescription >(*x27c_pickupParticleDesc), mgr.AllocateUniqueId(), true,
            CEntityInfo(GetCurrentAreaId(), CEntity::NullConnectionList, kInvalidEditorId),
            rstl::string_l("Explosion - Pickup Effect"), GetTransform(), 0,
            CVector3f(1.f, 1.f, 1.f), CColor::White()));
      }
    }

    if (mgr.GetWorld() != nullptr) {
      const uint32_t randoWorld = static_cast< uint32_t >(mgr.GetWorld()->IGetWorldAssetId());
      const uint32_t randoArea = static_cast< uint32_t >(
          mgr.GetWorld()->IGetAreaAlways(GetAreaId())->IGetAreaAssetId());
      const uint32_t randoEntity = static_cast< uint32_t >(GetEditorId().Value());
      PortRandomizer::RecordCheck(randoWorld, randoArea, randoEntity,
                                  static_cast< int >(itemType));
      // Tell the Archipelago server about the check too, when this location has
      // an id configured. PortAp builds the same key the randomizer uses.
      char checkKey[32];
      PortRandomizer::FormatLocationKey(randoWorld, randoArea, randoEntity, checkKey,
                                        sizeof(checkKey));
      if (apOwned)
        PortAp::RecordPickup(randoWorld, randoArea, randoEntity);
      PortAp::QueueCheck(checkKey);
      if (apOwned)
        PortAp::AnnouncePickup(randoWorld, randoArea, randoEntity);
    }
    if (!apOwned) {
      mgr.PlayerState()->InitializePowerUp(itemType, x260_capacity);
      mgr.PlayerState()->IncrPickUp(itemType, x25c_amount);
      if (mgr.GetWorld() != nullptr) {
        PortRandomizer::LogPickupGrant(
            static_cast< uint32_t >(mgr.GetWorld()->IGetWorldAssetId()),
            static_cast< uint32_t >(mgr.GetWorld()->IGetAreaAlways(GetAreaId())->IGetAreaAssetId()),
            static_cast< uint32_t >(GetEditorId().Value()), static_cast< int >(itemType), x260_capacity,
            x25c_amount, mgr.GetPlayerState()->GetItemAmount(itemType),
            mgr.GetPlayerState()->GetItemCapacity(itemType));
      }
    }
    mgr.DeleteObjectRequest(GetUniqueId());
    SendScriptMsgs(kSS_Arrived, mgr, kSM_None);
    if (apOwned)
      return;

    if (x260_capacity > 0) {
      const CPlayerState* playerState = mgr.GetPlayerState();
      int total = playerState->GetTotalPickupCount();
      int colRate = playerState->CalculateItemCollectionRate();
      if (colRate == total) {
        CSystemState& state = gpGameState->SystemState();
        CAssetId id =
            gpResourceFactory
                ->GetResourceIdByName(state.GetAllItemsCollected() ? "STRG_AllPickupsFound_2"
                                                                   : "STRG_AllPickupsFound_1")
                ->GetId();
        mgr.QueueMessage(mgr.GetHUDMessageFrameCount() + 1, id, 0.f);
        state.SetAllItemsCollected(true);
      }
    }

    if (itemType == CPlayerState::kIT_PowerBombs) {
      CSystemState& state = gpGameState->SystemState();
      if (state.GetShowPowerBombAmmoMessage()) {
        state.IncrementPowerBombAmmoCount();
        CSamusHud::DisplayHudMemo(rstl::wstring_l(gpStringTable->GetString(109)),
                                  CHUDMemoParms(5.f, true, false, false));
      }
    }
  }
}

rstl::optional_object< CAABox > CScriptPickup::GetTouchBounds() const {
  return CPhysicsActor::GetBoundingBox();
}

void CScriptPickup::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId uid, CStateManager& mgr) {
  CPhysicsActor::AcceptScriptMsg(msg, uid, mgr);
}

void CScriptPickup::Render(const CStateManager& mgr) const { CPhysicsActor::Render(mgr); }

ENTITY_ACCEPT_IMPL(CScriptPickup)

CPlayerState::EItemType CScriptPickup::GetItem() const { return x258_itemType; }

float CScriptPickup::GetPossibility() const { return x264_possibility; }

void CScriptPickup::SetWasGenerated() { x28c_24_generated = true; }
