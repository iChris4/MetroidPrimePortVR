#include "port_pointer_gesture.h"
#include "port_pointer_math.h"
#include "port_test.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

using namespace PortPointerMath;
using PortPointer::Click;
using PortPointer::Device;
using PortPointer::Recognizer;

PortPointerMath::Rect Box(float x0, float y0, float x1, float y1) {
  PortPointerMath::Rect r;
  r.Add(x0, y0);
  r.Add(x1, y1);
  return r;
}
} // namespace

int main() {
  // Present fit: a 640x480 image in a 1920x1080 window is pillarboxed.
  {
    const Viewport v = PresentViewport(1920.f, 1080.f, 640.f, 480.f);
    REQUIRE(Near(v.left, 240.f) && Near(v.top, 0.f) && Near(v.width, 1440.f) && Near(v.height, 1080.f));
    float cx, cy;
    REQUIRE(SurfaceToContent(v, 640.f, 480.f, 960.f, 540.f, cx, cy));
    REQUIRE(Near(cx, 320.f) && Near(cy, 240.f));
    REQUIRE(!SurfaceToContent(v, 640.f, 480.f, 100.f, 540.f, cx, cy));  // left bar
    REQUIRE(Near(cx, 0.f));
    // Letterboxed the other way.
    const Viewport t = PresentViewport(800.f, 1000.f, 640.f, 480.f);
    REQUIRE(Near(t.width, 800.f) && Near(t.height, 600.f) && Near(t.top, 200.f));
    REQUIRE(PresentViewport(0.f, 10.f, 640.f, 480.f).width == 0.f);
  }

  // Eye space to viewport: ortho and perspective.
  {
    Projection ortho;
    ortho.left = -320.f;
    ortho.right = 320.f;
    ortho.top = 224.f;
    ortho.bottom = -224.f;
    const Viewport vp{0.f, 0.f, 640.f, 448.f};
    float px, py;
    REQUIRE(EyeToViewport(ortho, vp, 0.f, 5.f, 0.f, px, py));
    REQUIRE(Near(px, 320.f) && Near(py, 224.f));
    REQUIRE(EyeToViewport(ortho, vp, -320.f, 5.f, 224.f, px, py));
    REQUIRE(Near(px, 0.f) && Near(py, 0.f));

    Projection persp;
    persp.perspective = true;
    persp.znear = 1.f;
    persp.left = -1.f;
    persp.right = 1.f;
    persp.top = 1.f;
    persp.bottom = -1.f;
    const Viewport sq{10.f, 20.f, 100.f, 100.f};
    REQUIRE(EyeToViewport(persp, sq, 2.f, 4.f, -2.f, px, py));  // (0.5, -0.5) on the near plane
    REQUIRE(Near(px, 10.f + 75.f) && Near(py, 20.f + 75.f));
    REQUIRE(!EyeToViewport(persp, sq, 0.f, -1.f, 0.f, px, py));  // behind the eye
  }

  // Hit list: the last drawn wins, owners filter, parents bound their children.
  {
    HitList hits;
    int frame = 10, ownerA = 0, ownerB = 0, parent = 0, w1 = 0, w2 = 0, w3 = 0;
    const void* c1[] = {&w1, &parent};
    const void* c2[] = {&w2, &parent};
    const void* c3[] = {&w3};
    hits.Add(frame, &ownerA, c1, 2, Box(0, 0, 100, 100));
    hits.Add(frame, &ownerA, c2, 2, Box(50, 50, 150, 150));
    hits.Add(frame, &ownerB, c3, 1, Box(60, 60, 70, 70));
    // Input before the next draw sees the list built in frame 10.
    REQUIRE(hits.Pick(frame + 1, nullptr, 65.f, 65.f)->chain[0] == &w3);
    REQUIRE(hits.Pick(frame + 1, &ownerA, 65.f, 65.f)->chain[0] == &w2);
    REQUIRE(hits.Pick(frame + 1, &ownerA, 10.f, 10.f)->chain[0] == &w1);
    REQUIRE(hits.Pick(frame + 1, &ownerA, 200.f, 10.f) == nullptr);
    REQUIRE(HitList::Under(*hits.Pick(frame + 1, &ownerA, 10.f, 10.f), &parent));
    PortPointerMath::Rect r;
    REQUIRE(hits.Bounds(frame + 1, &parent, true, r));
    REQUIRE(Near(r.x0, 0.f) && Near(r.x1, 150.f));
    REQUIRE(!hits.Bounds(frame + 1, &parent, false, r));
    REQUIRE(hits.Bounds(frame + 1, &w1, false, r) && Near(r.x1, 100.f));
    // The last rect a widget drew on its own: a text pane's glyphs, which come
    // after its pane.
    hits.Add(frame, &ownerA, c2, 2, Box(60, 60, 120, 90));
    REQUIRE(hits.LastBounds(frame + 1, &w2, r));
    REQUIRE(Near(r.x0, 60.f) && Near(r.y0, 60.f) && Near(r.x1, 120.f) && Near(r.y1, 90.f));
    REQUIRE(hits.Bounds(frame + 1, &w2, false, r) && Near(r.x0, 50.f) && Near(r.x1, 150.f));
    // Frame 11 starts drawing: lookups keep using frame 10's complete list.
    hits.Add(frame + 1, &ownerA, c3, 1, Box(0, 0, 10, 10));
    REQUIRE(hits.Pick(frame + 1, &ownerA, 65.f, 65.f)->chain[0] == &w2);
    REQUIRE(hits.Pick(frame + 2, &ownerA, 65.f, 65.f) == nullptr);  // now frame 11's list
    REQUIRE(hits.Pick(frame + 2, &ownerA, 5.f, 5.f)->chain[0] == &w3);
    // A frame not drawn for a while has no hits.
    REQUIRE(hits.Pick(frame + 5, nullptr, 5.f, 5.f) == nullptr);
    // Empty rects are dropped.
    hits.Add(frame + 6, &ownerA, c3, 1, PortPointerMath::Rect{});
    REQUIRE(hits.Pick(frame + 7, nullptr, 0.f, 0.f) == nullptr);
    hits.Clear();
    REQUIRE(hits.Pick(frame + 7, nullptr, 0.f, 0.f) == nullptr);
  }

  // Mouse click, double click, and a drag past the slop.
  {
    Recognizer g;
    Click c;
    REQUIRE(!g.Moved());
    g.MouseMove(100.f, 100.f, 0.f);
    REQUIRE(g.Moved() && g.LastDevice() == Device::Mouse);
    g.ClearMoved();
    g.MouseMove(100.f, 100.f, 0.01f);  // an idle cursor doesn't steal the selection
    REQUIRE(!g.Moved());
    g.MouseButton(PortPointer::kButtonLeft, true, 100.f, 100.f, 0.1f);
    g.MouseMove(102.f, 101.f, 0.12f);  // inside the slop
    g.MouseButton(PortPointer::kButtonLeft, false, 102.f, 101.f, 0.15f);
    REQUIRE(g.TakeClick(c) && c.count == 1 && c.device == Device::Mouse && Near(c.x, 102.f));
    REQUIRE(!g.TakeClick(c));
    g.MouseButton(PortPointer::kButtonLeft, true, 103.f, 101.f, 0.3f);
    g.MouseButton(PortPointer::kButtonLeft, false, 103.f, 101.f, 0.32f);
    REQUIRE(g.TakeClick(c) && c.count == 2);
    // A third quick click is a new single click, not a triple.
    g.MouseButton(PortPointer::kButtonLeft, true, 103.f, 101.f, 0.4f);
    g.MouseButton(PortPointer::kButtonLeft, false, 103.f, 101.f, 0.42f);
    REQUIRE(g.TakeClick(c) && c.count == 1);

    g.MouseButton(PortPointer::kButtonLeft, true, 200.f, 200.f, 1.f);
    g.MouseMove(203.f, 200.f, 1.01f);
    REQUIRE(!g.Dragging());
    g.MouseMove(210.f, 200.f, 1.02f);
    REQUIRE(g.Dragging());
    g.MouseMove(215.f, 205.f, 1.03f);
    float dx, dy;
    uint8_t buttons;
    REQUIRE(g.TakeDrag(dx, dy, buttons));
    REQUIRE(Near(dx, 15.f) && Near(dy, 5.f) && buttons == PortPointer::kButtonLeft);
    g.MouseButton(PortPointer::kButtonLeft, false, 215.f, 205.f, 1.05f);
    REQUIRE(!g.TakeClick(c));  // a drag is no click
    REQUIRE(!g.TakeDrag(dx, dy, buttons));
  }

  // Right click is back; a right drag is not; a chord ends with its last button.
  {
    Recognizer g;
    Click c;
    g.MouseButton(PortPointer::kButtonRight, true, 10.f, 10.f, 0.f);
    g.MouseButton(PortPointer::kButtonRight, false, 10.f, 10.f, 0.05f);
    REQUIRE(g.TakeBack() && !g.TakeBack() && !g.TakeClick(c));
    g.MouseButton(PortPointer::kButtonRight, true, 10.f, 10.f, 1.f);
    g.MouseMove(40.f, 10.f, 1.02f);
    g.MouseButton(PortPointer::kButtonRight, false, 40.f, 10.f, 1.05f);
    REQUIRE(!g.TakeBack());
    float dx, dy;
    uint8_t buttons;
    REQUIRE(g.TakeDrag(dx, dy, buttons) && buttons == PortPointer::kButtonRight && Near(dx, 30.f));
    g.MouseButton(PortPointer::kButtonLeft, true, 50.f, 50.f, 2.f);
    g.MouseButton(PortPointer::kButtonRight, true, 50.f, 50.f, 2.01f);
    g.MouseButton(PortPointer::kButtonLeft, false, 50.f, 50.f, 2.02f);
    REQUIRE(!g.TakeClick(c) && !g.TakeBack() && g.Pressed());
    g.MouseButton(PortPointer::kButtonRight, false, 50.f, 50.f, 2.03f);
    REQUIRE(g.TakeBack() && !g.Pressed());
    g.Back(3.f);
    REQUIRE(g.TakeBack());
  }

  // Touch: tap, double tap, slop, and the finger left over after a pinch.
  {
    Recognizer g;
    Click c;
    g.FingerDown(1, 300.f, 300.f, 0.f);
    g.FingerMove(1, 310.f, 300.f, 0.02f);  // inside the touch slop
    g.FingerUp(1, 310.f, 300.f, 0.05f);
    REQUIRE(g.TakeClick(c) && c.device == Device::Touch && c.count == 1);
    REQUIRE(g.LastDevice() == Device::Touch);
    g.FingerDown(2, 312.f, 302.f, 0.2f);
    g.FingerUp(2, 312.f, 302.f, 0.25f);
    REQUIRE(g.TakeClick(c) && c.count == 2);
    // A mouse click right after a tap is not a double.
    g.MouseButton(PortPointer::kButtonLeft, true, 312.f, 302.f, 0.3f);
    g.MouseButton(PortPointer::kButtonLeft, false, 312.f, 302.f, 0.31f);
    REQUIRE(g.TakeClick(c) && c.count == 1);

    g.FingerDown(3, 100.f, 100.f, 1.f);
    g.FingerMove(3, 100.f, 130.f, 1.02f);
    REQUIRE(g.Dragging());
    g.FingerUp(3, 100.f, 130.f, 1.05f);
    float dx, dy;
    uint8_t buttons;
    REQUIRE(!g.TakeClick(c));
    REQUIRE(g.TakeDrag(dx, dy, buttons) && Near(dy, 30.f) && buttons == PortPointer::kButtonLeft);

    // Two fingers: pan by the centroid, pinch by the span; no tap, no drag.
    g.FingerDown(4, 100.f, 100.f, 2.f);
    g.FingerDown(5, 200.f, 100.f, 2.01f);
    REQUIRE(g.FingerCount() == 2 && g.Dragging());
    g.FingerMove(4, 90.f, 110.f, 2.02f);
    g.FingerMove(5, 230.f, 110.f, 2.03f);
    float px, py;
    REQUIRE(g.TakePan(px, py));
    REQUIRE(Near(px, 10.f) && Near(py, 10.f));
    REQUIRE(Near(g.TakePinch(), 140.f / 100.f));
    REQUIRE(Near(g.TakePinch(), 1.f));
    g.FingerUp(4, 90.f, 110.f, 2.05f);
    g.FingerMove(5, 260.f, 160.f, 2.06f);  // the leftover finger doesn't drag
    REQUIRE(!g.TakeDrag(dx, dy, buttons));
    g.FingerUp(5, 260.f, 160.f, 2.08f);
    REQUIRE(!g.TakeClick(c) && g.FingerCount() == 0);

    // A two-finger twist: a quarter turn clockwise on screen, across the +-pi wrap.
    g.TakeTwist();
    g.FingerDown(6, 200.f, 100.f, 3.f);
    g.FingerDown(7, 100.f, 100.f, 3.01f);  // the pair points at pi
    g.FingerMove(7, 150.f, 50.f, 3.02f);   // -3pi/4: +pi/4 through the wrap
    g.FingerMove(6, 150.f, 150.f, 3.03f);  // -pi/2: another +pi/4
    REQUIRE(Near(g.TakeTwist(), 0.5f * 3.14159265f));
    REQUIRE(Near(g.TakeTwist(), 0.f));
    g.FingerUp(6, 150.f, 150.f, 3.05f);
    g.FingerUp(7, 150.f, 50.f, 3.06f);
    REQUIRE(!g.TakeClick(c));

    // A system-cancelled gesture leaves no tap behind.
    g.FingerDown(6, 50.f, 50.f, 3.f);
    g.CancelTouches();
    g.FingerUp(6, 50.f, 50.f, 3.05f);
    REQUIRE(!g.TakeClick(c) && !g.Pressed());
  }

  // Wheel, expiry of untaken results, other devices, reset.
  {
    Recognizer g;
    Click c;
    g.Wheel(1.f, 0.f);
    g.Wheel(-0.5f, 0.01f);
    REQUIRE(Near(g.TakeWheel(), 0.5f) && Near(g.TakeWheel(), 0.f));
    g.MouseButton(PortPointer::kButtonLeft, true, 1.f, 1.f, 1.f);
    g.MouseButton(PortPointer::kButtonLeft, false, 1.f, 1.f, 1.01f);
    g.Wheel(2.f, 1.01f);
    g.Back(1.01f);
    g.Expire(1.1f);
    REQUIRE(g.PeekClick(c) && Near(g.TakeWheel(), 2.f));
    g.Wheel(2.f, 1.2f);
    g.Expire(1.5f);
    REQUIRE(!g.TakeClick(c) && !g.TakeBack() && Near(g.TakeWheel(), 0.f));
    REQUIRE(g.LastDevice() == Device::Mouse);
    g.NoteOtherDevice();
    REQUIRE(g.LastDevice() == Device::None);
    g.MouseButton(PortPointer::kButtonLeft, true, 1.f, 1.f, 2.f);
    g.Reset();
    g.MouseButton(PortPointer::kButtonLeft, false, 1.f, 1.f, 2.01f);
    REQUIRE(!g.TakeClick(c) && !g.Pressed());
  }

  std::puts("port_pointer_tests: ok");
  return 0;
}
