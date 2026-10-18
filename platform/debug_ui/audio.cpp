// F1 overlay: Audio and voice mutes. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_debug.h"
#include "port_pointer.h"

#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <musyx/port_voices.h>

#include <algorithm>
#include <vector>

namespace PortDebug {

void DrawAudio() {
  bool ai = AiAudioEnabled();
  if (ImGui::Checkbox("Streamed audio (music/movies)", &ai)) {
    SetAiAudioEnabled(ai);
    MarkDirty();
  }
  bool musyx = sMusyxAudioEnabled;
  if (ImGui::Checkbox("MusyX audio (effects/streams)", &musyx)) {
    SetMusyxAudioEnabled(musyx);
    MarkDirty();
  }
}

void DrawVoices() {
  struct Agg {
    PortMusyxVoice voice;
    int instances;
  };

  // Held between frames so "Freeze list" can keep the last collection on screen.
  // Ordered by id, not loudness: sorting by rms made rows swap places every
  // frame as the levels moved, which made the checkboxes hard to hit and hid
  // which sample a row belonged to.
  static std::vector< Agg > aggs;
  static bool freeze = false;

  ImGui::Checkbox("Freeze list", &freeze);
  ImGui::SameLine();
  ImGui::TextUnformatted("(hold the current list so its rows stay put)");
  ImGui::SetItemTooltip("Freezing keeps the samples and levels shown right now, so the rows\n"
                        "stay readable while you look for one. Mute still applies: the\n"
                        "checkbox reads and writes the live mute state, so you can silence\n"
                        "a sample whether or not it is still playing. Untick to follow the\n"
                        "live voices again. Not saved between runs.");

  if (!freeze) {
    aggs.clear();
    PortMusyxVoice voices[64];
    const int count = MusyxPortCopyVoices(voices, 64);
    for (int i = 0; i < count; ++i) {
      bool found = false;
      for (Agg& agg : aggs) {
        if (agg.voice.smpId == voices[i].smpId) {
          ++agg.instances;
          if (voices[i].rms > agg.voice.rms) {
            agg.voice = voices[i];
          }
          found = true;
          break;
        }
      }
      if (!found) {
        aggs.push_back(Agg{voices[i], 1});
      }
    }
    std::sort(aggs.begin(), aggs.end(),
              [](const Agg& a, const Agg& b) { return a.voice.smpId < b.voice.smpId; });
  }

  ImGui::TextUnformatted("Active samples, lowest id first. Mute one to isolate it.");
  if (aggs.empty()) {
    ImGui::TextUnformatted(freeze ? "No samples in the frozen list." : "No active MusyX voices.");
  }
  for (const Agg& agg : aggs) {
    ImGui::PushID(static_cast< int >(agg.voice.smpId));
    bool muted = MusyxPortIsSampleMuted(agg.voice.smpId) != 0;
    if (ImGui::Checkbox("##mute", &muted)) {
      MusyxPortSetSampleMuted(agg.voice.smpId, muted ? 1 : 0);
      MarkDirty();
    }
    ImGui::SameLine();
    ImGui::Text("smp %u  %s  len %u  pitch %u  rms %d  vol %u/%u  x%d", agg.voice.smpId,
                agg.voice.looped ? "loop" : "one-shot", agg.voice.length, agg.voice.pitch,
                agg.voice.rms, agg.voice.volL, agg.voice.volR, agg.instances);
    ImGui::PopID();
  }
  // Deliberately not behind the empty-list case above: with nothing playing
  // there is still a mute set to clear.
  if (ImGui::Button("Unmute all")) {
    MusyxPortClearSampleMutes();
    MarkDirty();
  }
}

} // namespace PortDebug
