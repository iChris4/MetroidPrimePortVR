// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if defined(MP_ENABLE_OPENXR)

#include "vr/openxr_controller_snapshot.h"
#include "vr/openxr_hand_inputs.h"
#include "vr/openxr_runtime.h"
#include "vr/openxr_screen_math.h"
#include "vr/openxr_settings_panel.h"
#include "vr/vr_settings.h"

#include <array>
#include <cstdint>
#include <string>

namespace PortVr {

// The rectangle the game's picture occupies on whichever virtual screen is
// showing it, in the application reference space. The pose faces +Z with +X
// right and +Y up across the picture. Invalid when no screen can be pointed at.
struct OpenXRPointerScreen {
    bool valid = false;
    XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    float half_width_meters = 0.0f;
    float half_height_meters = 0.0f;
};

// Where PrimedGun's VR menu and its pointer are this frame, for the menu's
// quad layers (openxr_backend.h OpenXRPanelLayer).
struct OpenXRMenuPlacement {
    bool placed = false;
    XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    float width_meters = 0.0f;
    float height_meters = 0.0f;
    bool laser = false;
    XrPosef laser_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    float laser_length_meters = 0.0f;
    bool dot = false;
    XrPosef dot_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
};

// What the controllers are to the game, from the VR settings.
OpenXRControllerMode OpenXRGetControllerMode() noexcept;

// OpenXR action-based controller input.
//
// This module syncs one action set on the XR pacing thread every frame and
// publishes what it read in two forms:
//
// PrimedGun (default): an OpenXRInputSnapshot (openxr_controller_snapshot.h):
// every button and axis of both controllers, their aim and grip poses located
// at the measured current time, the grip's velocities, the head pose, and the
// interaction profiles. The game thread turns it into Metroid Prime's pad and
// the 6DOF arm cannon (vr_pad.cpp and the PortVr game hooks); nothing is
// written to SDL in this mode.
//
// Gamepad: the same controllers as one ordinary SDL gamepad, through a virtual
// joystick (SDL_AttachVirtualJoystick) that Aurora's controller code assigns to
// a port like a physical pad, so every binding in the port's controller tab
// applies: right A / B -> South / East, left X / Y -> West / North, index
// triggers -> trigger axes, grips -> shoulders, thumbsticks -> sticks (clicks ->
// stick buttons), left menu -> Start. The snapshot is published too.
//
// None: nothing to the game (the virtual joystick stays unplugged and the
// snapshot reads idle). The controllers still open the settings panel.
//
// Bindings are suggested for the Oculus Touch, Valve Index, Windows Mixed
// Reality and HTC Vive profiles, and khr/simple_controller so an unknown
// runtime still offers a select, a menu and something to point with.
//
// PrimedGun's VR menu (openxr_settings_panel.h, vr_menu.h): the off hand's
// thumbstick click or menu button opens and closes it (both thumbsticks clicked
// together as a gamepad). It hangs on the off hand, or floats ahead when
// detached, and the cannon hand's laser points at it. While it is open, and
// until every button has been released after it closes, the game sees idle
// controllers: the laser and the triggers belong to the menu.
//
// Haptics: the game's rumble request (OpenXRSetRumble, from the PrimedGun pad's
// port 0-2 motors or the virtual gamepad's rumble) is applied every frame to the
// hand(s) the settings name, scaled by the rumble intensity; the split target
// puts the arm cannon's on the gun hand and the grapple's on the off hand.
//
// Lifetime: Create after the session exists (attaches the action set, which
// OpenXR permits once per session), Sync once per xrWaitFrame, Idle while the
// session is not running, Destroy before the session is destroyed. All of them
// run on the XR pacing thread; OpenXRApplyVirtualGamepad() performs the SDL
// writes on the game thread.
class OpenXRInput final {
public:
    explicit OpenXRInput(OpenXRLogCallback logger = {});
    ~OpenXRInput();

    OpenXRInput(const OpenXRInput&) = delete;
    OpenXRInput& operator=(const OpenXRInput&) = delete;

    // Creates the action set/actions/spaces and attaches them to the session.
    // Returns false (with LastError set) if the runtime rejects the action set;
    // the caller continues without controller input rather than failing VR.
    bool Create(OpenXRRuntime& runtime);
    void Destroy();

    // xrSyncActions + state reads, then publishes the snapshot (and the virtual
    // gamepad in Gamepad mode). predicted_display_time is the frame's XrTime;
    // screen is where the game picture is this frame (kept for a future menu
    // pointer). panel_available says whether the backend can show the VR menu;
    // without it the menu never opens.
    void Sync(XrTime predicted_display_time, const OpenXRPointerScreen& screen, bool panel_available);

    // Where the last Sync placed the VR menu and its pointer.
    const OpenXRMenuPlacement& MenuPlacement() const noexcept { return m_menu_placement; }

    // Publishes an idle snapshot and stops the haptics, for frames without
    // focused input.
    void Idle();

    // Rumble for the given hand (0 = left, 1 = right); amplitude 0..1.
    void ApplyHaptic(uint32_t hand, float amplitude, XrDuration duration);

    bool IsCreated() const noexcept { return m_created; }
    const std::string& LastError() const noexcept { return m_last_error; }

private:
    static constexpr uint32_t kHands = 2;

    bool CreateActions();
    bool SuggestBindings();
    void CreatePoseSpaces();
    void DestroyPoseSpaces();
    void LoadInputClock();
    XrTime InputSampleTime(XrTime predicted_display_time) const;
    // Locates `space` in the application space at `time`; false unless both
    // its position and orientation are valid.
    bool Locate(XrSpace space, XrTime time, screen_math::Pose& pose) const;
    // Places the VR menu, aims the pointer hand's laser at it and publishes the
    // pointer for the game thread.
    void PlaceMenu(XrTime input_time, const settings_panel::Frame& frame, const PortVrSettings& settings);
    // The snapshot for the game thread: `hands` as the game may see them (idle
    // while withheld), poses located at input_time.
    void PublishSnapshot(XrTime input_time, const std::array<HandInputs, kHands>& hands,
                         const std::array<bool, kHands>& connected, bool withheld);
    void LogInteractionProfiles();
    void UpdateRumble();
    void StopRumble();
    bool Check(XrResult result, const char* operation);
    void Log(OpenXRLogLevel level, const std::string& message) const noexcept;

    OpenXRLogCallback m_logger;
    OpenXRRuntime* m_runtime = nullptr;
    XrActionSet m_action_set = XR_NULL_HANDLE;
    XrAction m_thumbstick = XR_NULL_HANDLE;
    XrAction m_thumbstick_click = XR_NULL_HANDLE;
    XrAction m_thumbstick_touch = XR_NULL_HANDLE;
    XrAction m_trigger = XR_NULL_HANDLE;
    XrAction m_trigger_click = XR_NULL_HANDLE;
    XrAction m_squeeze = XR_NULL_HANDLE;
    XrAction m_squeeze_click = XR_NULL_HANDLE;
    XrAction m_squeeze_force = XR_NULL_HANDLE;
    XrAction m_trackpad_click = XR_NULL_HANDLE;
    XrAction m_trackpad_force = XR_NULL_HANDLE;
    XrAction m_button_primary = XR_NULL_HANDLE;   // A / X
    XrAction m_button_secondary = XR_NULL_HANDLE; // B / Y
    XrAction m_dpad_up = XR_NULL_HANDLE, m_dpad_down = XR_NULL_HANDLE;
    XrAction m_dpad_left = XR_NULL_HANDLE, m_dpad_right = XR_NULL_HANDLE;
    XrAction m_shoulder = XR_NULL_HANDLE;
    XrAction m_button_tertiary = XR_NULL_HANDLE, m_button_quaternary = XR_NULL_HANDLE;
    XrAction m_menu = XR_NULL_HANDLE;
    XrAction m_aim_pose = XR_NULL_HANDLE;
    XrAction m_grip_pose = XR_NULL_HANDLE;
    XrAction m_haptic = XR_NULL_HANDLE;
    XrPath m_hand_paths[kHands]{};
    XrSpace m_aim_spaces[kHands]{};
    XrSpace m_grip_spaces[kHands]{};
    // xrConvertWin32PerformanceCounterToTimeKHR / xrConvertTimespecTimeToTimeKHR,
    // when the runtime offers them; the input time falls back to display time.
    PFN_xrVoidFunction m_convert_now_to_xr_time = nullptr;
    settings_panel::Controls m_panel_controls;
    XrTime m_last_input_time = 0;
    OpenXRMenuPlacement m_menu_placement{};
    // "DETACH VR MENU FROM HAND": where the menu was latched when it opened.
    bool m_floating_valid = false;
    screen_math::Pose m_floating_pose{};
    bool m_haptics_active[kHands]{};
    uint64_t m_profile_serial = 0;
    std::array<std::string, kHands> m_profile_names{};
    uint64_t m_frame_serial = 0;
    // The profile-specific extras (touches, clicks, forces) read this frame,
    // folded into the snapshot by PublishSnapshot.
    std::array<OpenXRControllerState, kHands> m_pending_extra{};

    bool m_created = false;
    bool m_logged_sync_failure = false;
    std::string m_last_error;
};

// Game thread: plugs the virtual gamepad in or out as the controller mode asks,
// then writes the gamepad the pacing thread last published, if any. Does
// nothing when no OpenXR controllers are attached.
void OpenXRApplyVirtualGamepad() noexcept;

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR)
