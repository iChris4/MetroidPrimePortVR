// F1 overlay: The Remastered import, importers and mods sections. State and helpers shared with the
// other parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_pointer.h"
#include "port_mods.h"
#include "port_importers.h"
#include "port_remastered_import.h"
#include "port_remastered_text.h"
#include "port_savestate.h"

#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_misc.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace PortDebug {

void DrawRemasteredImport() {
  static char sImage[1024] = "";
  static char sKeys[1024] = "";
#if defined(__ANDROID__)
  static std::string sPickNames[2];
#endif
  static bool sFilled = false;
  const auto remember = [](int which, const std::string& path) {
    std::string& saved = which == 0 ? sRemasteredImagePath : sRemasteredKeysPath;
    if (!path.empty() && saved != path) {
      saved = path;
      MarkDirty();
    }
  };
  if (!sFilled) {
    sFilled = true;
    std::snprintf(sKeys, sizeof(sKeys), "%s", PortRemastered::DefaultKeysPath().c_str());
#if defined(__ANDROID__)
    // The files picked last time open again as if picked now.
    std::lock_guard lock(sRemasteredPickMutex);
    if (!sRemasteredImagePath.empty()) {
      sRemasteredPicks.emplace_back(0, sRemasteredImagePath);
    }
    if (!sRemasteredKeysPath.empty()) {
      sRemasteredPicks.emplace_back(1, sRemasteredKeysPath);
    }
    // A pick the previous process never saw: after the remembered ones, so it wins.
    TakeRemasteredPickFiles();
#else
    if (!sRemasteredImagePath.empty()) {
      std::snprintf(sImage, sizeof(sImage), "%s", sRemasteredImagePath.c_str());
    }
    if (!sRemasteredKeysPath.empty()) {
      std::snprintf(sKeys, sizeof(sKeys), "%s", sRemasteredKeysPath.c_str());
    }
#endif
  }
  {
    std::lock_guard lock(sRemasteredPickMutex);
#if defined(__ANDROID__)
    for (const auto& [which, path] : sRemasteredPicks) {
      const std::string opened = OpenRemasteredPick(which, path);
      std::snprintf(which == 0 ? sImage : sKeys, sizeof(sImage), "%s", opened.c_str());
      sPickNames[which] = opened.empty() ? "could not be opened" : RemasteredPickName(path);
      if (!opened.empty()) {
        remember(which, path);
      }
      std::remove(RemasteredPickFile(which).c_str());
    }
#else
    for (const auto& [which, path] : sRemasteredPicks) {
      std::snprintf(which == 0 ? sImage : sKeys, sizeof(sImage), "%s", path.c_str());
      remember(which, path);
    }
#endif
    sRemasteredPicks.clear();
  }
  if (!ImGui::CollapsingHeader("Metroid Prime Remastered models")) {
    return;
  }
  const PortRemastered::ImportState state = PortRemastered::ImportStatus();
  ImGui::TextWrapped("Converts the models of your own copy of Metroid Prime Remastered into a mod. It needs the "
                     "game's .nsp or .xci and your console's key file (prod.keys), and takes a few minutes.");
#if defined(__ANDROID__)
  ImGui::TextDisabled("It is installed into the app's own storage (remastered-imports), not into the mods folder: "
                      "with the data folder on shared storage, moving a folder there goes file by file through "
                      "Android's media provider, which is what made an install slow. The data folder setting does "
                      "not move it, an app update keeps it, and an uninstall or \"clear data\" removes it, so it "
                      "has to be imported again.");
  ImGui::TextDisabled("A remastered-models in the mods folder keeps working as the fallback until a full import "
                      "installs the app's own copy; nothing deletes it by itself.");
#endif
  if (PortMods::StaleRemasteredImport() != nullptr) {
    ImGui::TextColored(ThemeWarnColor(),
                       "Re-import needed: the installed models were made by an older version.");
  }
  ImGui::BeginDisabled(state.running);
#if defined(__ANDROID__)
  // No path to type here: the files are picked, and shown by name.
  if (ImGui::Button("Pick the .nsp/.xci...##remastered-image")) {
    OpenRemasteredDialog(0);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sPickNames[0].empty() ? "Metroid Prime Remastered .nsp or .xci" : sPickNames[0].c_str());
  if (ImGui::Button("Pick the keys...##remastered-keys")) {
    OpenRemasteredDialog(1);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sPickNames[1].empty() ? "prod.keys" : sPickNames[1].c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Android may close the game while you pick a file. The pick is kept: open this page again.");
  ImGui::PopStyleColor();
#else
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("##remastered-image", "Metroid Prime Remastered .nsp or .xci", sImage, sizeof(sImage));
  ImGui::SameLine();
  if (ImGui::Button("Browse...##remastered-image")) {
    OpenRemasteredDialog(0);
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("##remastered-keys", "prod.keys", sKeys, sizeof(sKeys));
  ImGui::SameLine();
  if (ImGui::Button("Browse...##remastered-keys")) {
    OpenRemasteredDialog(1);
  }
#endif
  ImGui::EndDisabled();
  if (state.running) {
    // The whole import, step by step, and within a step as far as it can say.
    const float within = state.stepTotal > 0 ? std::min(1.f, float(state.stepDone) / float(state.stepTotal)) : 0.f;
    float fraction = state.total > 0 ? float(state.done) / float(state.total) : 0.f;
    std::string label = state.cancelling ? "Cancelling..." : state.message;
    if (state.steps > 0 && state.step > 0) {
      fraction = (float(state.step - 1) + within) / float(state.steps);
      if (!state.cancelling) {
        label = "Step " + std::to_string(state.step) + " of " + std::to_string(state.steps) + ": " + state.message;
      }
    }
    ImGui::ProgressBar(fraction, ImVec2(-1.f, 0.f), label.c_str());
    if (sImportUnloadedMods) {
      ImGui::TextDisabled("The mods are unloaded until the import ends.");
      DrawModReloadMessage();
    }
    ImGui::BeginDisabled(state.cancelling || state.installing);
    if (ImGui::Button("Cancel##remastered")) {
      PortRemastered::CancelImport();
    }
    ImGui::EndDisabled();
    if (state.cancelling) {
      ImGui::SameLine();
      ImGui::TextDisabled("Stopping at the next file, which can take a moment.");
    }
  } else {
    if (ImGui::TreeNode("remastered-parts", "What to import")) {
      // Kept as the parts left out, so a part added later is imported by default.
      PortRemastered::ImportParts parts = RemasteredImportParts();
      bool changed = false;
      changed |= ImGui::Checkbox("Room geometry", &parts.geometry);
      ItemHelp("Every model the rooms place. Most of the import's size (about 5.5 GB of 6.5 GB) and time; "
               "the game has to be restarted to draw it.");
      changed |= ImGui::Checkbox("Particle effects", &parts.effects);
      changed |= ImGui::Checkbox("HUD", &parts.hud);
      changed |= ImGui::Checkbox("Text", &parts.text);
      changed |= ImGui::Checkbox("Movies", &parts.movies);
      changed |= ImGui::Checkbox("Gallery", &parts.gallery);
      if (changed) {
        SetRemasteredImportParts(parts);
      }
      ImGui::TextDisabled("Used by the next import. The MP_REMASTERED_* switches can still leave a part out.");
      ImGui::TreePop();
    }
    ImGui::BeginDisabled(sImage[0] == '\0' || sKeys[0] == '\0');
#if !defined(__ANDROID__)
    // A typed path is kept once it is used (Android keeps what was picked).
    const auto rememberTyped = [&] {
      remember(0, sImage);
      remember(1, sKeys);
    };
#else
    const auto rememberTyped = [] {};
#endif
    if (ImGui::Button("Import##remastered")) {
      rememberTyped();
      StartRemasteredImport(sImage, sKeys);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("The mods are unloaded while it runs, and loaded again with the new import when it ends.");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool remakeAll = PortRemastered::RemakeAll();
    if (ImGui::Checkbox("Full import (no reuse)##remastered", &remakeAll)) {
      PortRemastered::SetRemakeAll(remakeAll);
    }
    ItemHelp("Converts everything again instead of reusing the previous import, so it takes as long "
             "as the first one. Normally only the stages whose inputs changed are made again. Lasts until the game is closed.");
    if (state.finished && state.ok) {
      ImGui::PushStyleColor(ImGuiCol_Text, ThemeGoodColor(ImVec4(0.5f, 1.f, 0.5f, 1.f)));
      ImGui::TextWrapped("%s", state.message.c_str());
      ImGui::PopStyleColor();
      if (state.failed > 0) {
        ImGui::TextColored(ThemeWarnColor(), "%d of %d models could not be converted and keep the disc's look; "
                           "the notes below say why.", state.failed, state.total);
      }
      // It loads on its own when the import ends; this is to load it again
      // by hand.
      if (ImGui::Button("Load it again##remastered")) {
        PortSaveState::RequestModReload();
      }
      DrawModReloadMessage();
    } else if (state.finished && state.cancelled) {
      ImGui::TextDisabled("The import was cancelled.");
    } else if (state.finished) {
      ImGui::PushStyleColor(ImGuiCol_Text, ThemeBadColor());
      ImGui::TextWrapped("The import failed: %s", state.message.c_str());
      ImGui::PopStyleColor();
    }
  }
  if (!state.lines.empty() && ImGui::TreeNode("remastered-log", "%d notes", int(state.lines.size()))) {
    if (ImGui::SmallButton("Copy the notes##remastered")) {
      std::string all;
      for (const std::string& line : state.lines) {
        all += line;
        all += '\n';
      }
      ImGui::SetClipboardText(all.c_str());
    }
    if (state.lines.size() >= PortRemastered::kImportMaxLines) {
      ImGui::SameLine();
      ImGui::TextDisabled("(the newest %d)", int(PortRemastered::kImportMaxLines));
    }
    for (const std::string& line : state.lines) {
      ImGui::TextWrapped("%s", line.c_str());
    }
    ImGui::TreePop();
  }
}

namespace {
#if !defined(__ANDROID__)
// Importers (port_importers.h): the user's own programs that build a mod.
// Nothing is drawn until the importers folder holds one.
void DrawImporters() {
  // The folder is read when the panel opens and after a run, not every frame.
  static std::vector<std::string> sNames;
  static int sListedFrame = -2;
  static char sArgument[512] = "";
  const int frame = ImGui::GetFrameCount();
  const PortImporters::State& state = PortImporters::Poll();
  if (sListedFrame != frame - 1 || ImGui::IsWindowAppearing()) {
    sNames = PortImporters::List();
  }
  sListedFrame = frame;
  if (sNames.empty() && !state.running && !state.finished) {
    return;
  }
  ImGui::SeparatorText("Importers");
  ImGui::BeginDisabled(state.running);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
  ImGui::InputTextWithHint("Argument", "optional, e.g. the file to import", sArgument, sizeof(sArgument));
  ItemHelp("An importer is a program in the importers folder beside the mods folder; it builds a mod "
           "from files of your own. The argument is passed to it.");
  for (const std::string& name : sNames) {
    if (ImGui::Button(("Run " + name).c_str())) {
      PortImporters::Start(name, sArgument);
    }
  }
  ImGui::EndDisabled();
  if (state.running) {
    ImGui::TextColored(ThemeWarnColor(), "%s is running...", state.name.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      PortImporters::Cancel();
    }
  } else if (state.finished && state.exitCode == 0) {
    ImGui::TextColored(ThemeGoodColor(ImVec4(0.5f, 1.f, 0.5f, 1.f)), "%s finished. Reload the mods to load it.",
                       state.name.c_str());
  } else if (state.finished && state.cancelled) {
    ImGui::TextDisabled("%s was cancelled.", state.name.c_str());
  } else if (state.finished) {
    ImGui::TextColored(ThemeBadColor(), "%s failed (exit code %d).", state.name.c_str(), state.exitCode);
  }
  if (!state.lines.empty()) {
    // The tail of the output; a failure shows more of it.
    const size_t shown = std::min<size_t>(state.lines.size(), state.finished && state.exitCode != 0 && !state.cancelled ? 12 : 3);
    for (size_t i = state.lines.size() - shown; i < state.lines.size(); ++i) {
      ImGui::TextWrapped("%s", state.lines[i].c_str());
    }
  }
}
#endif
} // namespace

void DrawMods() {
  ImGui::SeparatorText("Mods");
  const PortMods::Status& status = PortMods::CurrentStatus();
  if (sOriginalExperience) {
    ImGui::TextDisabled("Original experience is on: no mods are loaded.");
  }
  bool enabled = sModsEnabled;
  if (ImGui::Checkbox("Load mods", &enabled)) {
    SetModsEnabled(enabled);
  }
  ItemHelp("Each folder in the mods folder is a mod; later names win. A file at a disc path "
           "(Metroid1.pak, Audio/..., Video/...) replaces that file, and a resource named by id and "
           "type (1A2B3C4D.TXTR) replaces it in every PAK. Mods load at startup.");
  // What the settings would load next time, against what this launch loaded.
  std::vector<std::string> disabled = PortMods::SplitDisabled(sModsDisabled);
  bool changed = ModsEnabled() != status.active;
  bool listed = false;
  for (const PortMods::ModInfo& mod : status.mods) {
    const auto found = std::find(disabled.begin(), disabled.end(), mod.name);
    bool on = found == disabled.end();
    // The Remastered import is switched on its own page.
    if (mod.name == PortRemastered::kImportModName) {
      changed = changed || (ModsEnabled() && on) != mod.enabled;
      continue;
    }
    listed = true;
    ImGui::BeginDisabled(!sModsEnabled);
    if (ImGui::Checkbox(mod.name.c_str(), &on)) {
      if (on) {
        disabled.erase(std::remove(disabled.begin(), disabled.end(), mod.name), disabled.end());
      } else {
        disabled.push_back(mod.name);
      }
      SetModsDisabled(PortMods::JoinDisabled(disabled));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (mod.enabled) {
      if (mod.textures > 0) {
        ImGui::TextDisabled("%d file(s), %d resource(s), %d native texture(s)", mod.files, mod.resources, mod.textures);
      } else {
        ImGui::TextDisabled("%d file(s), %d resource(s)", mod.files, mod.resources);
      }
    } else {
      ImGui::TextDisabled("not loaded");
    }
    if (mod.importStale) {
      ImGui::SameLine();
      ImGui::TextColored(ThemeWarnColor(), "re-import needed");
    }
    changed = changed || (ModsEnabled() && on) != mod.enabled;
  }
  if (!listed) {
    ImGui::TextDisabled("No mods in the folder.");
  }
  if (PortMods::Suspended()) {
    ImGui::TextColored(ThemeWarnColor(), "Unloaded while the Remastered import runs; they load again when it ends.");
  } else if (changed) {
    ImGui::TextColored(ThemeWarnColor(), "Reload the mods to apply.");
  }
  if (ImGui::Button("Reload mods")) {
    PortSaveState::RequestModReload();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Reads the mods folder again and reloads the room, as a save state does.");
  }
#if !defined(__ANDROID__)
  if (!status.folder.empty()) {
    ImGui::SameLine();
    if (ImGui::Button("Open mods folder")) {
      std::string url = status.folder.front() == '/' ? "file://" : "file:///";
      for (const char c : status.folder) {
        url += c == ' ' ? std::string("%20") : std::string(1, c == '\\' ? '/' : c);
      }
      SDL_OpenURL(url.c_str());
    }
  }
#endif
  DrawModReloadMessage();
  for (const std::string& message : status.messages) {
    ImGui::TextColored(ThemeBadColor(), "%s", message.c_str());
  }
  if (PortMods::NativeTextureCount() > 0) {
    ImGui::TextDisabled("Native textures: %zu, %zu in use", PortMods::NativeTextureCount(), PortMods::NativeTexturesBound());
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("Folder: %s", status.folder.c_str());
  ImGui::PopStyleColor();
#if !defined(__ANDROID__)
  DrawImporters();
#endif
}
} // namespace PortDebug
