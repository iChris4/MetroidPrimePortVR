#include "vr/vr_wrist_visor.h"
#include <cstdio>
#include <cstdlib>
using namespace PortVr;
static void Check(bool ok, const char* message) { if (!ok) { std::fprintf(stderr, "%s\n", message); std::abort(); } }
int main() {
    Check(WristVisor::VisorForSlot == std::array<int,4>{0,2,3,1}, "row order is X-ray, Thermal, Scan, Combat");
    Check(WristVisor::SlotMask(1u) == 1u, "Combat is selectable only in the far-right slot");
    Check(WristVisor::SlotMask(4u) == 2u, "Scan stays in the middle-right slot");
    const auto rightUp = screen_math::Rotate(WristVisor::SymbolRotation(1), {0.f,0.f,-1.f});
    const auto leftUp = screen_math::Rotate(WristVisor::SymbolRotation(0), {0.f,0.f,-1.f});
    Check(rightUp[0] < -0.99f && leftUp[0] > 0.99f, "left-hand symbol rotation corrects the upside-down quarter-turn");
    OpenXRInputSnapshot s;
    s.runtime_active = true;
    for (auto& h : s.controllers) { h.connected = true; h.grip_pose.valid = h.aim_pose.valid = true; }
    for (unsigned main : {0u, 1u}) {
        for (int i = 0; i < 4; ++i) {
            s.controllers[1-main].grip_pose.position = {0.2f, 1.1f, -0.4f};
            const auto& p = s.controllers[1-main].grip_pose.position;
            for (int a = 0; a < 3; ++a) s.controllers[main].aim_pose.position[a] = p[a] + WristVisor::Center(i, main)[a];
            Check(WristVisor::Hover(s, main) == i, "each icon is picked on either wrist");
        }
    }
    s.controllers[0].grip_pose.orientation = {0, 0, 0.70710678f, 0.70710678f};
    const auto rotated = screen_math::Rotate(s.controllers[0].grip_pose.orientation, WristVisor::Center(1, 1));
    for (int a=0; a<3; ++a) s.controllers[1].aim_pose.position[a] = s.controllers[0].grip_pose.position[a]+rotated[a];
    Check(WristVisor::Hover(s, 1) == 1, "hover follows wrist rotation");
    s.controllers[1].connected = false;
    Check(WristVisor::Hover(s, 1) == -1, "lost main controller disables targeting");
    s.controllers[1].connected = true; s.controllers[1].aim_pose.position = {100,100,100};
    Check(WristVisor::Hover(s, 1) == -1, "distant controller cannot select");
    // Practical hit area extends above the row and keeps selection stable at boundaries.
    s.controllers[0].grip_pose.orientation = {0,0,0,1};
    const auto wrist = s.controllers[0].grip_pose.position;
    const auto place = [&](const screen_math::Vec3& panelPoint) {
        const auto local = screen_math::Rotate(WristVisor::PanelRotation(1), panelPoint);
        for (int a = 0; a < 3; ++a) s.controllers[1].aim_pose.position[a] = wrist[a] + local[a];
    };
    const auto normal = screen_math::Vec3{0.f, 0.10f, 0.f};
    place({normal[0], 0.035f + normal[1], 0.11f + normal[2]});
    Check(WristVisor::Hover(s, 1) == 1, "controller can hover ten centimetres above a visible icon");
    place({0.f, 0.035f, 0.137f});
    Check(WristVisor::Hover(s, 1, 1) == 1, "small boundary tremor keeps previous highlight");
    place({0.f, 0.035f, 0.16f});
    Check(WristVisor::Hover(s, 1, 1) == 2, "deliberate move changes highlight");
    Check(std::fabs(WristVisor::Center(0, 1)[0] + WristVisor::Center(0, 0)[0]) < 0.00001f,
          "row follows the side of either forearm, mirrored by handedness");
    const PortVrSettings defaults;
    Check(defaults.wrist_pitch == 40.f && defaults.wrist_yaw == -40.f && defaults.wrist_roll == 50.f,
          "default calibration matches the supplied reference");
    WristVisor::Calibration calibrated;
    calibrated.offset = {0.07f, -0.03f, 0.04f};
    calibrated.pitch = 45.f; calibrated.yaw = -30.f; calibrated.roll = 90.f;
    for (unsigned main : {0u, 1u}) {
        s.controllers[1-main].grip_pose.orientation = {0,0,0,1};
        const auto base = s.controllers[1-main].grip_pose.position;
        const auto panelNormal = screen_math::Rotate(WristVisor::PanelRotation(main, calibrated), {0.f,1.f,0.f});
        const auto first = WristVisor::Center(0, main, calibrated);
        for (int icon = 0; icon < 4; ++icon) {
            const auto center = WristVisor::Center(icon, main, calibrated);
            screen_math::Vec3 offset{};
            for (int a=0; a<3; ++a) {
                s.controllers[main].aim_pose.position[a] = base[a]+center[a];
                offset[a] = center[a]-first[a];
            }
            Check(WristVisor::Hover(s, main, -1, calibrated) == icon, "live calibration moves hit areas with every icon");
            Check(std::fabs(screen_math::Dot(offset,panelNormal)) < 0.00001f,
                  "calibrated row stays in one plane, with parallel icon faces");
        }
    }
    WristVisor::SelectionQueue queue;
    queue.Submit(2, 10.0);
    Check(queue.Take(10.4) == 2, "press survives frames without a simulation tick and short visor transitions");
    Check(queue.Take(10.41) == -1, "simulation consumes a queued selection exactly once");
    queue.Submit(1, 20.0);
    Check(queue.Take(20.7) == -1, "stale selection expires instead of switching later");
    queue.Submit(3, 30.0); queue.Reset();
    Check(queue.Take(30.1) == -1, "leaving gameplay cancels pending selection");
    s.controllers[0].primary = false; s.controllers[1].primary = true;
    Check(!WristVisor::SelectionPressed(s, false), "right A cannot select wrist icons in left-hand mode");
    s.controllers[0].primary = true; s.controllers[1].primary = false;
    Check(WristVisor::SelectionPressed(s, false), "left X selects wrist icons in left-hand mode");
    Check(!WristVisor::SelectionPressed(s, true), "left X cannot select wrist icons in right-hand mode");
    s.controllers[0].primary = s.controllers[1].primary = false;
    for (unsigned main : {0u, 1u}) {
        s.controllers[main].trigger_value = 0.7f;
        Check(WristVisor::SelectionPressed(s, main == 1), "main trigger selects wrist icons");
        Check(!WristVisor::SelectionPressed(s, main != 1), "off-hand trigger cannot select wrist icons");
        PortVrSettings custom; custom.custom_layout_enabled = true; custom.custom_trigger_threshold = 0.8f;
        Check(!WristVisor::SelectionPressed(s, main == 1, custom), "selection respects customized trigger threshold");
        s.controllers[main].trigger_value = 0.9f;
        Check(WristVisor::SelectionPressed(s, main == 1, custom), "custom trigger threshold permits selection");
        s.controllers[main].primary = s.controllers[main].trigger_click = true;
        s.controllers[1-main].trigger_value = 0.3f;
        WristVisor::ConsumeSelection(s, main);
        Check(!s.controllers[main].primary && !s.controllers[main].trigger_click && s.controllers[main].trigger_value == 0.f,
              "captured selection cannot also fire or invoke a custom trigger action");
        Check(s.controllers[1-main].trigger_value == 0.3f, "consumption preserves off-hand input");
        s.controllers[1-main].trigger_value = 0.f;
        s.controllers[main].trigger_click = true;
        Check(WristVisor::SelectionPressed(s, main == 1), "digital-only triggers also select");
        s.controllers[main].trigger_click = false;
    }
    WristVisor::Tracker t;
    Check(t.Update(true, 1, true, 15, true).selected == 1, "fresh primary press selects hovered visor");
    Check(t.Update(true, 2, true, 15, true).selected == -1, "holding primary cannot select another visor");
    Check(t.Update(true, -1, true, 15, true).consume, "moving away keeps captured primary consumed until release");
    Check(!t.Update(true, -1, false, 15, true).consume, "release restores normal primary action");
    Check(t.Update(true, 3, true, 3, true).selected == -1, "locked visor cannot be selected");
    t.Update(true, 0, false, 15, true);
    Check(t.Update(true, 0, true, 15, false).selected == -1, "scanning/grappling/transition blocks selection");
    t.Update(false, -1, true, 15, true);
    Check(t.Update(true, 0, true, 15, true).selected == -1, "held press cannot select on gameplay entry");
    Check(t.Update(false, 0, true, 15, true).hover == -1, "disabled wrist UI has no hover");
}
