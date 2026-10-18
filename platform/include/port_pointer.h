#pragma once

#include <string>

// Port: mouse and touch for the game's own menus and map. The main loop feeds
// SDL events here; screens that support the pointer read gestures in GUI
// (EFB pixel) coordinates and hit-test the widgets drawn last frame. The state
// lives outside CFinalInput, so pad and keyboard input is untouched.

#include "port_pointer_gesture.h"
#include "port_pointer_math.h"

union SDL_Event;
class CGuiFrame;
class CGuiWidget;
class CTransform4f;
class CVector3f;

namespace PortPointer {

// The pointer_menus setting (saved by debug_ui.cpp; set through PortDebug::SetPointerMenus).
bool Enabled();
void SetEnabled(bool enabled);
// The map_swap_fingers setting: touch pans with one finger and rotates with two.
bool MapSwapFingers();
void SetMapSwapFingers(bool swap);

// One SDL event from the main loop. Synthesized touch mice are ignored (the
// fingers arrive on their own); keys and pad buttons only mark that the
// pointer is no longer the last-used device. Esc and Android back are left to
// their usual bindings: a screen reads them as back through its pad B.
void HandleEvent(const SDL_Event& event);
// Once per presented frame: drop results nobody took.
void BeginFrame();
// Focus loss or a screen change: forget presses and pending results.
void Reset();

Recognizer& Gestures();

// Window points to EFB pixels through the presented letterbox. False in the
// bars (the coordinates are clamped to the image).
bool WindowToGui(float wx, float wy, float& gx, float& gy);
bool GuiToWindow(float gx, float gy, float& wx, float& wy);
// The pointer position in EFB pixels; false before any pointer event or in the bars.
bool Position(float& gx, float& gy);
// EFB pixels per window point (gestures measured in points scale by it).
float GuiScale();

// The last input came from the mouse / a finger.
bool MouseIsLast();
bool TouchIsLast();

// A screen that handles the pointer this frame says so; the main loop shows
// the cursor and releases the mouse lock while one is active, and the mouse
// menu buttons (click = A) stand down so a click isn't taken twice.
void NoteScreenActive();
bool ScreenActive();

// Scripted input for the console (`pointer`), in EFB pixels.
void ScriptMove(float gx, float gy);
// Press or release without moving on: drags that span frames, as real ones do.
void ScriptPress(float gx, float gy, bool down, uint8_t buttons, bool touch);
void ScriptClick(float gx, float gy, bool doubleClick, bool touch);
void ScriptDrag(float gx0, float gy0, float gx1, float gy1, uint8_t buttons, bool touch);
void ScriptWheel(float notches);
void ScriptPinch(float gx, float gy, float ratio);
void ScriptBack();

} // namespace PortPointer

namespace PortGuiHit {

// From a visible widget's Draw: the box [min, max] (eight corners) in the
// space of model, projected through the current CGraphics view, projection
// and viewport. Draw runs with the port's spread/HUD transforms applied, so
// the rects match what is on screen.
void Record(const CGuiWidget* widget, const CTransform4f& model, const CVector3f& localMin,
            const CVector3f& localMax);
// The same for a point list (x, y, z triples), such as a pane's quad.
void RecordPoints(const CGuiWidget* widget, const CTransform4f& model, const float* xyz, int count);

// The GUI point of point in the space of model, through the current CGraphics
// view, projection and viewport. False when it is behind the eye.
bool Project(const CTransform4f& model, const CVector3f& point, float& gx, float& gy);
// The last-drawn widget of frame under the GUI point (EFB pixels), or null.
const CGuiWidget* Pick(const CGuiFrame* frame, float gx, float gy);
// Screen bounds drawn last frame for widget (and its children).
bool Bounds(const CGuiWidget* widget, bool children, PortPointerMath::Rect& out);
// The bounds of the last thing widget itself drew (a text pane's glyphs, drawn
// after its pane), or false when it drew none.
bool LastBounds(const CGuiWidget* widget, PortPointerMath::Rect& out);

// Console diagnostics: the hit list's state and the rects under a point.
std::string Describe(float gx, float gy);

} // namespace PortGuiHit
