#include "port_pointer.h"

#include "GuiSys/CGuiWidget.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"

#include <SDL3/SDL.h>

#include <cstdio>

namespace PortPointer {
namespace {

bool sEnabled = true;
bool sMapSwapFingers = false;
Recognizer sGestures;
SDL_WindowID sWindowId = 0;
double sScreenActiveTime = -1.0;
bool sScriptMoved = false;
// A console "down … touch" holds the script finger until "up".
bool sScriptFingerDown = false;
// Scripted fingers use ids no real touch screen hands out.
constexpr uint64_t kScriptFinger0 = 0xfffffffffff00000ull;
constexpr uint64_t kScriptFinger1 = kScriptFinger0 + 1;

double Now() { return static_cast< double >(SDL_GetTicksNS()) * 1e-9; }
float NowF() { return static_cast< float >(Now()); }

SDL_Window* Window() {
  SDL_Window* window = sWindowId != 0 ? SDL_GetWindowFromID(sWindowId) : nullptr;
  if (window == nullptr) {
    window = SDL_GetMouseFocus();
  }
  if (window == nullptr) {
    window = SDL_GetKeyboardFocus();
  }
  return window;
}

void NoteWindow(SDL_WindowID id) {
  if (id != 0) {
    sWindowId = id;
  }
}

// The window in points and pixels, and where Aurora presents the EFB in it.
struct Layout {
  float pointsW = 0.f, pointsH = 0.f;
  float pixelsW = 0.f, pixelsH = 0.f;
  float efbW = 0.f, efbH = 0.f;
  PortPointerMath::Viewport present;
};

bool GetLayout(Layout& out) {
  SDL_Window* window = Window();
  if (window == nullptr) {
    return false;
  }
  int pw = 0, ph = 0, xw = 0, xh = 0;
  if (!SDL_GetWindowSize(window, &pw, &ph) || !SDL_GetWindowSizeInPixels(window, &xw, &xh) ||
      pw <= 0 || ph <= 0 || xw <= 0 || xh <= 0) {
    return false;
  }
  const GXRenderModeObj& mode = CGraphics::GetRenderMode();
  if (mode.fbWidth == 0 || mode.efbHeight == 0) {
    return false;
  }
  out.pointsW = static_cast< float >(pw);
  out.pointsH = static_cast< float >(ph);
  out.pixelsW = static_cast< float >(xw);
  out.pixelsH = static_cast< float >(xh);
  out.efbW = static_cast< float >(mode.fbWidth);
  out.efbH = static_cast< float >(mode.efbHeight);
  out.present = PortPointerMath::PresentViewport(out.pixelsW, out.pixelsH, out.efbW, out.efbH);
  return out.present.width > 0.f;
}

void WindowPoint(SDL_WindowID id, float nx, float ny, float& x, float& y) {
  SDL_Window* window = id != 0 ? SDL_GetWindowFromID(id) : Window();
  int w = 0, h = 0;
  if (window != nullptr) {
    SDL_GetWindowSize(window, &w, &h);
  }
  x = nx * static_cast< float >(w);
  y = ny * static_cast< float >(h);
}

uint8_t ButtonBit(Uint8 sdlButton) {
  switch (sdlButton) {
  case SDL_BUTTON_LEFT:
    return kButtonLeft;
  case SDL_BUTTON_RIGHT:
    return kButtonRight;
  case SDL_BUTTON_MIDDLE:
    return kButtonMiddle;
  default:
    return 0;
  }
}

bool SynthesizedMouse(SDL_MouseID which) { return which == SDL_TOUCH_MOUSEID || which == SDL_PEN_MOUSEID; }

} // namespace

bool Enabled() { return sEnabled; }
bool MapSwapFingers() { return sMapSwapFingers; }
void SetMapSwapFingers(bool swap) { sMapSwapFingers = swap; }
void SetEnabled(bool enabled) {
  if (sEnabled != enabled) {
    sGestures.Reset();
  }
  sEnabled = enabled;
}

Recognizer& Gestures() { return sGestures; }

void HandleEvent(const SDL_Event& e) {
  const float t = NowF();
  switch (e.type) {
  case SDL_EVENT_MOUSE_MOTION:
    if (!SynthesizedMouse(e.motion.which)) {
      NoteWindow(e.motion.windowID);
      sGestures.MouseMove(e.motion.x, e.motion.y, t);
    }
    break;
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP:
    if (!SynthesizedMouse(e.button.which)) {
      if (const uint8_t bit = ButtonBit(e.button.button)) {
        NoteWindow(e.button.windowID);
        sGestures.MouseButton(bit, e.button.down, e.button.x, e.button.y, t);
      }
    }
    break;
  case SDL_EVENT_MOUSE_WHEEL:
    if (!SynthesizedMouse(e.wheel.which)) {
      const float notches = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
      sGestures.Wheel(notches, t);
    }
    break;
  case SDL_EVENT_FINGER_DOWN:
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_UP: {
    NoteWindow(e.tfinger.windowID);
    float x = 0.f, y = 0.f;
    WindowPoint(e.tfinger.windowID, e.tfinger.x, e.tfinger.y, x, y);
    if (e.type == SDL_EVENT_FINGER_DOWN) {
      sGestures.FingerDown(e.tfinger.fingerID, x, y, t);
    } else if (e.type == SDL_EVENT_FINGER_MOTION) {
      sGestures.FingerMove(e.tfinger.fingerID, x, y, t);
    } else {
      sGestures.FingerUp(e.tfinger.fingerID, x, y, t);
    }
    break;
  }
  case SDL_EVENT_FINGER_CANCELED:
    sGestures.CancelTouches();
    break;
  case SDL_EVENT_KEY_DOWN:
    if (!e.key.repeat) {
      sGestures.NoteOtherDevice();
    }
    break;
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
  case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    sGestures.NoteOtherDevice();
    break;
  case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    if (e.gaxis.value > 16000 || e.gaxis.value < -16000) {
      sGestures.NoteOtherDevice();
    }
    break;
  case SDL_EVENT_WINDOW_FOCUS_LOST:
    sGestures.Reset();
    break;
  default:
    break;
  }
}

// Runs before the frame's events, so Moved() means "moved this frame".
void BeginFrame() {
  sGestures.Expire(NowF());
  // A console move lands between frames; let the next frame see it.
  if (sScriptMoved) {
    sScriptMoved = false;
  } else {
    sGestures.ClearMoved();
  }
}

void Reset() { sGestures.Reset(); }

bool WindowToGui(float wx, float wy, float& gx, float& gy) {
  Layout l;
  if (!GetLayout(l)) {
    gx = gy = 0.f;
    return false;
  }
  const float sx = wx * l.pixelsW / l.pointsW;
  const float sy = wy * l.pixelsH / l.pointsH;
  return PortPointerMath::SurfaceToContent(l.present, l.efbW, l.efbH, sx, sy, gx, gy);
}

bool GuiToWindow(float gx, float gy, float& wx, float& wy) {
  Layout l;
  if (!GetLayout(l)) {
    wx = wy = 0.f;
    return false;
  }
  const float sx = l.present.left + gx / l.efbW * l.present.width;
  const float sy = l.present.top + gy / l.efbH * l.present.height;
  wx = sx * l.pointsW / l.pixelsW;
  wy = sy * l.pointsH / l.pixelsH;
  return true;
}

bool Position(float& gx, float& gy) {
  if (!sGestures.HasPosition()) {
    gx = gy = 0.f;
    return false;
  }
  return WindowToGui(sGestures.X(), sGestures.Y(), gx, gy);
}

float GuiScale() {
  Layout l;
  if (!GetLayout(l)) {
    return 1.f;
  }
  return l.efbH / l.present.height * l.pixelsH / l.pointsH;
}

bool MouseIsLast() { return sGestures.LastDevice() == Device::Mouse; }
bool TouchIsLast() { return sGestures.LastDevice() == Device::Touch; }

void NoteScreenActive() { sScreenActiveTime = Now(); }
bool ScreenActive() { return sEnabled && sScreenActiveTime >= 0.0 && Now() - sScreenActiveTime < 0.3; }

void ScriptMove(float gx, float gy) {
  float x = 0.f, y = 0.f;
  GuiToWindow(gx, gy, x, y);
  if (sScriptFingerDown) {
    sGestures.FingerMove(kScriptFinger0, x, y, NowF());
  } else {
    sGestures.MouseMove(x, y, NowF());
  }
  sScriptMoved = true;
}

void ScriptPress(float gx, float gy, bool down, uint8_t buttons, bool touch) {
  float x = 0.f, y = 0.f;
  GuiToWindow(gx, gy, x, y);
  const float t = NowF();
  if (touch) {
    if (down) {
      sGestures.FingerDown(kScriptFinger0, x, y, t);
    } else {
      sGestures.FingerUp(kScriptFinger0, x, y, t);
    }
    sScriptFingerDown = down;
    return;
  }
  sGestures.MouseMove(x, y, t);
  for (uint8_t bit = 1; bit <= kButtonMiddle; bit <<= 1) {
    if (buttons & bit) sGestures.MouseButton(bit, down, x, y, t);
  }
}

void ScriptClick(float gx, float gy, bool doubleClick, bool touch) {
  float x = 0.f, y = 0.f;
  GuiToWindow(gx, gy, x, y);
  const float t = NowF();
  for (int i = 0; i < (doubleClick ? 2 : 1); ++i) {
    if (touch) {
      sGestures.FingerDown(kScriptFinger0, x, y, t);
      sGestures.FingerUp(kScriptFinger0, x, y, t);
    } else {
      sGestures.MouseMove(x, y, t);
      sGestures.MouseButton(kButtonLeft, true, x, y, t);
      sGestures.MouseButton(kButtonLeft, false, x, y, t);
    }
  }
}

void ScriptDrag(float gx0, float gy0, float gx1, float gy1, uint8_t buttons, bool touch) {
  float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
  GuiToWindow(gx0, gy0, x0, y0);
  GuiToWindow(gx1, gy1, x1, y1);
  const float t = NowF();
  constexpr int kSteps = 8;
  if (touch) {
    sGestures.FingerDown(kScriptFinger0, x0, y0, t);
  } else {
    sGestures.MouseMove(x0, y0, t);
    for (uint8_t bit = 1; bit <= kButtonMiddle; bit <<= 1) {
      if (buttons & bit) sGestures.MouseButton(bit, true, x0, y0, t);
    }
  }
  for (int i = 1; i <= kSteps; ++i) {
    const float f = static_cast< float >(i) / kSteps;
    const float x = x0 + (x1 - x0) * f, y = y0 + (y1 - y0) * f;
    if (touch) {
      sGestures.FingerMove(kScriptFinger0, x, y, t);
    } else {
      sGestures.MouseMove(x, y, t);
    }
  }
  if (touch) {
    sGestures.FingerUp(kScriptFinger0, x1, y1, t);
  } else {
    for (uint8_t bit = 1; bit <= kButtonMiddle; bit <<= 1) {
      if (buttons & bit) sGestures.MouseButton(bit, false, x1, y1, t);
    }
  }
}

void ScriptWheel(float notches) { sGestures.Wheel(notches, NowF()); }

void ScriptPinch(float gx, float gy, float ratio) {
  float x = 0.f, y = 0.f;
  GuiToWindow(gx, gy, x, y);
  const float t = NowF();
  constexpr float kHalfSpan = 50.f;
  sGestures.FingerDown(kScriptFinger0, x - kHalfSpan, y, t);
  sGestures.FingerDown(kScriptFinger1, x + kHalfSpan, y, t);
  sGestures.FingerMove(kScriptFinger0, x - kHalfSpan * ratio, y, t);
  sGestures.FingerMove(kScriptFinger1, x + kHalfSpan * ratio, y, t);
  sGestures.FingerUp(kScriptFinger1, x + kHalfSpan * ratio, y, t);
  sGestures.FingerUp(kScriptFinger0, x - kHalfSpan * ratio, y, t);
}

void ScriptBack() { sGestures.Back(NowF()); }

} // namespace PortPointer

namespace PortGuiHit {
namespace {

PortPointerMath::HitList sHits;

const void* Key(const CGuiObject* object) { return object; }

// The current CGraphics projection and viewport.
void CurrentProjection(PortPointerMath::Projection& proj, PortPointerMath::Viewport& vp) {
  const CGraphics::CProjectionState& ps = CGraphics::GetProjectionState();
  proj.perspective = ps.IsPerspective();
  proj.left = ps.GetLeft();
  proj.right = ps.GetRight();
  proj.top = ps.GetTop();
  proj.bottom = ps.GetBottom();
  proj.znear = ps.GetNear();
  proj.zfar = ps.GetFar();
  const CViewport& v = CGraphics::GetViewport();
  vp = {static_cast< float >(v.mLeft), static_cast< float >(v.mTop), static_cast< float >(v.mWidth),
        static_cast< float >(v.mHeight)};
}

void RecordCorners(const CGuiWidget* widget, const CTransform4f& model, const CVector3f* points,
                   int count) {
  PortPointerMath::Projection proj;
  PortPointerMath::Viewport vp;
  CurrentProjection(proj, vp);
  const CTransform4f toEye = CGraphics::GetViewMatrix().GetInverse() * model;

  PortPointerMath::Rect rect;
  for (int i = 0; i < count; ++i) {
    const CVector3f eye = toEye * points[i];
    float px = 0.f, py = 0.f;
    if (!PortPointerMath::EyeToViewport(proj, vp, eye.GetX(), eye.GetY(), eye.GetZ(), px, py)) {
      return;
    }
    rect.Add(px, py);
  }
  // A widget never reaches outside its viewport on screen.
  rect.x0 = std::max(rect.x0, vp.left);
  rect.y0 = std::max(rect.y0, vp.top);
  rect.x1 = std::min(rect.x1, vp.left + vp.width);
  rect.y1 = std::min(rect.y1, vp.top + vp.height);

  const void* chain[PortPointerMath::HitList::kMaxDepth];
  int depth = 0;
  for (const CGuiObject* o = widget; o != nullptr && depth < PortPointerMath::HitList::kMaxDepth;
       o = o->GetParent()) {
    chain[depth++] = Key(o);
  }
  sHits.Add(CGraphics::GetFrameCounter(), widget->GetParentFrame(), chain, depth, rect);
}

} // namespace

void Record(const CGuiWidget* widget, const CTransform4f& model, const CVector3f& localMin,
            const CVector3f& localMax) {
  if (widget == nullptr || !PortPointer::Enabled()) {
    return;
  }
  CVector3f corners[8];
  for (int i = 0; i < 8; ++i) {
    corners[i] = CVector3f((i & 1) ? localMax.GetX() : localMin.GetX(),
                           (i & 2) ? localMax.GetY() : localMin.GetY(),
                           (i & 4) ? localMax.GetZ() : localMin.GetZ());
  }
  RecordCorners(widget, model, corners, 8);
}

bool Project(const CTransform4f& model, const CVector3f& point, float& gx, float& gy) {
  PortPointerMath::Projection proj;
  PortPointerMath::Viewport vp;
  CurrentProjection(proj, vp);
  const CVector3f eye = CGraphics::GetViewMatrix().GetInverse() * model * point;
  return PortPointerMath::EyeToViewport(proj, vp, eye.GetX(), eye.GetY(), eye.GetZ(), gx, gy);
}

void RecordPoints(const CGuiWidget* widget, const CTransform4f& model, const float* xyz, int count) {
  if (widget == nullptr || xyz == nullptr || count <= 0 || !PortPointer::Enabled()) {
    return;
  }
  constexpr int kMax = 16;
  CVector3f points[kMax];
  const int n = count < kMax ? count : kMax;
  for (int i = 0; i < n; ++i) {
    points[i] = CVector3f(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]);
  }
  RecordCorners(widget, model, points, n);
}

const CGuiWidget* Pick(const CGuiFrame* frame, float gx, float gy) {
  const PortPointerMath::HitList::Entry* e = sHits.Pick(CGraphics::GetFrameCounter(), frame, gx, gy);
  if (e == nullptr) {
    return nullptr;
  }
  return static_cast< const CGuiWidget* >(static_cast< const CGuiObject* >(e->chain[0]));
}

bool Bounds(const CGuiWidget* widget, bool children, PortPointerMath::Rect& out) {
  return sHits.Bounds(CGraphics::GetFrameCounter(), Key(widget), children, out);
}

bool LastBounds(const CGuiWidget* widget, PortPointerMath::Rect& out) {
  if (widget == nullptr) {
    return false;
  }
  return sHits.LastBounds(CGraphics::GetFrameCounter(), Key(widget), out);
}

std::string Describe(float gx, float gy) {
  const int frame = CGraphics::GetFrameCounter();
  const auto& list = sHits.Current(frame);
  char buf[256];
  std::snprintf(buf, sizeof(buf), "hits frame=%d building=%d done=%d fresh=%d entries=%zu", frame,
                sHits.BuildingFrame(), sHits.DoneFrame(), sHits.Fresh(frame) ? 1 : 0, list.size());
  std::string out = buf;
  // Under the point, or every rect when the point is off screen.
  const bool all = gx < 0.f || gy < 0.f;
  int shown = 0;
  for (size_t i = list.size(); i-- > 0 && shown < (all ? 64 : 8);) {
    const auto& e = list[i];
    if (!all && !e.rect.Contains(gx, gy)) {
      continue;
    }
    const auto* w = static_cast< const CGuiWidget* >(static_cast< const CGuiObject* >(e.chain[0]));
    std::snprintf(buf, sizeof(buf), "\n  id=%d worker=%d [%.0f,%.0f %.0f,%.0f] depth=%d",
                  w->GetWidgetID(), w->GetWorkerId(),
                  e.rect.x0, e.rect.y0, e.rect.x1, e.rect.y1, e.depth);
    out += buf;
    ++shown;
  }
  return out;
}

} // namespace PortGuiHit
