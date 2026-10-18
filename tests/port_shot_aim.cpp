// PortShotAim (port_shot_aim.h): a shot moved off retail's line rejoins it at the assisted aim
// point only when retail's shot was aimed there (#134). The aim target outlives the shot's aim at
// it (after leaving the targeting box or the line of sight, or with assist clamped or off), and
// converging on it then would bend unrelated shots off course.

#include "port_shot_aim.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>

namespace {

constexpr float kMinDepth = 2.f;
constexpr float kPi = 3.14159265f;

void Unit(float x, float y, float z, float out[3]) {
  const float n = std::sqrt(x * x + y * y + z * z);
  out[0] = x / n;
  out[1] = y / n;
  out[2] = z / n;
}

// `dir` turned by `degrees` towards the unit vector `side`, which is at right angles to it.
void Turned(const float dir[3], const float side[3], float degrees, float out[3]) {
  const float r = degrees * kPi / 180.f;
  for (int i = 0; i < 3; ++i) {
    out[i] = dir[i] * std::cos(r) + side[i] * std::sin(r);
  }
}

void At(const float origin[3], const float dir[3], float depth, float out[3]) {
  for (int i = 0; i < 3; ++i) {
    out[i] = origin[i] + dir[i] * depth;
  }
}
} // namespace

int main() {
  // The gun, and the muzzle the shot leaves from, a little off it, as in the game.
  const float gun[3] = {1.f, 2.f, 3.f};
  const float muzzle[3] = {1.25f, 2.3f, 2.85f};
  float target[3];
  float assist[3];
  float side[3];
  float aim[3];
  Unit(0.2f, 1.f, -0.1f, assist);
  Unit(assist[1], -assist[0], 0.f, side);

  // Locked on and assisted: the assist direction points from the gun at the aim point, the shot
  // flies parallel to it from the muzzle, and rejoins retail's line level with the aim point.
  At(gun, assist, 15.f, aim);
  CHECK_MSG(PortShotAim::SameDirection(assist, assist), "the shot took the assist direction");
  CHECK_MSG(PortShotAim::PointsAt(gun, assist, aim, kMinDepth), "an assisted shot is aimed at its target");
  const float depth = PortShotAim::DepthAlong(muzzle, assist, aim);
  CHECK_MSG(std::fabs(depth - PortShotAim::DepthAlong(gun, assist, aim) +
                      PortShotAim::DepthAlong(gun, assist, muzzle)) < 1e-4f,
            "the depth is the aim point's, measured along the shot from the muzzle");

  // Assist's own rounding (a pitched gun) stays inside the slack.
  Turned(assist, side, 1.f, target);
  At(gun, target, 15.f, aim);
  CHECK_MSG(PortShotAim::PointsAt(gun, assist, aim, kMinDepth), "a degree off is still aimed");

  // The transition the review asked about: the aim target is kept while the shot stops being
  // aimed at it (it left the targeting box, assist clamped at its limit). Swept away from the
  // assist direction, the target stops deciding where the shot rejoins as soon as it is out of
  // reach, and does not come back.
  bool wasAimed = true;
  bool switched = false;
  for (float degrees = 0.f; degrees <= 30.f; degrees += 0.25f) {
    Turned(assist, side, degrees, target);
    At(gun, target, 10.f, aim);
    const bool aimed = PortShotAim::PointsAt(gun, assist, aim, kMinDepth);
    CHECK_MSG(!aimed || degrees <= PortShotAim::kMaxAimDegrees + 1e-3f, "a target off the shot's aim is not used");
    CHECK_MSG(!(aimed && !wasAimed), "once off the aim, the target stays out");
    switched = switched || (wasAimed && !aimed);
    wasAimed = aimed;
  }
  CHECK_MSG(switched, "leaving the aim switches to the fallback");

  // Assist clamped vertically: the gun points at its limit, well below a target above it.
  Unit(0.f, 0.f, 1.f, side);
  Unit(side[0] - assist[0] * assist[2], side[1] - assist[1] * assist[2], side[2] - assist[2] * assist[2], side);
  Turned(assist, side, 12.f, target);
  At(gun, target, 10.f, aim);
  CHECK_MSG(!PortShotAim::PointsAt(gun, assist, aim, kMinDepth), "a target the shot was clamped off is not used");

  // The shot did not take the assist direction (fired along the gun while assist was not
  // applied): the target says nothing about the shot, whatever assist points at.
  float own[3];
  Turned(assist, side, 5.f, own);
  CHECK_MSG(!PortShotAim::SameDirection(own, assist), "a shot along the gun is not the assisted one");

  // Behind the gun, nearer than the minimum depth, or not a number: not used.
  At(gun, assist, -6.f, aim);
  CHECK_MSG(!PortShotAim::PointsAt(gun, assist, aim, kMinDepth), "a target behind is not used");
  At(gun, assist, 1.f, aim);
  CHECK_MSG(!PortShotAim::PointsAt(gun, assist, aim, kMinDepth), "a target nearer than the minimum is not used");
  const float bad[3] = {std::nanf(""), 0.f, 0.f};
  CHECK_MSG(!PortShotAim::PointsAt(gun, assist, bad, kMinDepth), "a NaN aim point is not used");

  if (port_test::Failures() == 0) {
    std::printf("port_shot_aim: ok\n");
  }
  return PORT_TEST_RESULT();
}
