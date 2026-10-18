#include "port_actor_shadow.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
  using namespace PortActorShadow;
  // Anchor cycle: 00 01 00 02 00 03 00 04, then repeats.
  const unsigned expect[8] = {0, 1, 0, 2, 0, 3, 0, 4};
  for (unsigned i = 0; i < 24; ++i) {
    REQUIRE(AnchorForUpdate(i) == expect[i % 8]);
  }
  // Opacity: full when the top touches the floor, none at the max height, scaled by the user alpha.
  REQUIRE(Near(Opacity(5.f, 5.f, 20.f, 1.f), 1.f));
  REQUIRE(Near(Opacity(15.f, 5.f, 20.f, 1.f), 0.5f));
  REQUIRE(Near(Opacity(30.f, 5.f, 20.f, 0.7f), 0.f));
  REQUIRE(Near(Opacity(5.f, 5.f, 20.f, 0.7f), 0.7f));
  REQUIRE(Near(Opacity(0.f, 5.f, 20.f, 1.f), 1.f));  // a hit above the origin clamps
  // Receiver radius: half the diagonal, scaled and expanded by 1.5.
  REQUIRE(Near(ReceiverRadius(3.f, 4.f, 1.f), 2.5f * 1.5f));
  REQUIRE(Near(ReceiverRadius(3.f, 4.f, 2.f), 2.5f * 3.f));
  // Bounds gate: 0.05 per corner.
  const float a[6] = {0, 0, 0, 1, 1, 1};
  float b[6] = {0.04f, 0, 0, 1, 1, 1.04f};
  REQUIRE(!BoundsMoved(a, b));
  b[3] = 1.06f;
  REQUIRE(BoundsMoved(a, b));
  // Receiver box: hits -1..3 plus 0.25, half height at least 0.5.
  const Box box = ReceiverBox(2.f, 3.f, -1.f, 3.f, 4.f);
  REQUIRE(Near(box.centre[0], 2.f) && Near(box.centre[1], 3.f) && Near(box.centre[2], 1.f));
  REQUIRE(Near(box.half[0], 4.f) && Near(box.half[1], 4.f) && Near(box.half[2], 2.25f));
  const Box flat = ReceiverBox(0.f, 0.f, 1.f, 1.f, 1.f);
  REQUIRE(Near(flat.half[2], kMinHalfHeight));
  // Fade: 0.5 starts at the middle of the box, zero falls back to 4 m from the bottom.
  float h0, inv;
  Fade(box, 0.5f, h0, inv);
  REQUIRE(Near(h0, 1.f) && Near(inv, 1.f / 2.25f));
  Fade(box, 0.f, h0, inv);
  REQUIRE(Near(h0, -1.25f) && Near(inv, 0.25f));
  // Planes: the box centre is inside all six, a point beyond x is outside.
  Plane planes[6];
  BoxPlanes(box, planes);
  const float c[3] = {2.f, 3.f, 1.f}, o[3] = {7.f, 3.f, 1.f};
  bool inC = true, inO = true;
  for (const Plane& p : planes) {
    inC = inC && p.n[0] * c[0] + p.n[1] * c[1] + p.n[2] * c[2] + p.d >= 0.f;
    inO = inO && p.n[0] * o[0] + p.n[1] * o[1] + p.n[2] * o[2] + p.d >= 0.f;
  }
  REQUIRE(inC && !inO);
  // Clip planes: the map's centre is clip (0, 0), its top z = 0 and its bottom z = 1.
  const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  float clip[16];
  CasterClip(identity, box, clip);
  const auto eval = [&](int row, float x, float y, float z) {
    return clip[row * 4] * x + clip[row * 4 + 1] * y + clip[row * 4 + 2] * z + clip[row * 4 + 3];
  };
  REQUIRE(Near(eval(0, 2.f, 3.f, 1.f), 0.f) && Near(eval(0, 6.f, 3.f, 1.f), 1.f));
  REQUIRE(Near(eval(1, 2.f, -1.f, 1.f), -1.f));
  REQUIRE(Near(eval(2, 2.f, 3.f, 3.25f), 0.f) && Near(eval(2, 2.f, 3.f, -1.25f), 1.f));
  REQUIRE(Near(clip[15], 1.f) && Near(clip[12], 0.f));
  // A view translation moves the planes with it: x_view = x_world + 5.
  const float shifted[3][4] = {{1, 0, 0, 5}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  CasterClip(shifted, box, clip);
  REQUIRE(Near(eval(0, 7.f, 3.f, 1.f), 0.f));
  // Decal params: opacity, layer, normal threshold, world up in view space.
  float params[kParamFloats];
  DecalParams(identity, box, box, 0.6f, 0.5f, 3, params);
  REQUIRE(Near(params[40], 0.6f) && Near(params[43], 3.f) && Near(params[47], kNormalThreshold));
  REQUIRE(Near(params[44], 0.f) && Near(params[46], 1.f));
  REQUIRE(Near(params[14], 1.f));  // the height row is world z
  std::puts("port_actor_shadow: ok");
  return 0;
}
