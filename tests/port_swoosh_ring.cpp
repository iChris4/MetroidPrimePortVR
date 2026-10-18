#include "port_swoosh_ring.h"
#include "port_test.h"

#include <cstdio>

namespace {

bool Near(float a, float b) { return a > b - 0.0001f && a < b + 0.0001f; }
bool Near(const PortSwooshRing::Vec& a, const PortSwooshRing::Vec& b) {
  return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z);
}
} // namespace

int main() {
  using namespace PortSwooshRing;
  // Trail along +Y (older at +Y), camera on -Z: A x Z = (0,0,-1) x (0,1,0) = (1,0,0).
  const Vec newer{0.f, 0.f, 0.f}, pos{0.f, 1.f, 0.f}, older{0.f, 2.f, 0.f};
  const Vec cam{0.f, 1.f, -5.f};
  Vec axis = WidthAxis(pos, cam, &older, &newer);
  CHECK_MSG(Near(axis, Vec{1.f, 0.f, 0.f}), "central difference axis");
  Vec v0, v1;
  Ring(pos, axis, 0.5f, 2.f, v0, v1);
  CHECK_MSG(Near(v0, Vec{2.f, 1.f, 0.f}), "angle 0 uses RRAD on +axis");
  CHECK_MSG(Near(v1, Vec{-0.5f, 1.f, 0.f}), "angle 180 uses LRAD on -axis");
  // No sign continuity: the camera on the other side flips the axis.
  axis = WidthAxis(pos, Vec{0.f, 1.f, 5.f}, &older, &newer);
  CHECK_MSG(Near(axis, Vec{-1.f, 0.f, 0.f}), "camera side flips the axis");
  // One-sided ends.
  axis = WidthAxis(newer, Vec{0.f, 0.f, -5.f}, &pos, nullptr);
  CHECK_MSG(Near(axis, Vec{1.f, 0.f, 0.f}), "newest point uses older - this");
  axis = WidthAxis(older, Vec{0.f, 2.f, -5.f}, nullptr, &pos);
  CHECK_MSG(Near(axis, Vec{1.f, 0.f, 0.f}), "oldest point uses this - newer");
  // Fallbacks give the identity rotation (+X).
  CHECK_MSG(Near(WidthAxis(pos, cam, nullptr, nullptr), Vec{1.f, 0.f, 0.f}), "no neighbour is identity");
  CHECK_MSG(Near(WidthAxis(pos, Vec{0.f, 9.f, 0.f}, &older, &newer), Vec{1.f, 0.f, 0.f}),
            "degenerate cross is identity");
  axis = WidthAxis(pos, Vec{0.f, 1.f, -5.f}, &pos, &pos);
  CHECK_MSG(Near(axis, Vec{1.f, 0.f, 0.f}), "unnormalisable direction uses forward vector");
  // |v|^2 = 1e-20 is above the 1e-28 direction threshold, so Direction still normalises it.
  Vec tiny;
  CHECK_MSG(Direction(Vec{1e-10f, 0.f, 0.f}, nullptr, &newer, tiny) && Near(tiny, Vec{1.f, 0.f, 0.f}),
            "tiny direction normalises");
  // Cross product 1e-9 (|v|^2 = 1e-18 < 2^-46) is degenerate: fallback +X, not the -X cross.
  axis = WidthAxis(Vec{0.f, 0.f, 1.f}, Vec{0.f, -1e-9f, 1.f}, nullptr, &newer);
  CHECK_MSG(Near(axis, Vec{1.f, 0.f, 0.f}), "tiny cross product falls back to +X");
  if (port_test::Failures() == 0) {
    std::printf("port_swoosh_ring: ok\n");
  }
  return PORT_TEST_RESULT();
}
