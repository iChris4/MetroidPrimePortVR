// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun's modern control scheme: the headset controllers' snapshot
// (openxr_controller_snapshot.h) turned into Metroid Prime's GameCube pad on
// port 0, once per simulation tick, as PrimedGun's GCPadEmu did inside Dolphin
// (ApplyPrimedGunModernControls). Which mapping applies comes from the game's
// own state rather than memory probes: classic (menus, map, cutscenes, morph
// ball), gameplay (first person, unmorphed), or orbit lock.
//
// The beam wheel: holding the weapon hand's B freezes a panel 0.26 m ahead of
// the aim pose; the aim ray's hit on it (or the hand's travel, as a fallback)
// picks Power / Wave / Ice / Plasma by its dominant axis, and letting go pulses
// the C-stick that way for eight samples.
//
// The visor gesture (vr_visor_dpad.h): with the off-hand controller held next
// to the headset, its stick is the D-pad, which picks the visors.
//
// The snap turn (vr_snap_turn.h): with the setting on, a flick of the look
// stick turns Samus by the snap turn angle instead of turning her smoothly.
//
// Directional movement (vr_directional_move.h): with the setting on, the move
// stick walks and strafes toward where its controller points, and only the
// look stick turns.

#pragma once

#include <cstdint>

class CStateManager;

namespace PortVr {

// Game thread, right before the game reads its controllers. `mgr` may be null
// (no state manager: the front end). Does nothing, and leaves port 0 to the
// real pads, unless a headset session is running in PrimedGun controller mode.
void VrPadUpdate(const CStateManager* mgr) noexcept;

// Game thread, every tick from CInGameGuiManager::Update: one of its paused
// screens (map, pause, logbook, save, HUD message) is open, opening or
// closing. The classic mapping applies meanwhile, as PrimedGun's game flow
// hooks made it, so the controllers' A confirms and B backs out.
void VrNoteInGameMenu(bool open) noexcept;

// Game thread, from CPlayer::PortVrSnapTurn: the snap turn a look stick flick
// asked for since the last call (vr_snap_turn.h), in degrees, positive to the
// right, or 0. Taking it clears it; a request left over when gameplay ends
// (menu, map, cinematic, morph ball) is dropped.
float VrTakeSnapTurn() noexcept;

// Directional movement for one tick: PrimedGun's left stick strafe
// (vr_directional_move.h).
struct VrMove {
    // The move stick walks Samus here rather than through the pad: the game's
    // walk force from the stick is left out. Set in gameplay outside an orbit
    // lock or a grapple, with the setting on and the heading's pose tracked.
    bool owns = false;
    // Past the deadzone: her horizontal velocity, in her frame (x right,
    // y forward), game units per second. Otherwise nothing is set and the
    // game's friction stops her.
    bool moving = false;
    float right = 0.0f;
    float forward = 0.0f;
};
// Game thread, from CPlayer::ComputeMovement while the player steers Samus on
// foot (CPlayer::PortVrPlayerSteers). `flatSpeed` is the horizontal speed she
// carries into the tick, `onGround` picks the ground or the air acceleration,
// `tick` is CStateManager::GetUpdateFrameIndex(): the speed ramps up over
// consecutive ticks and starts again from her speed after any tick without a
// call (a lock, the morph ball, a menu).
VrMove VrDirectionalMove(float flatSpeed, bool onGround, float dt, uint32_t tick) noexcept;

// What the pad synthesis last decided, for the game hooks and the overlays.
struct VrPadState {
    bool active = false;        // the VR controllers own port 0
    bool gameplay = false;      // first person, unmorphed, no menu: PrimedGun gameplay mapping
    float smooth_turn_speed_scale = 1.0f; // rate beyond the virtual stick's full deflection
    bool orbit_lock = false;    // gameplay with the lock button held on an orbit target
    bool weapon_panel = false;  // the beam wheel is open
    int weapon_selected = -1;   // 0 Power, 1 Wave, 2 Ice, 3 Plasma, -1 none
    uint32_t weapon_hand = 1;   // 0 left, 1 right
    bool wrist_visible = false;
    int wrist_hover = -1;
    unsigned wrist_owned = 0;
    bool visor_zone = false;    // the off hand is at the head: its stick is the D-pad
    int visor_direction = -1;   // VisorDpad::Dir: 0 up (combat), 1 right (X-ray), 2 down (thermal), 3 left (scan)
    bool move_owned = false;    // directional movement has the move stick (VrMove::owns)
    bool move_moving = false;   // and the stick is past its deadzone
    float move_heading_degrees = 0.0f; // the heading's yaw from Samus's forward, positive right
};
VrPadState GetVrPadState() noexcept;

// The headset controllers own port 0: VrPadUpdate has claimed it (and its
// motor) for PrimedGun's control scheme. A lock-free read, safe from any
// thread, and false in a build without OpenXR. The desktop's direct aim (mouse
// aim, twin stick) stands down meanwhile: the head and the tracked cannon aim,
// and the look stick turns or snaps. Gamepad mode's controllers are an
// ordinary SDL pad and never set it.
bool VrPadOwnsPort0() noexcept;
bool VrCustomLayoutActive() noexcept;
// Called only when CPlayer can accept a visor change, on an actual simulation tick.
int VrTakeWristVisorRequest() noexcept;

} // namespace PortVr
