// F1 overlay: Sections of the Game and System pages: cutscenes, unlocks, speedrun, Discord,
// updates, the texture pack status and the overlay options. State and helpers shared with the other
// parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_paths.h"
#include "port_discord.h"
#include "port_update_check.h"
#include "port_livesplit.h"
#include "port_skip_cutscenes.h"

#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Player/CGameState.hpp"

#include <aurora/imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL_misc.h>

#if defined(__ANDROID__)
#include "touch_pad.h"
#include <jni.h>
#endif

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>

namespace PortDebug {

void DrawCutscenesSection() {
  ImGui::SeparatorText("Cutscenes");
  const bool locked = BeginOriginalLocked();
  bool skippable = sSkippableCutscenes;
  if (ImGui::Checkbox("Skippable cutscenes", &skippable)) {
    SetSkippableCutscenes(skippable);
  }
  ItemHelp("Every cutscene can be skipped with Start or A, including the ones the game never "
           "lets you skip (randomprime's room patches). Applies to rooms loaded after the change.");
  if (PortSkipCutscenes::Forced()) {
    SameLineAfterHelp();
    ImGui::TextDisabled("(on in randomized games)");
  }
  int elevatorRide = sElevatorRide;
  if (ImGui::Combo("Elevator ride", &elevatorRide, "Original\0Fast\0Skip\0")) {
    SetElevatorRide(static_cast< EElevatorRide >(elevatorRide));
  }
  ItemHelp("The ride shown between worlds. Original lasts at least 5 s even though the port loads "
           "far faster; Fast ends it about 2 s in, once the next area is loaded; Skip shows black "
           "until the area is loaded. The cinematic played in the elevator room before the ride is "
           "not affected.");
  int bootLogos = sBootLogos;
  if (ImGui::Combo("Boot logos", &bootLogos, "Show\0Skip with A or Start\0Off\0")) {
    SetBootLogos(static_cast< EBootLogos >(bootLogos));
  }
  ItemHelp("The Nintendo, Retro and Dolby logos at launch. Skip with A or Start ends the current "
           "logo with a short fade; Off goes straight to the opening (which A or Start skips to "
           "the title). Applies at the next launch.");
  if (ImGui::SliderInt("Scan speed", &sScanSpeed, kScanSpeedMin, kScanSpeedMax, "%d%%",
                       ImGuiSliderFlags_AlwaysClamp)) {
    MarkDirty();
  }
  ItemHelp("How fast the scan visor fills its progress bar. 100% is the original; 200% scans in "
           "half the time. What a scan records is unchanged.");
  bool beamDoors = sBeamDoorsStayUnlocked;
  if (ImGui::Checkbox("Unlock beam doors once opened", &beamDoors)) {
    SetBeamDoorsStayUnlocked(beamDoors);
  }
  ItemHelp("A Wave, Ice, Plasma or Power Beam door you have opened by breaking its force field "
           "stays a plain blue door, on both sides, for the rest of the save: blue look, any beam "
           "opens it, blue map icon and scan. Missile, Bomb and Disabled doors and blast shields "
           "are not affected. Takes effect from the next door you open; turning it off shows the "
           "colours again in rooms loaded afterwards.");
  EndOriginalLocked(locked);

  // Not under the Original experience lock: the stored choice applies there too.
  ImGui::SeparatorText("Revision compatibility");
  const bool revisionForced = PortSkipCutscenes::Forced();
  bool revisionCompat = sRevision100Compatibility || revisionForced;
  ImGui::BeginDisabled(revisionForced);
  if (ImGui::Checkbox("1.00 sequence-break compatibility", &revisionCompat)) {
    SetRevision100Compatibility(revisionCompat);
  }
  ImGui::EndDisabled();
  ItemHelp("Restores supported revision-specific sequence-break behavior on newer/PAL discs. "
           "Requires reloading the game. Automatically enabled for randomizer and Archipelago games.");
  if (revisionForced) {
    SameLineAfterHelp();
    ImGui::TextDisabled("(automatic in randomizer and Archipelago games)");
  }
}

void DrawUnlocksSection() {
  ImGui::SeparatorText("Unlocks");
  const bool locked = BeginOriginalLocked();
  bool hardMode = sUnlockHardMode;
  if (ImGui::Checkbox("Hard mode", &hardMode)) {
    SetUnlockHardMode(hardMode);
  }
  ImGui::SetItemTooltip("Offered when starting a file, as after finishing the game.");
  ImGui::SameLine();
  bool fusionSuit = sUnlockFusionSuit;
  if (ImGui::Checkbox("Fusion Suit", &fusionSuit)) {
    SetUnlockFusionSuit(fusionSuit);
    // The suit choice itself is saved; without the unlock (or a real link)
    // there is no menu left to switch it back off, so drop it here.
    if (!fusionSuit && gpGameState != nullptr &&
        !(gpGameState->SystemState().GetFusionLinked() &&
          gpGameState->SystemState().GetNormalModeBeat())) {
      gpGameState->SystemState().SetHasFusion(false);
      gpGameState->PlayerState()->SetIsFusionEnabled(false);
    }
  }
  ImGui::SetItemTooltip("Under Fusion Bonus; retail needs a GBA link to Metroid Fusion.");
  ImGui::SameLine();
  bool galleries = sUnlockGalleries;
  if (ImGui::Checkbox("Image galleries", &galleries)) {
    SetUnlockGalleries(galleries);
  }
  ImGui::SetItemTooltip("All four of them.");
  ImGui::TextDisabled("What finishing the game unlocks. Not written into the save.");
  ItemHelp("Turning an option off locks it again. Metroid (NES) stays locked: its emulator can't run "
           "in the port.");
  EndOriginalLocked(locked);

}

void DrawSpeedrunSection() {
  ImGui::SeparatorText("Speedrun");
  bool timer = sSpeedrunTimer;
  if (ImGui::Checkbox("On-screen in-game time", &timer)) {
    SetSpeedrunTimer(timer);
  }
  ItemHelp("The play time the save file shows: it stops in cutscenes, menus and loads.");
  bool liveSplit = sLiveSplit;
  if (ImGui::Checkbox("LiveSplit", &liveSplit)) {
    SetLiveSplit(liveSplit);
  }
  ItemHelp("In LiveSplit: right-click, Control > Start TCP Server (port 16834), and compare against "
           "Game Time. A new file resets and starts the timer, the game time follows the in-game "
           "time, and it splits on the final blow.");
  SameLineAfterHelp();
  switch (PortLiveSplit::Status()) {
  case PortLiveSplit::kStatus_Off:
    ImGui::TextDisabled("off");
    break;
  case PortLiveSplit::kStatus_Connecting:
    ImGui::TextUnformatted("connecting...");
    break;
  case PortLiveSplit::kStatus_Connected:
    ImGui::TextColored(ThemeGoodColor(), "connected");
    break;
  case PortLiveSplit::kStatus_Failed:
    ImGui::TextColored(ThemeBadColor(), "%s", PortLiveSplit::LastError().c_str());
    break;
  }
  // Applied when the field loses focus; until then the text is left alone.
  static char address[128];
  static bool editingAddress = false;
  if (!editingAddress) {
    std::snprintf(address, sizeof(address), "%s", sLiveSplitAddress.c_str());
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.f);
  ImGui::InputText("Server (host:port)", address, sizeof(address));
  editingAddress = ImGui::IsItemActive();
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    SetLiveSplitAddress(address);
  }
  ImGui::SameLine();
  bool splitUpgrades = sLiveSplitSplitUpgrades;
  if (ImGui::Checkbox("Split on upgrades", &splitUpgrades)) {
    sLiveSplitSplitUpgrades = splitUpgrades;
    ApplyLiveSplit();
    MarkDirty();
  }
  ImGui::SetItemTooltip("Also splits on each new upgrade or artifact (not expansions or energy tanks).");

}

void DrawDiscordSection() {
  if (PortDiscord::Supported()) {
    ImGui::SeparatorText("Discord");
    bool discord = sDiscord;
    if (ImGui::Checkbox("Rich Presence", &discord)) {
      SetDiscordPresence(discord);
    }
    ImGui::SetItemTooltip("Shows the room, world, energy, missiles and item percentage on your "
                          "Discord profile while the Discord app runs.");
    ImGui::SameLine();
    switch (PortDiscord::Status()) {
    case PortDiscord::kStatus_Off:
      ImGui::TextDisabled("off");
      break;
    case PortDiscord::kStatus_Connecting:
      ImGui::TextUnformatted("connecting...");
      break;
    case PortDiscord::kStatus_Connected:
      ImGui::TextColored(ThemeGoodColor(), "connected");
      break;
    case PortDiscord::kStatus_Failed:
      ImGui::TextColored(ThemeBadColor(), "%s", PortDiscord::LastError().c_str());
      break;
    }
    if (sDiscord) {
      ImGui::TextDisabled("Showing: %s", PortDiscord::CurrentText().c_str());
    }
  }

}

// PrimedGun: upstream's update check stays off (ApplyUpdateCheck), so its checkbox and
// status would only ever read "off". This links this fork's releases instead.
void DrawUpdateSection() {
  ImGui::SeparatorText("Updates");
  if (ImGui::Button("PrimedGun releases")) {
    SDL_OpenURL("https://github.com/iChris4/MetroidPrimePortVR/releases");
  }
  ImGui::SetItemTooltip("Opens github.com/iChris4/MetroidPrimePortVR/releases in the browser.");
}

#if defined(__ANDROID__)
// The folder picker's progress, set from the Java copy thread.
std::mutex sTexturePackStatusMutex;
std::string sTexturePackStatus;

std::string TexturePackStatus() {
  std::lock_guard lock(sTexturePackStatusMutex);
  return sTexturePackStatus;
}

void SetTexturePackStatus(std::string status) {
  std::lock_guard lock(sTexturePackStatusMutex);
  sTexturePackStatus = std::move(status);
}

// MetroidPrimeActivity.pickTexturePack opens the system folder picker and
// copies the chosen folder in the background, reporting back through the
// nativeTexturePack* functions below.
void PickTexturePack() {
  JNIEnv* env = static_cast< JNIEnv* >(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast< jobject >(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    SetTexturePackStatus("Could not open the folder picker.");
    return;
  }
  jclass cls = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(cls, "pickTexturePack", "(Ljava/lang/String;)V");
  if (method != nullptr) {
    // The pack is copied into the data folder, wherever that is.
    jstring folder = env->NewStringUTF(PortPaths::UserFolder().c_str());
    env->CallVoidMethod(activity, method, folder);
    env->DeleteLocalRef(folder);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    SetTexturePackStatus("Could not open the folder picker.");
  }
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(activity);
}
#endif

// The look of this overlay: the floating window, the theme and the menu sounds.
void DrawOverlaySection() {
  ImGui::SeparatorText("Overlay");
#if !defined(__ANDROID__)
  if (ImGui::Checkbox("Overlay as a floating window", &sOverlayWindowed)) {
    MarkDirty();
  }
#endif
  {
    int theme = sUiTheme;
    if (ImGui::Combo("Menu theme", &theme, "Auto\0Metroid Prime\0Remastered\0Plain\0")) {
      sUiTheme = theme;
      MarkDirty();
    }
    ItemHelp("The look of this overlay. Auto uses the Remastered style while the Remastered import is "
             "loaded and the Metroid Prime pause-screen style otherwise; Plain is stock Dear ImGui.");
    if (ImGui::Checkbox("Menu sounds", &sUiSounds)) {
      MarkDirty();
    }
    ItemHelp("The game's own menu sounds for opening, choosing and moving through the overlay. Off "
             "with the Plain theme.");
  }
}

} // namespace PortDebug
