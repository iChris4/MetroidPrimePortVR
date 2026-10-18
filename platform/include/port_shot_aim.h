#pragma once

// Where a shot moved off retail's line rejoins it (CPlayerGun::PortShotFromDrawnMuzzle, #134).
// With the FOV slider off retail the arm cannon is drawn at the retail FOV, so a shot starts out
// at the drawn muzzle instead of the real one and is re-aimed at a point on retail's line. That
// point is the assisted aim point only when retail's shot was aimed at it: the shot took the
// assist direction, and that direction, from the gun, points at the aim point. A target that is
// kept (the aim target lingers after leaving the box or the line of sight) but did not steer the
// shot, because assist was clamped, off or ignored an axis, or the shot fired along the gun,
// says nothing about where the shot goes; converging on it would bend the shot off retail's line.
// Plain floats, so the rule is tested without the game (tests/port_shot_aim.cpp).

#include <cmath>

namespace PortShotAim {

// How far off the aim point a direction may be and still be aimed at it. Assist rebuilds its
// direction from yaw and pitch deltas in the gun's frame, which is exact only with the gun level,
// so a little slack; a target out of assist's reach is off by more than its limits.
inline constexpr float kMaxAimDegrees = 2.f;

// Whether `a` and `b`, unit vectors, are the same direction (the shot took the assist's).
inline bool SameDirection(const float a[3], const float b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] >= 0.99999f;
}

// Whether the line from `origin` along the unit vector `dir` points at `point`: ahead by at least
// `minDepth` and within kMaxAimDegrees of it.
inline bool PointsAt(const float origin[3], const float dir[3], const float point[3], float minDepth) {
  const float to[3] = {point[0] - origin[0], point[1] - origin[1], point[2] - origin[2]};
  const float along = to[0] * dir[0] + to[1] * dir[1] + to[2] * dir[2];
  const float length = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
  if (!(along >= minDepth) || !(length > 0.f)) {
    return false;
  }
  return along / length >= std::cos(kMaxAimDegrees * 3.14159265f / 180.f);
}

// How far along the shot's line (from `from` along the unit vector `dir`) it is level with
// `point`, the depth at which it rejoins retail's line.
inline float DepthAlong(const float from[3], const float dir[3], const float point[3]) {
  return (point[0] - from[0]) * dir[0] + (point[1] - from[1]) * dir[1] + (point[2] - from[2]) * dir[2];
}

} // namespace PortShotAim
