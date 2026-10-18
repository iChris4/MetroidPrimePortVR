#pragma once

// Port: pointer helpers shared by the game's menu screens (port_pointer.h).
// A screen calls them from its ProcessUserInput, behind the same gates as its
// pad input, so a click only acts when a press would.
//
// Rows: on the mouse, hover selects and a click selects and confirms. On
// touch, a tap selects and a tap on the selected row confirms.

class CFinalInput;
class CGuiSliderGroup;
class CGuiTableGroup;
class CGuiWidget;

namespace PortPointerMenu {

// Pointer menus are on; marks the screen as pointer-driven for this frame
// (cursor shown, mouse lock released). Call once per handled frame.
bool Begin();

enum class Action { None, Select, Advance };

// The selectable row of table drawn under the GUI point, or -1. Only rows
// minRow..maxRow count (maxRow -1 = the last), for tables whose edge workers
// are off-screen scroll sentinels.
int RowAt(CGuiTableGroup* table, float gx, float gy, int minRow = 0, int maxRow = -1);

// Hover and clicks on table's rows. A click outside the rows is left for
// someone else. With callAdvance, a confirm fires the table's advance
// callback; otherwise the caller acts on Action::Advance itself. Without
// hover, only clicks select (rows whose selection is a setting's value).
Action Table(CGuiTableGroup* table, bool callAdvance = true, bool hover = true, int minRow = 0,
             int maxRow = -1);

// The row of table under the pending click, without taking it, or -1. Works
// on an inactive table (a click there usually means "go back to it").
int PeekClickRow(CGuiTableGroup* table, int minRow = 0, int maxRow = -1);

// Press or drag on an active slider sets its value from the pointer's x.
// True while the slider holds the pointer (the caller skips other handlers).
bool Slider(CGuiSliderGroup* slider);

// A click or tap on widget (or its children), taken if it hit.
bool TakeClickOn(const CGuiWidget* widget);
// Any click or tap, taken.
bool TakeClick();
// The pending left click or tap in EFB pixels, taken; count is 2 for a double.
bool TakeClickPoint(float& gx, float& gy, int& count);
// A right click (or scripted back), taken.
bool TakeBack();

// Wheel notches and vertical drag, as whole row steps (positive = up).
// stepGui is the drag distance in EFB pixels that makes one step.
int TakeScrollSteps(float stepGui);

// The vertical distance in EFB pixels between rows a and b of table, or
// fallback when either isn't drawn.
float RowPitch(CGuiTableGroup* table, int a, int b, float fallback);

// Pointer motion for a 3D view this frame. rotate is a left drag (one finger
// on touch); pan is a right or middle drag or a two-finger pan; both in EFB
// pixels. zoom is wheel notches plus pinch (one notch per 41% spread),
// positive = closer. twist is a two-finger turn in radians, positive clockwise
// on screen. False when nothing moved.
struct ViewMotion {
  float rotateX = 0.f, rotateY = 0.f;
  float panX = 0.f, panY = 0.f;
  float zoom = 0.f;
  float twist = 0.f;
};
bool TakeViewMotion(ViewMotion& out);

// Merge a confirm or cancel into a copy of the frame's input.
// The caller turned a Table Advance (callAdvance off) into a confirm: the
// click's double-click partner is swallowed like Table's own advances.
void NoteAdvanced();

void PressA(CFinalInput& input);
void PressB(CFinalInput& input);
void PressX(CFinalInput& input);

} // namespace PortPointerMenu
