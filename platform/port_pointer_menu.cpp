#include "port_pointer_menu.h"

#include "port_pointer.h"

#include "GuiSys/CGuiSliderGroup.hpp"
#include "GuiSys/CGuiTableGroup.hpp"
#include "Kyoto/Input/CFinalInput.hpp"

#include <algorithm>
#include <cmath>

namespace PortPointerMenu {
namespace {

// The last click confirmed a row: its double-click partner is swallowed, so
// a habitual double click doesn't also act on the next screen.
bool sLastAdvanced = false;
float sWheelRest = 0.f;
float sDragRest = 0.f;
// The slider a press started on; it keeps the drag until release.
const CGuiSliderGroup* sGrabbed = nullptr;

bool ClickToGui(const PortPointer::Click& click, float& gx, float& gy) {
  return PortPointer::WindowToGui(click.x, click.y, gx, gy);
}

} // namespace

bool Begin() {
  // A grab or drag remainder never outlives its press, even when the screen
  // that held it has closed.
  if (!PortPointer::Enabled() || !PortPointer::Gestures().Pressed()) {
    sGrabbed = nullptr;
    sDragRest = 0.f;
  }
  if (!PortPointer::Enabled()) {
    return false;
  }
  PortPointer::NoteScreenActive();
  return true;
}

int RowAt(CGuiTableGroup* table, float gx, float gy, int minRow, int maxRow) {
  if (table == nullptr || !table->GetIsVisible()) {
    return -1;
  }
  // Rows can overlap (a highlight bar spanning its neighbours): the smallest
  // rect under the point wins.
  int best = -1;
  float bestArea = 0.f;
  const int end = maxRow < 0 ? table->GetElementCount() - 1
                             : std::min(maxRow, table->GetElementCount() - 1);
  for (int i = std::max(0, minRow); i <= end; ++i) {
    CGuiWidget* worker = table->GetWorkerWidget(i);
    PortPointerMath::Rect r;
    if (worker == nullptr || !worker->GetIsSelectable() ||
        !PortGuiHit::Bounds(worker, true, r) || !r.Contains(gx, gy)) {
      continue;
    }
    if (best < 0 || r.Area() < bestArea) {
      best = i;
      bestArea = r.Area();
    }
  }
  return best;
}

int PeekClickRow(CGuiTableGroup* table, int minRow, int maxRow) {
  PortPointer::Click click;
  float gx, gy;
  if (table == nullptr || !PortPointer::Gestures().PeekClick(click) ||
      !ClickToGui(click, gx, gy)) {
    return -1;
  }
  return RowAt(table, gx, gy, minRow, maxRow);
}

Action Table(CGuiTableGroup* table, bool callAdvance, bool hover, int minRow, int maxRow) {
  if (table == nullptr || !table->GetIsActive()) {
    return Action::None;
  }
  PortPointer::Recognizer& g = PortPointer::Gestures();
  Action action = Action::None;

  float gx, gy;
  if (hover && g.Moved() && PortPointer::MouseIsLast() && !g.Pressed() && PortPointer::Position(gx, gy)) {
    const int row = RowAt(table, gx, gy, minRow, maxRow);
    if (row >= 0 && table->PortSelect(row)) {
      action = Action::Select;
    }
  }

  PortPointer::Click click;
  if (g.PeekClick(click) && ClickToGui(click, gx, gy)) {
    const int row = RowAt(table, gx, gy, minRow, maxRow);
    if (row >= 0) {
      g.TakeClick(click);
      if (click.count == 2 && sLastAdvanced) {
        return action;
      }
      const bool wasSelected = row == table->GetUserSelection();
      table->PortSelect(row);
      if (click.device == PortPointer::Device::Mouse || wasSelected) {
        action = Action::Advance;
      } else {
        action = Action::Select;
      }
      // Only a click the caller acts on arms the double-click swallow; with
      // callAdvance off the caller reports it through NoteAdvanced.
      sLastAdvanced = action == Action::Advance && callAdvance;
      if (sLastAdvanced) {
        table->PortAdvance();
      }
    }
  }
  return action;
}

bool Slider(CGuiSliderGroup* slider) {
  if (slider == nullptr || !slider->GetIsActive() || !slider->GetIsVisible()) {
    if (sGrabbed == slider) {
      sGrabbed = nullptr;
    }
    return false;
  }
  PortPointer::Recognizer& g = PortPointer::Gestures();
  PortPointerMath::Rect range;
  if (!PortGuiHit::Bounds(slider, true, range)) {
    return false;
  }
  // The knob (worker 0) travels so its centre spans the range minus itself.
  float half = 0.f;
  PortPointerMath::Rect knob;
  if (slider->GetWorkerWidget(0) != nullptr &&
      PortGuiHit::Bounds(slider->GetWorkerWidget(0), true, knob)) {
    half = 0.5f * (knob.x1 - knob.x0);
  }
  // A thin bar is hard to hit with a finger: accept a band around it.
  const float band = std::max(0.f, 24.f - 0.5f * (range.y1 - range.y0));
  PortPointerMath::Rect hit = range;
  hit.y0 -= band;
  hit.y1 += band;

  float gx = -1.f, gy = -1.f;
  bool apply = false;
  PortPointer::Click click;
  if (g.Pressed() && (g.Buttons() & ~PortPointer::kButtonLeft) == 0 && g.FingerCount() <= 1 &&
      PortPointer::Position(gx, gy)) {
    float px, py;
    if (sGrabbed == nullptr && PortPointer::WindowToGui(g.PressX(), g.PressY(), px, py) &&
        hit.Contains(px, py)) {
      sGrabbed = slider;
    }
    apply = sGrabbed == slider;
  } else {
    if (sGrabbed == slider) {
      sGrabbed = nullptr;
    }
    // The release of a press on the slider ends in a click: it is ours.
    if (g.PeekClick(click) && ClickToGui(click, gx, gy) && hit.Contains(gx, gy)) {
      g.TakeClick(click);
      sLastAdvanced = false;
      apply = true;
    }
  }
  if (apply) {
    const float span = (range.x1 - range.x0) - 2.f * half;
    slider->PortSetFraction(span > 0.f ? (gx - range.x0 - half) / span : 0.f);
  }
  return apply;
}

bool TakeClickOn(const CGuiWidget* widget) {
  PortPointer::Recognizer& g = PortPointer::Gestures();
  PortPointer::Click click;
  float gx, gy;
  PortPointerMath::Rect r;
  if (widget == nullptr || !g.PeekClick(click) || !ClickToGui(click, gx, gy) ||
      !PortGuiHit::Bounds(widget, true, r) || !r.Contains(gx, gy)) {
    return false;
  }
  g.TakeClick(click);
  sLastAdvanced = false;
  return true;
}

bool TakeClickPoint(float& gx, float& gy, int& count) {
  PortPointer::Recognizer& g = PortPointer::Gestures();
  PortPointer::Click click;
  if (!g.PeekClick(click) || click.button != PortPointer::kButtonLeft ||
      !ClickToGui(click, gx, gy)) {
    return false;
  }
  g.TakeClick(click);
  count = click.count;
  sLastAdvanced = false;
  return true;
}

bool TakeClick() {
  PortPointer::Click click;
  if (!PortPointer::Gestures().TakeClick(click)) {
    return false;
  }
  sLastAdvanced = false;
  return true;
}

bool TakeBack() { return PortPointer::Gestures().TakeBack(); }

int TakeScrollSteps(float stepGui) {
  PortPointer::Recognizer& g = PortPointer::Gestures();
  sWheelRest += g.TakeWheel();
  int steps = static_cast< int >(sWheelRest);
  sWheelRest -= static_cast< float >(steps);

  float dx, dy;
  uint8_t buttons;
  if (g.TakeDrag(dx, dy, buttons) && (buttons & PortPointer::kButtonLeft) != 0 && stepGui > 0.f) {
    // Content follows the finger: dragging down shows the rows above.
    sDragRest += dy * PortPointer::GuiScale();
    const int dragSteps = static_cast< int >(sDragRest / stepGui);
    sDragRest -= static_cast< float >(dragSteps) * stepGui;
    steps += dragSteps;
  }
  if (!g.Pressed()) {
    sDragRest = 0.f;
  }
  return steps;
}

float RowPitch(CGuiTableGroup* table, int a, int b, float fallback) {
  PortPointerMath::Rect ra, rb;
  if (table == nullptr || !PortGuiHit::Bounds(table->GetWorkerWidget(a), true, ra) ||
      !PortGuiHit::Bounds(table->GetWorkerWidget(b), true, rb)) {
    return fallback;
  }
  const float pitch = std::fabs(rb.y0 - ra.y0);
  return pitch > 4.f ? pitch : fallback;
}

bool TakeViewMotion(ViewMotion& out) {
  out = ViewMotion();
  if (!PortPointer::Enabled()) {
    return false;
  }
  PortPointer::Recognizer& g = PortPointer::Gestures();
  const float scale = PortPointer::GuiScale();
  float dx, dy;
  uint8_t buttons;
  if (g.TakeDrag(dx, dy, buttons)) {
    if ((buttons & PortPointer::kButtonLeft) != 0) {
      out.rotateX = dx * scale;
      out.rotateY = dy * scale;
    } else {
      out.panX = dx * scale;
      out.panY = dy * scale;
    }
  }
  if (g.TakePan(dx, dy)) {
    out.panX += dx * scale;
    out.panY += dy * scale;
  }
  out.zoom = g.TakeWheel();
  const float pinch = g.TakePinch();
  if (pinch > 0.f && pinch != 1.f) {
    out.zoom += 2.f * std::log2(pinch);
  }
  out.twist = g.TakeTwist();
  return out.rotateX != 0.f || out.rotateY != 0.f || out.panX != 0.f || out.panY != 0.f ||
         out.zoom != 0.f || out.twist != 0.f;
}

void NoteAdvanced() { sLastAdvanced = true; }

void PressA(CFinalInput& input) { input.PortSetA(true, true); }
void PressB(CFinalInput& input) { input.PortSetB(true, true); }
void PressX(CFinalInput& input) { input.PortSetX(true, true); }

} // namespace PortPointerMenu
