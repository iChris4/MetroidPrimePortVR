// F1 overlay: The Tracker tab. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_apclient.h"
#include "port_tracker.h"

#include "MetroidPrime/Cameras/CCameraManager.hpp"

#include <aurora/imgui.h>
#include <imgui_internal.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace PortDebug {

void DrawTrackerCount(const char* label, const PortTracker::Count& count) {
  const bool done = count.total > 0 && count.have >= count.total;
  if (done) {
    ImGui::TextColored(ThemeGoodColor(), "%s %d/%d", label, count.have, count.total);
  } else {
    ImGui::Text("%s %d/%d", label, count.have, count.total);
  }
}

// The Archipelago checks within reach, coloured as on the map.
void DrawTrackerLogic() {
  static PortAp::LogicState state;
  if (!PortAp::Logic(state)) {
    return;
  }
  ImGui::SeparatorText("Archipelago checks");
  bool colors = sMapLogicColors;
  if (ImGui::Checkbox("Colour the map's dots by logic", &colors)) {
    SetMapLogicColors(colors);
  }
  ItemHelp("Worked out from your items and the seed's logic. Green is in logic; yellow needs a "
           "trick the seed doesn't count on; blue can be seen but not collected.");
  static const ImVec4 kColors[] = {
      ImVec4(0.95f, 0.30f, 0.30f, 1.f), // out of logic
      ImVec4(0.35f, 0.60f, 1.00f, 1.f), // inspect
      ImVec4(1.00f, 0.85f, 0.25f, 1.f), // sequence break
      ImVec4(0.35f, 0.90f, 0.40f, 1.f), // in logic
  };
  static const ImVec4 kGrey(0.55f, 0.55f, 0.55f, 1.f);
  size_t count = 0;
  const PortApLogic::Check* checks = PortApLogic::Checks(count);
  int totals[4] = {};
  int checked = 0;
  for (size_t i = 0; i < count; ++i) {
    if (state.checked[i]) {
      ++checked;
    } else {
      ++totals[static_cast< int >(state.levels[i])];
    }
  }
  ImGui::TextColored(kColors[3], "%d in logic", totals[3]);
  ImGui::SameLine();
  ImGui::TextColored(kColors[2], "%d sequence break", totals[2]);
  ImGui::SameLine();
  ImGui::TextColored(kColors[1], "%d visible only", totals[1]);
  ImGui::SameLine();
  ImGui::TextColored(kColors[0], "%d out of reach", totals[0]);
  ImGui::SameLine();
  ImGui::TextColored(kGrey, "%d checked", checked);

  // One header per area, holding what can be reached there, best first.
  const char* area = nullptr;
  bool open = false;
  for (size_t i = 0; i < count; ++i) {
    if (area == nullptr || std::strcmp(area, checks[i].area) != 0) {
      area = checks[i].area;
      int inLogic = 0;
      int other = 0;
      for (size_t j = 0; j < count; ++j) {
        if (std::strcmp(checks[j].area, area) != 0 || state.checked[j]) {
          continue;
        }
        if (state.levels[j] == PortApLogic::Level::Normal) {
          ++inLogic;
        } else if (state.levels[j] != PortApLogic::Level::None) {
          ++other;
        }
      }
      char header[160];
      std::snprintf(header, sizeof(header), "%s (%d in logic, %d other)###aplogic%s", area, inLogic,
                    other, area);
      ImGui::SetNextItemOpen(inLogic > 0, ImGuiCond_Once);
      open = ImGui::CollapsingHeader(header);
      if (open) {
        for (int level = 3; level >= 1; --level) {
          for (size_t j = 0; j < count; ++j) {
            if (std::strcmp(checks[j].area, area) != 0 || state.checked[j] ||
                static_cast< int >(state.levels[j]) != level) {
              continue;
            }
            if (checks[j].section[0] != 0) {
              ImGui::TextColored(kColors[level], "  %s - %s", checks[j].room, checks[j].section);
            } else {
              ImGui::TextColored(kColors[level], "  %s", checks[j].room);
            }
          }
        }
        if (inLogic + other == 0) {
          ImGui::TextDisabled("  nothing within reach");
        }
      }
    }
  }
}

void DrawTrackerTab() {
  bool reveal = sRevealMap;
  ImGui::BeginDisabled(sOriginalExperience);
  if (ImGui::Checkbox("Reveal map", &reveal)) {
    SetRevealMap(reveal);
  }
  ImGui::EndDisabled();
  ItemHelp("Shows every world's map as if its map station had been used, and lists every world on "
           "the star map. Hidden rooms stay hidden. The save isn't changed.");
  // Combo order: Off, Seen, Rooms, All.
  static const EMapPickups kPickupModes[] = {kMapPickups_Off, kMapPickups_Seen, kMapPickups_Rooms,
                                             kMapPickups_All};
  int pickups = 0;
  for (int i = 0; i < 4; ++i) {
    if (kPickupModes[i] == sMapPickups) {
      pickups = i;
    }
  }
  if (ImGui::Combo("Pickups on the map", &pickups, "Off\0Seen\0Rooms\0All\0")) {
    SetMapPickups(kPickupModes[pickups]);
  }
  ItemHelp("Marks uncollected pickups on the map and minimap. Seen: a white dot once you've seen "
           "the pickup. Rooms: no dots; rooms still holding a pickup are tinted amber. All: a dot "
           "for every pickup in the rooms the map shows. In Seen and All, Archipelago colours dots "
           "by what logic lets you reach.");
  const bool dots = sMapPickups == kMapPickups_Seen || sMapPickups == kMapPickups_All;
  ImGui::BeginDisabled(!dots);
  bool kinds = sMapPickupKinds;
  if (ImGui::Checkbox("Show major and minor items", &kinds)) {
    SetMapPickupKinds(kinds);
  }
  ImGui::EndDisabled();
  ItemHelp("Filled dot: major item. Ring: minor item (expansion or energy tank). Grey cross: "
           "collected. Randomized games show what each pickup holds; in Archipelago, another "
           "game's item is major if progression, and an unscouted pickup is a dot. Off: one white "
           "dot per item, gone once collected.");
  ImGui::BeginDisabled(sMapPickups != kMapPickups_Seen);
  bool station = sMapStationPickups;
  if (ImGui::Checkbox("Map stations show pickups", &station)) {
    SetMapStationPickups(station);
  }
  ImGui::EndDisabled();
  ItemHelp("In Seen, a map station also marks every pickup in the rooms it maps. With Reveal map "
           "on, that is every room, as in All.");
  bool extraUi = sMapExtraUi;
  if (ImGui::Checkbox("Extra map UI", &extraUi)) {
    SetMapExtraUi(extraUi);
  }
  ItemHelp(
      "The port's own lines on the world map: legends for pickup shapes and marked rooms, and the "
      "\"X Mark room\" prompt (in Single Player a click or tap marks the room). Off by default, so "
      "a first playthrough shows the disc's own legend. Room marks still work with X.");

  DrawTrackerLogic();

  ImGui::SeparatorText("Progress");
  bool progress = sTrackerProgress;
  if (ImGui::Checkbox("Show progress", &progress)) {
    sTrackerProgress = progress;
    MarkDirty();
  }
  ItemHelp("Item collection, scans and rooms visited, with what is still missing. Off by default, "
           "since it shows how much is left to find.");
  if (!sTrackerProgress) {
    return;
  }

  CStateManager* mgr = sStateManager;
  if (mgr == nullptr || mgr->GetPlayerState() == nullptr) {
    ImGui::TextDisabled("Progress shows once a game is running.");
    return;
  }
  const PortTracker::Summary summary = PortTracker::Collect(*mgr);

  ImGui::SeparatorText("Items");
  ImGui::Text("Item collection %d%%", summary.itemPercent);
  DrawTrackerCount("Energy Tanks", summary.energyTanks);
  ImGui::SameLine(ImGui::GetFontSize() * 12.f);
  DrawTrackerCount("Missile expansions", summary.missileExpansions);
  DrawTrackerCount("Power Bombs", summary.powerBombExpansions);
  ImGui::SameLine(ImGui::GetFontSize() * 12.f);
  DrawTrackerCount("Artifacts", summary.artifacts);
  const PortTracker::Count upgrades = {
      static_cast< int >(summary.upgradesHeld.size()),
      static_cast< int >(summary.upgradesHeld.size() + summary.upgradesMissing.size())};
  DrawTrackerCount("Upgrades", upgrades);
  if (!summary.upgradesMissing.empty()) {
    std::string missing;
    for (const std::string& name : summary.upgradesMissing) {
      missing += (missing.empty() ? "" : ", ") + name;
    }
    ImGui::TextWrapped("Missing: %s", missing.c_str());
  }

  ImGui::SeparatorText("Scans");
  for (int i = 0; i < PortTracker::kScan_Count; ++i) {
    DrawTrackerCount(PortTracker::ScanGroupName(i), summary.scans[i]);
    if (i % 2 == 0) {
      ImGui::SameLine(ImGui::GetFontSize() * 12.f);
    }
  }
  DrawTrackerCount("All scans", summary.scanTotal);

  ImGui::SeparatorText("Rooms visited");
  for (const PortTracker::World& world : summary.worlds) {
    const PortTracker::Count rooms = {world.visited, world.total};
    DrawTrackerCount(world.name.c_str(), rooms);
    if (world.mapStation || world.current) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s%s%s", world.mapStation ? "map station" : "",
                          world.mapStation && world.current ? ", " : "",
                          world.current ? "you are here" : "");
    }
  }

  int unvisited = 0;
  for (const PortTracker::Room& room : summary.rooms) {
    unvisited += room.visited ? 0 : 1;
  }
  char header[160];
  std::snprintf(header, sizeof(header), "Rooms not yet visited in %s (%d)###trackerrooms",
                summary.currentWorld.c_str(), unvisited);
  if (ImGui::CollapsingHeader(header)) {
    if (summary.roomNamesLoading > 0) {
      ImGui::TextDisabled("Loading room names...");
    }
    for (const PortTracker::Room& room : summary.rooms) {
      if (!room.visited) {
        ImGui::BulletText("%s", room.name.c_str());
      }
    }
  }
}

} // namespace PortDebug
