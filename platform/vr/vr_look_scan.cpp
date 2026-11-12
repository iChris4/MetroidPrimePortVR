// SPDX-License-Identifier: GPL-3.0-or-later
//
// Look to scan: PrimedGun's HMD scan targeting (NativeRuntime.cpp
// UpdateScanTargetingFromHmd, SeedScanIndicatorTargetsFromHmd and the
// "Gun Ray Lock/Scan Target Hook" on CPlayer::FindOrbitTargetId), on the
// decomp's own objects instead of memory reads.
//
// In the scan visor the body keeps its yaw-only facing while the head looks
// around, so the game's screen-box targeting (CPlayer::FindBestOrbitableObject
// against the first-person camera) would pick what is in front of the body.
// Here every tick the scannable objects around the head's gaze are measured
// with PrimedGun's cones (vr/vr_look_scan.h): the ones in the wide cone get
// their scan icons, and the best one in the narrow cone that the eye can see
// becomes the scan target. The game's own rules still apply to both: the
// object must pass CPlayer::ValidateOrbitTargetId (targetable, active, scan
// visor flag, same area, not straight above or below) and the target
// distance test, and the target the game's line-of-sight test.
//
// Look to lock-on and look to grapple do the same in the other visors for the
// orbit target (the L lock), each for its own kind of object: grapple points,
// and everything else that can be locked. PrimedGun picked those with the
// cannon's aim ray; the configured cannon ray picks while a controller tracks
// the cannon, else the head's (a gamepad), with the scan target's cone and
// FindBestOrbitableObject's rules (not the current target, the Grapple Beam
// and the orbit distance for a grapple point, a swing-locked point only from
// its swing plane, line of sight). Combat and grapple retain native body
// physics. CFirstPersonCamera keeps a separate acquisition offset for the
// view; vr_look_lock_no_camera_turn freezes that view heading when enabled.

#include "vr/vr_look_scan.h"
#include "vr/vr_settings.h"
#include "vr/vr_view.h"

#include "Collision/CMaterialFilter.hpp"
#include "Collision/CMaterialList.hpp"
#include "Collision/CRayCastResult.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CMath.hpp"
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/ScriptObjects/CScriptGrapplePoint.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayer.hpp"

#include <algorithm>

namespace PortVr {
namespace {

// PrimedGun keeps the 16 best objects for icons and the 8 best for the target.
constexpr int kMaxNearby = 16;
constexpr int kMaxTargets = 8;
// Line-of-sight tests per tick, best candidates first.
constexpr int kMaxSightTests = 4;
// PrimedGun's gun ray reaches 1.75 times further for objects without the
// target distance test (bosses and other large targets).
constexpr float kUntestedReach = 1.75f;

// CPlayerOrbit.cpp's line-of-sight filters, so the head's pick obeys the same
// visibility rule as the game's own scan targeting.
const CMaterialFilter kLineOfSightFilter = CMaterialFilter::MakeIncludeExclude(
    CMaterialList(kMT_Solid), CMaterialList(kMT_ProjectilePassthrough, kMT_ScanPassthrough, kMT_Player));
const CMaterialFilter kOccluderFilter = CMaterialFilter::MakeIncludeExclude(
    CMaterialList(kMT_Solid, kMT_Occluder), CMaterialList(kMT_ProjectilePassthrough, kMT_ScanPassthrough, kMT_Player));

struct Ranked {
    TUniqueId id = kInvalidUniqueId;
    float score = 0.0f;
};

struct LookState {
    unsigned int stamp = 0;
    // Look to scan, in the scan visor.
    bool active = false;
    TUniqueId target = kInvalidUniqueId;
    int nearbyCount = 0;
    TUniqueId nearby[kMaxNearby];
    // Look to lock-on and look to grapple, in the other visors.
    bool lockActive = false;
    bool lockOn = false;
    bool grapple = false;
    TUniqueId lockTarget = kInvalidUniqueId;
};
LookState s_look;

// Results are kept for the tick they were made in and the next, so readers
// that run before the player's input in a tick still see them, and a tick
// that skips the orbit input (morphing, cinematics) lets them lapse.
bool Recent(const CStateManager& mgr) noexcept { return mgr.GetUpdateFrameIndex() - s_look.stamp <= 2u; }
bool Fresh(const CStateManager& mgr) noexcept { return s_look.active && Recent(mgr); }
bool FreshLock(const CStateManager& mgr) noexcept { return s_look.lockActive && Recent(mgr); }

bool IsGrapplePoint(const CEntity* entity) noexcept {
    return TCastToConstPtr< CScriptGrapplePoint >(entity) != nullptr;
}

// Whether the head picks this kind of orbit target (the lock settings as of
// the last update).
bool HeadPicks(const CStateManager& mgr, TUniqueId id) noexcept {
    return IsGrapplePoint(mgr.GetObjectById(id)) ? s_look.grapple : s_look.lockOn;
}

LookScan::Vec3 ToLook(const CVector3f& v) noexcept { return {v.GetX(), v.GetY(), v.GetZ()}; }

LookScan::Aabb ToLook(const CAABox& box) noexcept { return {ToLook(box.GetMinPoint()), ToLook(box.GetMaxPoint())}; }

// PrimedGun ResolveScanActorRayMetrics, plus the orbit point the lock aims at:
// the nearest to the ray of the object's position, its orbit point, its render
// box and its collision box, the boxes only when small enough to trust.
LookScan::Metrics MeasureActor(const CActor& actor, const CStateManager& mgr, const LookScan::Ray& ray,
                               float maxAlong, float targetingRadius) noexcept {
    LookScan::Metrics best;
    LookScan::Metrics metrics;
    if (LookScan::MetricsForPoint(ray, ToLook(actor.GetTranslation()), maxAlong, metrics) &&
        LookScan::Prefer(metrics, best)) {
        best = metrics;
    }
    if (LookScan::MetricsForPoint(ray, ToLook(actor.GetOrbitPosition(mgr)), maxAlong, metrics) &&
        LookScan::Prefer(metrics, best)) {
        best = metrics;
    }
    const LookScan::Aabb render = ToLook(actor.GetRenderBoundsCached());
    if (LookScan::RenderAabbUsableForAim(render, targetingRadius) &&
        LookScan::MetricsForAabb(ray, render, maxAlong, metrics) && LookScan::Prefer(metrics, best)) {
        best = metrics;
    }
    if (const CPhysicsActor* physics = TCastToConstPtr< CPhysicsActor >(&actor)) {
        const LookScan::Aabb collision = ToLook(physics->GetBoundingBox());
        if (LookScan::PhysicsAabbUsableForAim(collision, targetingRadius) &&
            LookScan::MetricsForAabb(ray, collision, maxAlong, metrics) && LookScan::Prefer(metrics, best)) {
            best = metrics;
        }
    }
    return best;
}

// Keeps the `capacity` lowest scores (PrimedGun's insert_candidate).
void Insert(Ranked* ranked, int& count, int capacity, TUniqueId id, float score) noexcept {
    int slot = count;
    if (count == capacity) {
        slot = static_cast< int >(std::max_element(ranked, ranked + count,
                                                   [](const Ranked& a, const Ranked& b) { return a.score < b.score; }) -
                                  ranked);
        if (score >= ranked[slot].score) {
            return;
        }
    } else {
        ++count;
    }
    ranked[slot] = Ranked{id, score};
}

// CPlayer::FindBestOrbitableObject's test: nothing solid between the eye and
// the object's orbit point, ignoring the object itself and the geometry of
// areas that are occluded.
bool InSight(CStateManager& mgr, const CVector3f& eye, const CActor& actor) noexcept {
    const CVector3f eyeToOrbit = actor.GetOrbitPosition(mgr) - eye;
    if (!eyeToOrbit.CanBeNormalized()) {
        return true;
    }
    const float distance = eyeToOrbit.Magnitude();
    const CVector3f direction = eyeToOrbit.AsNormalized();
    TEntityList nearList;
    mgr.BuildNearList(nearList, eye, direction, distance, kOccluderFilter, &actor);
    for (auto it = nearList.begin(); it != nearList.end();) {
        if (const CEntity* entity = mgr.GetObjectById(*it)) {
            const TAreaId areaId = entity->GetCurrentAreaId();
            if (areaId == kInvalidAreaId ||
                (areaId != mgr.GetNextAreaId() &&
                 mgr.GetWorld()->GetAreaAlways(areaId).GetOcclusionState() == CGameArea::kOS_Occluded)) {
                it = nearList.erase(it);
                continue;
            }
        }
        ++it;
    }
    TUniqueId hitId = kInvalidUniqueId;
    return mgr.RayWorldIntersection(hitId, eye, direction, distance, kLineOfSightFilter, nearList).IsInvalid();
}

// The best-scored target the eye can see, the previous pick kept while it
// scores close to the best.
TUniqueId PickInSight(CStateManager& mgr, const CVector3f& eye, Ranked* targets, int targetCount,
                      TUniqueId previous) noexcept {
    std::sort(targets, targets + targetCount, [](const Ranked& a, const Ranked& b) { return a.score < b.score; });
    for (int i = 1; i < targetCount; ++i) {
        if (targets[i].id == previous && LookScan::KeepPrevious(targets[i].score, targets[0].score)) {
            std::rotate(targets, targets + i, targets + i + 1);
            break;
        }
    }
    for (int i = 0; i < targetCount && i < kMaxSightTests; ++i) {
        const CActor* actor = TCastToConstPtr< CActor >(mgr.GetObjectById(targets[i].id));
        if (actor != nullptr && InSight(mgr, eye, *actor)) {
            return targets[i].id;
        }
    }
    return kInvalidUniqueId;
}

void UpdateScan(CStateManager& mgr, const CPlayer& player, const PortVrSettings& settings, const LookScan::Ray& ray,
                const CVector3f& origin, TUniqueId previous) noexcept {
    const float targetingRadius = LookScan::TargetingRadius(settings.gun_targeting_radius);
    const float maxAlong = LookScan::MaxAlong(settings.gun_targeting_distance);
    const CVector3f reach(maxAlong, maxAlong, maxAlong);
    TEntityList nearList;
    mgr.BuildNearList(nearList, CAABox(origin - reach, origin + reach),
                      CMaterialFilter::MakeInclude(CMaterialList(kMT_Scannable)), &player);

    const CVector3f eye = player.GetEyePosition();
    const float maxTargetDistance = player.GetOrbitMaxTargetDistance(mgr);
    Ranked nearby[kMaxNearby];
    int nearbyCount = 0;
    Ranked targets[kMaxTargets];
    int targetCount = 0;
    for (auto it = nearList.begin(); it != nearList.end(); ++it) {
        const CActor* actor = TCastToConstPtr< CActor >(mgr.GetObjectById(*it));
        if (actor == nullptr || actor->GetUniqueId() == player.GetUniqueId() ||
            !actor->GetMaterialList().HasMaterial(kMT_Scannable) || actor->GetScannableObjectInfo() == nullptr) {
            continue;
        }
        // The game's own target rules (CPlayer::FindOrbitableObjects).
        if (player.ValidateOrbitTargetId(actor->GetUniqueId(), mgr) != CPlayer::kOVR_OK) {
            continue;
        }
        if (actor->GetDoTargetDistanceTest() &&
            (actor->GetOrbitPosition(mgr) - eye).Magnitude() > maxTargetDistance) {
            continue;
        }
        const LookScan::Metrics metrics = MeasureActor(*actor, mgr, ray, maxAlong, targetingRadius);
        if (!metrics.valid) {
            continue;
        }
        const float visualCone = LookScan::VisualConePerp(targetingRadius, metrics);
        if (metrics.perp <= visualCone) {
            Insert(nearby, nearbyCount, kMaxNearby, actor->GetUniqueId(), LookScan::VisualScore(metrics, visualCone));
        }
        const float targetCone = LookScan::TargetConePerp(targetingRadius, metrics);
        if (metrics.perp <= targetCone) {
            Insert(targets, targetCount, kMaxTargets, actor->GetUniqueId(), LookScan::TargetScore(metrics, targetCone));
        }
    }

    s_look.target = PickInSight(mgr, eye, targets, targetCount, previous);

    for (int i = 0; i < nearbyCount; ++i) {
        s_look.nearby[i] = nearby[i].id;
    }
    s_look.nearbyCount = nearbyCount;
    // The target always shows its icon (PrimedGun EnsureUidInScanVisorTargets).
    if (s_look.target != kInvalidUniqueId &&
        std::find(s_look.nearby, s_look.nearby + s_look.nearbyCount, s_look.target) ==
            s_look.nearby + s_look.nearbyCount) {
        if (s_look.nearbyCount < kMaxNearby) {
            ++s_look.nearbyCount;
        }
        s_look.nearby[s_look.nearbyCount - 1] = s_look.target;
    }
}

// FindBestOrbitableObject's grapple point rules: the Grapple Beam, within the
// orbit distance, and a swing-locked point only from in front or behind it.
bool GrapplePointUsable(const CStateManager& mgr, const CPlayer& player, const CScriptGrapplePoint& point,
                        float distance) noexcept {
    if (!mgr.GetPlayerState()->HasPowerUp(CPlayerState::kIT_GrappleBeam) ||
        !(distance < gpTweakPlayer->GetOrbitDistanceMax())) {
        return false;
    }
    if (point.GetGrappleParameters().GetLockSwingTurn()) {
        CVector3f pointToPlayer = player.GetTranslation() - point.GetTranslation();
        pointToPlayer.SetZ(0.f);
        if (pointToPlayer.CanBeNormalized()) {
            const CVector3f pointForward = point.GetTransform().GetForward().AsNormalized();
            if (CMath::AbsF(CVector3f::Dot(pointForward, pointToPlayer.AsNormalized())) <= 0.70710677f) {
                return false;
            }
        }
    }
    return true;
}

// `cannonRay`: the tracked cannon's ray picks, every kind of target alike;
// otherwise the head's gaze picks the kinds the look settings give it.
void UpdateLock(CStateManager& mgr, const CPlayer& player, const PortVrSettings& settings, const LookScan::Ray& ray,
                const CVector3f& origin, TUniqueId previous, bool cannonRay) noexcept {
    const float targetingRadius = LookScan::TargetingRadius(settings.gun_targeting_radius);
    const float maxAlong = LookScan::MaxAlong(settings.gun_targeting_distance);
    const float untestedAlong = maxAlong * kUntestedReach;
    const CVector3f reach(untestedAlong, untestedAlong, untestedAlong);
    TEntityList nearList;
    mgr.BuildNearList(nearList, CAABox(origin - reach, origin + reach),
                      CMaterialFilter::MakeInclude(CMaterialList(kMT_Orbit)), &player);

    const CVector3f eye = player.GetEyePosition();
    const float maxTargetDistance = player.GetOrbitMaxTargetDistance(mgr);
    Ranked targets[kMaxTargets];
    int targetCount = 0;
    for (auto it = nearList.begin(); it != nearList.end(); ++it) {
        const CActor* actor = TCastToConstPtr< CActor >(mgr.GetObjectById(*it));
        // The current target is not a next one (FindBestOrbitableObject).
        if (actor == nullptr || actor->GetUniqueId() == player.GetUniqueId() ||
            actor->GetUniqueId() == player.GetOrbitTargetId() || !actor->GetMaterialList().HasMaterial(kMT_Orbit)) {
            continue;
        }
        const CScriptGrapplePoint* point = TCastToConstPtr< CScriptGrapplePoint >(actor);
        if (!cannonRay && (point != nullptr ? !settings.look_to_grapple : !settings.look_to_lock_on)) {
            continue;
        }
        // The game's own target rules (CPlayer::FindOrbitableObjects).
        if (player.ValidateOrbitTargetId(actor->GetUniqueId(), mgr) != CPlayer::kOVR_OK) {
            continue;
        }
        const float distance = (actor->GetOrbitPosition(mgr) - eye).Magnitude();
        if (actor->GetDoTargetDistanceTest() && distance > maxTargetDistance) {
            continue;
        }
        if (point != nullptr && !GrapplePointUsable(mgr, player, *point, distance)) {
            continue;
        }
        const float along = actor->GetDoTargetDistanceTest() ? maxAlong : untestedAlong;
        const LookScan::Metrics metrics = MeasureActor(*actor, mgr, ray, along, targetingRadius);
        if (!metrics.valid) {
            continue;
        }
        const float cone = LookScan::LockConePerp(targetingRadius, metrics, point != nullptr);
        if (metrics.perp <= cone) {
            Insert(targets, targetCount, kMaxTargets, actor->GetUniqueId(), LookScan::TargetScore(metrics, cone));
        }
    }

    s_look.lockTarget = PickInSight(mgr, eye, targets, targetCount, previous);
}

} // namespace

void VrLookTargetUpdate(CStateManager& mgr, const CPlayer& player) noexcept {
    const TUniqueId previousScan = s_look.target;
    const TUniqueId previousLock = s_look.lockTarget;
    s_look.stamp = mgr.GetUpdateFrameIndex();
    s_look.active = false;
    s_look.target = kInvalidUniqueId;
    s_look.nearbyCount = 0;
    s_look.lockActive = false;
    s_look.lockTarget = kInvalidUniqueId;
    if (!VrImmersive() || player.GetMorphballTransitionState() != CPlayer::kMS_Unmorphed) {
        return;
    }
    const PortVrSettings settings = GetVrSettings();
    const bool scanVisor = mgr.GetPlayerState()->GetCurrentVisor() == CPlayerState::kPV_Scan;
    CVector3f origin;
    CVector3f direction;
    const CTransform4f camera = VrRoomScaleCameraTransform(player.GetFirstPersonCameraTransform(mgr));
    // Cannon targeting needs a tracked cannon. Without one (a gamepad, or
    // controllers that lost tracking) the head's gaze picks, as the look
    // settings say, rather than nothing doing so.
    const bool cannonTargeting =
        settings.CannonTargetingForVisor(scanVisor) && VrCannonAim(player, camera, origin, direction);
    if (!cannonTargeting &&
        (scanVisor ? !settings.patch_gun_ray_target : (!settings.look_to_lock_on && !settings.look_to_grapple))) {
        return;
    }
    if (!cannonTargeting && !VrHeadGaze(camera, origin, direction)) {
        return;
    }
    s_look.active = scanVisor;
    s_look.lockActive = !scanVisor;
    s_look.lockOn = cannonTargeting || settings.look_to_lock_on;
    s_look.grapple = cannonTargeting || settings.look_to_grapple;
    // Orbit disable sources (CPlayer::UpdateOrbitableObjects): nothing to target.
    if (player.CheckOrbitDisableSourceList()) {
        return;
    }
    const LookScan::Ray ray{ToLook(origin), ToLook(direction)};
    if (scanVisor) {
        UpdateScan(mgr, player, settings, ray, origin, previousScan);
    } else {
        UpdateLock(mgr, player, settings, ray, origin, previousLock, cannonTargeting);
    }
}

bool VrCannonTargetPoint(const CStateManager& mgr, CVector3f& point) noexcept {
    const PortVrSettings settings = GetVrSettings();
    const CPlayer& player = *mgr.GetPlayer();
    const bool scanVisor = mgr.GetPlayerState()->GetCurrentVisor() == CPlayerState::kPV_Scan;
    if (!VrImmersive() || player.GetMorphballTransitionState() != CPlayer::kMS_Unmorphed) {
        return false;
    }
    // The tracked cannon's ray with cannon targeting on. Without a tracked
    // cannon (a gamepad), the ray the cannon was turned along by a lock or R's
    // free aim: the level view would put the crosshair straight ahead of the
    // body rather than where the shots go.
    CVector3f origin;
    CVector3f direction;
    const bool cannon = settings.CannonTargetingForVisor(scanVisor) &&
                        VrCannonAim(player, mgr.GetCameraManager()->GetCurrentCameraTransform(mgr), origin, direction);
    if (!cannon && (scanVisor || !VrPadAimRay(mgr, origin, direction))) {
        return false;
    }
    const float distance = LookScan::MaxAlong(settings.gun_targeting_distance);
    TEntityList nearList;
    mgr.BuildNearList(nearList, origin, direction, distance, kLineOfSightFilter, &player);
    TUniqueId hitId = kInvalidUniqueId;
    const CRayCastResult hit = mgr.RayWorldIntersection(hitId, origin, direction, distance,
                                                       kLineOfSightFilter, nearList);
    point = hit.IsValid() ? hit.GetPoint() : origin + direction * distance;
    return true;
}

bool VrLookToScanTarget(const CStateManager& mgr, TUniqueId& id) noexcept {
    if (!Fresh(mgr)) {
        return false;
    }
    id = s_look.target;
    return true;
}

const TUniqueId* VrLookToScanNearby(const CStateManager& mgr, int& count) noexcept {
    count = Fresh(mgr) ? s_look.nearbyCount : 0;
    return count > 0 ? s_look.nearby : nullptr;
}

bool VrLookToLockTarget(const CStateManager& mgr, TUniqueId gamePick, TUniqueId& id) noexcept {
    if (!FreshLock(mgr)) {
        return false;
    }
    id = s_look.lockTarget;
    if (id == kInvalidUniqueId && gamePick != kInvalidUniqueId && !HeadPicks(mgr, gamePick)) {
        id = gamePick;
    }
    return true;
}

bool VrLookHoldsFacing(const CStateManager& mgr, const CPlayer&) noexcept {
    // Only scan locks freeze the body. Combat orbit and grapple physics must
    // keep their target-facing basis, independently of the headset view.
    return Fresh(mgr);
}

} // namespace PortVr
