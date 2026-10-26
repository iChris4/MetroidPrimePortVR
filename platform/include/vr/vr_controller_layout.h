#pragma once

#include "vr/openxr_controller_snapshot.h"
#include "vr/openxr_hand_inputs.h"
#include "vr/vr_settings.h"
#include <algorithm>
#include <cstddef>
#include <cmath>

namespace PortVr::ControllerLayout {
struct Thresholds {
    float trigger = 0.55f, grip = 0.55f, touchpad = 0.55f, stick = 0.55f;
    Thresholds() = default;
    explicit Thresholds(const PortVrSettings& s) : trigger(s.custom_trigger_threshold), grip(s.custom_grip_threshold),
        touchpad(s.custom_touchpad_threshold), stick(s.custom_stick_threshold) {}
};
inline constexpr int ControlsPerHand = 16;
inline constexpr int MaxControl = 36;
// 0 unbound; each hand: primary, secondary, trigger, grip, stick click,
// menu, trackpad click, stick up/down/left/right, physical D-pad U/D/L/R, shoulder.
inline float Value(const std::array<OpenXRControllerState, 2>& hands, int binding) noexcept {
    if (binding < 1 || binding > MaxControl) return 0.f;
    if (binding > 32) {
        const auto& extra = hands[binding <= 34 ? 1 : 0];
        return extra.connected && ((binding % 2) ? extra.tertiary : extra.quaternary);
    }
    const auto& h = hands[(binding - 1) / ControlsPerHand];
    if (!h.connected) return 0.f;
    switch ((binding - 1) % ControlsPerHand + 1) {
    case 1: return h.primary;
    case 2: return h.secondary;
    case 3: return ((h.trigger_analog || h.trigger_value > 0.f) ? h.trigger_value : h.trigger_click ? 1.f : 0.f);
    case 4: return ((h.grip_analog || std::max(h.squeeze_value, h.squeeze_force) > 0.f) ? std::max(h.squeeze_value, h.squeeze_force) : h.squeeze_click ? 1.f : 0.f);
    case 5: return h.thumbstick_click;
    case 6: return h.menu;
    case 7: return ((h.touchpad_analog || h.trackpad_force > 0.f) ? h.trackpad_force : h.trackpad_click ? 1.f : 0.f);
    case 8: return std::max(0.f, h.thumbstick_y);
    case 9: return std::max(0.f, -h.thumbstick_y);
    case 10: return std::max(0.f, -h.thumbstick_x);
    case 11: return std::max(0.f, h.thumbstick_x);
    case 12: return h.dpad_up;
    case 13: return h.dpad_down;
    case 14: return h.dpad_left;
    case 15: return h.dpad_right;
    case 16: return h.shoulder;
    }
    return 0.f;
}
inline bool Pressed(const std::array<OpenXRControllerState, 2>& hands, int binding, const Thresholds& thresholds = {}) noexcept {
    const int control = binding >= 1 && binding <= 32 ? (binding - 1) % ControlsPerHand + 1 : 0;
    const float threshold = control == 3 ? thresholds.trigger : control == 4 ? thresholds.grip :
        control == 7 ? thresholds.touchpad : control >= 8 && control <= 11 ? thresholds.stick : 0.55f;
    return Value(hands, binding) > threshold;
}
// Translate the overlay's toggle/select controls too. All physical buttons are
// retained as release-gate input, except grips not explicitly bound to confirm.
inline std::array<HandInputs, 2> PanelInputs(const std::array<OpenXRControllerState, 2>& hands,
                                          int menu, int confirm, int back, bool open, uint32_t pointer, const Thresholds& thresholds = {}) noexcept {
    std::array<HandInputs, 2> out{};
    for (size_t i = 0; i < hands.size(); ++i) {
        const auto& h = hands[i];
        if (!h.connected) continue;
        out[i].stick_x = h.thumbstick_x; out[i].stick_y = h.thumbstick_y;
        const int grip = static_cast<int>(i) * ControlsPerHand + 4;
        const bool gripMapped = confirm == grip;
        out[i].secondary = h.primary || h.secondary || h.menu || h.thumbstick_click || h.trackpad_click ||
            h.dpad_up || h.dpad_down || h.dpad_left || h.dpad_right || h.shoulder || h.tertiary || h.quaternary ||
            h.trigger_click || h.trigger_value > std::min(0.5f, thresholds.trigger) ||
            (gripMapped && (h.squeeze_click || std::max(h.squeeze_value, h.squeeze_force) > std::min(0.5f, thresholds.grip))) ||
            h.trackpad_force > std::min(0.5f, thresholds.touchpad);
    }
    out[0].thumbstick_click = Pressed(hands, menu, thresholds) || (open && Pressed(hands, back, thresholds));
    out[pointer & 1u].trigger = Pressed(hands, confirm, thresholds) ? 1.f : 0.f;
    return out;
}
} // namespace PortVr::ControllerLayout
