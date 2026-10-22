// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pose and virtual-screen geometry shared by the OpenXR input layer and the
// pacing thread: where an aim ray meets a screen, how a pointer survives a
// tracking blip, and how a letterboxed picture fits a quad. Lifted from
// Wiicompiled VR's openxr_wii_remote.h minus everything Wii Remote.

#pragma once

#include "vr/openxr_hand_inputs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace PortVr::screen_math {

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>; // x, y, z, w

struct Pose {
    Vec3 position{};
    Quat orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

// Tracking flags alone do not guarantee finite data or a unit quaternion.
// OpenXR rejects an entire frame if any submitted layer has an invalid pose.
inline bool NormalizePose(Pose& pose) noexcept {
    for (float component : pose.position) {
        if (!std::isfinite(component)) return false;
    }
    double length_squared = 0.0;
    for (float component : pose.orientation) {
        if (!std::isfinite(component)) return false;
        length_squared += double(component) * double(component);
    }
    if (length_squared < 1.0e-12) return false;
    const double inverse_length = 1.0 / std::sqrt(length_squared);
    for (float& component : pose.orientation) {
        component = static_cast<float>(double(component) * inverse_length);
    }
    return true;
}

// The pointer keeps tracking this far past the picture's centre, in half-widths
// and half-heights, like a real remote's camera past the sensor bar.
inline constexpr float kPointerMarginU = 1.9f;
inline constexpr float kPointerMarginV = 1.5f;
// A lost hit or an excursion beyond the margins holds or pins the cursor this
// long before it disappears, so tracking spikes do not drop it.
inline constexpr int64_t kPointerHideDelayNs = 100'000'000;

inline float Dot(const Vec3& a, const Vec3& b) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// q * v * conjugate(q) for a unit quaternion.
inline Vec3 Rotate(const Quat& q, const Vec3& v) noexcept {
    const Vec3 t{2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]),
                 2.0f * (q[0] * v[1] - q[1] * v[0])};
    return {v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]),
            v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]),
            v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0])};
}

inline Quat Conjugate(const Quat& q) noexcept {
    return {-q[0], -q[1], -q[2], q[3]};
}

// A flat rectangle: the part of a virtual screen the game's picture covers.
struct Screen {
    Pose pose;
    float half_width = 0.0f;
    float half_height = 0.0f;
};

// Where the aim ray meets the screen's plane, the same absolute mapping as
// DolphinXR's ComputeVirtualScreenHit: aiming at a point puts the pointer
// there, with nothing to recenter.
inline ScreenHit RaycastScreen(const Pose& aim, const Screen& screen) noexcept {
    ScreenHit hit{};
    if (!(screen.half_width > 0.0f) || !(screen.half_height > 0.0f)) {
        return hit;
    }
    const Quat inverse = Conjugate(screen.pose.orientation);
    const Vec3 offset{aim.position[0] - screen.pose.position[0], aim.position[1] - screen.pose.position[1],
                      aim.position[2] - screen.pose.position[2]};
    const Vec3 origin = Rotate(inverse, offset);
    const Vec3 direction = Rotate(inverse, Rotate(aim.orientation, {0.0f, 0.0f, -1.0f}));
    // Only from in front of the picture, and only towards it.
    if (!(origin[2] > 0.0f) || !(direction[2] < -1.0e-6f)) {
        return hit;
    }
    const float t = -origin[2] / direction[2];
    hit.valid = true;
    hit.u = (origin[0] + t * direction[0]) / screen.half_width;
    hit.v = (origin[1] + t * direction[1]) / screen.half_height;
    // Perpendicular distance: rotating the controller must not move it.
    hit.distance_meters = origin[2];
    return hit;
}

// Hides the pointer the way a real remote loses the sensor bar, without
// dropping it on every tracking hiccup: brief excursions and lost hits hold or
// pin the last position, and only a sustained one hides it.
class PointerFilter {
public:
    ScreenHit Update(const ScreenHit& hit, int64_t time_ns) noexcept {
        const bool on_screen = hit.valid && std::fabs(hit.u) <= kPointerMarginU &&
                               std::fabs(hit.v) <= kPointerMarginV;
        if (on_screen) {
            m_off_screen = false;
            m_held = hit;
            return hit;
        }
        if (!m_off_screen) {
            m_off_screen = true;
            m_off_since_ns = time_ns;
        }
        if (!m_held.valid || time_ns - m_off_since_ns >= kPointerHideDelayNs) {
            m_held.valid = false;
            return {};
        }
        if (!hit.valid) {
            return m_held;
        }
        ScreenHit pinned = hit;
        pinned.u = std::clamp(hit.u, -kPointerMarginU, kPointerMarginU);
        pinned.v = std::clamp(hit.v, -kPointerMarginV, kPointerMarginV);
        return pinned;
    }

    void Reset() noexcept {
        m_held = {};
        m_off_screen = false;
    }

private:
    ScreenHit m_held{};
    bool m_off_screen = false;
    int64_t m_off_since_ns = 0;
};

// The part of an aspect-ratio-preserving fit a `content` aspect takes inside a
// `container` aspect, as fractions of the container's width and height.
inline std::array<float, 2> FitFraction(float content_aspect, float container_aspect) noexcept {
    if (!(content_aspect > 0.0f) || !(container_aspect > 0.0f)) {
        return {1.0f, 1.0f};
    }
    return content_aspect >= container_aspect ? std::array<float, 2>{1.0f, container_aspect / content_aspect}
                                              : std::array<float, 2>{content_aspect / container_aspect, 1.0f};
}

// Half extents, in metres, of the game picture on the menu quad. The quad is
// `quad_width` across with the eye texture's aspect; Aurora fits the desktop
// snapshot into that texture and the game picture into the snapshot, both
// letterboxed, so a 4:3 picture in a 16:9 window keeps its pillarboxes.
inline std::array<float, 2> MenuPictureHalfExtents(float quad_width, float eye_aspect, float snapshot_aspect,
                                                   float picture_aspect) noexcept {
    const float quad_half_width = 0.5f * quad_width;
    const float quad_half_height = eye_aspect > 0.0f ? quad_half_width / eye_aspect : quad_half_width;
    const std::array<float, 2> snapshot = FitFraction(snapshot_aspect, eye_aspect);
    const std::array<float, 2> picture = FitFraction(picture_aspect, snapshot_aspect);
    return {quad_half_width * snapshot[0] * picture[0], quad_half_height * snapshot[1] * picture[1]};
}

} // namespace PortVr::screen_math
