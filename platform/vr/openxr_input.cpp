// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MP_ENABLE_OPENXR)

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "vr/openxr_input.h"

#include "vr/openxr_controller_snapshot.h"
#include "vr/openxr_diagnostics.h"
#include "vr/openxr_screen_math.h"
#include "vr/vr_menu.h"
#include "vr/vr_settings.h"
#include "vr/vr_visor_dpad.h"
#include "vr/vr_controller_layout.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <time.h>
#endif

namespace PortVr {
namespace {

// SDL holds its joystick lock for as long as an enumeration takes, and a device
// rescan closes and reopens every HID device: 15 ms on a plain desk, over
// 200 ms on a machine carrying several HID devices, such as a Lighthouse
// setup's base-station dongles. The pacing thread must never wait on that,
// because the OpenXR frame it holds open costs the compositor every display
// slot that passes. So it leaves the gamepad here, and the game thread writes
// it to SDL where it already polls controllers.
//
// The relay also owns the virtual joystick. Attaching and detaching it take the
// same lock, so a live switch of the controller mode is followed on the game
// thread too; only the input's creation and destruction plug it in or out
// where they run. The joystick exists only in Gamepad mode.
class VirtualGamepadRelay {
public:
    struct Pad {
        std::array<int16_t, SDL_GAMEPAD_AXIS_COUNT> axes{};
        std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    };

    void Open(OpenXRLogCallback logger) {
        std::scoped_lock lock(m_sdl, m_state);
        m_logger = std::move(logger);
        m_open = true;
        m_attach_failed = false;
        m_pending = false;
        FollowControllerMode();
    }

    void Close() {
        std::scoped_lock lock(m_sdl, m_state);
        m_open = false;
        Detach();
        m_pending = false;
        m_logger = {};
    }

    uint32_t JoystickId() const noexcept { return m_id.load(std::memory_order_relaxed); }

    // Pacing thread.
    void Publish(const Pad& pad) {
        std::lock_guard lock(m_state);
        m_pad = pad;
        m_pending = true;
    }

    // Game thread. Holding m_sdl here is what keeps Close from closing the
    // joystick underneath the writes.
    void Apply() {
        std::lock_guard sdl(m_sdl);
        FollowControllerMode();
        Pad pad;
        {
            std::lock_guard lock(m_state);
            if (!m_pending || m_joystick == nullptr) {
                return;
            }
            pad = m_pad;
            m_pending = false;
        }
        for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
            SDL_SetJoystickVirtualAxis(m_joystick, static_cast<SDL_GamepadAxis>(axis),
                                       pad.axes[static_cast<size_t>(axis)]);
        }
        for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
            SDL_SetJoystickVirtualButton(m_joystick, static_cast<SDL_GamepadButton>(button),
                                         pad.buttons[static_cast<size_t>(button)]);
        }
    }

private:
    // m_sdl held. A joystick SDL refused is not asked for again until the mode
    // unplugs it or the input is created anew.
    void FollowControllerMode() {
        if (!m_open) {
            return;
        }
        if (OpenXRGetControllerMode() != OpenXRControllerMode::Gamepad) {
            m_attach_failed = false;
            Detach();
        } else if (m_joystick == nullptr && !m_attach_failed) {
            m_attach_failed = !Attach();
        }
    }

    bool Attach() {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                           (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH) |
                           (1u << SDL_GAMEPAD_BUTTON_START) | (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) |
                           (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK) |
                           (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) |
                           (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY) |
                         (1u << SDL_GAMEPAD_AXIS_RIGHTX) | (1u << SDL_GAMEPAD_AXIS_RIGHTY) |
                         (1u << SDL_GAMEPAD_AXIS_LEFT_TRIGGER) | (1u << SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        desc.name = "OpenXR Controllers";
        desc.Rumble = Rumble;
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        if (id == 0) {
            Refused("SDL_AttachVirtualJoystick");
            return false;
        }
        SDL_Joystick* joystick = SDL_OpenJoystick(id);
        if (joystick == nullptr) {
            Refused("SDL_OpenJoystick");
            SDL_DetachVirtualJoystick(id);
            return false;
        }
        m_joystick = joystick;
        m_id.store(id, std::memory_order_relaxed);
        return true;
    }

    void Detach() {
        const SDL_JoystickID id = m_id.exchange(0, std::memory_order_relaxed);
        if (m_joystick != nullptr) {
            SDL_CloseJoystick(m_joystick);
            m_joystick = nullptr;
        }
        if (id != 0) {
            SDL_DetachVirtualJoystick(id);
            // A rumble left on when the pad went away would otherwise keep playing.
            OpenXRSetRumble(0.0f);
        }
    }

    // The game's rumble on whichever port this pad was given, at the strength
    // that port's controller tab sets, for the pacing thread to play on the
    // controllers. SDL calls it from the thread that rumbles the pad.
    static bool SDLCALL Rumble(void*, Uint16 low_frequency, Uint16 high_frequency) {
        OpenXRSetRumble(static_cast<float>(std::max(low_frequency, high_frequency)) / 65535.0f);
        return true;
    }

    void Refused(const char* operation) const {
        if (!m_logger) {
            return;
        }
        try {
            m_logger(OpenXRLogLevel::Warning, std::string(operation) + " failed: " + SDL_GetError() +
                                                  "; OpenXR controllers will not reach the game as a gamepad");
        } catch (...) {
        }
    }

    std::mutex m_sdl;
    std::mutex m_state;
    OpenXRLogCallback m_logger;
    bool m_open = false;
    bool m_attach_failed = false;
    SDL_Joystick* m_joystick = nullptr;
    std::atomic<uint32_t> m_id{0};
    Pad m_pad;
    bool m_pending = false;
};

VirtualGamepadRelay& Relay() {
    static VirtualGamepadRelay relay;
    return relay;
}

#if defined(__ANDROID__)
// Debug-only button presses for headset experiments driven over adb, so a menu
// can be reached without someone wearing the headset:
//   adb shell setprop debug.primedgun.inject <sequence>:<button>
// A new sequence number holds the button for kInjectHoldFrames XR frames.
// Buttons: a, b, x, y, start, up, down, left, right, and `panel` for the
// settings panel's button (opening or closing it, where `a` then selects).
constexpr uint32_t kInjectHoldFrames = 12;
constexpr uint32_t kInjectPollFrames = 4;

struct InjectedPress {
    long sequence = -1;
    std::string button;
    uint32_t frames_left = 0;
    uint32_t poll_countdown = 0;
};

InjectedPress& Injection() {
    static InjectedPress press;
    return press;
}

void PollInjection() {
    InjectedPress& press = Injection();
    if (press.frames_left > 0) {
        --press.frames_left;
    }
    if (press.poll_countdown > 0) {
        --press.poll_countdown;
        return;
    }
    press.poll_countdown = kInjectPollFrames;
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.primedgun.inject", value) <= 0) {
        return;
    }
    char* end = nullptr;
    const long sequence = std::strtol(value, &end, 10);
    if (end == value || *end != ':' || sequence == press.sequence) {
        return;
    }
    const bool first_read = press.sequence < 0;
    press.sequence = sequence;
    if (first_read) {
        return; // A value left over from an earlier run is not a new press.
    }
    press.button = end + 1;
    press.frames_left = kInjectHoldFrames;
}

bool Injected(const char* button) {
    const InjectedPress& press = Injection();
    return press.frames_left > 0 && press.button == button;
}

using ConvertNowToXrTime = XrResult(XRAPI_PTR*)(XrInstance, const struct timespec*, XrTime*);
#else
void PollInjection() {}
bool Injected(const char*) { return false; }

#if defined(_WIN32)
using ConvertNowToXrTime = XrResult(XRAPI_PTR*)(XrInstance, const LARGE_INTEGER*, XrTime*);
#endif
#endif

constexpr uint32_t kHandCount = 2;

// Re-sent every frame while the game holds the motor on, so a rumble whose stop
// never arrives (or a stalled pacing thread) dies out on its own.
constexpr XrDuration kRumblePulseNs = 50'000'000;

struct Binding {
    XrAction* action;
    const char* path;
};

Sint16 ToAxis(float value) noexcept {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<Sint16>(std::lround(clamped * 32767.0f));
}

// Trigger axes are reported by SDL gamepads on the positive half only.
Sint16 ToTrigger(float value) noexcept {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<Sint16>(std::lround(clamped * 32767.0f));
}

screen_math::Pose ToPose(const XrPosef& pose) noexcept {
    return {{pose.position.x, pose.position.y, pose.position.z},
            {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}};
}

XrPosef ToXrPose(const screen_math::Pose& pose) noexcept {
    XrPosef xr{};
    xr.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2], pose.orientation[3]};
    xr.position = {pose.position[0], pose.position[1], pose.position[2]};
    return xr;
}

OpenXRPoseState ToPoseState(const XrPosef& pose, bool valid) noexcept {
    OpenXRPoseState state{};
    state.valid = valid;
    state.position = {pose.position.x, pose.position.y, pose.position.z};
    state.orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
    return state;
}

constexpr XrSpaceLocationFlags kPoseValidFlags =
    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

} // namespace

OpenXRControllerMode OpenXRGetControllerMode() noexcept {
    const auto settings = GetVrSettings();
    if (settings.custom_layout_enabled) return OpenXRControllerMode::PrimedGun;
    switch (settings.controller_mode) {
    case ControllerModeSetting::Gamepad:
        return OpenXRControllerMode::Gamepad;
    case ControllerModeSetting::None:
        return OpenXRControllerMode::None;
    case ControllerModeSetting::PrimedGun:
        break;
    }
    return OpenXRControllerMode::PrimedGun;
}

OpenXRInput::OpenXRInput(OpenXRLogCallback logger) : m_logger(std::move(logger)) {}

OpenXRInput::~OpenXRInput() {
    Destroy();
}

bool OpenXRInput::Create(OpenXRRuntime& runtime) {
    m_last_error.clear();
    if (m_created) {
        return true;
    }
    if (!runtime.IsInitialized() || !runtime.HasSession()) {
        m_last_error = "OpenXR input needs an initialized runtime with a session";
        return false;
    }
    m_runtime = &runtime;

    if (!Check(xrStringToPath(runtime.Instance(), "/user/hand/left", &m_hand_paths[0]),
               "xrStringToPath(/user/hand/left)") ||
        !Check(xrStringToPath(runtime.Instance(), "/user/hand/right", &m_hand_paths[1]),
               "xrStringToPath(/user/hand/right)")) {
        Destroy();
        return false;
    }
    if (!CreateActions() || !SuggestBindings()) {
        Destroy();
        return false;
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &m_action_set;
    if (!Check(xrAttachSessionActionSets(runtime.Session(), &attach), "xrAttachSessionActionSets")) {
        Destroy();
        return false;
    }
    m_created = true;
    CreatePoseSpaces();
    LoadInputClock();
    Relay().Open(m_logger);
    switch (OpenXRGetControllerMode()) {
    case OpenXRControllerMode::PrimedGun:
        Log(OpenXRLogLevel::Info, "OpenXR controller actions attached (PrimedGun controls)");
        break;
    case OpenXRControllerMode::Gamepad:
        Log(OpenXRLogLevel::Info, "OpenXR controller actions attached (gamepad)");
        break;
    case OpenXRControllerMode::None:
        Log(OpenXRLogLevel::Info, "OpenXR controller actions attached (none: the game does not see them)");
        break;
    }
    return true;
}

bool OpenXRInput::CreateActions() {
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(set_info.actionSetName, "primedgun_gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(set_info.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    set_info.priority = 0;
    if (!Check(xrCreateActionSet(m_runtime->Instance(), &set_info, &m_action_set), "xrCreateActionSet")) {
        return false;
    }

    struct Spec {
        XrAction* action;
        const char* name;
        const char* localized;
        XrActionType type;
    };
    const std::array<Spec, 23> specs{{
        {&m_dpad_up, "dpad_up", "D-pad Up", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_dpad_down, "dpad_down", "D-pad Down", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_dpad_left, "dpad_left", "D-pad Left", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_dpad_right, "dpad_right", "D-pad Right", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_shoulder, "shoulder", "Shoulder", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_button_tertiary, "button_tertiary", "X Button", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_button_quaternary, "button_quaternary", "Y Button", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_thumbstick, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT},
        {&m_thumbstick_click, "thumbstick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_thumbstick_touch, "thumbstick_touch", "Thumbstick Touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT},
        {&m_trigger_click, "trigger_click", "Trigger Click", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_squeeze, "squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT},
        {&m_squeeze_click, "squeeze_click", "Grip Click", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_squeeze_force, "squeeze_force", "Grip Force", XR_ACTION_TYPE_FLOAT_INPUT},
        {&m_trackpad_click, "trackpad_click", "Trackpad Click", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_trackpad_force, "trackpad_force", "Trackpad Force", XR_ACTION_TYPE_FLOAT_INPUT},
        {&m_button_primary, "button_primary", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_button_secondary, "button_secondary", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_menu, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_aim_pose, "aim_pose", "Pointer", XR_ACTION_TYPE_POSE_INPUT},
        {&m_grip_pose, "grip_pose", "Motion", XR_ACTION_TYPE_POSE_INPUT},
        {&m_haptic, "haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT},
    }};
    for (const Spec& spec : specs) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = spec.type;
        std::strncpy(info.actionName, spec.name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, spec.localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.countSubactionPaths = kHandCount;
        info.subactionPaths = m_hand_paths;
        if (!Check(xrCreateAction(m_action_set, &info, spec.action), spec.name)) {
            return false;
        }
    }
    return true;
}

bool OpenXRInput::SuggestBindings() {
    const auto suggest = [&](const char* profile, const std::vector<Binding>& bindings, bool required) {
        XrPath profile_path = XR_NULL_PATH;
        if (!Check(xrStringToPath(m_runtime->Instance(), profile, &profile_path), profile)) {
            return false;
        }
        std::vector<XrActionSuggestedBinding> suggested;
        suggested.reserve(bindings.size());
        for (const Binding& binding : bindings) {
            XrPath path = XR_NULL_PATH;
            if (XR_FAILED(xrStringToPath(m_runtime->Instance(), binding.path, &path))) {
                continue;
            }
            suggested.push_back({*binding.action, path});
        }
        XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile = profile_path;
        info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
        info.suggestedBindings = suggested.data();
        const XrResult result = xrSuggestInteractionProfileBindings(m_runtime->Instance(), &info);
        m_runtime->ObserveResult(result);
        if (XR_FAILED(result)) {
            std::ostringstream message;
            message << "xrSuggestInteractionProfileBindings(" << profile << ") failed (" << result << ')';
            if (required) {
                m_last_error = message.str();
                Log(OpenXRLogLevel::Error, m_last_error);
                return false;
            }
            Log(OpenXRLogLevel::Warning, message.str());
        }
        return true;
    };

    // The poses and haptics every profile has.
    const auto common = [&](std::vector<Binding>& bindings) {
        bindings.push_back({&m_aim_pose, "/user/hand/left/input/aim/pose"});
        bindings.push_back({&m_aim_pose, "/user/hand/right/input/aim/pose"});
        bindings.push_back({&m_grip_pose, "/user/hand/left/input/grip/pose"});
        bindings.push_back({&m_grip_pose, "/user/hand/right/input/grip/pose"});
        bindings.push_back({&m_haptic, "/user/hand/left/output/haptic"});
        bindings.push_back({&m_haptic, "/user/hand/right/output/haptic"});
    };

    // Meta Quest Touch controllers (Quest 2 / 3 / Pro all expose this profile).
    std::vector<Binding> touch{
        {&m_thumbstick, "/user/hand/left/input/thumbstick"},
        {&m_thumbstick, "/user/hand/right/input/thumbstick"},
        {&m_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
        {&m_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
        {&m_thumbstick_touch, "/user/hand/left/input/thumbstick/touch"},
        {&m_thumbstick_touch, "/user/hand/right/input/thumbstick/touch"},
        {&m_trigger, "/user/hand/left/input/trigger/value"},
        {&m_trigger, "/user/hand/right/input/trigger/value"},
        {&m_squeeze, "/user/hand/left/input/squeeze/value"},
        {&m_squeeze, "/user/hand/right/input/squeeze/value"},
        {&m_button_primary, "/user/hand/left/input/x/click"},
        {&m_button_primary, "/user/hand/right/input/a/click"},
        {&m_button_secondary, "/user/hand/left/input/y/click"},
        {&m_button_secondary, "/user/hand/right/input/b/click"},
        {&m_menu, "/user/hand/left/input/menu/click"},
    };
    common(touch);
    if (!suggest("/interaction_profiles/oculus/touch_controller", touch, false)) {
        return false;
    }

    // Optional native PICO profiles share the same button/axis paths as Touch.
    // A runtime need not emulate the Oculus profile to use these controllers.
    suggest("/interaction_profiles/bytedance/pico4_controller", touch, false);
    suggest("/interaction_profiles/bytedance/pico_neo3_controller", touch, false);

    // Valve Index: grips and trackpads report force; A / B on both hands.
    std::vector<Binding> index{
        {&m_thumbstick, "/user/hand/left/input/thumbstick"},
        {&m_thumbstick, "/user/hand/right/input/thumbstick"},
        {&m_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
        {&m_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
        {&m_thumbstick_touch, "/user/hand/left/input/thumbstick/touch"},
        {&m_thumbstick_touch, "/user/hand/right/input/thumbstick/touch"},
        {&m_trigger, "/user/hand/left/input/trigger/value"},
        {&m_trigger, "/user/hand/right/input/trigger/value"},
        {&m_trigger_click, "/user/hand/left/input/trigger/click"},
        {&m_trigger_click, "/user/hand/right/input/trigger/click"},
        {&m_squeeze, "/user/hand/left/input/squeeze/value"},
        {&m_squeeze, "/user/hand/right/input/squeeze/value"},
        {&m_squeeze_force, "/user/hand/left/input/squeeze/force"},
        {&m_squeeze_force, "/user/hand/right/input/squeeze/force"},
        {&m_trackpad_force, "/user/hand/left/input/trackpad/force"},
        {&m_trackpad_force, "/user/hand/right/input/trackpad/force"},
        {&m_trackpad_click, "/user/hand/left/input/trackpad/touch"},
        {&m_trackpad_click, "/user/hand/right/input/trackpad/touch"},
        {&m_button_primary, "/user/hand/left/input/a/click"},
        {&m_button_primary, "/user/hand/right/input/a/click"},
        {&m_button_secondary, "/user/hand/left/input/b/click"},
        {&m_button_secondary, "/user/hand/right/input/b/click"},
    };
    common(index);
    suggest("/interaction_profiles/valve/index_controller", index, false);

    // Windows Mixed Reality: a thumbstick, a trackpad, a menu and a grip click.
    std::vector<Binding> wmr{
        {&m_thumbstick, "/user/hand/left/input/thumbstick"},
        {&m_thumbstick, "/user/hand/right/input/thumbstick"},
        {&m_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
        {&m_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
        {&m_trigger, "/user/hand/left/input/trigger/value"},
        {&m_trigger, "/user/hand/right/input/trigger/value"},
        {&m_squeeze_click, "/user/hand/left/input/squeeze/click"},
        {&m_squeeze_click, "/user/hand/right/input/squeeze/click"},
        {&m_trackpad_click, "/user/hand/left/input/trackpad/click"},
        {&m_trackpad_click, "/user/hand/right/input/trackpad/click"},
        {&m_menu, "/user/hand/left/input/menu/click"},
        {&m_menu, "/user/hand/right/input/menu/click"},
    };
    common(wmr);
    suggest("/interaction_profiles/microsoft/motion_controller", wmr, false);

    // HTC Vive wands: the trackpad stands in for the thumbstick.
    std::vector<Binding> vive{
        {&m_thumbstick, "/user/hand/left/input/trackpad"},
        {&m_thumbstick, "/user/hand/right/input/trackpad"},
        {&m_thumbstick_click, "/user/hand/left/input/trackpad/click"},
        {&m_thumbstick_click, "/user/hand/right/input/trackpad/click"},
        {&m_thumbstick_touch, "/user/hand/left/input/trackpad/touch"},
        {&m_thumbstick_touch, "/user/hand/right/input/trackpad/touch"},
        {&m_trigger, "/user/hand/left/input/trigger/value"},
        {&m_trigger, "/user/hand/right/input/trigger/value"},
        {&m_trigger_click, "/user/hand/left/input/trigger/click"},
        {&m_trigger_click, "/user/hand/right/input/trigger/click"},
        {&m_squeeze_click, "/user/hand/left/input/squeeze/click"},
        {&m_squeeze_click, "/user/hand/right/input/squeeze/click"},
        {&m_menu, "/user/hand/left/input/menu/click"},
        {&m_menu, "/user/hand/right/input/menu/click"},
    };
    common(vive);
    suggest("/interaction_profiles/htc/vive_controller", vive, false);

    // Native Steam Frame profile: retain the real D-pad and all four right face buttons.
    if (std::find(m_runtime->EnabledExtensions().begin(), m_runtime->EnabledExtensions().end(),
                  "XR_VALVE_frame_controller_interaction") != m_runtime->EnabledExtensions().end()) {
        std::vector<Binding> frame;
        common(frame);
        std::vector<std::string> framePaths;
        framePaths.reserve(32);
        const auto add = [&](XrAction* action, const std::string& path) {
            framePaths.push_back(path);
            frame.push_back({action, framePaths.back().c_str()});
        };
        for (const char* hand : {"left", "right"}) {
            const std::string base = std::string("/user/hand/") + hand + "/input/";
            add(&m_thumbstick, base + "thumbstick");
            add(&m_thumbstick_click, base + "thumbstick/click");
            add(&m_trigger, base + "trigger/value");
            add(&m_squeeze, base + "squeeze/value");
            add(&m_shoulder, base + "shoulder/click");
        }
        add(&m_menu, "/user/hand/left/input/view/click");
        add(&m_menu, "/user/hand/right/input/menu/click");
        add(&m_dpad_up, "/user/hand/left/input/dpad_up/click");
        add(&m_dpad_down, "/user/hand/left/input/dpad_down/click");
        add(&m_dpad_left, "/user/hand/left/input/dpad_left/click");
        add(&m_dpad_right, "/user/hand/left/input/dpad_right/click");
        add(&m_button_primary, "/user/hand/left/input/dpad_down/click");
        add(&m_button_secondary, "/user/hand/left/input/dpad_up/click");
        add(&m_button_primary, "/user/hand/right/input/a/click");
        add(&m_button_secondary, "/user/hand/right/input/b/click");
        add(&m_button_tertiary, "/user/hand/right/input/x/click");
        add(&m_button_quaternary, "/user/hand/right/input/y/click");
        suggest("/interaction_profiles/valve/frame_controller_valve", frame, false);
    }

    // Minimal fallback so an unfamiliar runtime still offers a select, a menu
    // and something to point with.
    std::vector<Binding> simple{
        {&m_button_primary, "/user/hand/right/input/select/click"},
        {&m_button_secondary, "/user/hand/left/input/select/click"},
        {&m_menu, "/user/hand/left/input/menu/click"},
    };
    common(simple);
    suggest("/interaction_profiles/khr/simple_controller", simple, false);
    return true;
}

void OpenXRInput::CreatePoseSpaces() {
    bool logged = false;
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        for (auto [action, spaces] : {std::pair{m_aim_pose, m_aim_spaces}, std::pair{m_grip_pose, m_grip_spaces}}) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = action;
            info.subactionPath = m_hand_paths[hand];
            info.poseInActionSpace.orientation.w = 1.0f;
            const XrResult result = xrCreateActionSpace(m_runtime->Session(), &info, &spaces[hand]);
            m_runtime->ObserveResult(result);
            if (XR_FAILED(result)) {
                spaces[hand] = XR_NULL_HANDLE;
                if (!logged) {
                    logged = true;
                    std::ostringstream message;
                    message << "xrCreateActionSpace failed (" << result
                            << "); the controllers will have no tracked poses";
                    Log(OpenXRLogLevel::Warning, message.str());
                }
            }
        }
    }
}

void OpenXRInput::DestroyPoseSpaces() {
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        for (XrSpace* space : {&m_aim_spaces[hand], &m_grip_spaces[hand]}) {
            if (*space != XR_NULL_HANDLE) {
                xrDestroySpace(*space);
                *space = XR_NULL_HANDLE;
            }
        }
    }
}

// Poses for input are located at the measured current time, not the frame's
// predicted display time: that lies tens of milliseconds ahead, and the runtime
// extrapolates a fast wrist turn that far past where the hand really is, which
// sprays the pointer and invents acceleration (DolphinXR's fast-motion fix).
void OpenXRInput::LoadInputClock() {
    const auto& extensions = m_runtime->EnabledExtensions();
    const auto enabled = [&](const char* name) {
        return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
    };
    PFN_xrVoidFunction function = nullptr;
#if defined(_WIN32)
    if (enabled("XR_KHR_win32_convert_performance_counter_time")) {
        m_runtime->GetInstanceProcAddress("xrConvertWin32PerformanceCounterToTimeKHR", &function);
    }
#elif defined(__ANDROID__)
    if (enabled("XR_KHR_convert_timespec_time")) {
        m_runtime->GetInstanceProcAddress("xrConvertTimespecTimeToTimeKHR", &function);
    }
#else
    (void)enabled;
#endif
    m_convert_now_to_xr_time = function;
    if (m_convert_now_to_xr_time == nullptr) {
        Log(OpenXRLogLevel::Info,
            "OpenXR offers no clock conversion; controller motion is sampled at display time");
    }
}

XrTime OpenXRInput::InputSampleTime(XrTime predicted_display_time) const {
    if (m_convert_now_to_xr_time == nullptr) {
        return predicted_display_time;
    }
    XrTime now = 0;
#if defined(_WIN32)
    LARGE_INTEGER counter{};
    if (QueryPerformanceCounter(&counter) == 0 ||
        XR_FAILED(reinterpret_cast<ConvertNowToXrTime>(m_convert_now_to_xr_time)(m_runtime->Instance(),
                                                                                  &counter, &now))) {
        return predicted_display_time;
    }
#elif defined(__ANDROID__)
    timespec spec{};
    if (clock_gettime(CLOCK_MONOTONIC, &spec) != 0 ||
        XR_FAILED(reinterpret_cast<ConvertNowToXrTime>(m_convert_now_to_xr_time)(m_runtime->Instance(),
                                                                                  &spec, &now))) {
        return predicted_display_time;
    }
#endif
    return now > 0 ? (std::min)(predicted_display_time, now) : predicted_display_time;
}

// Which interaction profile each hand moved to, whenever the runtime reports a
// change (a controller picked up or put down, a bare hand on the Quest).
void OpenXRInput::LogInteractionProfiles() {
    std::ostringstream message;
    message << "OpenXR interaction profiles:";
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        message << (hand == 0 ? " left " : ", right ");
        XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
        char path[XR_MAX_PATH_LENGTH]{};
        uint32_t length = 0;
        m_profile_names[hand].clear();
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(m_runtime->Session(), m_hand_paths[hand], &state)) &&
            state.interactionProfile != XR_NULL_PATH &&
            XR_SUCCEEDED(xrPathToString(m_runtime->Instance(), state.interactionProfile, sizeof(path), &length,
                                        path))) {
            m_profile_names[hand] = path;
            message << path;
        } else {
            message << "none";
        }
    }
    Log(OpenXRLogLevel::Info, message.str());
}

void OpenXRApplyVirtualGamepad() noexcept {
    Relay().Apply();
}

void OpenXRInput::Destroy() {
    if (m_created) {
        StopRumble();
        // The game must stop reading controllers whose session is going away.
        OpenXRInputSnapshot idle{};
        OpenXRPublishInputSnapshot(idle);
    }
    Relay().Close();
    m_profile_serial = 0;
    m_profile_names = {};
    DestroyPoseSpaces();
    if (m_action_set != XR_NULL_HANDLE) {
        // Destroying the set destroys every action created from it.
        xrDestroyActionSet(m_action_set);
        m_action_set = XR_NULL_HANDLE;
    }
    m_thumbstick = m_thumbstick_click = m_thumbstick_touch = XR_NULL_HANDLE;
    m_trigger = m_trigger_click = m_squeeze = m_squeeze_click = m_squeeze_force = XR_NULL_HANDLE;
    m_trackpad_click = m_trackpad_force = XR_NULL_HANDLE;
    m_button_primary = m_button_secondary = m_menu = m_haptic = XR_NULL_HANDLE;
    m_dpad_up = m_dpad_down = m_dpad_left = m_dpad_right = XR_NULL_HANDLE;
    m_shoulder = m_button_tertiary = m_button_quaternary = XR_NULL_HANDLE;
    m_aim_pose = m_grip_pose = XR_NULL_HANDLE;
    m_hand_paths[0] = m_hand_paths[1] = XR_NULL_PATH;
    m_convert_now_to_xr_time = nullptr;
    m_panel_controls.Reset();
    m_last_input_time = 0;
    m_menu_placement = {};
    m_floating_valid = false;
    m_created = false;
    m_runtime = nullptr;
}

void OpenXRInput::Idle() {
    if (!m_created) {
        return;
    }
    // The panel stays as it was; only what the controllers were holding is forgotten.
    m_panel_controls.Reset();
    m_last_input_time = 0;
    m_menu_placement = {};
    OpenXRPublishSettingsPanelPointer(false, 0.0f, 0.0f, false);
    OpenXRInputSnapshot idle{};
    OpenXRPublishInputSnapshot(idle);
    StopRumble();
    // Nothing stays held on the gamepad either while input is away.
    Relay().Publish({});
}

void OpenXRInput::Sync(XrTime predicted_display_time, const OpenXRPointerScreen& screen, bool panel_available) {
    (void)screen; // The game picture's rectangle, for a pointer on the game's own menus.
    if (!m_created || m_runtime == nullptr) {
        return;
    }
    if (!m_runtime->IsSessionFocused()) {
        Idle();
        return;
    }
    XrActiveActionSet active{m_action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    const XrResult result = diagnostics::Measure(diagnostics::Stage::SyncActions, [&] {
        return xrSyncActions(m_runtime->Session(), &sync);
    });
    m_runtime->ObserveResult(result);
    if (XR_FAILED(result)) {
        if (!m_logged_sync_failure) {
            m_logged_sync_failure = true;
            std::ostringstream message;
            message << "xrSyncActions failed (" << result << ')';
            Log(OpenXRLogLevel::Warning, message.str());
        }
        Idle();
        return;
    }

    // `active`, when given, says whether the action is bound to a source the
    // runtime has right now (a controller in that hand).
    const auto boolean = [&](XrAction action, uint32_t hand, bool* active = nullptr) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = m_hand_paths[hand];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        const bool bound = XR_SUCCEEDED(xrGetActionStateBoolean(m_runtime->Session(), &info, &state)) &&
                           state.isActive == XR_TRUE;
        if (active != nullptr) {
            *active = bound;
        }
        return bound && state.currentState == XR_TRUE;
    };
    const auto scalar = [&](XrAction action, uint32_t hand, bool* active = nullptr) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = m_hand_paths[hand];
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        const bool bound = XR_SUCCEEDED(xrGetActionStateFloat(m_runtime->Session(), &info, &state)) &&
                           state.isActive == XR_TRUE;
        if (active != nullptr) {
            *active = bound;
        }
        return bound ? state.currentState : 0.0f;
    };
    const auto vector = [&](XrAction action, uint32_t hand) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = m_hand_paths[hand];
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_FAILED(xrGetActionStateVector2f(m_runtime->Session(), &info, &state)) ||
            state.isActive != XR_TRUE) {
            return XrVector2f{0.0f, 0.0f};
        }
        return state.currentState;
    };

    std::array<HandInputs, kHands> hands{};
    std::array<bool, kHands> connected{};
    std::array<OpenXRControllerState, kHands> extra{};
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        HandInputs& inputs = hands[hand];
        bool trigger_active = false, squeeze_active = false, primary_active = false;
        bool secondary_active = false, menu_active = false;
        inputs.primary = boolean(m_button_primary, hand, &primary_active);
        inputs.secondary = boolean(m_button_secondary, hand, &secondary_active);
        inputs.menu = boolean(m_menu, hand, &menu_active);
        inputs.thumbstick_click = boolean(m_thumbstick_click, hand);
        inputs.trigger = scalar(m_trigger, hand, &trigger_active);
        inputs.squeeze = scalar(m_squeeze, hand, &squeeze_active);
        const XrVector2f stick = vector(m_thumbstick, hand);
        inputs.stick_x = stick.x;
        inputs.stick_y = stick.y;
        connected[hand] = trigger_active || squeeze_active || primary_active || secondary_active || menu_active;
        extra[hand].thumbstick_touch = boolean(m_thumbstick_touch, hand);
        extra[hand].trigger_click = boolean(m_trigger_click, hand);
        extra[hand].squeeze_click = boolean(m_squeeze_click, hand);
        bool squeeze_force_active = false;
        extra[hand].squeeze_force = scalar(m_squeeze_force, hand, &squeeze_force_active);
        extra[hand].trigger_analog = trigger_active;
        extra[hand].grip_analog = squeeze_active || squeeze_force_active;
        extra[hand].trackpad_click = boolean(m_trackpad_click, hand);
        extra[hand].trackpad_force = scalar(m_trackpad_force, hand, &extra[hand].touchpad_analog);
        extra[hand].dpad_up = boolean(m_dpad_up, hand);
        extra[hand].dpad_down = boolean(m_dpad_down, hand);
        extra[hand].dpad_left = boolean(m_dpad_left, hand);
        extra[hand].dpad_right = boolean(m_dpad_right, hand);
        extra[hand].shoulder = boolean(m_shoulder, hand);
        extra[hand].tertiary = boolean(m_button_tertiary, hand);
        extra[hand].quaternary = boolean(m_button_quaternary, hand);
        connected[hand] = connected[hand] || extra[hand].shoulder ||
                          extra[hand].dpad_up || extra[hand].dpad_down;
    }

    PollInjection();
    if (Injected("up")) {
        hands[0].stick_y = 1.0f;
    } else if (Injected("down")) {
        hands[0].stick_y = -1.0f;
    } else if (Injected("left")) {
        hands[0].stick_x = -1.0f;
    } else if (Injected("right")) {
        hands[0].stick_x = 1.0f;
    }

    if (m_runtime->InteractionProfileSerial() != m_profile_serial) {
        m_profile_serial = m_runtime->InteractionProfileSerial();
        LogInteractionProfiles();
    }

    const XrTime input_time = InputSampleTime(predicted_display_time);
    const float dt_seconds =
        m_last_input_time != 0 && input_time > m_last_input_time
            ? static_cast<float>(input_time - m_last_input_time) * 1.0e-9f
            : 0.0f;
    m_last_input_time = input_time;
    std::array<HandInputs, kHands> panel_hands = hands;
    if (Injected("panel")) {
        panel_hands[0].secondary = true;
        panel_hands[0].thumbstick_click = true;
        panel_hands[1].thumbstick_click = true;
    }
    if (Injected("a")) {
        panel_hands[1].primary = true;
    }
    // The game thread may open or close the menu too; only a change made here
    // is written back.
    const bool was_open = OpenXRSettingsPanelOpen();
    bool open = was_open;
    const OpenXRControllerMode mode = OpenXRGetControllerMode();
    const PortVrSettings settings = GetVrSettings();
    settings_panel::ToggleOptions options;
    options.allowed = panel_available && settings.vr_overlays_enabled;
    options.pointing_hand = settings.use_right_hand ? 1u : 0u;
    if (settings.custom_layout_enabled) {
        std::array<OpenXRControllerState, 2> controls = extra;
        for (uint32_t h = 0; h < kHands; ++h) {
            controls[h].connected = connected[h];
            controls[h].primary = hands[h].primary; controls[h].secondary = hands[h].secondary;
            controls[h].menu = hands[h].menu; controls[h].thumbstick_click = hands[h].thumbstick_click;
            controls[h].trigger_value = hands[h].trigger;
            // Custom grip bindings keep their own threshold, applied to Index
            // pressure rather than finger closure.
            controls[h].squeeze_value = m_profile_names[h].find("valve/index_controller") != std::string::npos
                ? extra[h].squeeze_force : hands[h].squeeze;
            controls[h].thumbstick_x = hands[h].stick_x; controls[h].thumbstick_y = hands[h].stick_y;
        }
        panel_hands = ControllerLayout::PanelInputs(controls, settings.custom_menu, settings.custom_confirm,
                                                    settings.custom_back, was_open, options.pointing_hand, ControllerLayout::Thresholds(settings));
    }
    options.long_press = settings.custom_layout_enabled ? settings.custom_menu_long_press : settings.vr_menu_hold_left_stick;
    options.long_press_seconds = settings.custom_layout_enabled ? settings.custom_menu_hold_seconds
        : static_cast<float>(VrMenu::kLongPressSeconds);
    options.needs_head_zone = !settings.custom_layout_enabled && settings.vr_menu_requires_head_zone;
    if (options.needs_head_zone && !was_open) {
        // PrimedGun's LeftControllerNearHead: the physical left hand's aim pose in the
        // visor gesture's zone.
        screen_math::Pose head{};
        screen_math::Pose hand{};
        options.hand_near_head =
            Locate(m_runtime->ViewSpace(), input_time, head) &&
            Locate(m_aim_spaces[0], input_time, hand) &&
            VisorDpad::HandNearHead(hand.position, head.position, settings.xr_dpad_head_radius,
                                    settings.xr_dpad_head_y_below);
    }
    const settings_panel::Frame panel = m_panel_controls.Update(panel_hands, open, dt_seconds, mode, options);
    // While the menu has the controllers, and always when they are nothing to
    // the game, the game sees them idle.
    const bool withheld = panel.withheld || mode == OpenXRControllerMode::None;
    // Pointer first: the game thread reads it as soon as it sees the menu open.
    PlaceMenu(input_time, panel, settings);
    if (open != was_open) {
        OpenXRSetSettingsPanelOpen(open);
    }

    // What the game sees of the controllers: nothing held while they are withheld.
    static const std::array<HandInputs, kHands> kIdleHands{};
    const auto& game_hands = withheld ? kIdleHands : hands;
    const auto injected = [withheld](const char* button) { return !withheld && Injected(button); };

    if (mode == OpenXRControllerMode::Gamepad && Relay().JoystickId() != 0) {
        const HandInputs& left = game_hands[0];
        const HandInputs& right = game_hands[1];
        VirtualGamepadRelay::Pad pad;
        // OpenXR thumbsticks report +Y up; SDL gamepads report +Y down.
        pad.axes[SDL_GAMEPAD_AXIS_LEFTX] = ToAxis(left.stick_x);
        pad.axes[SDL_GAMEPAD_AXIS_LEFTY] = ToAxis(-left.stick_y);
        pad.axes[SDL_GAMEPAD_AXIS_RIGHTX] = ToAxis(right.stick_x);
        pad.axes[SDL_GAMEPAD_AXIS_RIGHTY] = ToAxis(-right.stick_y);
        pad.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = ToTrigger(left.trigger);
        pad.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = ToTrigger(right.trigger);

        pad.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = right.primary || injected("a");
        pad.buttons[SDL_GAMEPAD_BUTTON_EAST] = right.secondary || injected("b");
        pad.buttons[SDL_GAMEPAD_BUTTON_WEST] = left.primary || injected("x");
        pad.buttons[SDL_GAMEPAD_BUTTON_NORTH] = left.secondary || injected("y");
        pad.buttons[SDL_GAMEPAD_BUTTON_START] = left.menu || injected("start");
        pad.buttons[SDL_GAMEPAD_BUTTON_LEFT_STICK] = left.thumbstick_click;
        pad.buttons[SDL_GAMEPAD_BUTTON_RIGHT_STICK] = right.thumbstick_click;
        pad.buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = left.squeeze > kPressThreshold;
        pad.buttons[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = right.squeeze > kPressThreshold;
        Relay().Publish(pad);
    }

    // The snapshot for the game thread, with the profile-specific extras folded in.
    std::array<HandInputs, kHands> snapshot_hands = game_hands;
    if (!withheld) {
        for (uint32_t hand = 0; hand < kHands; ++hand) {
            if (injected("a") && hand == 1) snapshot_hands[hand].primary = true;
            if (injected("b") && hand == 1) snapshot_hands[hand].secondary = true;
            if (injected("x") && hand == 0) snapshot_hands[hand].primary = true;
            if (injected("y") && hand == 0) snapshot_hands[hand].secondary = true;
            if (injected("start") && hand == 0) snapshot_hands[hand].menu = true;
        }
    }
    m_pending_extra = withheld ? std::array<OpenXRControllerState, kHands>{} : extra;
    PublishSnapshot(input_time, snapshot_hands, connected, withheld);
    UpdateRumble();
}

bool OpenXRInput::Locate(XrSpace space, XrTime time, screen_math::Pose& pose) const {
    if (space == XR_NULL_HANDLE) {
        return false;
    }
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(space, m_runtime->AppSpace(), time, &location)) ||
        (location.locationFlags & kPoseValidFlags) != kPoseValidFlags) {
        return false;
    }
    pose = ToPose(location.pose);
    return screen_math::NormalizePose(pose);
}

// PrimedGun's menu placement (UpdateVrMenu): on the off hand's grip, or, when
// detached, latched 2.7 m ahead of the head as it opens. The cannon hand's aim
// ray picks the canvas point (texture pixels) and draws the laser, with a
// short tick in that hand when a click starts.
void OpenXRInput::PlaceMenu(XrTime input_time, const settings_panel::Frame& frame, const PortVrSettings& settings) {
    m_menu_placement = {};
    if (!frame.open) {
        m_floating_valid = false;
        OpenXRPublishSettingsPanelPointer(false, 0.0f, 0.0f, false);
        return;
    }
    screen_math::Pose head{};
    const bool head_valid = Locate(m_runtime->ViewSpace(), input_time, head);

    screen_math::Pose panel{};
    float width = 0.0f;
    float height = 0.0f;
    if (settings.vr_menu_floating) {
        // Latched when it opens, or when it is detached while open.
        if (!m_floating_valid && head_valid) {
            m_floating_valid = VrMenu::FloatingPoseFromHead(head, m_floating_pose);
        }
        if (m_floating_valid) {
            panel = m_floating_pose;
            width = VrMenu::kFloatingWidth;
            height = VrMenu::kFloatingHeight;
        }
    } else {
        m_floating_valid = false;
        const uint32_t hand = settings.use_right_hand ? 0u : 1u;
        screen_math::Pose grip{};
        if (Locate(m_grip_spaces[hand], input_time, grip) || Locate(m_aim_spaces[hand], input_time, grip)) {
            panel = VrMenu::PanelPoseFromGrip(grip);
            width = VrMenu::kWristWidth;
            height = VrMenu::kWristHeight;
        }
    }
    const bool placed = width > 0.0f;

    screen_math::Pose aim{};
    const bool aim_valid = Locate(m_aim_spaces[frame.pointing_hand], input_time, aim);
    VrMenu::PointerHit hit{};
    if (placed && aim_valid) {
        hit = VrMenu::HitPanel(panel, width, height, aim);
    }
    if (frame.clicked) {
        constexpr XrDuration kTickNs = 15'000'000;
        ApplyHaptic(frame.pointing_hand, 0.35f, kTickNs);
    }
    OpenXRPublishSettingsPanelPointer(hit.valid, hit.x * static_cast<float>(VrMenu::kWidth),
                                      hit.y * static_cast<float>(VrMenu::kMenuHeight), frame.clicked);

    if (!placed) {
        return;
    }
    m_menu_placement.placed = true;
    m_menu_placement.pose = ToXrPose(panel);
    m_menu_placement.width_meters = width;
    m_menu_placement.height_meters = height;
    if (aim_valid) {
        const float length = hit.valid ? hit.distance : VrMenu::kPointerMaxDistance;
        const screen_math::Vec3 eye = head_valid ? head.position : panel.position;
        m_menu_placement.laser = true;
        m_menu_placement.laser_pose = ToXrPose(VrMenu::LaserPose(aim, length, eye));
        m_menu_placement.laser_length_meters = length;
        if (hit.valid) {
            m_menu_placement.dot = true;
            m_menu_placement.dot_pose = ToXrPose(VrMenu::DotPose(panel, aim, hit.distance));
        }
    }
}

void OpenXRInput::PublishSnapshot(XrTime input_time, const std::array<HandInputs, kHands>& hands,
                                  const std::array<bool, kHands>& connected, bool withheld) {
    OpenXRInputSnapshot snapshot{};
    snapshot.runtime_active = true;
    snapshot.frame_serial = ++m_frame_serial;
    snapshot.sample_time_xr = input_time;
    snapshot.interaction_profiles = m_profile_names;

    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (m_runtime->ViewSpace() != XR_NULL_HANDLE &&
        XR_SUCCEEDED(xrLocateSpace(m_runtime->ViewSpace(), m_runtime->AppSpace(), input_time, &head))) {
        snapshot.head_pose = ToPoseState(head.pose, (head.locationFlags & kPoseValidFlags) == kPoseValidFlags);
    }

    for (uint32_t hand = 0; hand < kHands; ++hand) {
        OpenXRControllerState& state = snapshot.controllers[hand];
        state = m_pending_extra[hand];
        state.connected = connected[hand];
        const HandInputs& inputs = hands[hand];
        state.primary = inputs.primary;
        state.secondary = inputs.secondary;
        state.menu = inputs.menu;
        state.thumbstick_click = inputs.thumbstick_click;
        state.trigger_value = inputs.trigger;
        state.squeeze_value = inputs.squeeze;
        state.thumbstick_x = inputs.stick_x;
        state.thumbstick_y = inputs.stick_y;
        if (withheld) {
            // The panel has the controllers: poses still track, nothing is held.
            state.thumbstick_touch = state.trigger_click = state.squeeze_click = state.trackpad_click = false;
            state.squeeze_force = state.trackpad_force = 0.0f;
            state.dpad_up = state.dpad_down = state.dpad_left = state.dpad_right = false;
            state.shoulder = state.tertiary = state.quaternary = false;
        }
        if (m_aim_spaces[hand] != XR_NULL_HANDLE) {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(m_aim_spaces[hand], m_runtime->AppSpace(), input_time, &location))) {
                state.aim_pose =
                    ToPoseState(location.pose, (location.locationFlags & kPoseValidFlags) == kPoseValidFlags);
            }
        }
        if (m_grip_spaces[hand] != XR_NULL_HANDLE) {
            XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            location.next = &velocity;
            if (XR_SUCCEEDED(xrLocateSpace(m_grip_spaces[hand], m_runtime->AppSpace(), input_time, &location))) {
                state.grip_pose =
                    ToPoseState(location.pose, (location.locationFlags & kPoseValidFlags) == kPoseValidFlags);
                state.velocity_valid = (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0 &&
                                       (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
                state.linear_velocity = {velocity.linearVelocity.x, velocity.linearVelocity.y,
                                         velocity.linearVelocity.z};
                state.angular_velocity = {velocity.angularVelocity.x, velocity.angularVelocity.y,
                                          velocity.angularVelocity.z};
            }
        }
    }
    OpenXRPublishInputSnapshot(snapshot);
}

// PrimedGun's rumble: the game's requested amplitude, scaled by the intensity
// setting, re-applied every frame on the chosen hand(s) while it is non-zero.
// The split target gives each hand its own (vr_rumble.h).
void OpenXRInput::UpdateRumble() {
    const PortVrSettings settings = GetVrSettings();
    if (!settings.rumble_enabled) {
        StopRumble();
        return;
    }
    const std::array<float, 2> amplitudes =
        RumbleHandAmplitudes(settings.rumble_hand, settings.use_right_hand, OpenXRTakeRumble());
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        const float amplitude = amplitudes[hand] * settings.rumble_intensity;
        if (amplitude > 0.0f) {
            ApplyHaptic(hand, amplitude, kRumblePulseNs);
            m_haptics_active[hand] = true;
        } else if (m_haptics_active[hand]) {
            ApplyHaptic(hand, 0.0f, 0);
            m_haptics_active[hand] = false;
        }
    }
}

void OpenXRInput::StopRumble() {
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        if (m_haptics_active[hand]) {
            ApplyHaptic(hand, 0.0f, 0);
            m_haptics_active[hand] = false;
        }
    }
}

void OpenXRInput::ApplyHaptic(uint32_t hand, float amplitude, XrDuration duration) {
    if (!m_created || m_runtime == nullptr || hand >= kHandCount || m_haptic == XR_NULL_HANDLE) {
        return;
    }
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    vibration.duration = duration;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = m_haptic;
    info.subactionPath = m_hand_paths[hand];
    if (vibration.amplitude <= 0.0f) {
        xrStopHapticFeedback(m_runtime->Session(), &info);
        return;
    }
    xrApplyHapticFeedback(m_runtime->Session(), &info,
                          reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

bool OpenXRInput::Check(XrResult result, const char* operation) {
    if (m_runtime != nullptr) {
        m_runtime->ObserveResult(result);
    }
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    std::ostringstream message;
    message << operation << " failed (" << result << ')';
    m_last_error = message.str();
    Log(OpenXRLogLevel::Error, m_last_error);
    return false;
}

void OpenXRInput::Log(OpenXRLogLevel level, const std::string& message) const noexcept {
    if (!m_logger) {
        return;
    }
    try {
        m_logger(level, message);
    } catch (...) {
    }
}

} // namespace PortVr

#endif // defined(MP_ENABLE_OPENXR)
