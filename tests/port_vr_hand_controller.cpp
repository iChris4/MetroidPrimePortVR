// The hand-tracked controller (platform/include/vr/vr_hand_controller.h): the
// Joy-Con pair's button map, the frame of a fist on an upright controller, and
// the One Euro pose filter.

#include "vr/vr_hand_controller.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "vr hand controller regression failed: %s\n", what);
    std::abort();
  }
}

using namespace PortVr::HandController;

bool Near(float a, float b, float tolerance = 1.0e-4f) { return std::fabs(a - b) <= tolerance; }
bool Near(Vec3 a, Vec3 b, float tolerance = 1.0e-4f) {
  return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance);
}
} // namespace

int main() {
  // --- Buttons ---------------------------------------------------------------
  {
    JoyConPairButtons pad;
    auto hands = MapJoyConPair(pad);
    Check(hands[0].trigger == 0.0f && !hands[0].primary && !hands[1].secondary, "a pair at rest presses nothing");

    pad.zr = true;
    pad.r = true;
    pad.a = true;
    hands = MapJoyConPair(pad);
    Check(hands[1].trigger == 1.0f && hands[1].squeeze == 1.0f && hands[1].primary, "ZR, R and A are the right hand's");
    Check(hands[0].trigger == 0.0f && hands[0].squeeze == 0.0f && !hands[0].primary, "and not the left hand's");

    pad = {};
    pad.b = true;
    hands = MapJoyConPair(pad);
    Check(hands[1].secondary && !hands[1].primary, "B is the right hand's Circle");

    pad = {};
    pad.zl = true;
    pad.l = true;
    pad.minus = true;
    hands = MapJoyConPair(pad);
    Check(hands[0].trigger == 1.0f && hands[0].squeeze == 1.0f && hands[0].menu, "ZL, L and minus are the left hand's");
    Check(!hands[1].menu, "the right hand has no menu");

    pad = {};
    pad.dpadDown = true;
    Check(MapJoyConPair(pad)[0].primary, "Down is Square (Morph Ball)");
    pad = {};
    pad.dpadLeft = true;
    Check(MapJoyConPair(pad)[0].primary, "Left is Square too");
    pad = {};
    pad.dpadUp = true;
    Check(MapJoyConPair(pad)[0].secondary, "Up is Triangle (pause)");
    pad = {};
    pad.plus = true;
    Check(MapJoyConPair(pad)[0].secondary && !MapJoyConPair(pad)[1].secondary, "plus pauses as the left Triangle");

    pad = {};
    pad.dpadRight = true;
    pad.x = true;
    hands = MapJoyConPair(pad);
    Check(!hands[0].primary && !hands[0].secondary && !hands[1].primary && !hands[1].secondary && !hands[0].menu,
          "Right and X do nothing");
    pad = {};
    pad.y = true;
    hands = MapJoyConPair(pad);
    Check(hands[0].primary && !hands[0].secondary && !hands[1].primary && !hands[1].secondary,
          "Y is Square (Morph Ball)");

    pad = {};
    pad.leftStickX = -0.5f;
    pad.leftStickY = 0.25f;
    pad.rightStickX = 0.75f;
    pad.rightStickY = -1.0f;
    pad.leftStickClick = true;
    hands = MapJoyConPair(pad);
    Check(hands[0].thumbstickX == -0.5f && hands[0].thumbstickY == 0.25f && hands[0].thumbstickClick,
          "the left stick is the left hand's");
    Check(hands[1].thumbstickX == 0.75f && hands[1].thumbstickY == -1.0f && !hands[1].thumbstickClick,
          "the right stick is the right hand's");
  }

  // --- Holding frame ---------------------------------------------------------
  {
    // A right fist in front of the body, forearm pointing ahead (-Z), thumb up:
    // the index knuckle 9 cm ahead of the wrist, the little knuckle 6 cm below it.
    const Vec3 wrist{0.20f, 1.00f, -0.30f};
    const Vec3 index{0.20f, 1.00f, -0.39f};
    const Vec3 little{0.20f, 0.94f, -0.38f};
    Frame frame;
    Check(HoldingFrame(wrist, index, little, 0.0f, frame), "a fist has a frame");
    Check(Near(frame.back, {0.0f, 0.0f, 1.0f}), "it aims along the forearm");
    Check(Near(frame.up, {0.0f, 1.0f, 0.0f}), "its up is the thumb's side");
    Check(Near(frame.right, {1.0f, 0.0f, 0.0f}), "and its right the wearer's right: no roll");

    // The left fist is the right one mirrored across the body (x -> -x): the
    // same frame, without a mirrored formula.
    Frame left;
    Check(HoldingFrame({-0.20f, 1.00f, -0.30f}, {-0.20f, 1.00f, -0.39f}, {-0.20f, 0.94f, -0.38f}, 0.0f, left),
          "the left fist has a frame");
    Check(Near(left.back, frame.back) && Near(left.up, frame.up) && Near(left.right, frame.right),
          "the left fist's frame is the right's");

    // The frame is right-handed and orthonormal.
    Check(Near(Cross(frame.right, frame.up), frame.back), "right x up = back");

    // A fist turned 30 degrees to the left keeps no roll and turns its aim.
    const float yaw = 30.0f * 3.14159265f / 180.0f;
    const Vec3 ahead{-std::sin(yaw), 0.0f, -std::cos(yaw)};
    Frame turned;
    Check(HoldingFrame(wrist, wrist + ahead * 0.09f, wrist + ahead * 0.08f + Vec3{0.0f, -0.06f, 0.0f}, 0.0f, turned),
          "a turned fist has a frame");
    Check(Near(-turned.back, ahead) && Near(turned.up, {0.0f, 1.0f, 0.0f}), "it aims where it turned, upright");

    // The pitch offset tilts the aim up (positive) about right.
    Frame pitched;
    Check(HoldingFrame(wrist, index, little, 30.0f, pitched), "a pitched frame");
    const float s = std::sin(30.0f * 3.14159265f / 180.0f);
    const float c = std::cos(30.0f * 3.14159265f / 180.0f);
    Check(Near(-pitched.back, {0.0f, s, -c}), "positive pitch aims higher");
    Check(Near(pitched.right, frame.right), "pitch keeps right");

    // A slightly crooked grip (knuckles leaning forward) still stands upright.
    Frame crooked;
    Check(HoldingFrame(wrist, index, {0.20f, 0.94f, -0.43f}, 0.0f, crooked), "a crooked fist has a frame");
    Check(Near(crooked.back, frame.back) && Near(crooked.up, frame.up), "up is squared to forward");

    Frame none;
    Check(!HoldingFrame(wrist, wrist, little, 0.0f, none), "coincident wrist and knuckle have no frame");
    Check(!HoldingFrame(wrist, index, wrist + (index - wrist) * 0.5f, 0.0f, none),
          "knuckles in line with the forearm have no frame");

    // The quaternion of the frame rotates OpenXR's forward onto the aim.
    const Quat q = QuatFromBasis(turned.right, turned.up, turned.back);
    Check(Near(Rotate(q, {0.0f, 0.0f, -1.0f}), ahead), "the frame's quaternion aims ahead");
    Check(Near(Rotate(q, {0.0f, 1.0f, 0.0f}), turned.up), "and keeps its up");
  }

  // --- One Euro --------------------------------------------------------------
  {
    OneEuroVec3 still(kPositionFilter);
    for (int i = 0; i < 90; ++i) {
      const Vec3 out = still.Filter({1.0f, 2.0f, 3.0f}, i / 90.0);
      Check(Near(out, {1.0f, 2.0f, 3.0f}), "a still input stays put");
    }

    OneEuroVec3 step(kPositionFilter);
    step.Filter({0.0f, 0.0f, 0.0f}, 0.0);
    Vec3 out{};
    out = step.Filter({0.1f, 0.0f, 0.0f}, 1.0 / 90.0);
    Check(out.x > 0.0f && out.x < 0.1f, "a step is smoothed");
    for (int i = 2; i < 180; ++i) {
      out = step.Filter({0.1f, 0.0f, 0.0f}, i / 90.0);
    }
    Check(Near(out.x, 0.1f, 1.0e-3f), "and converges within two seconds");

    // Jitter of 2 mm at rest is mostly taken out.
    OneEuroVec3 jitter(kPositionFilter);
    float worst = 0.0f;
    for (int i = 0; i < 270; ++i) {
      const float noise = (i % 2 == 0) ? 0.002f : -0.002f;
      const Vec3 o = jitter.Filter({noise, 0.0f, 0.0f}, i / 90.0);
      if (i > 90) {
        worst = std::fmax(worst, std::fabs(o.x));
      }
    }
    Check(worst < 0.0007f, "millimetre jitter at rest is damped");

    // A fast move lags less than a slow filter would: beta opens the cutoff.
    OneEuroVec3 fast(kPositionFilter);
    OneEuroVec3 slow(OneEuroParams{kPositionFilter.minCutoffHz, 0.0f, 1.0f});
    Vec3 fastOut{}, slowOut{};
    for (int i = 0; i <= 18; ++i) {
      const Vec3 at{i * 0.01f, 0.0f, 0.0f}; // 0.9 m/s
      fastOut = fast.Filter(at, i / 90.0);
      slowOut = slow.Filter(at, i / 90.0);
    }
    Check(0.18f - fastOut.x < 0.5f * (0.18f - slowOut.x), "speed opens the filter");

    OneEuroVec3 same(kPositionFilter);
    same.Filter({0.0f, 0.0f, 0.0f}, 0.0);
    const Vec3 first = same.Filter({0.1f, 0.0f, 0.0f}, 0.01);
    const Vec3 again = same.Filter({0.5f, 0.0f, 0.0f}, 0.01);
    Check(Near(first, again), "a sample at the same time changes nothing");
  }
  {
    OneEuroQuat rotation(kRotationFilter);
    const float half = 0.5f * 20.0f * 3.14159265f / 180.0f;
    const Quat turned{0.0f, std::sin(half), 0.0f, std::cos(half)};
    rotation.Filter({}, 0.0);
    Quat out{};
    for (int i = 1; i < 270; ++i) {
      out = rotation.Filter(turned, i / 90.0);
    }
    Check(AngleBetween(out, turned) < 0.2f * 3.14159265f / 180.0f, "a turn converges");

    // q and -q are one rotation: alternating signs must not read as motion.
    OneEuroQuat flips(kRotationFilter);
    flips.Filter(turned, 0.0);
    for (int i = 1; i < 90; ++i) {
      const Quat q = (i % 2 == 0) ? turned : Quat{-turned.x, -turned.y, -turned.z, -turned.w};
      out = flips.Filter(q, i / 90.0);
      Check(AngleBetween(out, turned) < 1.0e-3f, "a sign flip is no spin");
    }
  }
  {
    PoseSmoother smoother;
    HandPose raw;
    raw.aimOrigin = {0.1f, 1.0f, -0.4f};
    raw.gripOrigin = {0.1f, 0.95f, -0.35f};
    const HandPose first = smoother.Update(raw, 1'000'000'000);
    Check(Near(first.aimOrigin, raw.aimOrigin), "the first pose passes through");
    HandPose moved = raw;
    moved.aimOrigin = {0.2f, 1.0f, -0.4f};
    const HandPose second = smoother.Update(moved, 1'011'111'111);
    const HandPose repeat = smoother.Update(raw, 1'011'111'111);
    const HandPose older = smoother.Update(raw, 1'000'000'000);
    Check(Near(second.aimOrigin, repeat.aimOrigin) && Near(second.aimOrigin, older.aimOrigin),
          "aim and grip of one frame get one answer");
    smoother.Reset();
    Check(!smoother.Primed(), "a reset forgets the pose");
    Check(Near(smoother.Update(moved, 2'000'000'000).aimOrigin, moved.aimOrigin), "and starts again from the hand");
  }

  std::puts("vr hand controller: ok");
  return 0;
}
