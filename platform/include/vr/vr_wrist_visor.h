// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "vr/openxr_controller_snapshot.h"
#include "vr/openxr_screen_math.h"
#include "vr/vr_settings.h"
#include "vr/vr_controller_layout.h"
#include <array>
#include <cmath>

namespace PortVr::WristVisor {
// A row following the side of the off-hand forearm (+local Z). All distances are metres.
inline constexpr float HalfIcon = 0.021f;
inline constexpr float TouchHeight = 0.12f;
inline constexpr std::array<screen_math::Vec3, 4> Centers{{
    {0.f, 0.035f, 0.06f}, {0.f, 0.035f, 0.11f},
    {0.f, 0.035f, 0.16f}, {0.f, 0.035f, 0.21f}}};
// Slots run from wrist (right end) towards forearm (left end).
// Viewed left to right: X-ray, Thermal, Scan, Combat.
// Values are CPlayerState::EPlayerVisor indices, independent of their position.
inline constexpr std::array<int, 4> VisorForSlot{0, 2, 3, 1};
inline unsigned SlotMask(unsigned visors) {
    unsigned slots = 0;
    for (int i = 0; i < 4; ++i) if (visors & (1u << VisorForSlot[i])) slots |= 1u << i;
    return slots;
}
inline bool SelectionPressed(const OpenXRInputSnapshot& snapshot, bool rightHanded,
                             const PortVrSettings& settings = {}) {
    // Physical right A/trigger or left X/trigger, independent of action remaps.
    const auto& hand = snapshot.controllers[rightHanded ? 1 : 0];
    const bool trigger = settings.custom_layout_enabled
        ? ControllerLayout::Pressed(snapshot.controllers, rightHanded ? 19 : 3, ControllerLayout::Thresholds(settings))
        : hand.trigger_value > 0.5f || hand.trigger_click;
    return snapshot.runtime_active && hand.connected && (hand.primary || trigger);
}
inline void ConsumeSelection(OpenXRInputSnapshot& snapshot, unsigned weaponHand) {
    auto& hand = snapshot.controllers[weaponHand];
    hand.primary = false;
    hand.trigger_value = 0.f;
    hand.trigger_click = false;
}
inline screen_math::Quat SymbolRotation(unsigned weaponHand) {
    // Left-handed placement needs the opposite quarter-turn to remain upright.
    return {0.f, weaponHand == 1 ? 0.70710678f : -0.70710678f, 0.f, 0.70710678f};
}
struct Calibration {
    screen_math::Vec3 offset{};
    float pitch = 0.f, yaw = 0.f, roll = 0.f;
    Calibration() : Calibration(PortVrSettings{}) {}
    explicit Calibration(const PortVrSettings& s) : offset{s.wrist_offset_x, s.wrist_offset_y, s.wrist_offset_z},
        pitch(s.wrist_pitch), yaw(s.wrist_yaw), roll(s.wrist_roll) {}
};
inline screen_math::Quat Multiply(const screen_math::Quat& a, const screen_math::Quat& b) {
    return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
            a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
            a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
            a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
}
inline screen_math::Quat PanelRotation(unsigned weaponHand, const Calibration& c = {}) {
    constexpr float sinHalf = 0.38268343f, cosHalf = 0.92387953f;
    constexpr float halfRadians = 3.14159265f / 360.f;
    const float side = weaponHand == 1 ? 1.f : -1.f;
    const auto base = screen_math::Quat{0.f, 0.f, side * sinHalf, cosHalf};
    const float pitch = -c.pitch * halfRadians, yaw = -c.yaw * side * halfRadians, roll = -c.roll * side * halfRadians;
    return Multiply(base, Multiply({0.f, std::sin(yaw), 0.f, std::cos(yaw)},
        Multiply({std::sin(pitch), 0.f, 0.f, std::cos(pitch)}, {0.f, 0.f, std::sin(roll), std::cos(roll)})));
}
inline screen_math::Vec3 Center(int icon, unsigned weaponHand, const Calibration& c = {}) {
    auto point = screen_math::Rotate(PanelRotation(weaponHand, c), Centers[icon]);
    for (int i = 0; i < 3; ++i) point[i] += c.offset[i];
    return point;
}
inline const OpenXRPoseState& WristPose(const OpenXRControllerState& hand) {
    return hand.grip_pose.valid ? hand.grip_pose : hand.aim_pose;
}
inline int Hover(const OpenXRInputSnapshot& snapshot, unsigned weaponHand, int previous = -1, const Calibration& calibration = {}) {
    if (!snapshot.runtime_active || weaponHand > 1) return -1;
    const auto& main = snapshot.controllers[weaponHand];
    const auto& off = snapshot.controllers[1 - weaponHand];
    const auto& wrist = WristPose(off);
    if (!main.connected || !off.connected || !main.aim_pose.valid || !wrist.valid) return -1;
    screen_math::Vec3 delta{};
    for (int i = 0; i < 3; ++i) delta[i] = main.aim_pose.position[i] - wrist.position[i];
    auto handLocal = screen_math::Rotate(screen_math::Conjugate(wrist.orientation), delta);
    for (int i = 0; i < 3; ++i) handLocal[i] -= calibration.offset[i];
    const auto local = screen_math::Rotate(screen_math::Conjugate(PanelRotation(weaponHand, calibration)), handLocal);
    // Test the same tilted per-icon volume that is rendered. Row positions
    // remain fixed along the arm when only icon pitch/roll is adjusted.
    constexpr float margin = HalfIcon + 0.018f;
    float best = 100.f;
    int hovered = -1;
    for (int icon = 0; icon < 4; ++icon) {
        screen_math::Vec3 offset{};
        for (int axis = 0; axis < 3; ++axis) offset[axis] = local[axis] - Centers[icon][axis];
        const auto& face = offset;
        if (face[1] < -0.025f || face[1] > TouchHeight ||
            std::fabs(face[0]) > margin || std::fabs(face[2]) > margin) continue;
        const float score = face[0] * face[0] + face[2] * face[2] - (icon == previous ? 0.0004f : 0.f);
        if (score < best) { best = score; hovered = icon; }
    }
    return hovered;
}
// Keep a press until the simulation actually consumes it. Render frames can
// contain zero game ticks, and visor transitions temporarily reject changes.
class SelectionQueue {
    int pending = -1;
    double until = 0;
public:
    void Reset() { pending = -1; until = 0; }
    void Submit(int visor, double now) { if (visor >= 0 && visor < 4) { pending = visor; until = now + 0.65; } }
    int Take(double now) { const int result = now <= until ? pending : -1; Reset(); return result; }
};
struct Result { int hover = -1; int selected = -1; bool consume = false; };
class Tracker {
    bool previous = false, captured = false;
public:
    void Reset() { previous = captured = false; }
    Result Update(bool active, int hover, bool primary, unsigned owned, bool accepting) {
        Result out;
        if (!active) { previous = primary; captured = false; return out; }
        out.hover = hover;
        if (!primary) captured = false;
        if (primary && hover >= 0) captured = true;
        out.consume = captured;
        if (primary && !previous && hover >= 0 && (owned & (1u << hover)) && accepting)
            out.selected = hover;
        previous = primary;
        return out;
    }
};
} // namespace PortVr::WristVisor
