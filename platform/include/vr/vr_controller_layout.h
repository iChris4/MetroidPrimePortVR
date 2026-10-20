#pragma once

#include "vr/openxr_controller_snapshot.h"
#include <algorithm>
#include <cmath>

namespace PortVr::ControllerLayout {
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
    case 3: return std::max(h.trigger_value, h.trigger_click ? 1.f : 0.f);
    case 4: return std::max({h.squeeze_value, h.squeeze_force, h.squeeze_click ? 1.f : 0.f});
    case 5: return h.thumbstick_click;
    case 6: return h.menu;
    case 7: return std::max(h.trackpad_force, h.trackpad_click ? 1.f : 0.f);
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
inline bool Pressed(const std::array<OpenXRControllerState, 2>& hands, int binding) noexcept {
    return Value(hands, binding) > 0.55f;
}
} // namespace PortVr::ControllerLayout
