// F1 overlay: The Remastered tab, PAL languages and Remastered debugging. State and helpers shared
// with the other parts are in debug_ui_internal.h.

#include "debug_ui_internal.h"

#include "port_env.h"
#include "port_debug.h"
#include "port_gyro_motion.h"
#include "port_pointer.h"
#include "port_room_env.h"
#include "port_room_geo.h"
#include "port_paths.h"
#include "port_mods.h"
#include "port_remastered_import.h"
#include "port_remastered_text.h"
#include "port_savestate.h"
#include "port_disc.h"
#include "port_pal_languages.h"

#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "MetroidPrime/Player/CGameOptions.hpp"

#include <aurora/imgui.h>
#include <dolphin/gx/GXExtra.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace PortDebug {

void DrawRemasteredWarning() {
  {
    const ImVec4 bad = ThemeBadColor();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(bad.x * 0.25f, bad.y * 0.25f, bad.z * 0.25f, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Border, bad);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.f);
    ImGui::BeginChild("RemasteredWarning", ImVec2(0.f, 0.f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextColored(bad, "EXPERIMENTAL - EXPECT BREAKAGE");
    ImGui::SetWindowFontScale(1.f);
    ImGui::TextWrapped("Remastered support is unfinished and broken in places. Expect wrong materials, missing or "
                       "misplaced models and effects, visual glitches, slowdowns and crashes. Re-imports are often "
                       "needed after updates.");
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
  }

}

// The PAL disc's languages for a USA one (port_pal_languages.h).
void DrawPalLanguages() {
  if (PortDisc::Current() == PortDisc::Version::Pal) {
    ImGui::TextDisabled("PAL disc: support is experimental.");
    ItemHelp("The port runs the USA 1.00 code on the PAL disc's data. It has been checked on every "
             "room, but less played than a USA disc: please report anything that differs.");
    return;
  }
  std::string picked;
  {
    std::lock_guard lock(sRemasteredPickMutex);
#if defined(__ANDROID__)
    // A pick the previous process never saw (the game was killed behind the picker).
    static bool sTookFile = false;
    if (!sTookFile) {
      sTookFile = true;
      std::ifstream in(RemasteredPickFile(kPalLanguagesPick));
      std::string uri;
      if (std::getline(in, uri) && !uri.empty()) {
        sPalLanguagesPick = uri;
      }
    }
#endif
    picked.swap(sPalLanguagesPick);
  }
  static std::string sStartError;
  if (!picked.empty()) {
#if defined(__ANDROID__)
    std::remove(RemasteredPickFile(kPalLanguagesPick).c_str());
#endif
    sStartError = PortPalLanguages::Start(picked) ? "" : "An import is already running.";
  }
  const PortPalLanguages::State state = PortPalLanguages::Status();
  const int installed = PortPalLanguages::Installed();
  ImGui::BeginDisabled(state.running);
  if (ImGui::Button(installed > 0 ? "Import the PAL languages again..." : "Add languages from a PAL disc...")) {
    OpenRemasteredDialog(kPalLanguagesPick);
  }
  ImGui::EndDisabled();
  ItemHelp("For a USA disc: pick your own PAL Metroid Prime image (GM8P01) to add its French, German, "
           "Spanish and Italian text and its fonts. Only the text is taken; the game stays the USA "
           "one. The disc's full Flaahgra boss theme is added too (F1 > Settings > Audio). Restart "
           "afterwards so every menu and the font take it.");
  if (installed > 0 && !state.running) {
    ImGui::SameLine();
    if (ImGui::Button("Remove")) {
      PortPalLanguages::Remove();
    }
    ImGui::SetItemTooltip("Deletes the imported PAL languages. Text already loaded stays until a restart.");
  }
  if (state.running) {
    ImGui::TextDisabled("%s...", state.message.c_str());
  } else if (state.finished) {
    if (state.ok) {
      ImGui::TextWrapped("%s", state.message.c_str());
    } else {
      ImGui::TextColored(ThemeWarnColor(), "%s", state.message.c_str());
    }
  } else if (!sStartError.empty()) {
    ImGui::TextColored(ThemeWarnColor(), "%s", sStartError.c_str());
  } else if (installed > 0) {
    ImGui::TextDisabled("PAL languages added (%d text tables).", installed);
  }
}

void DrawLanguageSection() {
  ImGui::SeparatorText("Language");
  {
    int language = 0;
    const char* current = TextLanguage();
    for (size_t i = 0; i < PortRemastered::kTextLanguageCount; ++i) {
      if (std::strcmp(current, PortRemastered::kTextLanguages[i].code) == 0) {
        language = static_cast< int >(i) + 1;
      }
    }
    const auto name = [](void*, int index) {
      return index == 0 ? "English" : PortRemastered::kTextLanguages[index - 1].name;
    };
    if (ImGui::Combo("Language", &language, name, nullptr,
                     static_cast< int >(PortRemastered::kTextLanguageCount) + 1)) {
      SetTextLanguage(language == 0 ? "" : PortRemastered::kTextLanguages[language - 1].code);
    }
    ItemHelp("The language of the game's text. A USA disc has only English; a PAL disc also has French, "
             "German, Spanish and Italian (or add them below), and the Remastered import adds its "
             "languages (its text wins). Text missing in a language stays English. Text already on screen "
             "changes the next time its menu or screen opens; the font changes after a restart.");
  }
  DrawPalLanguages();

}

// The saved settings for Remastered's rooms, at the top of Debug's room models header.
void DrawRemasteredRoomModels(bool geoReady) {
  ImGui::BeginDisabled(!geoReady);
  static const char* const kModes[] = {"off", "in place of the room", "on top of the room"};
  int mode = int(PortRoomGeo::GetMode());
  if (ImGui::Combo("Room geometry", &mode, kModes, 3)) {
    PortRoomGeo::SetMode(PortRoomGeo::Mode(mode));
  }
  ImGui::SetItemTooltip("Recommended: in place of the room.\n"
                        "Draws a mod's Remastered room models instead of the original rooms, or on\n"
                        "top of them to compare the two. Does nothing without a room geometry mod.");
#ifdef __ANDROID__
  // The defaults (MSAA off, 1x) are fine; this catches a phone set up for the original rooms.
  if (PortRoomGeo::GetMode() != PortRoomGeo::Mode::Off && (EffectiveMsaa() > 1 || EffectiveRenderScale() <= 0.f || EffectiveRenderScale() > 2.f)) {
    ImGui::TextColored(ThemeWarnColor(), "Slow on phones with MSAA or native scale.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Use 2x, MSAA off")) {
      SetMsaa(1);
      SetRenderScale(2.f);
      MarkDirty();
    }
    ImGui::SetItemTooltip("Turns anti-aliasing off and sets the EFB scale to 2x (the Quality settings).\n"
                          "The Remastered rooms draw many more models, and a phone's GPU pays for\n"
                          "each one at every pixel it renders.");
  }
#endif
  bool areaLights = PortRoomGeo::AreaLights();
  if (ImGui::Checkbox("Take the area's lights", &areaLights)) {
    PortRoomGeo::SetAreaLights(areaLights);
  }
  ImGui::SetItemTooltip("Recommended: off.\n"
                        "Also lights the Remastered rooms with the game's own lights where the room\n"
                        "has baked light. Off matches Remastered.");
  float minPixels = PortRoomGeo::MinPixels();
  if (ImGui::SliderFloat("Skip small models", &minPixels, 0.f, 8.f, minPixels > 0.f ? "under %.1f px" : "off")) {
    PortRoomGeo::SetMinPixels(minPixels);
    MarkDirty();
  }
  ImGui::SetItemTooltip("Leaves out room models that look smaller than this on screen (in the game's\n"
                        "own 480-line pixels, whatever the render scale). Raise it if frames are\n"
                        "slow; 0 draws everything.");
  float lodDistance = PortRoomGeo::LodDistance();
  if (ImGui::SliderFloat("Detail distance", &lodDistance, 0.f, 4.f, lodDistance > 0.f ? "x%.2f" : "full detail")) {
    PortRoomGeo::SetLodDistance(lodDistance);
    MarkDirty();
  }
  ImGui::SetItemTooltip("Where room models switch to Remastered's simpler versions of themselves\n"
                        "farther away: 1 is Remastered's own distances, higher keeps the full models\n"
                        "farther out, 0 never switches. Lower it if frames are slow. Needs an import\n"
                        "made by this version.");
  ImGui::EndDisabled();
  bool resident = sRoomGeoResident;
  if (ImGui::Checkbox("Keep on the GPU (next start)", &resident)) {
    PortDebug::SetRoomGeoResident(resident);
  }
  ImGui::SetItemTooltip(
      "Recommended: on.\n"
      "Uploads a room geometry mod's models once when they load instead of\n"
      "every frame, so a frame's buffers can be smaller. Takes effect from the\n"
      "next start.");
  ImGui::Separator();
}

// Switches the imported mod on and off, as the Mods page does for the others.
void DrawRemasteredModToggle() {
  const PortMods::Status& status = PortMods::CurrentStatus();
  const auto mod = std::find_if(status.mods.begin(), status.mods.end(), [](const PortMods::ModInfo& m) {
    return m.name == PortRemastered::kImportModName;
  });
  if (mod == status.mods.end()) {
    ImGui::TextDisabled("Not imported yet.");
    return;
  }
  std::vector<std::string> disabled = PortMods::SplitDisabled(sModsDisabled);
  bool on = std::find(disabled.begin(), disabled.end(), mod->name) == disabled.end();
  ImGui::BeginDisabled(!sModsEnabled);
  if (ImGui::Checkbox("Use the Remastered models", &on)) {
    if (on) {
      disabled.erase(std::remove(disabled.begin(), disabled.end(), mod->name), disabled.end());
    } else {
      disabled.push_back(mod->name);
    }
    SetModsDisabled(PortMods::JoinDisabled(disabled));
  }
  ImGui::EndDisabled();
  ItemHelp("Off plays the retail game. Takes effect when the mods are reloaded.");
  if (!sModsEnabled) {
    SameLineAfterHelp();
    ImGui::TextDisabled("(mods are off: Mods page)");
  }
  if (PortMods::Suspended()) {
    return;
  }
  if ((ModsEnabled() && on) != mod->enabled) {
    ImGui::TextColored(ThemeWarnColor(), "Reload the mods to apply.");
    ImGui::SameLine();
    if (ImGui::Button("Reload mods##remastered")) {
      PortSaveState::RequestModReload();
    }
  }
}

void DrawRemasteredTab() {
  DrawRemasteredWarning();
  ImGui::SeparatorText("Remastered models");
  DrawRemasteredModToggle();
  ImGui::SeparatorText("Import");
  DrawRemasteredImport();

  ImGui::SeparatorText("Gallery");
  {
    const std::vector<std::string> pictures = PortMods::GalleryPaths();
    if (pictures.empty()) {
      ImGui::TextDisabled("Remastered's concept art. It comes with the Remastered import.");
    } else if (ImGui::Button(("Open gallery (" + std::to_string(pictures.size()) + " pictures)").c_str())) {
      sGalleryPaths = pictures;
      sGalleryIndex = std::min(sGalleryIndex, int(pictures.size()) - 1);
      ReleaseGalleryTexture();
      sGalleryOpen = true;
    }
  }
}

// The Remastered pass tests and inspectors, for the Debug page.
void DrawRemasteredDebug() {
  ImGui::SeparatorText("Remastered");
  // What the middle of the screen looks at.
  float origin[3];
  float forward[3];
  const bool inWorld = PortDebug::ViewRay(origin, forward);

  if (ImGui::CollapsingHeader("Remastered room models")) {
    // The frame's buffers are only sized for room geometry when the game started with some.
    const bool geoReady = PortRoomGeo::BuffersReady();
    if (!geoReady && PortMods::RoomGeometryLoaded()) {
      ImGui::TextColored(ThemeWarnColor(), "Restart the game to see the Remastered rooms.");
      ImGui::SetItemTooltip("The game started without room geometry installed, so it set no room aside\n"
                            "for it. Once it starts with some, mods can be changed without a restart.");
    }
    DrawRemasteredRoomModels(geoReady);
    ImGui::BeginDisabled(!geoReady);
    bool merged = PortRoomGeo::MergedDraws();
    if (ImGui::Checkbox("Draw merged copies", &merged)) {
      PortRoomGeo::SetMergedDraws(merged);
    }
    ImGui::SetItemTooltip("Recommended: on. Not saved.\n"
                          "Draws repeated props (rocks, grass) near each other as one model, lit as\n"
                          "a group. Off draws each copy on its own, with its own lights: slower, but a\n"
                          "way to check whether the merge changes how something looks.");
    bool frontToBack = PortRoomGeo::FrontToBack();
    if (ImGui::Checkbox("Draw nearest first", &frontToBack)) {
      PortRoomGeo::SetFrontToBack(frontToBack);
    }
    ImGui::SetItemTooltip("Recommended: on. Not saved.\n"
                          "Draws the room's solid models nearest first, so the GPU skips shading what\n"
                          "they hide. Looks the same either way; off is for comparing frame rates.");
    bool prepass = PortRoomGeo::DepthPrepass();
    if (ImGui::Checkbox("Depth pre-pass for cut-outs", &prepass)) {
      PortRoomGeo::SetDepthPrepass(prepass);
    }
    ImGui::SetItemTooltip("Recommended: off. Not saved. Needs \"Draw nearest first\".\n"
                          "Draws grass and leaves once for depth only, then shades just the parts that\n"
                          "stay in front. Looks the same either way; slower on the phones tried so far.");
    static constexpr const char* kCostTests[] = {"Off", "Flat", "No lights", "No ambient volume",
                                                 "No reflections", "No normal maps", "No ORM/emissive maps",
                                                 "Maps only", "Maps without anisotropy",
                                                 "Maps without anisotropy or mip blend",
                                                 "No post-processing", "No screen copies", "No bloom",
                                                 "Post without the frame copy",
                                                 "Post without the depth reload",
                                                 "Post always reloading the depth"};
    static bool gpuTimes = false;
    if (ImGui::Checkbox("GPU pass times", &gpuTimes)) {
      WantGpuTimes(kGpuTimesPanel, gpuTimes);
    }
    ImGui::SetItemTooltip("Not saved. Times each render pass on the GPU (timestamp queries), averaged\n"
                          "over 60 frames. Total is the sum of the passes; span is first start to last end.");
    if (gpuTimes) {
      if (!GXPortGpuTimesSupported()) {
        ImGui::TextUnformatted("timestamps unsupported");
      } else {
        GXPortGpuTime times[32];
        float total = 0.f;
        float span = 0.f;
        const u32 count = GXPortGetGpuTimes(times, 32, &total, &span);
        if (count == 0) {
          ImGui::TextUnformatted("collecting...");
        } else if (ImGui::BeginTable("gputimes", 3, ImGuiTableFlags_SizingFixedFit)) {
          ImGui::TableSetupColumn("Pass");
          ImGui::TableSetupColumn("ms");
          ImGui::TableSetupColumn("x");
          ImGui::TableHeadersRow();
          for (u32 i = 0; i < count; ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(times[i].name);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", times[i].msPerFrame);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", times[i].passesPerFrame);
          }
          ImGui::EndTable();
          ImGui::Text("Total %.3f ms, span %.3f ms", total, span);
        }
      }
    }
    int costTest = int(GXGetPBRCostTest());
    if (ImGui::Combo("Shading cost test", &costTest, kCostTests, IM_ARRAYSIZE(kCostTests))) {
      GXSetPBRCostTest(u32(costTest));
    }
    ImGui::SetItemTooltip("Not saved. A speed test: leaves out one part of the Remastered surfaces'\n"
                          "shading (Flat leaves out all of it but the colour map), so the frame rate\n"
                          "shows what that part costs. Maps only is Flat with every map still read, to\n"
                          "tell reading the maps from the maths. Looks wrong on purpose. The two\n"
                          "without anisotropy shade in full but filter the maps more cheaply, and look a\n"
                          "little softer. The rest time the frame around the shading: no bloom and\n"
                          "colour grade, no copies of the screen for effects such as heat haze, and\n"
                          "then one part of the post-processing at a time: the bloom (the grade stays),\n"
                          "its copy of the frame, and reloading depth for the HUD after it. The last\n"
                          "looks right: it reloads the depth even when the HUD doesn't need it.");
    ImGui::EndDisabled();

    static std::string picked;
    static uint32_t pickedModel = 0;
    static std::string materials;
    ImGui::BeginDisabled(!inWorld);
    if (ImGui::Button("Pick the model ahead")) {
      picked.clear();
      pickedModel = PortRoomGeo::Pick(CVector3f(origin[0], origin[1], origin[2]),
                                      CVector3f(forward[0], forward[1], forward[2]), picked);
      materials = pickedModel != 0 ? PortRoomGeo::Materials(pickedModel) : std::string();
      if (picked.empty()) {
        picked = "No room geometry ahead.";
      }
    }
    ImGui::SetItemTooltip("Names the room model in the middle of the screen and lists its materials.");
    ImGui::EndDisabled();
    if (pickedModel != 0) {
      ImGui::SameLine();
      if (ImGui::Button("Hide it")) {
        PortRoomGeo::SetHidden(pickedModel, true);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Show all")) {
      PortRoomGeo::SetHidden(0, false);
    }
    if (!picked.empty()) {
      ImGui::TextUnformatted(picked.c_str());
    }
    if (!materials.empty() && ImGui::TreeNode("Materials of the first")) {
      ImGui::TextUnformatted(materials.c_str());
      ImGui::TreePop();
    }
  }

  if (ImGui::CollapsingHeader("Remastered materials and lighting")) {
    int view = PortDebug::PbrView();
    if (ImGui::BeginCombo("PBR surfaces show", view == 0 ? "the shaded result" : PortDebug::PbrViewName(view))) {
      for (int i = 0; i < PortDebug::PbrViewCount(); ++i) {
        if (ImGui::Selectable(i == 0 ? "the shaded result" : PortDebug::PbrViewName(i), i == view)) {
          PortDebug::SetPbrView(i);
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Recommended: the shaded result.\n"
                          "The other choices show one input of a mod's PBR materials alone: base colour\n"
                          "(albedo), normals, roughness, metalness, ambient occlusion, ambient light,\n"
                          "reflection, glow, exposure, or the material kind.");
    static const char* const kProbes[] = {"off", "on", "mirror", "window"};
    int probe = std::clamp(CCubeMaterial::sPortPBRProbeMode, 0, 3);
    if (ImGui::Combo("Reflection probe", &probe, kProbes, 4)) {
      CCubeMaterial::sPortPBRProbeMode = probe;
    }
    ImGui::SetItemTooltip("Recommended: on.\n"
                          "Where shiny PBR surfaces get their reflections: the room's own cube when\n"
                          "the mod has one, otherwise a live capture of the world around the camera.\n"
                          "Mirror and window draw surfaces as perfect mirrors of it, or windows onto it,\n"
                          "to check what it holds.");

    bool env = PortRoomEnv::Enabled();
    if (ImGui::Checkbox("Room environments", &env)) {
      PortRoomEnv::SetEnabled(env);
    }
    ImGui::SetItemTooltip("Recommended: on.\n"
                          "Uses the lighting a mod brings for each room: its reflection cubes, baked\n"
                          "ambient light, exposure and bloom.");
    ImGui::BeginDisabled(!env);
    ImGui::SameLine();
    bool exposed = PortRoomEnv::RoomExposed();
    if (ImGui::Checkbox("Exposure by room", &exposed)) {
      PortRoomEnv::SetRoomExposed(exposed);
    }
    ImGui::SetItemTooltip("Recommended: on.\n"
                          "Sets the brightness as Remastered does, from the room the camera is in and\n"
                          "its tone curve. Off exposes every reflection cube to a neutral middle grey.");
    ImGui::SameLine();
    bool volumes = PortRoomEnv::VolumesEnabled();
    if (ImGui::Checkbox("Baked light per pixel", &volumes)) {
      PortRoomEnv::SetVolumesEnabled(volumes);
    }
    ImGui::SetItemTooltip("Recommended: on.\n"
                          "Lights the Remastered rooms from the room's grid of baked light, so the light\n"
                          "changes across a surface. Off gives each model one ambient colour.");
    float ambient = PortRoomEnv::AmbientScale();
    if (ImGui::SliderFloat("Baked ambient scale", &ambient, 0.f, 4.f, "%.2f")) {
      PortRoomEnv::SetAmbientScale(ambient);
    }
    ImGui::SetItemTooltip("Recommended: 1.\n"
                          "Multiplies the baked ambient light. 0 uses the game's own ambient instead.");
    static const char* const kVolumeViews[] = {"the shaded surface", "volume coordinates", "the baked light"};
    int volumeView = std::clamp(PortRoomEnv::VolumeView(), 0, 2);
    if (ImGui::Combo("Baked surfaces show", &volumeView, kVolumeViews, 3)) {
      PortRoomEnv::SetVolumeView(volumeView);
    }
    ImGui::SetItemTooltip("Recommended: the shaded surface.\n"
                          "The others show where each pixel samples the baked light grid, or the baked\n"
                          "light alone, to check it lines up.");
    ImGui::EndDisabled();
    if (inWorld && ImGui::TreeNode("Room environment here")) {
      ImGui::TextUnformatted(PortRoomEnv::Info(origin).c_str());
      ImGui::TreePop();
    }
  }
}

} // namespace PortDebug
