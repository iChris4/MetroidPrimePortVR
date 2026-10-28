// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/vr_view.h"

#include "vr/openxr_controller_snapshot.h"
#include "vr/openxr_integration.h"
#include "vr/prime_vr_policy.h"
#include "vr/vr_beam_wheel.h"
#include "vr/vr_pad.h"
#include "vr/vr_settings.h"
#include "vr/vr_room_scale.h"
#include "port_surface_culling_math.h"

#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "MetaRender/CCubeRenderer.hpp"
#include "aurora/gfx.h"
#include "dolphin/gx/GXAurora.h"
#include "Kyoto/Math/CQuaternion.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CGameCollision.hpp"
#include "MetroidPrime/CGroundMovement.hpp"
#include "Collision/CCollisionInfo.hpp"
#include "Collision/CCollisionPrimitive.hpp"
#include "WorldFormat/CMetroidAreaCollider.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace PortVr {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDegreesToRadians = kPi / 180.0f;
// PrimedGun: the cannon pose keeps 24 % of the previous tick, and snaps when
// the controller jumped more than two units (a tracking glitch or a recenter).
constexpr float kCannonSmoothKeep = 0.24f;
constexpr float kCannonSnapDistance = 2.0f;
// PrimedGun BASE_MODEL_FORWARD_BACK_OFFSET: the model sits this far back along
// the aim, in metres, before the user's model offset.
constexpr float kModelForwardBackOffset = -0.080f;

struct CannonState {
    bool tracked = false;
    bool smoothValid = false;
    CQuaternion smoothRotation;
    CVector3f smoothPosition;
    CVector3f modelOffsetWorld;
};
CannonState s_cannon;
float s_ballEntryYaw = 0.f;
CVector3f s_ballEntryDirection(0.f, 1.f, 0.f);
CTransform4f s_unmorphBase = CTransform4f::Identity();
CTransform4f s_unmorphFacing = CTransform4f::Identity();
CVector3f s_unmorphOffset = CVector3f::Zero();
RoomScale::State s_roomScale;

bool ImmersiveNow() noexcept {
    if (!OpenXRIsRunning()) {
        return false;
    }
    return PrimeVRPolicyGetSnapshot().presentation == VRPresentationMode::Immersive;
}

// OpenXR (x right, y up, -z forward) to Prime (x right, y forward, z up): a
// vector (x, y, z) becomes (x, -z, y). The map is a proper rotation, so a
// rotation quaternion's vector part converts the same way.
CVector3f PrimeFromXr(const std::array<float, 3>& v) noexcept { return CVector3f(v[0], -v[2], v[1]); }

CQuaternion PrimeFromXr(const std::array<float, 4>& q) noexcept { return CQuaternion(q[3], q[0], -q[2], q[1]); }

CQuaternion AxisQuaternion(float x, float y, float z, float radians) noexcept {
    const float half = 0.5f * radians;
    const float s = std::sin(half);
    return CQuaternion(std::cos(half), x * s, y * s, z * s);
}

// The camera's yaw about world up as a rotation: the body facing.
CQuaternion BodyYaw(const CTransform4f& cameraXf) noexcept {
    CVector3f forward = cameraXf.GetForward();
    forward[kDZ] = 0.f;
    if (!forward.CanBeNormalized()) {
        return CQuaternion::NoRotation();
    }
    forward.Normalize();
    // Prime's forward is (-sin yaw, cos yaw, 0) for a yaw about +z.
    return AxisQuaternion(0.f, 0.f, 1.f, std::atan2(-forward.GetX(), forward.GetY()));
}

bool HeadRotation(CTransform4f& rotation) noexcept {
    OpenXRFrameRequest request{};
    if (!OpenXRLatestFrameRequest(request) || !request.head_valid) {
        return false;
    }
    rotation = PrimeFromXr(request.head_orientation).BuildNormalized().BuildTransform4f();
    return true;
}

CVector3f RoomScaleOffset(const OpenXRFrameRequest& request) noexcept {
    if (!s_roomScale.valid || !request.immersive || !request.base_valid ||
        request.tracking_origin_generation != s_roomScale.generation) {
        return CVector3f::Zero();
    }
    const float lean = GetVrSettings().lean_back_degrees * kDegreesToRadians;
    return AxisQuaternion(1.f, 0.f, 0.f, -lean).Transform(PrimeFromXr(s_roomScale.consumed)) *
           request.units_per_meter;
}

CQuaternion Nlerp(const CQuaternion& from, CQuaternion to, float t) noexcept {
    if (CQuaternion::Dot(from, to) < 0.f) {
        to = CQuaternion(-to.GetScalar(), -to.GetVector());
    }
    const float keep = 1.f - t;
    return CQuaternion(from.GetScalar() * keep + to.GetScalar() * t,
                       from.GetVector() * keep + to.GetVector() * t)
        .BuildNormalized();
}

} // namespace

bool VrImmersive() noexcept { return ImmersiveNow(); }

CTransform4f VrRoomScaleCameraTransform(const CTransform4f& cameraXf) noexcept {
    OpenXRFrameRequest request{};
    if (!ImmersiveNow() || !OpenXRLatestFrameRequest(request)) {
        return cameraXf;
    }
    CTransform4f result = cameraXf;
    result.SetTranslation(cameraXf.GetTranslation() - cameraXf.Rotate(RoomScaleOffset(request)));
    return result;
}

void VrRoomScaleUpdate(CStateManager& mgr, CPlayer& player, float dt) noexcept {
    OpenXRFrameRequest request{};
    const bool tracked = ImmersiveNow() && OpenXRLatestFrameRequest(request) &&
                         request.immersive && request.head_valid && request.base_valid;
    const bool enabled = tracked && GetVrSettings().room_scale && dt > 0.f &&
                         mgr.GetGameState() == CStateManager::kGS_Running &&
                         PrimeVRPolicyGetSnapshot().game_mode == VRGameMode::InGame &&
                         !mgr.GetCameraManager()->IsInCinematicCamera() &&
                         player.GetMorphballTransitionState() == CPlayer::kMS_Unmorphed &&
                         player.GetCameraState() == CPlayer::kCS_FirstPerson &&
                         !player.GetControlsFrozen() && !player.IsAttached() &&
                         player.GetGrappleState() == CPlayer::kGS_None && player.GetDeathTime() == 0.f;
    const auto step = s_roomScale.Update(tracked, enabled, request.tracking_origin_generation,
                                        request.head_position, request.base_position);
    if (!enabled) {
        return;
    }
    // Physical walking is horizontal; height/crouching stays in head tracking.
    CVector3f remaining = BodyYaw(mgr.GetCameraManager()->GetSimulationCameraTransform(mgr))
                              .Transform(PrimeFromXr(step) * request.units_per_meter);
    if (!remaining.CanBeNormalized()) {
        return;
    }
    if (player.GetPlayerMovementState() == NPlayer::kMS_OnGround &&
        player.GetMaterialList().HasMaterial(kMT_GroundCollider) && !player.IsStandardCollider()) {
        // Reuse the same quarter/full-height step attempts, ceiling checks,
        // step-down, floor messages and platform contacts as stick walking.
        RoomScale::MoveGroundedDisplacement(player, remaining, dt, [&] {
            if (gkUseNewPlayerMovement) {
                CGroundMovement::MoveGroundCollider_New(mgr, player, dt, nullptr);
            } else {
                CGroundMovement::MoveGroundCollider(mgr, player, dt, nullptr);
            }
        });
        mgr.UpdateActorInSortedLists(player);
        return;
    }
    const CCollisionPrimitive& prim = *player.GetCollisionPrimitive();
    CAABox bounds = prim.CalculateAABox(player.GetPrimitiveTransform());
    bounds.AccumulateBounds(bounds.GetMinPoint() + remaining);
    bounds.AccumulateBounds(bounds.GetMaxPoint() + remaining);
    const CVector3f margin(0.2f, 0.2f, 0.2f);
    CAreaCollisionCache cache(CAABox(bounds.GetMinPoint() - margin, bounds.GetMaxPoint() + margin));
    CGameCollision::BuildAreaCollisionCache(mgr, cache);
    TEntityList nearList;
    mgr.BuildColliderList(nearList, player, cache.GetCacheBounds());
    // Sweep the full player bounds, then slide along the blocking surface.
    // Do not alter stick velocity, gravity, jump state or player orientation.
    for (int slide = 0; slide < 3 && remaining.CanBeNormalized(); ++slide) {
        const float length = remaining.Magnitude();
        const CVector3f direction = remaining / length;
        double distance = length;
        CCollisionInfo info;
        TUniqueId hit = kInvalidUniqueId;
        const bool blocked = CGameCollision::DetectCollision_Cached_Moving(
            mgr, cache, prim, player.GetPrimitiveTransform(), player.GetMaterialFilter(),
            nearList, direction, hit, info, distance);
        const float travel = blocked ? std::clamp(float(distance) - 0.002f, 0.f, length) : length;
        player.SetTranslation(player.GetTranslation() + direction * travel);
        if (!blocked) {
            break;
        }
        remaining -= direction * travel;
        CVector3f normal = info.GetNormalLeft();
        normal.SetZ(0.f);
        if (!normal.CanBeNormalized()) {
            break;
        }
        normal.Normalize();
        const float intoWall = CVector3f::Dot(remaining, normal);
        if (intoWall >= 0.f) {
            break;
        }
        remaining -= normal * intoWall;
    }
    mgr.UpdateActorInSortedLists(player);
}

CTransform4f VrHeadFacingTransform(const CTransform4f& cameraXf) noexcept {
    CTransform4f head = CTransform4f::Identity();
    if (!ImmersiveNow() || !HeadRotation(head)) {
        return cameraXf;
    }
    CTransform4f facing = cameraXf.GetRotation() * head;
    facing.SetTranslation(cameraXf.GetTranslation());
    return facing;
}

bool VrHeadGaze(const CTransform4f& cameraXf, CVector3f& origin, CVector3f& direction) noexcept {
    if (!ImmersiveNow()) {
        return false;
    }
    OpenXRFrameRequest request{};
    if (!OpenXRLatestFrameRequest(request) || !request.head_valid) {
        return false;
    }
    // The eyes see the game camera's space through lean^-1 * head
    // (openxr_integration.cpp ViewFromBase), lean being a pitch about the
    // right axis, which Prime and OpenXR share.
    const float leanBack = GetVrSettings().lean_back_degrees * kDegreesToRadians;
    const CQuaternion leanInverse = AxisQuaternion(1.f, 0.f, 0.f, -leanBack);
    const CQuaternion head = (leanInverse * PrimeFromXr(request.head_orientation)).BuildNormalized();
    direction = cameraXf.Rotate(head.Transform(CVector3f(0.f, 1.f, 0.f)));
    if (!direction.CanBeNormalized()) {
        return false;
    }
    direction.Normalize();
    CVector3f offset = CVector3f::Zero();
    if (request.base_valid) {
        const std::array<float, 3> fromBase{
            request.head_position[0] - request.base_position[0],
            request.head_position[1] - request.base_position[1],
            request.head_position[2] - request.base_position[2],
        };
        offset = leanInverse.Transform(PrimeFromXr(fromBase)) * request.units_per_meter;
    }
    origin = cameraXf.GetTranslation() + cameraXf.Rotate(offset);
    return true;
}

CTransform4f VrHeadViewTransform(const CTransform4f& cameraXf) noexcept {
    CVector3f origin;
    CVector3f direction;
    if (!VrHeadGaze(cameraXf, origin, direction)) {
        return cameraXf;
    }
    return CTransform4f(VrHeadViewRotation(cameraXf), origin);
}

CMatrix3f VrHeadViewRotation(const CTransform4f& cameraXf) noexcept {
    const CMatrix3f body = cameraXf.BuildMatrix3f();
    if (!ImmersiveNow()) {
        return body;
    }
    OpenXRFrameRequest request{};
    if (!OpenXRLatestFrameRequest(request) || !request.head_valid) {
        return body;
    }
    // As VrHeadGaze: the eyes see the game camera's space through lean^-1 * head.
    const float leanBack = GetVrSettings().lean_back_degrees * kDegreesToRadians;
    const CQuaternion leanInverse = AxisQuaternion(1.f, 0.f, 0.f, -leanBack);
    const CQuaternion head = (leanInverse * PrimeFromXr(request.head_orientation)).BuildNormalized();
    const CTransform4f view = cameraXf.GetRotation() * head.BuildTransform4f();
    // Upright: the head's facing without its roll. The right axis stays level,
    // so up is as close to the world's up as the facing allows; looking
    // straight up or down, where level is undefined, the head's own right.
    const CVector3f forward = view.GetForward();
    CVector3f right = CVector3f::Cross(forward, CVector3f(0.f, 0.f, 1.f));
    if (right.Magnitude() < 1.e-3f) {
        right = view.GetRight();
    }
    right.Normalize();
    return CTransform4f::FromColumns(right, forward, CVector3f::Cross(right, forward), CVector3f::Zero())
        .BuildMatrix3f();
}

bool VrCullingFrustum(const CTransform4f& cameraXf, float nearZ, CFrustumPlanes& frustum) noexcept {
    if (!ImmersiveNow()) {
        return false;
    }
    const PortVrSettings settings = GetVrSettings();
    // Disabling headset culling must not restore the vanilla forward-only
    // frustum: water and its fog also consume this CPU visibility volume.
    if (!settings.frustum_culling_enabled) {
        frustum = CFrustumPlanes();
        return true;
    }
    const float degrees = std::clamp(settings.frustum_culling_degrees, 60.0f, 179.0f);
    CTransform4f facing = VrHeadFacingTransform(cameraXf);
    CVector3f headOrigin, headDirection;
    if (VrHeadGaze(cameraXf, headOrigin, headDirection)) {
        facing.SetTranslation(headOrigin);
    } else {
        // Unknown tracking must retain geometry, not cull against the body.
        frustum = CFrustumPlanes();
        return true;
    }
    frustum = CFrustumPlanes(facing, degrees * kDegreesToRadians, 1.0f, nearZ, false, 100.f);
    return true;
}

bool VrFlattenLookPitch() noexcept { return ImmersiveNow(); }

void VrBeginBallCamera(const CTransform4f& firstPersonXf) noexcept {
    s_ballEntryYaw = 0.f;
    s_ballEntryDirection = firstPersonXf.GetForward();
    if (!ImmersiveNow()) {
        return;
    }
    const CVector3f headRight = VrHeadViewRotation(firstPersonXf).GetColumn(kDX);
    const CVector3f bodyRight = firstPersonXf.GetRight();
    const float yaw = std::atan2(headRight.GetY(), headRight.GetX()) -
                      std::atan2(bodyRight.GetY(), bodyRight.GetX());
    s_ballEntryYaw = yaw;
    s_ballEntryDirection = CVector3f(-headRight.GetY(), headRight.GetX(), 0.f);
}

CVector3f VrBallCameraEntryDirection() noexcept { return s_ballEntryDirection; }

CTransform4f VrBallCameraBaseTransform(const CTransform4f& camera, float weight) noexcept {
    if (!ImmersiveNow()) {
        return camera;
    }
    CTransform4f base = camera.GetRotation() *
        AxisQuaternion(0.f, 0.f, 1.f, -s_ballEntryYaw * std::clamp(weight, 0.f, 1.f)).BuildTransform4f();
    base.SetTranslation(camera.GetTranslation());
    return base;
}

void VrBeginUnmorphCamera(const CTransform4f& camera, const CVector3f& eyePosition) noexcept {
    s_unmorphBase = camera.GetRotation();
    // The visible stand-up animation faces the HMD, while the render base
    // stays unchanged: stereo replay still adds the tracked head rotation.
    CVector3f facing = VrHeadViewRotation(camera).GetColumn(kDY);
    facing.SetZ(0.f);
    if (!facing.CanBeNormalized()) {
        facing = camera.GetForward();
        facing.SetZ(0.f);
    }
    s_unmorphFacing = facing.CanBeNormalized()
        ? CTransform4f::LookAt(CVector3f::Zero(), facing, CVector3f::Up())
        : s_unmorphBase;
    OpenXRFrameRequest request{};
    const CVector3f consumed = OpenXRLatestFrameRequest(request) ? camera.Rotate(RoomScaleOffset(request))
                                                               : CVector3f::Zero();
    s_unmorphOffset = camera.GetTranslation() + consumed - eyePosition;
}

CTransform4f VrUnmorphCameraBase(const CTransform4f& camera) noexcept {
    CTransform4f view = s_unmorphBase;
    view.SetTranslation(camera.GetTranslation());
    return view;
}

CTransform4f VrUnmorphFacingTransform(const CTransform4f& player) noexcept {
    CTransform4f facing = s_unmorphFacing;
    facing.SetTranslation(player.GetTranslation());
    return facing;
}

CTransform4f VrUnmorphCameraTransform(const CVector3f& eyePosition, float progress) noexcept {
    CTransform4f view = s_unmorphBase;
    view.SetTranslation(eyePosition + s_unmorphOffset * (1.f - std::clamp(progress, 0.f, 1.f)));
    return view;
}

bool VrSurfaceCullingVolume(const CTransform4f& cameraXf,
                            PortSurfaceCulling::StereoVolume& volume) noexcept {
    volume = {};
    const PortVrSettings settings = GetVrSettings();
    OpenXRFrameRequest request{};
    if (!settings.frustum_culling_enabled || !ImmersiveNow() ||
        !OpenXRLatestFrameRequest(request) || !request.immersive || !request.head_valid ||
        !request.base_valid || !std::isfinite(request.units_per_meter) || request.units_per_meter <= 0.f) {
        return false;
    }
    const CQuaternion leanInverse = AxisQuaternion(1.f, 0.f, 0.f, -settings.lean_back_degrees * kDegreesToRadians);
    const auto vector = [](const CVector3f& v) {
        return PortSurfaceCulling::Vector{v.GetX(), v.GetY(), v.GetZ()};
    };
    for (size_t eye = 0; eye < 2; ++eye) {
        float norm = 0.f;
        for (float component : request.eye_orientation[eye]) { norm += component * component; }
        if (!std::isfinite(norm) || norm < 1.e-8f) { return false; }
        const CQuaternion rotation = (leanInverse * PrimeFromXr(request.eye_orientation[eye])).BuildNormalized();
        const CTransform4f facing = cameraXf.GetRotation() * rotation.BuildTransform4f();
        const std::array<float, 3> fromBase{
            request.eye_position[eye][0] - request.base_position[0],
            request.eye_position[eye][1] - request.base_position[1],
            request.eye_position[eye][2] - request.base_position[2],
        };
        const CVector3f origin = cameraXf.GetTranslation() +
            cameraXf.Rotate(leanInverse.Transform(PrimeFromXr(fromBase)) * request.units_per_meter);
        // Keep 7.5 degrees and 10 cm beyond each located eye. The renderer also
        // retains anything inside its existing draw frustum, including probes.
        if (!volume.eyes[eye].Build(vector(origin), vector(facing.GetRight()), vector(facing.GetForward()),
                                   vector(facing.GetUp()), request.eye_fov[eye],
                                   7.5f * kDegreesToRadians, 0.10f * request.units_per_meter)) {
            return false;
        }
    }
    return true;
}

bool VrNoCameraBob() noexcept { return ImmersiveNow() && GetVrSettings().patch_no_idle_sway; }

bool VrNoArmCannonFidget() noexcept {
    return ImmersiveNow() && GetVrSettings().patch_disable_arm_cannon_idle_fidget;
}

bool VrRemoveCinematicBars() noexcept {
    return OpenXRIsRunning() && GetVrSettings().remove_cinematic_bars;
}

bool VrSkyAtInfinity() noexcept { return OpenXRIsRunning() && GetVrSettings().sky_at_infinity; }

bool VrHideSpaceWarp() noexcept { return OpenXRIsRunning() && !GetVrSettings().space_warp; }

bool VrScanWindowNoZoom() noexcept { return ImmersiveNow() && !GetVrSettings().scan_zoom; }

namespace {
// Game thread only, like vr_pad.cpp's in-game menu note.
bool s_messageBoxOpen = false;
bool s_messageBoxCinematic = false;
} // namespace

void VrNoteMessageBox(bool open, bool cinematic) noexcept {
    s_messageBoxOpen = open;
    s_messageBoxCinematic = open && cinematic;
}

bool VrMessageBoxOpen() noexcept { return s_messageBoxOpen; }

bool VrMessageBoxCinematic() noexcept { return s_messageBoxCinematic; }

bool VrFloatingHud() noexcept { return GetVrSettings().floating_hud; }

namespace {
bool s_gameMenuOpen = false; // game thread only, like the message box note
// The 3D map: its centre 1.2 m ahead, the map camera's view 0.45 m above and
// below it (about the angle the TV's map fills), nothing nearer than 0.3 m.
constexpr float kMapHologramDistanceMeters = 1.2f;
constexpr float kMapHologramHalfHeightMeters = 0.45f;
constexpr float kMapHologramClearanceMeters = 0.5f;
constexpr float kMapHologramNearMeters = 0.3f;
} // namespace

void VrNoteGameMenu(bool open) noexcept { s_gameMenuOpen = open; }

bool VrGameMenuOpen() noexcept { return s_gameMenuOpen; }

bool VrGameMenuInView() noexcept {
    return s_gameMenuOpen && OpenXRIsRunning() &&
           PrimeVRPolicyPresentationFor(VRGameMode::Paused) == VRPresentationMode::Immersive;
}

namespace {
unsigned s_pauseScreenRoute = 0; // game thread (CInGameGuiManager::Draw)
// A pause model stands this far in front of the pause menu's screen.
constexpr float kPauseModelAheadMeters = 0.15f;
} // namespace

void VrHeadLockedScales(float& xy, float& depth) noexcept {
    const PortVrSettings& settings = GetVrSettings();
    depth = settings.metroid_hud_distance;
    xy = settings.metroid_hud_size * settings.metroid_hud_distance;
}

void VrResetEyeDepth() noexcept {
    const CGraphics::CProjectionState projection = CGraphics::GetProjectionState();
    const CTransform4f viewPoint = CGraphics::GetViewMatrix();
    const CTransform4f model = CGraphics::GetModelMatrix();
    const float depthNear = CGraphics::GetDepthNear();
    const float depthFar = CGraphics::GetDepthFar();
    const rstl::pair< CVector2f, CVector2f > vp = gpRender->SetViewportOrtho(true, -4096.f, 4096.f);
    const CVector2f& lt = vp.first;
    const CVector2f& rb = vp.second;
    CGraphics::SetDepthRange(1.f, 1.f);
    gpRender->SetModelMatrix(CTransform4f::Identity());
    gpRender->SetBlendMode_NoColorWrite();
    CGraphics::SetDepthWriteMode(true, kE_Always, true);
    gpRender->BeginTriangleStrip(4);
    gpRender->PrimColor(CColor::White());
    gpRender->PrimVertex(CVector3f(lt.GetX() - 1.f, 0.f, 1.f + rb.GetY()));
    gpRender->PrimVertex(CVector3f(lt.GetX() - 1.f, 0.f, lt.GetY() - 1.f));
    gpRender->PrimVertex(CVector3f(1.f + rb.GetX(), 0.f, 1.f + rb.GetY()));
    gpRender->PrimVertex(CVector3f(1.f + rb.GetX(), 0.f, lt.GetY() - 1.f));
    gpRender->EndPrimitive();
    CGraphics::SetDepthWriteMode(true, kE_LEqual, true);
    gpRender->SetBlendMode_AlphaBlended();
    CGraphics::SetDepthRange(depthNear, depthFar);
    gpRender->SetModelMatrix(model);
    CGraphics::SetViewPointMatrix(viewPoint);
    CGraphics::SetProjectionState(projection);
}

void VrScreenPlane(float& halfWidth, float& halfHeight, float& distance) noexcept {
    const PortVrSettings& settings = GetVrSettings();
    const float unitsPerMeter = settings.world_scale;
    const CViewport& viewport = CGraphics::GetViewport();
    halfWidth = 0.5f * settings.screen_width_meters * unitsPerMeter;
    halfHeight = viewport.mWidth > 0 ? halfWidth * static_cast< float >(viewport.mHeight) /
                                           static_cast< float >(viewport.mWidth)
                                     : 0.f;
    distance = settings.screen_distance_meters * unitsPerMeter;
}

void VrSetPauseScreenRoute(unsigned route) noexcept { s_pauseScreenRoute = route; }

VrPauseModel3D::VrPauseModel3D() noexcept {
    if (s_pauseScreenRoute == 0) {
        return;
    }
    // The model's own camera: its vertical focal term and how far ahead the
    // model's origin (where the dolls stand) lies.
    const CGraphics::CProjectionState& projection = CGraphics::GetProjectionState();
    const float height = std::fabs(projection.GetTop() - projection.GetBottom());
    const float depth = (CGraphics::GetViewMatrix().GetQuickInverse() * CVector3f::Zero()).GetY();
    // The pause menu's screen: the mono picture fills it, so a point of the
    // model's camera space at the origin's depth lands at
    // (x, y) * halfHeight * focalY / depth on it.
    float halfWidth = 0.f;
    float halfHeight = 0.f;
    float distance = 0.f;
    VrScreenPlane(halfWidth, halfHeight, distance);
    if (!projection.IsPerspective() || height <= 0.f || depth <= projection.GetNear() ||
        halfHeight <= 0.f || distance <= 0.f) {
        return;
    }
    const float focalY = 2.f * projection.GetNear() / height;
    const float unitsPerMeter = GetVrSettings().world_scale;
    // A little in front of the screen, at the same angular size: scale and
    // distance shrink together.
    const float ahead = std::max(0.5f * distance, distance - kPauseModelAheadMeters * unitsPerMeter);
    const float scale = halfHeight * focalY / depth * (ahead / distance);
    // GX camera space looks down -z: the origin, at -depth, goes to -ahead.
    const float offsetZ = scale * depth - ahead;
    // Its own depth over whatever the screen's panels wrote: the reset quad
    // must cover each whole eye, so on WORLD.
    AuroraSetStereoDrawRoute(AURORA_STEREO_ROUTE_WORLD);
    VrResetEyeDepth();
    AuroraSetStereoDrawRoute(s_pauseScreenRoute == AURORA_STEREO_ROUTE_FLOATING_HUD ? AURORA_STEREO_ROUTE_FLOATING_3D
                                                                                 : AURORA_STEREO_ROUTE_ANCHORED_3D);
    AuroraSetStereoPlacement3D(scale, 0.f, 0.f, offsetZ);
    active_ = true;
}

VrPauseModel3D::~VrPauseModel3D() noexcept {
    if (!active_) {
        return;
    }
    AuroraSetStereoPlacement3D(1.f, 0.f, 0.f, 0.f);
    AuroraSetStereoDrawRoute(s_pauseScreenRoute);
}

void VrMapHologram(float& distance, float& halfHeight, float& clearance, float& nearPlane) noexcept {
    const float unitsPerMeter = GetVrSettings().world_scale;
    distance = kMapHologramDistanceMeters * unitsPerMeter;
    halfHeight = kMapHologramHalfHeightMeters * unitsPerMeter;
    clearance = kMapHologramClearanceMeters * unitsPerMeter;
    nearPlane = kMapHologramNearMeters * unitsPerMeter;
}

bool VrMessageBoxInView() noexcept {
    if (!s_messageBoxOpen || !OpenXRIsRunning()) {
        return false;
    }
    // The mode main.cpp publishes for this box, asked before the frame is drawn
    // (the current presentation is still the previous frame's).
    const VRGameMode mode = s_messageBoxCinematic ? VRGameMode::Cinematic : VRGameMode::Message;
    return PrimeVRPolicyPresentationFor(mode) == VRPresentationMode::Immersive;
}

int VrBeamWheelHoverBeam() noexcept {
    if (!GetVrSettings().beam_wheel_hud_highlight) {
        return -1;
    }
    const VrPadState pad = GetVrPadState();
    if (!pad.active || !pad.gameplay || !pad.weapon_panel) {
        return -1;
    }
    // The wheel counts PrimedGun's way (Power, Wave, Ice, Plasma); the game's
    // EBeamId puts Ice before Wave.
    if (pad.weapon_selected < 0 || pad.weapon_selected > 3) {
        return -1;
    }
    return BeamWheel::GameBeamId(static_cast<BeamWheel::Beam>(pad.weapon_selected));
}

CTransform4f VrTrackingToWorldTransform(const CPlayer& player, const CTransform4f& cameraXf) noexcept {
    const auto settings = GetVrSettings();
    const auto snapshot = OpenXRGetInputSnapshot();
    OpenXRFrameRequest request{};
    const bool haveRequest = OpenXRLatestFrameRequest(request);
    const float scale = haveRequest && request.units_per_meter > 0.f ? request.units_per_meter : settings.world_scale;
    std::array<float, 3> base{};
    if (haveRequest && request.base_valid) {
        base = request.base_position;
        if (s_roomScale.valid && request.tracking_origin_generation == s_roomScale.generation) {
            base[0] += s_roomScale.consumed[0]; base[2] += s_roomScale.consumed[2];
        }
    } else if (snapshot.head_pose.valid) base = snapshot.head_pose.position;
    const auto body = BodyYaw(cameraXf);
    const auto origin = player.GetEyePosition() - body.Transform(PrimeFromXr(base) * scale);
    const auto x = body.Transform(CVector3f(scale, 0.f, 0.f));
    const auto y = body.Transform(CVector3f(0.f, 0.f, scale));
    const auto z = body.Transform(CVector3f(0.f, -scale, 0.f));
    return CTransform4f(x.GetX(), y.GetX(), z.GetX(), origin.GetX(),
                        x.GetY(), y.GetY(), z.GetY(), origin.GetY(),
                        x.GetZ(), y.GetZ(), z.GetZ(), origin.GetZ());
}

namespace {
bool TrackedCannonPose(const CPlayer& player, const CTransform4f& cameraXf,
                       const PortVrSettings& settings, CVector3f& worldPosition,
                       CQuaternion& worldRotation, float& scale) noexcept {
    if (!settings.patch_cannon_rotation || !ImmersiveNow()) {
        return false;
    }
    const OpenXRInputSnapshot snapshot = OpenXRGetInputSnapshot();
    const OpenXRControllerState& hand = snapshot.controllers[settings.use_right_hand ? 1 : 0];
    if (!snapshot.runtime_active || !hand.connected || !hand.aim_pose.valid) {
        return false;
    }

    OpenXRFrameRequest request{};
    const bool haveRequest = OpenXRLatestFrameRequest(request);
    scale = haveRequest && request.units_per_meter > 0.f ? request.units_per_meter : settings.world_scale;
    // The controller relative to the tracking base the eyes are placed from;
    // without a base, relative to the head.
    std::array<float, 3> base{};
    if (haveRequest && request.base_valid) {
        base = request.base_position;
        if (s_roomScale.valid && request.tracking_origin_generation == s_roomScale.generation) {
            base[0] += s_roomScale.consumed[0];
            base[2] += s_roomScale.consumed[2];
        }
    } else if (snapshot.head_pose.valid) {
        base = snapshot.head_pose.position;
    }
    const std::array<float, 3> relativeXr{
        hand.aim_pose.position[0] - base[0],
        hand.aim_pose.position[1] - base[1],
        hand.aim_pose.position[2] - base[2],
    };
    // PrimedGun's position offsets: x right, y up, z back (OpenXR axes), in metres.
    const CVector3f relative =
        PrimeFromXr(relativeXr) * scale +
        CVector3f(settings.offset_x * scale, -settings.offset_z * scale, settings.offset_y * scale);

    const CQuaternion body = BodyYaw(cameraXf);
    worldPosition = player.GetEyePosition() + body.Transform(relative);

    // PrimedGun's rotation offsets are local yaw (about up), pitch (about
    // right) and roll (about the aim), applied in that order to the controller.
    const CQuaternion offsets = AxisQuaternion(0.f, 0.f, 1.f, settings.rot_offset_y * kDegreesToRadians) *
                                AxisQuaternion(1.f, 0.f, 0.f, settings.rot_offset_x * kDegreesToRadians) *
                                AxisQuaternion(0.f, 1.f, 0.f, -settings.rot_offset_z * kDegreesToRadians);
    worldRotation = (body * PrimeFromXr(hand.aim_pose.orientation) * offsets).BuildNormalized();
    return true;
}
} // namespace

bool VrCannonAim(const CPlayer& player, const CTransform4f& cameraXf,
                 CVector3f& origin, CVector3f& direction) noexcept {
    CQuaternion rotation;
    float scale;
    if (!TrackedCannonPose(player, cameraXf, GetVrSettings(), origin, rotation, scale)) {
        return false;
    }
    direction = rotation.Transform(CVector3f(0.f, 1.f, 0.f));
    return true;
}

bool VrCannonTransform(const CStateManager& mgr, const CPlayer& player, const CTransform4f& cameraXf,
                       CTransform4f& gunXf) noexcept {
    (void)mgr;
    s_cannon.tracked = false;
    s_cannon.modelOffsetWorld = CVector3f::Zero();
    const PortVrSettings settings = GetVrSettings();
    CVector3f worldPosition;
    CQuaternion worldRotation;
    float scale;
    if (!TrackedCannonPose(player, cameraXf, settings, worldPosition, worldRotation, scale)) {
        s_cannon.smoothValid = false;
        return false;
    }

    const CVector3f jump = worldPosition - s_cannon.smoothPosition;
    if (!s_cannon.smoothValid || CVector3f::Dot(jump, jump) > kCannonSnapDistance * kCannonSnapDistance) {
        s_cannon.smoothRotation = worldRotation;
        s_cannon.smoothPosition = worldPosition;
        s_cannon.smoothValid = true;
    } else {
        s_cannon.smoothPosition = s_cannon.smoothPosition * kCannonSmoothKeep + worldPosition * (1.f - kCannonSmoothKeep);
        s_cannon.smoothRotation = Nlerp(s_cannon.smoothRotation, worldRotation, 1.f - kCannonSmoothKeep);
    }

    gunXf = s_cannon.smoothRotation.BuildTransform4f();
    gunXf.SetTranslation(s_cannon.smoothPosition);
    const CVector3f modelLocal(settings.model_offset_x * scale,
                               (kModelForwardBackOffset + settings.model_offset_y) * scale,
                               settings.model_offset_z * scale);
    s_cannon.modelOffsetWorld = gunXf.Rotate(modelLocal);
    s_cannon.tracked = true;
    return true;
}

bool VrCannonTracked() noexcept { return s_cannon.tracked; }

bool VrMirrorWeaponModels() noexcept { return ImmersiveNow() && !GetVrSettings().use_right_hand; }

CTransform4f VrWeaponModelTransform(const CTransform4f& pose) noexcept {
    return VrMirrorWeaponModels() ? pose * CTransform4f::Scale(-1.f, 1.f, 1.f) : pose;
}

void VrResetCannonSmoothing() noexcept { s_cannon.smoothValid = false; }

CVector3f VrCannonModelOffsetWorld() noexcept {
    return s_cannon.tracked ? s_cannon.modelOffsetWorld : CVector3f::Zero();
}

} // namespace PortVr
