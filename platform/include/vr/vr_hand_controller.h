// SPDX-License-Identifier: GPL-3.0-or-later
//
// A hand-tracked controller: a gamepad held in a hand ARKit tracks, played as
// a tracked VR controller. On Apple Vision Pro a pair of Nintendo Switch
// Joy-Cons has buttons but no pose, so the visionOS OpenXR provider
// (platform/vr/visionos/xr_visionos_hand_controllers.mm) reads each half's
// buttons as one hand's Touch controller and takes that hand's pose from its
// skeleton. The game then plays them as it plays the PlayStation VR2 Sense
// controllers: the arm cannon follows the hand.
//
// Three pieces live here, free of ARKit, simd and OpenXR so that
// tests/port_vr_hand_controller.cpp checks them on any machine:
//   - the button map of a combined Joy-Con pair onto two Touch controllers;
//   - the frame of a fist closed on an upright controller, from three joints;
//   - a One Euro filter (Casiez, Roussel and Vogel, CHI 2012) for its pose,
//     which a camera-tracked skeleton needs before it can aim a gun.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace PortVr::HandController {

// ---------------------------------------------------------------------------
// Small vector maths (metres, OpenXR axes: +X right, +Y up, -Z forward).

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 a) noexcept { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(Vec3 a) noexcept { return std::sqrt(Dot(a, a)); }

// A rotation as a unit quaternion (x, y, z, w).
struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

inline float Dot(Quat a, Quat b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

inline Quat Normalized(Quat q) noexcept {
    const float length = std::sqrt(Dot(q, q));
    if (length < 1.0e-8f) {
        return {};
    }
    return {q.x / length, q.y / length, q.z / length, q.w / length};
}

// The rotation whose matrix has these orthonormal columns.
inline Quat QuatFromBasis(Vec3 right, Vec3 up, Vec3 back) noexcept {
    const float m00 = right.x, m10 = right.y, m20 = right.z;
    const float m01 = up.x, m11 = up.y, m21 = up.z;
    const float m02 = back.x, m12 = back.y, m22 = back.z;
    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return Normalized(q);
}

inline Vec3 Rotate(Quat q, Vec3 v) noexcept {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = Cross(u, v) * 2.0f;
    return v + t * q.w + Cross(u, t);
}

// The angle between two rotations, in radians (0..pi).
inline float AngleBetween(Quat a, Quat b) noexcept {
    const float d = std::fabs(Dot(a, b));
    return 2.0f * std::acos(d > 1.0f ? 1.0f : d);
}

// Spherical interpolation along the shorter arc.
inline Quat Slerp(Quat a, Quat b, float t) noexcept {
    float d = Dot(a, b);
    if (d < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f) {
        return Normalized({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                           a.w + (b.w - a.w) * t});
    }
    const float theta = std::acos(d);
    const float sinTheta = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / sinTheta;
    const float wb = std::sin(t * theta) / sinTheta;
    return Normalized({a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
}

// ---------------------------------------------------------------------------
// Buttons. A combined Joy-Con pair as GameController reports it: the face
// buttons under their Nintendo labels (A right, B bottom, X top, Y left), the
// left Joy-Con's four arrow buttons as the D-pad, ZL and ZR digital.

struct JoyConPairButtons {
    bool a = false;
    bool b = false;
    bool x = false;
    bool y = false;
    bool dpadUp = false;
    bool dpadDown = false;
    bool dpadLeft = false;
    bool dpadRight = false;
    bool l = false;
    bool r = false;
    bool zl = false;
    bool zr = false;
    bool minus = false;
    bool plus = false;
    bool leftStickClick = false;
    bool rightStickClick = false;
    float leftStickX = 0.0f;
    float leftStickY = 0.0f;
    float rightStickX = 0.0f;
    float rightStickY = 0.0f;
};

// One hand's Touch controller, as the provider's ControllerSample holds it.
struct HandButtons {
    float trigger = 0.0f;
    float squeeze = 0.0f;
    bool primary = false;   // A on the right, X on the left
    bool secondary = false; // B on the right, Y on the left
    bool menu = false;
    bool thumbstickClick = false;
    float thumbstickX = 0.0f;
    float thumbstickY = 0.0f;
};

// The pair as two Touch controllers, [0] left and [1] right, laid out as the
// Sense controllers play (docs/VISIONOS.md), by the Nintendo labels:
//   right: ZR trigger (fire), R grip (missile), A = Cross (confirm, bomb),
//          B = Circle (back, beam wheel), the right stick and its click;
//   left:  ZL trigger (lock-on), L grip (map), Y, Down or Left = Square
//          (Morph Ball), Up = Triangle (pause), the left stick and its click,
//          and minus = Create (the VR menu).
// Plus is the right Joy-Con's Start, so it pauses too: it reads as the left
// controller's Triangle, which is where the game takes pause from. X and the
// Right arrow have no Sense counterpart and do nothing; Home and Capture
// belong to the system. (The game also jumps with A and the right stick's up
// push at once while a pair plays: vr_pad.cpp.)
inline std::array<HandButtons, 2> MapJoyConPair(const JoyConPairButtons& in) noexcept {
    std::array<HandButtons, 2> hands{};
    HandButtons& left = hands[0];
    left.trigger = in.zl ? 1.0f : 0.0f;
    left.squeeze = in.l ? 1.0f : 0.0f;
    left.primary = in.y || in.dpadDown || in.dpadLeft;
    left.secondary = in.dpadUp || in.plus;
    left.menu = in.minus;
    left.thumbstickClick = in.leftStickClick;
    left.thumbstickX = in.leftStickX;
    left.thumbstickY = in.leftStickY;
    HandButtons& right = hands[1];
    right.trigger = in.zr ? 1.0f : 0.0f;
    right.squeeze = in.r ? 1.0f : 0.0f;
    right.primary = in.a;
    right.secondary = in.b;
    right.thumbstickClick = in.rightStickClick;
    right.thumbstickX = in.rightStickX;
    right.thumbstickY = in.rightStickY;
    return hands;
}

// ---------------------------------------------------------------------------
// The holding frame. A Joy-Con is held upright in a fist, thumb on the stick:
// the knuckles stand one above the other, the index's on top, and the forearm
// points where a pistol grip would aim. So, from joint positions alone (ARKit's
// own joint axes never matter):
//   forward (-Z) runs from the wrist to the index knuckle;
//   up (+Y) runs from the little knuckle to the index knuckle, made square to
//   forward (the thumb's side, for either hand: no mirroring);
//   right (+X) is forward x up.
// The bare-hand frame (HandAxes, palm down, +Y out of the back of the hand)
// would roll such a grip by a quarter turn. `pitchDegrees` then tilts the aim
// about right, positive up. False when the joints coincide or line up.

struct Frame {
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 back{0.0f, 0.0f, 1.0f}; // -forward
};

inline bool HoldingFrame(Vec3 wrist, Vec3 indexKnuckle, Vec3 littleKnuckle, float pitchDegrees, Frame& out) noexcept {
    Vec3 forward = indexKnuckle - wrist;
    const float forwardLength = Length(forward);
    if (forwardLength < 1.0e-4f) {
        return false;
    }
    forward = forward * (1.0f / forwardLength);
    const Vec3 across = indexKnuckle - littleKnuckle;
    Vec3 up = across - forward * Dot(across, forward);
    const float upLength = Length(up);
    if (upLength < 1.0e-4f) {
        return false;
    }
    up = up * (1.0f / upLength);
    if (pitchDegrees != 0.0f) {
        const float radians = pitchDegrees * (3.14159265358979f / 180.0f);
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        const Vec3 pitchedForward = forward * c + up * s;
        up = up * c - forward * s;
        forward = pitchedForward;
    }
    out.right = Cross(forward, up);
    out.up = up;
    out.back = -forward;
    return true;
}

// ---------------------------------------------------------------------------
// One Euro filter: a low-pass whose cutoff rises with speed, so a hand held
// still stops shaking and a fast swing barely lags.

struct OneEuroParams {
    float minCutoffHz = 1.0f;
    // Cutoff gained per unit of speed (per m/s for positions, per rad/s for rotations).
    float beta = 0.0f;
    float derivativeCutoffHz = 1.0f;
};

inline float SmoothingFactor(float dtSeconds, float cutoffHz) noexcept {
    const float tau = 1.0f / (2.0f * 3.14159265358979f * cutoffHz);
    return 1.0f / (1.0f + tau / dtSeconds);
}

class OneEuroVec3 {
public:
    explicit OneEuroVec3(OneEuroParams params = {}) noexcept : m_params(params) {}

    // The filtered value at `tSeconds`. The first sample, and any sample that
    // does not move time forward, passes through unfiltered and restarts nothing.
    Vec3 Filter(Vec3 value, double tSeconds) noexcept {
        if (!m_primed) {
            m_value = value;
            m_speed = 0.0f;
            m_time = tSeconds;
            m_primed = true;
            return value;
        }
        const float dt = static_cast<float>(tSeconds - m_time);
        if (dt <= 0.0f) {
            return m_value;
        }
        const float speed = Length(value - m_value) / dt;
        m_speed += SmoothingFactor(dt, m_params.derivativeCutoffHz) * (speed - m_speed);
        const float alpha = SmoothingFactor(dt, m_params.minCutoffHz + m_params.beta * m_speed);
        m_value = m_value + (value - m_value) * alpha;
        m_time = tSeconds;
        return m_value;
    }

    void Reset() noexcept { m_primed = false; }

private:
    OneEuroParams m_params;
    bool m_primed = false;
    Vec3 m_value{};
    float m_speed = 0.0f;
    double m_time = 0.0;
};

class OneEuroQuat {
public:
    explicit OneEuroQuat(OneEuroParams params = {}) noexcept : m_params(params) {}

    // The same filter on rotations: the speed is the angle turned per second,
    // and each step slerps towards the sample along the shorter arc (q and -q
    // are one rotation; a sign flip of the input must not read as a spin).
    Quat Filter(Quat value, double tSeconds) noexcept {
        value = Normalized(value);
        if (!m_primed) {
            m_value = value;
            m_speed = 0.0f;
            m_time = tSeconds;
            m_primed = true;
            return value;
        }
        const float dt = static_cast<float>(tSeconds - m_time);
        if (dt <= 0.0f) {
            return m_value;
        }
        const float speed = AngleBetween(m_value, value) / dt;
        m_speed += SmoothingFactor(dt, m_params.derivativeCutoffHz) * (speed - m_speed);
        const float alpha = SmoothingFactor(dt, m_params.minCutoffHz + m_params.beta * m_speed);
        m_value = Slerp(m_value, value, alpha);
        m_time = tSeconds;
        return m_value;
    }

    void Reset() noexcept { m_primed = false; }

private:
    OneEuroParams m_params;
    bool m_primed = false;
    Quat m_value{};
    float m_speed = 0.0f;
    double m_time = 0.0;
};

// ---------------------------------------------------------------------------
// One hand's pose: the holding frame's rotation, shared by aim and grip, and
// their two origins.

struct HandPose {
    Quat rotation{};
    Vec3 aimOrigin{};
    Vec3 gripOrigin{};
};

// Starting values, to be tuned on the headset. ARKit's skeleton shakes by a
// few millimetres at rest, which over the 8 cm from wrist to knuckle is about
// a degree of aim; at 1 Hz that settles, and the beta terms open the filter
// back up within a fraction of a turn.
inline constexpr OneEuroParams kRotationFilter{1.2f, 1.5f, 1.0f};
inline constexpr OneEuroParams kPositionFilter{1.5f, 6.0f, 1.0f};

// Filters a hand's pose once per point in time: the app locates aim and grip,
// sometimes more than once, for the same frame, and each must get the same
// answer, so a time that does not move forward returns the last output.
class PoseSmoother {
public:
    HandPose Update(const HandPose& raw, int64_t timeNanos) noexcept {
        if (m_primed && timeNanos <= m_timeNanos) {
            return m_output;
        }
        const double t = static_cast<double>(timeNanos) * 1.0e-9;
        m_output.rotation = m_rotation.Filter(raw.rotation, t);
        m_output.aimOrigin = m_aim.Filter(raw.aimOrigin, t);
        m_output.gripOrigin = m_grip.Filter(raw.gripOrigin, t);
        m_timeNanos = timeNanos;
        m_primed = true;
        return m_output;
    }

    void Reset() noexcept {
        m_rotation.Reset();
        m_aim.Reset();
        m_grip.Reset();
        m_primed = false;
    }

    bool Primed() const noexcept { return m_primed; }
    const HandPose& Last() const noexcept { return m_output; }
    int64_t LastTimeNanos() const noexcept { return m_timeNanos; }

private:
    OneEuroQuat m_rotation{kRotationFilter};
    OneEuroVec3 m_aim{kPositionFilter};
    OneEuroVec3 m_grip{kPositionFilter};
    HandPose m_output{};
    int64_t m_timeNanos = 0;
    bool m_primed = false;
};

} // namespace PortVr::HandController
