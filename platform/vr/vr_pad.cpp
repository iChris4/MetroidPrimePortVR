// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/vr_pad.h"

#include "vr/openxr_controller_snapshot.h"
#include "vr/openxr_integration.h"
#include "vr/openxr_screen_math.h"
#include "vr/vr_beam_wheel.h"
#include "vr/vr_directional_move.h"
#include "vr/vr_rumble.h"
#include "vr/vr_settings.h"
#include "vr/vr_snap_turn.h"
#include "vr/vr_smooth_turn.h"
#include "vr/vr_controller_layout.h"
#include "vr/vr_visor_dpad.h"

#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/ScriptObjects/CScriptGrapplePoint.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <dolphin/pad.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace PortVr {
namespace {

using screen_math::Quat;
using screen_math::Vec3;

constexpr int8_t kStickFull = 100;   // PADClamp trims this to the octagon
constexpr uint8_t kTriggerFull = 255;
constexpr float kJumpStickThreshold = 0.55f;
constexpr uint32_t kGripGraceSamples = 30; // after gameplay starts, like PrimedGun
constexpr uint32_t kBeamPulseSamples = 8;

// The beam wheel's panel and pick (vr/vr_beam_wheel.h).
using BeamWheel::Beam;

struct WheelState {
    bool open = false;
    BeamWheel::Panel panel;
    bool first_sample = true;
    float zero_x = 0.0f;
    float zero_y = 0.0f;
    Beam selected = Beam::None;
    Beam pulse = Beam::None;
    uint32_t pulse_samples = 0;
};

// Directional movement: VrPadUpdate's reading of the move stick, for the ticks
// that follow it (VrDirectionalMove).
struct MoveState {
    bool owns = false;
    std::array<float, 2> heading{0.0f, 1.0f};
    DirectionalMove::Wish wish;
    float speed = 0.0f;
    float accel = 0.0f;
    float air_accel = 0.0f;
};

std::mutex g_mutex;
VrPadState g_state;
WheelState g_wheel;
VisorDpad::Tracker g_visor;
SnapTurn::Tracker g_snap;
float g_snap_pending = 0.0f; // degrees, positive right: VrTakeSnapTurn's, game thread only
MoveState g_move;             // game thread only, as the ramp and its tick
DirectionalMove::Ramp g_move_ramp;
bool g_move_ramp_ticked = false;
uint32_t g_move_ramp_tick = 0;
bool g_was_active = false;
bool g_recenter_was_pressed = false; // the right stick click's last state
bool g_was_gameplay = false;
bool g_in_game_menu = false; // VrNoteInGameMenu, from the game thread like VrPadUpdate
uint32_t g_gameplay_samples = 0;
std::atomic<bool> g_custom_layout{false};
std::atomic<bool> g_owns_motor{false}; // port 0's rumble goes to the headset controllers

// PADControlMotor's hook. While the headset controllers own port 0, its motor
// drives their haptics, on or off as PrimedGun's GCPad::SetOutput fed them,
// instead of the physical pad assigned to that port. Ports 1 and 2 carry the
// arm cannon's and the grapple arm's rumble meanwhile (vr_rumble.h). The game
// rumbles no other port, so those two never reach a real pad, not even when
// the controllers let go in the middle of an effect.
BOOL ClaimRumbleMotors(u32 chan, u32 cmd) {
    const bool owns = g_owns_motor.load(std::memory_order_relaxed);
    if (chan >= kRumbleChannelCount || (chan == 0 && !owns)) {
        return FALSE;
    }
    OpenXRSetRumble(owns && cmd == PAD_MOTOR_RUMBLE ? 1.0f : 0.0f, static_cast<RumbleChannel>(chan));
    return TRUE;
}

Quat Multiply(const Quat& a, const Quat& b) noexcept {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

int8_t ToStick(float value) noexcept {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int8_t>(std::lround(clamped * kStickFull));
}

bool Pressed(float value) noexcept { return value > 0.5f; }

double NowSeconds() noexcept {
    using namespace std::chrono;
    return duration< double >(steady_clock::now().time_since_epoch()).count();
}

// The guard at the top of CPlayer::UpdateVisorState: a visor press counts only
// unmorphed, out of a grapple and a grapple point lock, between transitions and
// outside a scan. Anything else the game drops.
bool VisorPressAccepted(const CStateManager& mgr, const CPlayer& player) {
    if (player.GetOrbitState() == CPlayer::kOS_Grapple ||
        TCastToConstPtr< CScriptGrapplePoint >(mgr.GetObjectById(player.GetOrbitTargetId()))) {
        return false;
    }
    return player.GetMorphballTransitionState() == CPlayer::kMS_Unmorphed &&
           !mgr.GetPlayerState()->GetIsVisorTransitioning() &&
           player.GetPlayerScanState() == CPlayer::kSS_NotScanning;
}

// The game's control map gives the visors to the D-pad (up combat, right
// X-ray, down thermal, left scan on the retail disc).
uint16_t DpadButton(VisorDpad::Dir dir) noexcept {
    switch (dir) {
    case VisorDpad::Dir::Up:
        return PAD_BUTTON_UP;
    case VisorDpad::Dir::Right:
        return PAD_BUTTON_RIGHT;
    case VisorDpad::Dir::Down:
        return PAD_BUTTON_DOWN;
    case VisorDpad::Dir::Left:
        return PAD_BUTTON_LEFT;
    default:
        return 0;
    }
}

// A grip "press" per PrimedGun: the squeeze past half travel, or on a Valve
// Index the grip force / trackpad force against the configured thresholds.
bool GripPressed(const OpenXRControllerState& hand, const std::string& profile, const PortVrSettings& settings) {
    const bool index = profile.find("valve/index_controller") != std::string::npos;
    if (index) {
        if (settings.grip_inputs_use_trackpad) {
            return hand.trackpad_force > settings.trackpad_press_threshold || hand.trackpad_click;
        }
        return hand.squeeze_force > settings.index_grip_press_threshold || hand.squeeze_click;
    }
    return hand.squeeze_click || hand.squeeze_value > 0.5f;
}

void SetBeamCStick(Beam beam, PADStatus& pad) noexcept {
    const std::array<int, 2> direction = BeamWheel::CStickDirection(beam);
    if (direction[0] != 0) {
        pad.substickX = static_cast<int8_t>(direction[0] * kStickFull);
    }
    if (direction[1] != 0) {
        pad.substickY = static_cast<int8_t>(direction[1] * kStickFull);
    }
}

// The beam wheel, run every sample so a release is seen. Returns true while
// the modifier is held (the wheel owns the weapon hand's B).
void UpdateWeaponWheel(const OpenXRControllerState& weapon, bool modifier, bool gameplay, PADStatus& pad) {
    WheelState& wheel = g_wheel;
    if (wheel.pulse_samples > 0) {
        SetBeamCStick(wheel.pulse, pad);
        if (--wheel.pulse_samples == 0) {
            wheel.pulse = Beam::None;
        }
    }
    if (!modifier || !gameplay) {
        if (wheel.open && wheel.selected != Beam::None) {
            wheel.pulse = wheel.selected;
            wheel.pulse_samples = kBeamPulseSamples;
            SetBeamCStick(wheel.pulse, pad);
        }
        wheel.open = false;
        wheel.selected = Beam::None;
        return;
    }
    const OpenXRPoseState& pose = weapon.aim_pose.valid ? weapon.aim_pose : weapon.grip_pose;
    if (!pose.valid) {
        return;
    }
    if (!wheel.open) {
        wheel.open = true;
        wheel.panel = BeamWheel::OpenPanel(pose.position, pose.orientation);
        wheel.first_sample = true;
        wheel.selected = Beam::None;
    }
    // The roll-free aim ray against the frozen panel, read from where the
    // first sample landed: up Power, right Wave, down Ice, left Plasma.
    const std::array<float, 2> aim = BeamWheel::Measure(wheel.panel, pose.position, pose.orientation);
    if (wheel.first_sample) {
        wheel.first_sample = false;
        wheel.zero_x = aim[0];
        wheel.zero_y = aim[1];
    }
    wheel.selected = BeamWheel::Pick(aim[0] - wheel.zero_x, aim[1] - wheel.zero_y);
}

// Classic: menus, the map, cutscenes, the morph ball. The plain GameCube
// layout on the two controllers.
void ApplyClassic(const OpenXRControllerState& left, const OpenXRControllerState& right, uint32_t weapon_hand,
                  bool morph_ball, const PortVrSettings& settings,
                  const std::array<std::string, 2>& profiles, PADStatus& pad) {
    const OpenXRControllerState& weapon = weapon_hand == 1 ? right : left;
    const OpenXRControllerState& off = weapon_hand == 1 ? left : right;
    pad.stickX = ToStick(left.thumbstick_x);
    pad.stickY = ToStick(left.thumbstick_y);
    pad.substickX = ToStick(right.thumbstick_x);
    pad.substickY = ToStick(right.thumbstick_y);
    if (settings.custom_layout_enabled) {
        const std::array<OpenXRControllerState, 2> hands{left, right};
        const auto bound = [&](int binding) { return ControllerLayout::Pressed(hands, binding); };
        if (morph_ball) {
            if (bound(settings.custom_bomb)) pad.button |= PAD_BUTTON_A;
            if (bound(settings.custom_unmorph)) pad.button |= PAD_BUTTON_X;
            if (bound(settings.custom_boost)) pad.button |= PAD_BUTTON_B;
            if (bound(settings.custom_power_bomb)) pad.button |= PAD_BUTTON_Y;
            if (bound(settings.custom_spider)) { pad.button |= PAD_TRIGGER_L; pad.triggerLeft = 255; }
            pad.substickY = std::min<int8_t>(pad.substickY, 0);
            if (bound(settings.custom_spring)) pad.substickY = kStickFull;
        } else {
            if (bound(settings.custom_confirm)) pad.button |= PAD_BUTTON_A;
            if (bound(settings.custom_back)) pad.button |= PAD_BUTTON_B;
            if (bound(settings.custom_lock)) { pad.button |= PAD_TRIGGER_L; pad.triggerLeft = 255; }
            if (bound(settings.custom_shoot)) { pad.button |= PAD_TRIGGER_R; pad.triggerRight = 255; }
            if (bound(settings.custom_missile)) pad.button |= PAD_BUTTON_Y;
        }
        if (bound(settings.custom_pause)) pad.button |= PAD_BUTTON_START;
        if (bound(settings.custom_map)) pad.button |= PAD_TRIGGER_Z;
        pad.analogA = (pad.button & PAD_BUTTON_A) ? 255 : 0;
        pad.analogB = (pad.button & PAD_BUTTON_B) ? 255 : 0;
        return;
    }
    // In Morph Ball, the weapon hand drops bombs and the off hand unmorphs.
    // This is independent of primary-button jump; menu bindings stay fixed.
    if (morph_ball ? off.primary : left.primary) pad.button |= PAD_BUTTON_X;
    if (left.secondary) pad.button |= PAD_BUTTON_START;
    if (morph_ball ? weapon.primary : right.primary) pad.button |= PAD_BUTTON_A;
    if (right.secondary) pad.button |= PAD_BUTTON_B;
    if (Pressed(off.trigger_value) || off.trigger_click) {
        pad.button |= PAD_TRIGGER_L;
        pad.triggerLeft = kTriggerFull;
    }
    if (Pressed(weapon.trigger_value) || weapon.trigger_click) {
        pad.button |= PAD_TRIGGER_R;
        pad.triggerRight = kTriggerFull;
    }
    if (settings.grip_inputs_enabled) {
        if (GripPressed(off, profiles[weapon_hand == 1 ? 0 : 1], settings)) pad.button |= PAD_TRIGGER_Z;
        if (GripPressed(weapon, profiles[weapon_hand], settings)) pad.button |= PAD_BUTTON_Y;
    }
}

// Gameplay and orbit lock: PrimedGun's first-person layout.
float ApplyGameplay(const OpenXRControllerState& left, const OpenXRControllerState& right, uint32_t weapon_hand,
                   bool orbit, bool grapple, bool grips_ready, const PortVrSettings& settings,
                   const std::array<std::string, 2>& profiles, PADStatus& pad) {
    const OpenXRControllerState& weapon = weapon_hand == 1 ? right : left;
    const OpenXRControllerState& off = weapon_hand == 1 ? left : right;
    const OpenXRControllerState& move = settings.directional_movement_use_right_stick ? right : left;
    const OpenXRControllerState& look = settings.custom_layout_enabled ? (settings.custom_look_hand == 0 ? left : right)
        : settings.directional_movement_use_right_stick ? left : right;
    const std::array<OpenXRControllerState, 2> hands{left, right};
    const bool custom = settings.custom_layout_enabled;
    const auto bound = [&](int binding) {
        if (custom && (binding == settings.custom_beam ||
            (settings.custom_visor_mode == 1 && binding == settings.custom_visor_hold))) return false;
        return ControllerLayout::Pressed(hands, binding);
    };
    const bool modifier = custom ? ControllerLayout::Pressed(hands, settings.custom_beam) : weapon.secondary; // hold + swing
    const bool a_jump = settings.combat_jump_use_primary_button;

    // Forward / back on the move stick. Sideways: the orbit strafe in a lock,
    // otherwise the look stick's turn. Directional movement takes the move
    // stick's X for its strafe (VrDirectionalMove), as PrimedGun did; without
    // it that X turns too, as on the TV. The Y stays on the pad either way:
    // the game's jump reads it for the jump's arc.
    //
    // A grapple has no strafe, and its swing turns on the pad's X
    // (CPlayer::ApplyGrappleForces), so there the move stick's X turns again.
    // With the snap turn on it is the only thing that does: the look stick
    // then only snaps, and a snap waits until the grapple is over. PrimedGun's
    // lock layout, which L held on the grapple point gave it, did the same.
    float turnSpeedScale = 1.0f;
    pad.stickY = ToStick(move.thumbstick_y);
    if (orbit) {
        pad.stickX = ToStick(move.thumbstick_x);
    } else {
        float turn = settings.directional_movement_enabled && !grapple ? 0.0f : move.thumbstick_x;
        if (!settings.snap_turn_enabled && !modifier) {
            // Grapple steering stays on the game's ordinary swing input.
            const float sensitivity = grapple ? std::min(settings.look_yaw_sensitivity, 1.0f)
                                              : settings.look_yaw_sensitivity;
            const auto smooth = SmoothTurn::Mix(turn, look.thumbstick_x, sensitivity);
            turn = smooth.stick;
            turnSpeedScale = smooth.speed_scale;
        }
        pad.stickX = ToStick(turn);
    }
    // Jump: the look stick pushed up, or the weapon hand's primary button
    // when enabled (A right-handed, X left-handed).
    const auto jumpBinding = [&](int binding) {
        const int part = binding > 0 ? (binding - 1) % 16 + 1 : 0;
        return bound(binding) && !(modifier && part >= 8 && part <= 11);
    };
    const bool jump = custom ? (jumpBinding(settings.custom_jump) || jumpBinding(settings.custom_jump_extra))
                             : a_jump ? weapon.primary : (look.thumbstick_y > kJumpStickThreshold && !modifier);
    if (jump) {
        pad.button |= PAD_BUTTON_B;
        pad.analogB = kTriggerFull;
    }
    // Fire on the weapon trigger.
    if (custom ? bound(settings.custom_shoot) : (Pressed(weapon.trigger_value) || weapon.trigger_click)) {
        pad.button |= PAD_BUTTON_A;
        pad.analogA = kTriggerFull;
    }
    // Lock / scan on the off trigger.
    if (custom ? bound(settings.custom_lock) : (Pressed(off.trigger_value) || off.trigger_click)) {
        pad.button |= PAD_TRIGGER_L;
        pad.triggerLeft = kTriggerFull;
    }
    // Morph ball stays on the off hand's primary button (X right-handed,
    // A left-handed), even when the weapon hand's primary button jumps.
    if (custom ? bound(settings.custom_morph) : off.primary) pad.button |= PAD_BUTTON_X;
    // Start on the off hand's Y, unless that hand is busy with the menu button
    // or the weapon modifier is held.
    if (custom ? bound(settings.custom_pause) : (off.secondary && !off.menu && !modifier)) pad.button |= PAD_BUTTON_START;
    // Grips: map on the off hand, missile on the weapon hand, after the grace.
    if (custom || (settings.grip_inputs_enabled && grips_ready)) {
        if (custom ? bound(settings.custom_map) : GripPressed(off, profiles[weapon_hand == 1 ? 0 : 1], settings)) pad.button |= PAD_TRIGGER_Z;
        if (custom ? bound(settings.custom_missile) : GripPressed(weapon, profiles[weapon_hand], settings)) pad.button |= PAD_BUTTON_Y;
    }
    return turnSpeedScale;
}

} // namespace

void VrPadUpdate(const CStateManager* mgr) noexcept {
    PortVrSettings settings = GetVrSettings();
    if (settings.custom_layout_enabled) {
        settings.controller_mode = ControllerModeSetting::PrimedGun;
        settings.directional_movement_use_right_stick = settings.custom_move_hand == 1;
    }
    const bool active = settings.enabled && settings.controller_mode == ControllerModeSetting::PrimedGun &&
                        OpenXRIsRunning();
    OpenXRInputSnapshot snapshot;
    if (active) {
        snapshot = OpenXRGetInputSnapshot();
    }
    const bool controllers = active && snapshot.runtime_active &&
                             (snapshot.controllers[0].connected || snapshot.controllers[1].connected);
    g_custom_layout.store(controllers && settings.custom_layout_enabled, std::memory_order_relaxed);
    if (!controllers) {
        if (g_was_active) {
            PADClearVirtualStatus(0);
            // The motor goes back to port 0's pad, and the headset stops.
            g_owns_motor.store(false, std::memory_order_relaxed);
            OpenXRClearRumble();
            g_was_active = false;
        }
        std::lock_guard lock(g_mutex);
        g_state = {};
        g_wheel = {};
        g_visor.Reset();
        g_snap.Reset();
        g_snap_pending = 0.0f;
        g_move = {};
        g_gameplay_samples = 0;
        return;
    }
    if (!g_was_active) {
        // The pad that had port 0 may be mid-pulse, and its stop would now
        // reach the headset instead: stop it before the controllers take over.
        PADControlMotor(0, PAD_MOTOR_STOP_HARD);
        OpenXRClearRumble();
        PADSetMotorCallback(ClaimRumbleMotors);
        g_owns_motor.store(true, std::memory_order_relaxed);
    }
    g_was_active = true;

    // The mapping, from the game's own state.
    bool gameplay = false;
    bool morph_ball = false;
    bool orbit = false;
    bool grapple = false;
    bool visor_press_accepted = false;
    bool visor_changing = false;
    if (mgr != nullptr && mgr->GetPlayer() != nullptr) {
        const CPlayer* player = mgr->GetPlayer();
        const bool first_person = player->GetCameraState() == CPlayer::kCS_FirstPerson;
        const bool unmorphed = player->GetMorphballTransitionState() == CPlayer::kMS_Unmorphed;
        const bool cinematic = mgr->GetCameraManager() != nullptr && mgr->GetCameraManager()->IsInCinematicCamera();
        gameplay = first_person && unmorphed && !cinematic && !mgr->GetInMapScreen() && !g_in_game_menu;
        morph_ball = player->GetMorphballTransitionState() == CPlayer::kMS_Morphed &&
                     !cinematic && !mgr->GetInMapScreen() && !g_in_game_menu;
        if (gameplay) {
            const CPlayer::EPlayerOrbitState state = player->GetOrbitState();
            orbit = state == CPlayer::kOS_OrbitObject || state == CPlayer::kOS_ForcedOrbitObject ||
                    state == CPlayer::kOS_OrbitPoint || state == CPlayer::kOS_OrbitCarcass;
            grapple = state == CPlayer::kOS_Grapple; // from the pull to the jump off
            visor_press_accepted = VisorPressAccepted(*mgr, *player);
            visor_changing = mgr->GetPlayerState()->GetIsVisorTransitioning();
        }
    }
    if (gameplay && !g_was_gameplay) {
        g_gameplay_samples = 0;
    }
    g_was_gameplay = gameplay;
    if (gameplay && g_gameplay_samples < kGripGraceSamples) {
        ++g_gameplay_samples;
    }
    const bool grips_ready = g_gameplay_samples >= kGripGraceSamples;

    const uint32_t weapon_hand = settings.use_right_hand ? 1u : 0u;
    const uint32_t visor_hand = settings.custom_layout_enabled ? settings.custom_visor_hand : weapon_hand == 1 ? 0u : 1u; // PrimedGun's D-pad hand: the off hand
    OpenXRControllerState left = snapshot.controllers[0];
    OpenXRControllerState right = snapshot.controllers[1];
    const OpenXRControllerState& weapon = snapshot.controllers[weapon_hand];

    // The visor gesture, in gameplay only (PrimedGun: first person, unmorphed).
    VisorDpad::Output visor;
    {
        const OpenXRControllerState& hand = snapshot.controllers[visor_hand];
        VisorDpad::Input in;
        in.armed = settings.xr_dpad_enabled && gameplay && hand.connected && hand.aim_pose.valid &&
                   snapshot.head_pose.valid;
        in.near_head = in.armed && VisorDpad::HandNearHead(hand.aim_pose.position, snapshot.head_pose.position,
                                                           settings.xr_dpad_head_radius,
                                                           settings.xr_dpad_head_y_below);
        in.stick_x = hand.thumbstick_x;
        in.stick_y = hand.thumbstick_y;
        in.deadzone = VisorDpad::EffectiveDeadzone(settings.xr_dpad_deadzone);
        in.accepting = visor_press_accepted;
        in.now = NowSeconds();
        if (settings.custom_layout_enabled) {
            const bool holdMode = settings.custom_visor_mode == 1;
            const bool held = ControllerLayout::Pressed(snapshot.controllers, settings.custom_visor_hold);
            in.armed = gameplay && hand.connected && settings.custom_visor_mode != 2 &&
                       (holdMode ? held : (hand.aim_pose.valid && snapshot.head_pose.valid));
            if (holdMode) {
                in.near_head = held;
                if (!held) g_visor.Reset();
            }
        }
        visor = g_visor.Update(in);
    }
    // The snap turn, on the look stick before the visor gesture takes it:
    // PrimedGun keeps a flick made at the head from counting afterwards.
    {
        const uint32_t look_hand = settings.custom_layout_enabled ? settings.custom_look_hand : settings.directional_movement_use_right_stick ? 0u : 1u;
        const OpenXRControllerState& hand = snapshot.controllers[look_hand];
        SnapTurn::Input in;
        in.enabled = settings.snap_turn_enabled;
        in.connected = hand.connected;
        in.stick_x = hand.thumbstick_x;
        in.near_head = (look_hand == visor_hand && visor.zone) ||
                       (!settings.custom_layout_enabled && hand.aim_pose.valid && snapshot.head_pose.valid &&
                        VisorDpad::HandNearHead(hand.aim_pose.position, snapshot.head_pose.position,
                                                settings.xr_dpad_head_radius, settings.xr_dpad_head_y_below));
        in.accepting = gameplay && !visor_changing;
        in.degrees = settings.snap_turn_degrees;
        in.now = NowSeconds();
        const float turn = g_snap.Update(in);
        if (turn != 0.0f) {
            g_snap_pending = turn;
        } else if (!gameplay) {
            g_snap_pending = 0.0f;
        }
    }
    // Directional movement: the move stick read along its controller's heading
    // (or the head's), for the ticks' CPlayer::ComputeMovement. In an orbit
    // lock the stick is the game's orbit strafe, as PrimedGun stood down while
    // the lock was held on a target, and in a grapple the swing's. With the
    // off hand at the head nothing walks, whichever stick moves: PrimedGun's
    // rule too.
    {
        const OpenXRControllerState& hand = settings.directional_movement_use_right_stick ? right : left;
        const OpenXRPoseState& pose =
            settings.directional_movement_use_hmd_direction ? snapshot.head_pose : hand.aim_pose;
        g_move = {};
        g_move.owns = settings.directional_movement_enabled && gameplay && !orbit && !grapple && hand.connected &&
                      pose.valid;
        if (g_move.owns) {
            g_move.heading = DirectionalMove::Heading(pose.orientation);
            if (!visor.zone) {
                g_move.wish = DirectionalMove::FromStick(g_move.heading, hand.thumbstick_x, hand.thumbstick_y,
                                                         settings.directional_movement_deadzone);
            }
            g_move.speed = settings.directional_movement_speed;
            g_move.accel = settings.directional_movement_accel;
            g_move.air_accel = settings.directional_movement_air_accel;
        }
    }
    // While the hand is at the head its stick is the D-pad's alone: no walking,
    // strafing, turning or jumping from it.
    if (visor.zone) {
        OpenXRControllerState& hand = visor_hand == 0 ? left : right;
        hand.thumbstick_x = 0.0f;
        hand.thumbstick_y = 0.0f;
    }

    // PrimedGun's one-click height set: the right stick click recenters the
    // immersive origin on where the head is now (position only, no yaw).
    // PrimedGun ignores it while the visor gesture is up.
    {
        const bool pressed = (settings.custom_layout_enabled
            ? ControllerLayout::Pressed(snapshot.controllers, settings.custom_height)
            : right.connected && right.thumbstick_click) && !visor.zone;
        if (pressed && !g_recenter_was_pressed) {
            OpenXRRequestRecenter();
        }
        g_recenter_was_pressed = pressed;
    }

    PADStatus pad{};
    pad.err = PAD_ERR_NONE;
    float turnSpeedScale = 1.0f;
    if (gameplay) {
        turnSpeedScale = ApplyGameplay(left, right, weapon_hand, orbit, grapple, grips_ready, settings,
                                       snapshot.interaction_profiles, pad);
    } else {
        ApplyClassic(left, right, weapon_hand, morph_ball, settings, snapshot.interaction_profiles, pad);
    }
    // The beam wheel owns the weapon hand's B while it is held; the orbit lock
    // keeps its L (the game does the locking) and the pad's C-stick is free.
    const bool beamHeld = settings.custom_layout_enabled
        ? ControllerLayout::Pressed(snapshot.controllers, settings.custom_beam) : weapon.secondary;
    UpdateWeaponWheel(weapon, gameplay && beamHeld, gameplay, pad);
    pad.button |= DpadButton(visor.held);
    if (settings.custom_layout_enabled) {
        const auto bound = [&](int binding) { return ControllerLayout::Pressed(snapshot.controllers, binding); };
        if (bound(settings.custom_visor_up)) pad.button |= PAD_BUTTON_UP;
        if (bound(settings.custom_visor_down)) pad.button |= PAD_BUTTON_DOWN;
        if (bound(settings.custom_visor_left)) pad.button |= PAD_BUTTON_LEFT;
        if (bound(settings.custom_visor_right)) pad.button |= PAD_BUTTON_RIGHT;
    }
    PADSetVirtualExclusive(0, settings.custom_layout_enabled);
    PADSetVirtualStatus(0, &pad);

    std::lock_guard lock(g_mutex);
    g_state.active = true;
    g_state.gameplay = gameplay;
    g_state.orbit_lock = orbit;
    g_state.smooth_turn_speed_scale = turnSpeedScale;
    g_state.weapon_panel = g_wheel.open;
    g_state.weapon_selected = static_cast<int>(g_wheel.selected);
    g_state.weapon_hand = weapon_hand;
    g_state.visor_zone = visor.zone;
    g_state.visor_direction = static_cast<int>(visor.direction);
    g_state.move_owned = g_move.owns;
    g_state.move_moving = g_move.wish.moving;
    g_state.move_heading_degrees =
        g_move.owns ? std::atan2(g_move.heading[0], g_move.heading[1]) * (180.0f / 3.14159265358979f) : 0.0f;
}

void VrNoteInGameMenu(bool open) noexcept { g_in_game_menu = open; }

float VrTakeSnapTurn() noexcept {
    const float turn = g_snap_pending;
    g_snap_pending = 0.0f;
    return turn;
}

VrMove VrDirectionalMove(float flatSpeed, bool onGround, float dt, uint32_t tick) noexcept {
    // The ramp carries on from the tick before, or within a tick (a grapple's
    // jump-off computes the movement twice); after any gap it starts again
    // from the speed she has, as PrimedGun's did whenever it stood down.
    if (!g_move_ramp_ticked || (tick != g_move_ramp_tick && tick != g_move_ramp_tick + 1)) {
        g_move_ramp.Reset();
    }
    g_move_ramp_ticked = true;
    g_move_ramp_tick = tick;

    VrMove move;
    move.owns = g_move.owns;
    if (!g_move.owns || !g_move.wish.moving) {
        g_move_ramp.Reset();
        return move;
    }
    const float speed = g_move_ramp.Step(flatSpeed, g_move.speed * g_move.wish.push,
                                         onGround ? g_move.accel : g_move.air_accel, dt);
    move.moving = true;
    move.right = g_move.wish.right * speed;
    move.forward = g_move.wish.forward * speed;
    return move;
}

VrPadState GetVrPadState() noexcept {
    std::lock_guard lock(g_mutex);
    return g_state;
}

// The motor claim is the port claim: VrPadUpdate sets it when the controllers
// take port 0 and clears it with PADClearVirtualStatus when they let go.
bool VrCustomLayoutActive() noexcept { return g_custom_layout.load(std::memory_order_relaxed); }

bool VrPadOwnsPort0() noexcept { return g_owns_motor.load(std::memory_order_relaxed); }

} // namespace PortVr
