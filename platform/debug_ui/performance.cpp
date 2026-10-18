// F1 overlay: The Performance tab. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include <aurora/imgui.h>
#include <imgui_internal.h>


namespace PortDebug {

void DrawPerformanceTab() {
  ImGui::SeparatorText("Frame rate");
  // PortVr: the game loop then waits on the headset's frame requests and skips both caps
  // (src/MetroidPrime/main.cpp), and the window presents without vsync (EffectiveVsync).
  if (PortVr::OpenXRIsRunning()) {
    ImGui::TextDisabled("The headset paces frames while it runs: these caps and Vsync apply to "
                        "flat-screen play only.");
  }
  const bool locked = BeginOriginalLocked();
  bool frameLimit = sFrameLimitEnabled;
  if (ImGui::Checkbox("60 FPS cap (target)", &frameLimit)) {
    SetFrameLimitEnabled(frameLimit);
    MarkDirty();
  }
  {
    // One past the top is "Unlimited" (saved as 0).
    ImGui::BeginDisabled(sFrameLimitEnabled);
    int cap = sFpsCap > 0 ? sFpsCap : kFpsCapMax + 1;
    if (ImGui::SliderInt("FPS cap", &cap, kFpsCapMin, kFpsCapMax + 1, cap > kFpsCapMax ? "Unlimited" : "%d FPS",
                         ImGuiSliderFlags_AlwaysClamp)) {
      sFpsCap = cap > kFpsCapMax ? 0 : ClampFpsCap(cap);
      ResetDynamicRes();
      MarkDirty();
    }
    ImGui::EndDisabled();
    ItemHelp("With the 60 FPS cap off, the most frames drawn a second. Vsync still holds it to "
             "the display's refresh rate.");
  }
  // Both, because they answer different questions. Throughput is what the
  // machine produces once the pacing wait is excluded; presented is what
  // reaches the screen. A player seeing stutter wants the second one, and the
  // gap between them is the headroom.
  ImGui::Text("Presented %.1f FPS    Throughput %.1f FPS    Frame %.2f ms", sActualFps, sThroughputFps,
              static_cast< double >(ImGui::GetIO().DeltaTime) * 1000.0);
  ImGui::SetItemTooltip("Presented is what reaches the screen. Throughput leaves out the wait for the\n"
                        "frame cap: the gap between the two is the headroom at the current frame cost.");
  if (SimAdaptive()) {
    ImGui::Text("Simulation %.1f ticks/s (adaptive)", sActualTps);
  } else {
    ImGui::Text("Simulation %.1f ticks/s (target %u)", sActualTps, SimRate());
  }
  ImGui::Text("Game thread per frame: update %.2f ms, world draw %.2f ms, HUD draw %.2f ms", sPhaseMs[kPhaseUpdate],
              sPhaseMs[kPhaseDrawWorld], sPhaseMs[kPhaseDrawGui]);

  bool smooth = sSmoothFrames;
  if (ImGui::Checkbox("Smooth uncapped frames", &smooth)) {
    PortDebug::SetSmoothFrames(smooth);
  }
  ItemHelp("With the FPS cap off, frames between the game's 60 Hz ticks turn the view with "
           "mouse, gyro and twin-stick look, and draw moving objects, animation and particles "
           "between their last two ticks. The game itself still runs at 60 Hz, so aim and "
           "shots are unchanged.");

  ImGui::SeparatorText("Simulation rate (very experimental)");
  ImGui::TextColored(ThemeWarnColor(), "For testing only. The game was written for 60 Hz: other rates can\n"
                                       "break physics, enemy behaviour, cutscenes and scripted events.");
  bool adaptive = sSimAdaptive;
  if (ImGui::Checkbox("Adaptive (follow frame rate)", &adaptive)) {
    PortDebug::SetSimAdaptive(adaptive);
  }
  ItemHelp("One step per frame with dt = the measured frame time (clamped 30-480 Hz), so a "
           "variable frame rate is matched exactly. Leave the FPS cap off.");
  ImGui::BeginDisabled(adaptive);
  int simRate = static_cast< int >(sSimRate);
  if (ImGui::SliderInt("Sim Hz", &simRate, 30, 480)) {
    PortDebug::SetSimRate(static_cast< unsigned >(simRate));
  }
  ImGui::EndDisabled();
  ItemHelp("60 Hz is console-accurate and the only rate that's supported. Other values step the "
           "game logic at that rate instead of smoothing frames between 60 Hz ticks; leave the "
           "FPS cap off for it to matter. For smooth high frame rates, use Smooth uncapped "
           "frames instead.");
  EndOriginalLocked(locked);
}

} // namespace PortDebug
