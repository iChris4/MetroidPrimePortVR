// SPDX-License-Identifier: GPL-3.0-or-later
//
// The headset's view of the game, for the game thread: what the game camera
// must do differently while the headset shows the world per eye, and the
// tracked arm cannon.
//
// The eyes get the head's rotation and position through the stereo packet
// (aurora composes them into every draw), so the game camera stays the body
// facing: yaw from the stick, no pitch, no bob. The game still needs the head's
// facing for what it computes on the CPU: the culling frustum, the audio
// listener. The cannon follows the controller's aim pose, placed relative to
// the player's eye with PrimedGun's calibration (world scale, position and
// rotation offsets, model offset, smoothing).

#pragma once

#include "Kyoto/Math/CMatrix3f.hpp"
#include <array>
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "MetroidPrime/TGameTypes.hpp"

class CFrustumPlanes;
class CPlayer;
class CStateManager;

namespace PortSurfaceCulling { struct StereoVolume; }
class CGuiModel;

namespace PortVr {

// The headset is running and this frame is shown immersively (the world per
// eye rather than on the virtual screen).
bool VrImmersive() noexcept;

// Horizontal physical walking moves the collision body. Rendering subtracts
// the consumed tracking displacement because stereo replay adds it again.
void VrRoomScaleUpdate(CStateManager& mgr, CPlayer& player, float dt) noexcept;
CTransform4f VrRoomScaleCameraTransform(const CTransform4f& cameraXf) noexcept;

// `cameraXf` with the head's rotation (relative to the recentred base)
// composed in; `cameraXf` itself when not immersive or the head is unknown.
CTransform4f VrHeadFacingTransform(const CTransform4f& cameraXf) noexcept;

// The head's gaze in the world, as the eyes see it: from the head's position
// (the game camera plus the head's offset from the tracking base) along its
// forward, lean back included. False when not immersive or the head is unknown.
bool VrHeadGaze(const CTransform4f& cameraXf, CVector3f& origin, CVector3f& direction) noexcept;

// The eyes' facing in the world, as VrHeadGaze sees it (lean back included),
// kept upright: `cameraXf`'s rotation with the head's yaw and pitch composed
// in but not its roll, or `cameraXf`'s own rotation when not immersive or the
// head is unknown. For the billboards the game turns to the camera (scan
// icons, target reticles), which would otherwise face the body and show their
// edge when the head turns, and would roll with a tilted head.
CMatrix3f VrHeadViewRotation(const CTransform4f& cameraXf) noexcept;
// Head orientation and position for CPU effects; the world render camera
// remains unchanged so stereo replay does not apply head tracking twice.
CTransform4f VrHeadViewTransform(const CTransform4f& cameraXf) noexcept;

// Replaces `frustum` with the head-facing cone of the settings' culling angle
// when immersive and culling is enabled; false leaves it alone.
bool VrCullingFrustum(const CTransform4f& cameraXf, float nearZ, CFrustumPlanes& frustum) noexcept;

// Conservative actual-eye volumes for static world surface culling. Includes
// lean-back, positional tracking and a margin around each eye. False keeps all.
bool VrSurfaceCullingVolume(const CTransform4f& cameraXf,
                            PortSurfaceCulling::StereoVolume& volume) noexcept;

// Immersive: the first-person camera keeps its yaw only (the headset supplies
// the pitch), does not bob, and the arm cannon does not fidget.
bool VrFlattenLookPitch() noexcept;
// Latch headset yaw once on morph entry for the simulation camera and controls.
void VrBeginBallCamera(const CTransform4f& firstPersonXf) noexcept;
CVector3f VrBallCameraEntryDirection() noexcept;
// Remove the latched yaw from the render base, since stereo adds head tracking.
CTransform4f VrBallCameraBaseTransform(const CTransform4f& camera, float weight = 1.f) noexcept;
// Unmorphing preserves the current render-base yaw and translates to eye height.
void VrBeginUnmorphCamera(const CTransform4f& camera, const CVector3f& eyePosition) noexcept;
CTransform4f VrUnmorphCameraBase(const CTransform4f& camera) noexcept;
// The stand-up model faces the HMD's captured horizontal heading, separately
// from the render base so the view does not apply head rotation twice.
CTransform4f VrUnmorphFacingTransform(const CTransform4f& player) noexcept;
CTransform4f VrUnmorphCameraTransform(const CVector3f& eyePosition, float progress) noexcept;
bool VrNoCameraBob() noexcept;
bool VrNoArmCannonFidget() noexcept;
// The headset is running and the settings remove the letterbox: the game's
// viewport scale stays 1 and the cinema-bars camera filter is not drawn.
bool VrRemoveCinematicBars() noexcept;
// The headset is running and the settings put the sky at infinity: the sky
// draw is routed AURORA_STEREO_ROUTE_SKY (CWorld::DrawSky), so each eye sees
// the camera-centred sky dome with the head's rotation only, without the eye
// offset that would place it at its modelled sixty units.
bool VrSkyAtInfinity() noexcept;
// The headset is running and the settings turn the space warp off
// (CStateManager::DrawSpaceWarp draws nothing).
bool VrHideSpaceWarp() noexcept;
// Immersive and the settings turn the scan window's zoom off: the window's
// copy is the pane's own size (CPlayerVisor::DrawScanEffect).
bool VrScanWindowNoZoom() noexcept;
// A modal message box or the in-game save prompt (kIGGS_PauseHUDMessage,
// kIGGS_PauseSaveGame), opening, open or closing. Game thread, at the end of
// every CInGameGuiManager::Update (and false from its destructor): `open`,
// and whether a cinematic camera is current, which then decides the frame.
void VrNoteMessageBox(bool open, bool cinematic) noexcept;
// The note, for the frame's game mode (main.cpp): Message, or Cinematic when
// the box was opened under a cinematic camera.
bool VrMessageBoxOpen() noexcept;
bool VrMessageBoxCinematic() noexcept;
// The noted box is shown in the immersive view: the headset runs and the
// policy would show its frame immersively (vr_message_box_immersive). The world
// then keeps being drawn under the pause blur, which each eye takes from its
// own image every frame (CPauseScreenBlur), and the box and the prompt are laid
// on a screen of the morph ball HUD's size and distance, placed ahead of the
// head when the box opens and held still (CInGameGuiManager::Draw).
bool VrMessageBoxInView() noexcept;
// The vr_floating_hud setting: the morph ball's HUD and the message boxes go on
// the easing plane (AURORA_STEREO_ROUTE_FLOATING_HUD) instead of the held
// screen (ANCHORED_SCREEN).
bool VrFloatingHud() noexcept;
// The pause menu, the logbook or the map screen (kIGGS_PauseGame,
// kIGGS_PauseLogBook, kIGGS_MapScreen), opening, open or closing. Game thread,
// at the end of every CInGameGuiManager::Update (and false from its destructor).
void VrNoteGameMenu(bool open) noexcept;
// The note, for the frame's game mode (main.cpp: Paused).
bool VrGameMenuOpen() noexcept;
// The noted menu is shown in the immersive view: the headset runs and the
// policy would show a Paused frame immersively (vr_game_menu_screen_enabled
// off). The world then keeps being drawn under the pause blur, as for a box.
bool VrGameMenuInView() noexcept;
// The map screen's 3D map in the headset (AURORA_STEREO_ROUTE_ANCHORED_3D): its
// centre `distance` game units ahead of the held anchor, the map camera's view
// half-height scaled to `halfHeight` units, how near its near side may come
// (`clearance`: its depth is compressed beyond that) and the near plane.
void VrMapHologram(float& distance, float& halfHeight, float& clearance, float& nearPlane) noexcept;
// The head-locked route's scales (vr_metroid_hud_size, vr_metroid_hud_distance):
// a HUD position's x and z (Prime camera space) by `xy`, its depth (y) by `depth`.
void VrHeadLockedScales(float& xy, float& depth) noexcept;
// Every eye's depth back to the far plane (an orthographic quad through a
// far-only depth window, depth written, no colour), keeping the projection,
// view and model. Draw it on a route that covers the whole eye (WORLD).
void VrResetEyeDepth() noexcept;
// The virtual screen the screen routes lay the mono picture on
// (aurora_set_stereo_screen_2d), in game units: half its width, half its
// height (the viewport's aspect) and its distance ahead of its anchor.
void VrScreenPlane(float& halfWidth, float& halfHeight, float& distance) noexcept;
// The screen route the pause menu is drawn on in the headset's view
// (CInGameGuiManager::Draw: AURORA_STEREO_ROUTE_ANCHORED_SCREEN or FLOATING_HUD),
// 0 when it is not.
void VrSetPauseScreenRoute(unsigned route) noexcept;
// A 3D model of the pause menu (the inventory's Samus, the logbook's artifacts,
// the options' GameCube), constructed right after its own perspective camera is
// set: on the pause menu's screen in the headset view, the model is drawn in 3D
// (AURORA_STEREO_ROUTE_ANCHORED_3D / FLOATING_3D) where its flat picture would
// stand, at that size, a little in front of the screen, over every eye's depth
// reset; the destructor goes back to the screen. Nothing otherwise.
class VrPauseModel3D {
public:
    VrPauseModel3D() noexcept;
    ~VrPauseModel3D() noexcept;
    VrPauseModel3D(const VrPauseModel3D&) = delete;
    VrPauseModel3D& operator=(const VrPauseModel3D&) = delete;
    // The model is being drawn in 3D.
    bool Active() const noexcept { return active_; }

private:
    bool active_ = false;
};
// The beam wheel's hover, for the HUD's beam menu (CHudVisorBeamMenu): the
// beam the aim ray points at while the weapon hand's B holds the wheel open
// (vr_pad.cpp), as CPlayerState::EBeamId (0 Power, 1 Ice, 2 Wave, 3 Plasma),
// or -1 when the wheel is closed, nothing is hovered, or the
// vr_beam_wheel_hud_highlight setting is off. The menu lights that beam's box,
// where PrimedGun drew its own panel with a frame around the hovered icon.
int VrBeamWheelHoverBeam() noexcept;

// Uncalibrated tracking-space point to world space, shared by wrist UI geometry.
CTransform4f VrTrackingToWorldTransform(const CPlayer& player, const CTransform4f& cameraXf) noexcept;
void VrWristVisorDraw(const CStateManager& mgr) noexcept;
void VrWristCaptureModels(CGuiModel* const* icons, CGuiModel* const* frames) noexcept;
void VrWristForgetModel(const CGuiModel* widget) noexcept;

// The 6DOF cannon. Once per simulation tick from CPlayer::UpdateGunTransform:
// when the tracked controller drives the cannon, replaces `gunXf` (world) and
// returns true. `cameraXf` is the current game camera (its yaw is the body).
bool VrCannonTransform(const CStateManager& mgr, const CPlayer& player, const CTransform4f& cameraXf,
                       CTransform4f& gunXf) noexcept;
// Whether the last VrCannonTransform placed the cannon.
bool VrCannonTracked() noexcept;
// Reflect only model geometry; tracked poses and projectile aim stay rigid.
bool VrMirrorWeaponModels() noexcept;
CTransform4f VrWeaponModelTransform(const CTransform4f& pose) noexcept;
// The selected cannon controller's calibrated world-space aim, without
// advancing model smoothing. False when the controller is not tracked.
bool VrCannonAim(const CPlayer& player, const CTransform4f& cameraXf,
                 CVector3f& origin, CVector3f& direction) noexcept;
// Idle reticle on the cannon ray's first world hit, or its maximum reach.
bool VrCannonTargetPoint(const CStateManager& mgr, CVector3f& point) noexcept;
// The body turned at once (the snap turn): the next VrCannonTransform places
// the cannon without easing from the old facing, as PrimedGun's snap turn
// dropped its cannon smoothing.
void VrResetCannonSmoothing() noexcept;

// Look to scan (PrimedGun's gun ray / scan target hook, vr_patch_gun_ray_target):
// in the scan visor the head picks the scan target and the scan icons, rather
// than the body's facing (vr/vr_look_scan.h has the maths). Game thread.
//
// Look to lock-on and look to grapple (vr_look_to_lock_on, vr_look_to_grapple)
// do the same outside the scan visor for the orbit target, each for its kind:
// grapple points, and everything else that can be locked.
//
// Once per tick from CPlayer::UpdateOrbitInput, after the orbitable objects
// are gathered: measures the scannable objects (scan visor) or the orbitable
// ones (other visors) around the head's gaze.
void VrLookTargetUpdate(CStateManager& mgr, const CPlayer& player) noexcept;
// True when look to scan owns this tick's choice (immersive, scan visor,
// unmorphed, setting on); `id` is then the head's pick, possibly none.
// CPlayer::FindOrbitTargetId returns it instead of its screen-box choice.
bool VrLookToScanTarget(const CStateManager& mgr, TUniqueId& id) noexcept;
// The scannable objects near where the head looks, for their scan icons
// (CPlayerVisor::UpdateScanObjectIndicators); null and 0 when inactive.
const TUniqueId* VrLookToScanNearby(const CStateManager& mgr, int& count) noexcept;
// True when look to lock-on or look to grapple owns this tick's choice
// (immersive, not the scan visor, unmorphed, either setting on); `id` is then
// the head's pick, or else `gamePick` (FindBestOrbitableObject's screen-box
// choice) if that is of a kind the head does not pick, or none.
// CPlayer::FindOrbitTargetId returns it.
bool VrLookToLockTarget(const CStateManager& mgr, TUniqueId gamePick, TUniqueId& id) noexcept;
// Scan locks hold the body's facing. Combat locks and grapples keep native
// target-facing physics; CFirstPersonCamera separately preserves the view's
// acquisition angle, without feeding tracked head turns back into the body.
bool VrLookHoldsFacing(const CStateManager& mgr, const CPlayer& player) noexcept;
// The rendered model's offset from the tracked pose (PrimedGun's model offset
// and its base forward offset), in world units; zero when not tracked.
CVector3f VrCannonModelOffsetWorld() noexcept;

// PrimedGun's VR menu (vr/vr_menu.h), once per presented frame from
// CGraphics::EndScene: while it is open in the headset, applies the laser's
// hover and clicks and hands Aurora the menu's image when it changes
// (vr_menu.cpp). Nothing without OpenXR.
void VrMenuUpdate() noexcept;

} // namespace PortVr
