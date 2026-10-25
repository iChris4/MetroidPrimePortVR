// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PrimedGun launcher's Qt-free core (launcher/core): editing
// port_settings.ini without disturbing the game's other lines, the key table
// against the game's own defaults and clamps, the disc check, the PrimedGun
// import and the cannon texture pack.

#include "../launcher/core/cannon_textures.h"
#include "../launcher/core/dds_preview.h"
#include "../launcher/core/disc_probe.h"
#include "../launcher/core/launcher_keys.h"
#include "../launcher/core/port_settings_file.h"
#include "../launcher/core/primedgun_import.h"
#include "../launcher/core/settings_model.h"

#include "vr/vr_settings.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace PrimedGunLauncher;

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

void WriteFile(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void TestSettingsFile(const fs::path& root) {
  const std::string original = "# Metroid Prime native port settings.\r\n"
                               "aspect=16:9\r\n"
                               "fullscreen=0\r\n"
                               "mods_disabled=a,b # kept\r\n"
                               "# VR (PrimedGun)\r\n"
                               "vr_enabled=1\r\n"
                               "vr_world_scale=1.5\r\n"
                               "vr_world_scale=2\r\n"
                               "vr_cannon_texture_slot=0\r\n";
  PortSettingsFile file;
  file.Parse(original);
  Check(file.Get("vr_world_scale") == "2", "the last line of a key is the one the game reads");
  Check(file.Get("mods_disabled") == "a,b", "a trailing comment is not part of the value");
  Check(!file.Get("vsync").has_value(), "an absent key has no value");
  Check(file.Text() == original, "an untouched file is written back byte for byte");

  file.Set("vr_world_scale", "1.75");
  file.Set("fullscreen", "1");
  file.Set("vr_rumble_hand", "left");
  file.Set("vsync", "1");
  file.Set("mods_disabled", "c");
  const std::string expected = "# Metroid Prime native port settings.\r\n"
                               "aspect=16:9\r\n"
                               "fullscreen=1\r\n"
                               "mods_disabled=c # kept\r\n"
                               "vsync=1\r\n"
                               "# VR (PrimedGun)\r\n"
                               "vr_enabled=1\r\n"
                               "vr_world_scale=1.5\r\n"
                               "vr_world_scale=1.75\r\n"
                               "vr_cannon_texture_slot=0\r\n"
                               "vr_rumble_hand=left\r\n";
  Check(file.Text() == expected,
        "a set rewrites the line the game reads; new keys join their block; CRLF is kept");

  PortSettingsFile noVr;
  noVr.Parse("aspect=4:3\nmsaa=4\n");
  noVr.Set("vr_use_right_hand", "0");
  noVr.Set("anisotropy", "8");
  Check(noVr.Text() == "aspect=4:3\nmsaa=4\nanisotropy=8\n# VR (PrimedGun)\nvr_use_right_hand=0\n",
        "a file from a build without VR keys gets the VR block");

  const fs::path path = root / "settings" / "port_settings.ini";
  std::string error;
  Check(file.Save(path, error), "the file saves");
  Check(ReadFile(path) == expected, "the saved bytes are the edited text");
  Check(!fs::exists(path.string() + ".launcher.tmp"), "the temporary file is renamed away");
  PortSettingsFile reread;
  Check(reread.Load(path) && reread.Get("vr_rumble_hand") == "left", "the saved file reads back");
  PortSettingsFile missing;
  Check(missing.Load(root / "nothing.ini") && !missing.Get("vr_enabled"), "a missing file is empty");
}

void TestModel() {
  PortSettingsFile legacyResume;
  legacyResume.Parse("quick_resume=1\n");
  SettingsModel resume;
  resume.Load(legacyResume);
  Check(!resume.Bool("quick_resume_enabled"), "legacy quick resume does not opt existing users in");
  resume.SetBool("quick_resume_enabled", true);
  resume.ApplyChanges(legacyResume);
  SettingsModel reloadedResume;
  reloadedResume.Load(legacyResume);
  Check(reloadedResume.Bool("quick_resume_enabled"), "explicit quick resume opt-in persists");
  legacyResume.Parse("quick_resume_enabled=0\n");
  reloadedResume.Refresh(legacyResume);
  Check(!reloadedResume.Bool("quick_resume_enabled"), "live quick resume disable reaches desktop");
  PortSettingsFile file;
  file.Parse("vr_world_scale=2.50\nvr_use_right_hand=yes\nvr_rumble_hand=LEFT\nmsaa=x\n");
  SettingsModel model;
  model.Load(file);
  Check(model.Value("vr_world_scale") == "2.5", "floats are held as the game writes them");
  Check(model.Bool("vr_use_right_hand"), "the game's boolean spellings are read");
  Check(model.Value("vr_rumble_hand") == "left", "choice names are lower-cased");
  Check(model.Value("msaa") == "1", "a value the game would ignore reads as the default");
  Check(model.Value("vr_metroid_hud_size") == "0.75", "an absent key reads as the default");
  Check(!model.Dirty(), "a fresh load has no changes");

  model.SetFloat("vr_world_scale", 2.5f);
  Check(!model.Dirty(), "setting the loaded value is not a change");
  model.SetFloat("vr_metroid_hud_size", static_cast<float>(0.05 * 19));
  model.SetBool("vr_use_right_hand", false);
  model.Set("not_a_launcher_key", "1");
  const auto changes = model.Changes();
  Check(changes.size() == 2, "only the two edited keys are changes");
  Check(model.Value("vr_metroid_hud_size") == "0.95", "slider steps print as the game prints them");

  model.ApplyChanges(file);
  Check(file.Get("vr_metroid_hud_size") == "0.95" && file.Get("vr_use_right_hand") == "0",
        "changes reach the file");
  Check(file.Get("msaa") == "x", "keys the launcher did not change keep their text");
  model.MarkSaved();
  Check(!model.Dirty(), "a save clears the changes");

  model.SetInt("vr_cannon_texture_slot", 3);
  model.SetBool("fullscreen", true);
  model.ResetAll();
  Check(model.Value("vr_metroid_hud_size") == "0.75" && model.Value("vr_use_right_hand") == "1",
        "Reset All restores the PrimedGun settings");
  Check(model.Int("vr_cannon_texture_slot") == 3 && model.Bool("fullscreen"),
        "Reset All leaves the cannon slot and the display settings");

  // What the game saved while it runs, under an edit not written yet.
  SettingsModel live;
  live.Load(file);
  live.SetBool("vr_snap_turn_enabled", true);
  PortSettingsFile saved;
  saved.Parse("vr_world_scale=3\nvr_snap_turn_enabled=0\n");
  live.Refresh(saved);
  Check(live.Value("vr_world_scale") == "3", "a refresh takes what the game saved");
  Check(live.Bool("vr_snap_turn_enabled") && live.Changes().size() == 1,
        "a refresh keeps the edit not written yet");
}

// The launcher's defaults are the game's: PortVrSettings{} for every vr_* key.
void TestDefaultsMatchGame() {
  const PortVr::PortVrSettings s{};
  auto mirror = [](PortVr::MirrorView v) {
    switch (v) {
    case PortVr::MirrorView::Both: return "both";
    case PortVr::MirrorView::Left: return "left";
    case PortVr::MirrorView::Right: return "right";
    case PortVr::MirrorView::None: return "none";
    default: return "normal";
    }
  };
  auto mode = [](PortVr::ControllerModeSetting m) {
    return m == PortVr::ControllerModeSetting::Gamepad ? "gamepad"
           : m == PortVr::ControllerModeSetting::None  ? "none"
                                                        : "primedgun";
  };
  auto hand = [](PortVr::RumbleHand h) {
    return h == PortVr::RumbleHand::Both    ? "both"
           : h == PortVr::RumbleHand::Left  ? "left"
           : h == PortVr::RumbleHand::Split ? "split"
                                            : "right";
  };
  auto foveation = [](PortVr::FoveationLevel f) {
    return f == PortVr::FoveationLevel::Low      ? "low"
           : f == PortVr::FoveationLevel::Medium ? "medium"
           : f == PortVr::FoveationLevel::High   ? "high"
                                                 : "off";
  };
  std::vector<std::pair<const char*, std::string>> expected = {
      {"vr_enabled", FormatBool(s.enabled)},
      {"vr_controller_mode", mode(s.controller_mode)},
      {"vr_mirror_view", mirror(s.mirror_view)},
      {"vr_render_scale", FormatFloat(s.render_scale)},
      {"vr_world_scale", FormatFloat(s.world_scale)},
      {"vr_immersive_replay", FormatBool(s.immersive_replay)},
      {"vr_room_scale", FormatBool(s.room_scale)},
      {"vr_multiview", FormatBool(s.multiview)},
      {"vr_foveation", foveation(s.foveation)},
      {"vr_direct_present", FormatBool(s.direct_present)},
      {"vr_pipelined_rendering", FormatBool(s.pipelined_rendering)},
      {"vr_deindex_vertices", FormatBool(s.deindex_vertices)},
      {"vr_remove_cinematic_bars", FormatBool(s.remove_cinematic_bars)},
      {"vr_sky_at_infinity", FormatBool(s.sky_at_infinity)},
      {"vr_space_warp", FormatBool(s.space_warp)},
      {"vr_scan_zoom", FormatBool(s.scan_zoom)},
      {"vr_screen_distance_meters", FormatFloat(s.screen_distance_meters)},
      {"vr_screen_width_meters", FormatFloat(s.screen_width_meters)},
      {"vr_lean_back_degrees", FormatFloat(s.lean_back_degrees)},
      {"vr_passthrough", FormatBool(s.passthrough)},
      {"vr_performance_level", s.performance_level},
      {"vr_display_refresh_rate", FormatFloat(s.display_refresh_rate)},
      {"vr_diagnostics_logging", FormatBool(s.diagnostics_logging)},
      {"vr_use_right_hand", FormatBool(s.use_right_hand)},
      {"vr_vr_menu_hold_left_stick", FormatBool(s.vr_menu_hold_left_stick)},
      {"vr_vr_menu_requires_head_zone", FormatBool(s.vr_menu_requires_head_zone)},
      {"vr_combat_jump_use_primary_button", FormatBool(s.combat_jump_use_primary_button)},
      {"vr_beam_wheel_hud_highlight", FormatBool(s.beam_wheel_hud_highlight)},
      {"vr_rumble_enabled", FormatBool(s.rumble_enabled)},
      {"vr_rumble_hand", hand(s.rumble_hand)},
      {"vr_grip_inputs_enabled", FormatBool(s.grip_inputs_enabled)},
      {"vr_grip_inputs_use_trackpad", FormatBool(s.grip_inputs_use_trackpad)},
      {"vr_trackpad_press_threshold", FormatFloat(s.trackpad_press_threshold)},
      {"vr_index_grip_press_threshold", FormatFloat(s.index_grip_press_threshold)},
      {"vr_rumble_intensity", FormatFloat(s.rumble_intensity)},
      {"vr_xr_dpad_enabled", FormatBool(s.xr_dpad_enabled)},
      {"vr_xr_dpad_head_radius", FormatFloat(s.xr_dpad_head_radius)},
      {"vr_xr_dpad_head_y_below", FormatFloat(s.xr_dpad_head_y_below)},
      {"vr_xr_dpad_deadzone", FormatFloat(s.xr_dpad_deadzone)},
      {"vr_directional_movement_enabled", FormatBool(s.directional_movement_enabled)},
      {"vr_directional_movement_use_right_stick", FormatBool(s.directional_movement_use_right_stick)},
      {"vr_directional_movement_use_hmd_direction",
       FormatBool(s.directional_movement_use_hmd_direction)},
      {"vr_directional_movement_deadzone", FormatFloat(s.directional_movement_deadzone)},
      {"vr_directional_movement_speed", FormatFloat(s.directional_movement_speed)},
      {"vr_directional_movement_accel", FormatFloat(s.directional_movement_accel)},
      {"vr_directional_movement_air_accel", FormatFloat(s.directional_movement_air_accel)},
      {"vr_look_yaw_sensitivity", FormatFloat(s.look_yaw_sensitivity)},
      {"vr_snap_turn_enabled", FormatBool(s.snap_turn_enabled)},
      {"vr_snap_turn_degrees", FormatInt(s.snap_turn_degrees)},
      {"vr_vr_overlays_enabled", FormatBool(s.vr_overlays_enabled)},
      {"vr_height_prompt_enabled", FormatBool(s.height_prompt_enabled)},
      {"vr_cinematic_screen_enabled", FormatBool(s.cinematic_screen_enabled)},
      {"vr_vr_menu_floating", FormatBool(s.vr_menu_floating)},
      {"vr_game_menu_screen_enabled", FormatBool(s.game_menu_screen_enabled)},
      {"vr_message_box_immersive", FormatBool(s.message_box_immersive)},
      {"vr_floating_hud", FormatBool(s.floating_hud)},
      {"vr_visor_helmet_enabled", FormatBool(s.visor_helmet_enabled)},
      {"vr_position_marker_enabled", FormatBool(s.position_marker_enabled)},
      {"vr_frustum_culling_enabled", FormatBool(s.frustum_culling_enabled)},
      {"vr_frustum_culling_degrees", FormatFloat(s.frustum_culling_degrees)},
      {"vr_metroid_hud_distance", FormatFloat(s.metroid_hud_distance)},
      {"vr_metroid_hud_size", FormatFloat(s.metroid_hud_size)},
      {"vr_metroid_hud_offset_up", FormatFloat(s.metroid_hud_offset_up)},
      {"vr_metroid_hud_offset_down", FormatFloat(s.metroid_hud_offset_down)},
      {"vr_metroid_hud_offset_left", FormatFloat(s.metroid_hud_offset_left)},
      {"vr_metroid_hud_offset_right", FormatFloat(s.metroid_hud_offset_right)},
      {"vr_gun_targeting_enabled", FormatBool(s.gun_targeting_enabled)},
      {"vr_gun_targeting_distance", FormatFloat(s.gun_targeting_distance)},
      {"vr_gun_targeting_radius", FormatFloat(s.gun_targeting_radius)},
      {"vr_offset_x", FormatFloat(s.offset_x)},
      {"vr_offset_y", FormatFloat(s.offset_y)},
      {"vr_offset_z", FormatFloat(s.offset_z)},
      {"vr_model_offset_x", FormatFloat(s.model_offset_x)},
      {"vr_model_offset_y", FormatFloat(s.model_offset_y)},
      {"vr_model_offset_z", FormatFloat(s.model_offset_z)},
      {"vr_rot_offset_x", FormatFloat(s.rot_offset_x)},
      {"vr_rot_offset_y", FormatFloat(s.rot_offset_y)},
      {"vr_rot_offset_z", FormatFloat(s.rot_offset_z)},
      {"vr_cannon_texture_slot", FormatInt(s.cannon_texture_slot)},
  };
  expected.emplace_back("vr_wrist_offset_x", FormatFloat(s.wrist_offset_x));
  expected.emplace_back("vr_wrist_offset_y", FormatFloat(s.wrist_offset_y));
  expected.emplace_back("vr_wrist_offset_z", FormatFloat(s.wrist_offset_z));
  expected.emplace_back("vr_wrist_pitch", FormatFloat(s.wrist_pitch));
  expected.emplace_back("vr_wrist_yaw", FormatFloat(s.wrist_yaw));
  expected.emplace_back("vr_wrist_roll", FormatFloat(s.wrist_roll));
  expected.emplace_back("vr_wrist_visor_enabled", FormatBool(s.wrist_visor_enabled));
  expected.emplace_back("vr_custom_layout_enabled", FormatBool(s.custom_layout_enabled));
  expected.emplace_back("vr_custom_trigger_threshold", FormatFloat(s.custom_trigger_threshold));
  expected.emplace_back("vr_custom_grip_threshold", FormatFloat(s.custom_grip_threshold));
  expected.emplace_back("vr_custom_touchpad_threshold", FormatFloat(s.custom_touchpad_threshold));
  expected.emplace_back("vr_custom_stick_threshold", FormatFloat(s.custom_stick_threshold));
  expected.emplace_back("vr_custom_label_profile", FormatInt(s.custom_label_profile));
  expected.emplace_back("vr_custom_move_hand", FormatInt(s.custom_move_hand));
  expected.emplace_back("vr_custom_look_hand", FormatInt(s.custom_look_hand));
  expected.emplace_back("vr_custom_visor_hand", FormatInt(s.custom_visor_hand));
  expected.emplace_back("vr_custom_visor_mode", FormatInt(s.custom_visor_mode));
#define VR_BIND(name, initial, group, label) expected.emplace_back("vr_custom_" #name, FormatInt(s.custom_##name));
#include "vr/vr_controller_bindings.inc"
#undef VR_BIND
  std::set<std::string> checked;
  for (const auto& [key, value] : expected) {
    const KeyInfo* info = FindKey(key);
    if (info == nullptr) {
      std::fprintf(stderr, "FAIL: %s is missing from the launcher's keys\n", key);
      ++sFailures;
      continue;
    }
    if (info->defaultValue != value) {
      std::fprintf(stderr, "FAIL: %s defaults to %.*s, the game to %s\n", key,
                   static_cast<int>(info->defaultValue.size()), info->defaultValue.data(),
                   value.c_str());
      ++sFailures;
    }
    checked.insert(key);
  }
  std::set<std::string_view> seen;
  for (const KeyInfo& info : LauncherKeys()) {
    Check(seen.insert(info.key).second, "every launcher key is listed once");
    if (info.key.substr(0, 3) == "vr_" && !checked.count(std::string(info.key))) {
      std::fprintf(stderr, "FAIL: %.*s has no PortVrSettings default check\n",
                   static_cast<int>(info.key.size()), info.key.data());
      ++sFailures;
    }
    Check(Canonical(info, info.defaultValue) == info.defaultValue,
          "every default is already in the game's spelling");
  }
}

// The game clamps what it reads (platform/vr/vr_settings.cpp kFields,
// platform/debug_ui.cpp ApplySetting); a slider must stay inside.
void TestRangesInsideGameClamps() {
  struct Clamp {
    const char* key;
    float low, high;
  };
  const Clamp clamps[] = {
      {"vr_custom_trigger_threshold", 0.05f, 0.95f},
      {"vr_custom_grip_threshold", 0.05f, 0.95f},
      {"vr_custom_touchpad_threshold", 0.05f, 0.95f},
      {"vr_custom_stick_threshold", 0.05f, 0.95f},
      {"vr_wrist_offset_x", -0.50f, 0.50f},
      {"vr_wrist_offset_y", -0.50f, 0.50f},
      {"vr_wrist_offset_z", -0.50f, 0.50f},
      {"vr_wrist_pitch", -180.00f, 180.00f},
      {"vr_wrist_yaw", -180.00f, 180.00f},
      {"vr_wrist_roll", -180.00f, 180.00f},
      {"vr_custom_label_profile", 0.f, 6.f},
      {"vr_custom_move_hand", 0.f, 1.f}, {"vr_custom_look_hand", 0.f, 1.f},
      {"vr_custom_visor_hand", 0.f, 1.f}, {"vr_custom_visor_mode", 0.f, 2.f},
#define VR_BIND(name, initial, group, label) {"vr_custom_" #name, 0.f, 36.f},
#include "vr/vr_controller_bindings.inc"
#undef VR_BIND

      {"vr_render_scale", 0.25f, 2.0f},
      {"vr_world_scale", 0.1f, 50.0f},
      {"vr_screen_distance_meters", 0.25f, 10.0f},
      {"vr_screen_width_meters", 0.25f, 10.0f},
      {"vr_lean_back_degrees", -45.0f, 45.0f},
      {"vr_display_refresh_rate", 0.0f, 144.0f},
      {"vr_trackpad_press_threshold", 0.05f, 1.0f},
      {"vr_index_grip_press_threshold", 0.05f, 1.0f},
      {"vr_rumble_intensity", 0.0f, 1.0f},
      {"vr_xr_dpad_head_radius", 0.05f, 0.6f},
      {"vr_xr_dpad_head_y_below", 0.0f, 0.6f},
      {"vr_xr_dpad_deadzone", 0.05f, 0.95f},
      {"vr_directional_movement_deadzone", 0.0f, 0.95f},
      {"vr_directional_movement_speed", 1.0f, 60.0f},
      {"vr_directional_movement_accel", 1.0f, 120.0f},
      {"vr_directional_movement_air_accel", 0.0f, 60.0f},
      {"vr_look_yaw_sensitivity", 0.2f, 3.0f},
      {"vr_snap_turn_degrees", 15.0f, 90.0f},
      {"vr_frustum_culling_degrees", 70.0f, 175.0f},
      {"vr_metroid_hud_distance", 0.1f, 3.0f},
      {"vr_metroid_hud_size", 0.1f, 3.0f},
      {"vr_metroid_hud_offset_up", -1.0f, 1.0f},
      {"vr_metroid_hud_offset_down", -1.0f, 1.0f},
      {"vr_metroid_hud_offset_left", -1.0f, 1.0f},
      {"vr_metroid_hud_offset_right", -1.0f, 1.0f},
      {"vr_gun_targeting_distance", 1.0f, 200.0f},
      {"vr_gun_targeting_radius", 0.1f, 25.0f},
      {"vr_offset_x", -2.0f, 2.0f},
      {"vr_offset_y", -2.0f, 2.0f},
      {"vr_offset_z", -2.0f, 2.0f},
      {"vr_model_offset_x", -2.0f, 2.0f},
      {"vr_model_offset_y", -2.0f, 2.0f},
      {"vr_model_offset_z", -2.0f, 2.0f},
      {"vr_rot_offset_x", -180.0f, 180.0f},
      {"vr_rot_offset_y", -180.0f, 180.0f},
      {"vr_rot_offset_z", -180.0f, 180.0f},
      {"vr_cannon_texture_slot", 0.0f, 5.0f},
      {"render_scale", 0.0f, 4.0f},
      {"anisotropy", 1.0f, 16.0f},
      {"msaa", 1.0f, 4.0f},
  };
  for (const KeyInfo& info : LauncherKeys()) {
    if (info.kind != KeyKind::Float && info.kind != KeyKind::Int) {
      continue;
    }
    const Clamp* clamp = nullptr;
    for (const Clamp& c : clamps) {
      if (info.key == c.key) {
        clamp = &c;
      }
    }
    if (clamp == nullptr) {
      std::fprintf(stderr, "FAIL: %.*s has no clamp to check against\n",
                   static_cast<int>(info.key.size()), info.key.data());
      ++sFailures;
      continue;
    }
    if (info.uiMin < clamp->low || info.uiMax > clamp->high || info.uiStep <= 0.0f) {
      std::fprintf(stderr, "FAIL: %s's slider %g..%g leaves the game's %g..%g\n", clamp->key,
                   info.uiMin, info.uiMax, clamp->low, clamp->high);
      ++sFailures;
    }
  }
}

void TestDiscProbe(const fs::path& root) {
  std::vector<uint8_t> iso(0x440, 0);
  std::memcpy(iso.data(), "GM8E01", 6);
  Check(ProbeDiscBytes(".iso", iso.data(), iso.size()).check == DiscCheck::Ok,
        "an NTSC-U 1.00 header is the right disc");
  iso[7] = 1;
  Check(ProbeDiscBytes(".iso", iso.data(), iso.size()).check == DiscCheck::Ok,
        "USA revision 1 is supported upstream");
  iso[7] = 2;
  Check(ProbeDiscBytes(".iso", iso.data(), iso.size()).check == DiscCheck::Ok,
        "USA revision 2 is supported upstream");
  iso[7] = 3;
  Check(ProbeDiscBytes(".iso", iso.data(), iso.size()).check == DiscCheck::WrongRevision,
        "USA revision 3 is not supported");
  iso[7] = 0;
  Check(!ProbeDiscBytes(".iso", iso.data(), iso.size()).Pal(), "a USA disc is not PAL");
  std::memcpy(iso.data(), "GM8P01", 6);
  const DiscInfo pal = ProbeDiscBytes(".gcm", iso.data(), iso.size());
  Check(pal.check == DiscCheck::Ok && pal.Pal(), "the PAL disc is accepted, as the game does");
  iso[7] = 1;
  Check(ProbeDiscBytes(".gcm", iso.data(), iso.size()).check == DiscCheck::WrongRevision,
        "a PAL revision other than 0 is the wrong revision");
  iso[7] = 0;
  std::memcpy(iso.data(), "GM8J01", 6);
  Check(ProbeDiscBytes(".gcm", iso.data(), iso.size()).check == DiscCheck::WrongGame,
        "the Japanese disc is the wrong game");
  std::memcpy(iso.data(), "GM8E01", 6);
  iso[6] = 1;
  Check(ProbeDiscBytes(".iso", iso.data(), iso.size()).check == DiscCheck::WrongGame,
        "a disc number other than 0 is the wrong game");
  iso[6] = 0;

  std::vector<uint8_t> rvz(0x200, 0);
  std::memcpy(rvz.data(), "RVZ\x01", 4);
  std::memcpy(rvz.data() + 0x58, "GM8E01", 6);
  const DiscInfo rvzInfo = ProbeDiscBytes(".rvz", rvz.data(), rvz.size());
  Check(rvzInfo.check == DiscCheck::Ok && rvzInfo.gameId == "GM8E01",
        "an RVZ image keeps the disc header at 0x58");

  std::vector<uint8_t> ciso(0x8100, 0);
  std::memcpy(ciso.data(), "CISO", 4);
  ciso[8] = 1;
  std::memcpy(ciso.data() + 0x8000, "GM8E01", 6);
  ciso[0x8007] = 3;
  Check(ProbeDiscBytes(".ciso", ciso.data(), ciso.size()).check == DiscCheck::WrongRevision,
        "a CISO image keeps the disc header at 0x8000");

  std::vector<uint8_t> wbfs(0x200, 0);
  std::memcpy(wbfs.data(), "WBFS", 4);
  std::vector<uint8_t> plain(0x200, 0);
  Check(ProbeDiscBytes(".WBFS", plain.data(), plain.size()).check == DiscCheck::Unverified,
        "an upper-case .WBFS is taken on trust like .wbfs");
  Check(ProbeDiscBytes(".iso", wbfs.data(), wbfs.size()).check == DiscCheck::Unverified,
        "a WBFS image is known by its magic whatever its name");
  std::vector<uint8_t> trilogy(0x400, 0);
  std::memcpy(trilogy.data(), "WBFS", 4);
  trilogy[8] = 9;  // 512-byte sectors: disc 0's header copy at 0x200
  trilogy[12] = 1; // disc slot 0 used
  std::memcpy(trilogy.data() + 0x200, "R3ME01", 6);
  Check(ProbeDiscBytes(".wbfs", trilogy.data(), trilogy.size()).check == DiscCheck::WrongGame,
        "a WBFS image of another game is refused by its header copy");
  std::memcpy(trilogy.data() + 0x200, "GM8E01", 6);
  Check(ProbeDiscBytes(".wbfs", trilogy.data(), trilogy.size()).check == DiscCheck::Ok,
        "a WBFS image of the right disc passes");

  Check(ProbeDisc(root / "game.gcz").check == DiscCheck::UnsupportedFormat,
        ".gcz is not a format the port opens");
  Check(IsSupportedDiscExtension("Metroid Prime (USA).nkit.iso"), "NKit's .nkit.iso is an .iso");
  Check(ProbeDisc(root / "absent.iso").check == DiscCheck::Unreadable, "a missing file is unreadable");
  iso[0] = 'G';
  std::memcpy(iso.data(), "GM8E01", 6);
  WriteFile(root / "disc" / "game.iso", std::string(iso.begin(), iso.end()));
  Check(ProbeDisc(root / "disc" / "game.iso").check == DiscCheck::Ok, "a file on disk is probed");
}

void TestImport(const fs::path& root) {
  const IniSections ini = ParseIni("[Runtime]\r\n"
                                   "enabled = False\r\n"
                                   "use_right_hand = False\r\n"
                                   "rumble_hand_mode = 0\r\n"
                                   "primedgun_grip_inputs_use_trackpad = True\r\n"
                                   "vr_menu_floating = True\r\n"
                                   "vr_overlays_enabled = False\r\n"
                                   "metroid_hud_distance = 0.9\r\n"
                                   "visor_helmet_skip_hidden_draw = True\r\n"
                                   "vr_state_slot = 4\r\n"
                                   "cannon_texture_slot = 2\r\n");
  const auto section = ini.find("runtime");
  Check(section != ini.end(), "section names are found case-insensitively");
  if (section == ini.end()) {
    return;
  }
  const auto values = MapRuntimeSettings(section->second);
  auto value = [&values](const char* key) -> std::string {
    for (const auto& [k, v] : values) {
      if (k == key) {
        return v;
      }
    }
    return "(absent)";
  };
  Check(value("vr_enabled") == "(absent)", "PrimedGun's enabled flag is not imported");
  Check(value("vr_use_right_hand") == "0", "Dolphin's False is the game's 0");
  Check(value("vr_rumble_hand") == "both", "rumble_hand_mode 0 is both hands");
  Check(value("vr_grip_inputs_use_trackpad") == "1", "the grip keys lose the primedgun_ prefix");
  Check(value("vr_vr_menu_floating") == "1" && value("vr_vr_overlays_enabled") == "0",
        "the VR-menu keys keep their own vr_ prefix");
  Check(value("vr_metroid_hud_distance") == "0.9", "floats come across");
  Check(value("vr_cannon_texture_slot") == "2", "the cannon slot comes across");
  Check(values.size() == 7, "settings the port has no key for are left out");

  // An old portable install: x64 beside the launcher's parent.
  const fs::path old = root / "PrimedGun 1.2" / "x64" / "User";
  WriteFile(old / "GC" / "MemoryCardA.USA.raw", std::string(0x2000, '\0'));
  WriteFile(old / "GC" / "MemoryCardA.USA.backup-20260101-000000.raw", "x");
  // Dolphin's card for PAL discs, beside the USA one.
  WriteFile(old / "GC" / "MemoryCardA.EUR.raw", std::string(0x2000, '\0'));
  WriteFile(old / "Config" / "PrimedGun.ini", "[Runtime]\nmetroid_hud_size = 1.25\n");
  WriteFile(old / "Config" / "Qt.ini",
            "[mainwindow]\nselected_metroid_prime_path=D:/Games/Metroid Prime.iso\n"
            "[primedgun]\ncannon_texture_slot=3\n");
  // The port's own card next to another build must not be taken for it.
  WriteFile(root / "build" / "native" / "USA" / "Card A" / "01-GM8E-MetroidPrime A.gci", "x");
  fs::create_directories(root / "build" / "vr");

  const OldCard nearby = FindNearbyOldCard(root / "build" / "vr");
  Check(nearby.Found() && nearby.path.filename() == "MemoryCardA.USA.raw" && !nearby.isFolder,
        "PrimedGun's search finds the old raw card");
  const OldCard nearbyPal = FindNearbyOldCard(root / "build" / "vr", "EUR");
  Check(nearbyPal.Found() && nearbyPal.path.filename() == "MemoryCardA.EUR.raw" &&
            !nearbyPal.isFolder,
        "a PAL disc's search takes the EUR card beside the USA one");
  Check(!FindNearbyOldCard(root / "elsewhere" / "deep" / "x64").Found(),
        "nothing is found far from an old install");
  const OldCard picked = FindOldCardUnder(root / "PrimedGun 1.2");
  Check(picked.Found() && picked.UserFolder() == (old).lexically_normal(),
        "a picked folder is searched with the same layouts");

  const OldSettings settings = ReadOldSettings(picked.UserFolder());
  Check(settings.source.filename() == "PrimedGun.ini", "PrimedGun.ini is preferred");
  bool hud = false, slot = false;
  for (const auto& [k, v] : settings.values) {
    hud |= k == "vr_metroid_hud_size" && v == "1.25";
    slot |= k == "vr_cannon_texture_slot" && v == "3";
  }
  Check(hud, "the runtime settings are read");
  Check(slot, "the Qt launcher's applied cannon slot wins");
  Check(settings.gamePath == "D:/Games/Metroid Prime.iso", "the old selected disc is read");

  const fs::path gci = root / "Dolphin" / "User" / "GC" / "USA" / "Card A";
  WriteFile(gci / "01-GM8E-MetroidPrime A.gci", "x");
  const OldCard folder = FindOldCardUnder(root / "Dolphin");
  Check(folder.Found() && folder.isFolder && folder.UserFolder() == (root / "Dolphin" / "User"),
        "a Dolphin GCI folder is found when there is no raw card");
  Check(!FindOldCardUnder(root / "Dolphin", "EUR").Found(),
        "Dolphin's USA card is not taken for a PAL disc");

  // A PAL player's Dolphin: GM8P saves in the EUR GCI folder, and a USA raw card
  // that holds none of them.
  const fs::path palGc = root / "Dolphin PAL" / "User" / "GC";
  WriteFile(palGc / "EUR" / "Card A" / "01-GM8P-MetroidPrime A.gci", "x");
  WriteFile(palGc / "MemoryCardA.USA.raw", std::string(0x2000, '\0'));
  const OldCard palFolder = FindOldCardUnder(root / "Dolphin PAL", "EUR");
  Check(palFolder.Found() && palFolder.isFolder &&
            palFolder.UserFolder() == (root / "Dolphin PAL" / "User"),
        "a PAL disc takes Dolphin's EUR GCI folder, not a USA raw card");
  const OldCard usaRaw = FindOldCardUnder(root / "Dolphin PAL");
  Check(usaRaw.Found() && !usaRaw.isFolder && usaRaw.path.filename() == "MemoryCardA.USA.raw",
        "a USA disc still takes the USA raw card there");
}

void TestDds() {
  std::vector<uint8_t> dds(128 + 8, 0);
  std::memcpy(dds.data(), "DDS ", 4);
  dds[12] = 4;  // height
  dds[16] = 4;  // width
  std::memcpy(dds.data() + 84, "DXT1", 4);
  // c0 = pure red (0xF800), c1 = pure blue (0x001F), every texel index 0
  // except the second, which is index 1.
  dds[128] = 0x00;
  dds[129] = 0xF8;
  dds[130] = 0x1F;
  dds[131] = 0x00;
  dds[132] = 0x04;
  const RgbaImage image = DecodeDxt1Dds(dds.data(), dds.size());
  Check(image.width == 4 && image.height == 4, "the size comes from the header");
  if (image.pixels.size() == 64) {
    Check(image.pixels[0] == 255 && image.pixels[2] == 0 && image.pixels[3] == 255,
          "index 0 is the first colour");
    Check(image.pixels[4] == 0 && image.pixels[6] == 255, "index 1 is the second colour");
  }
  dds[84] = 'X';
  Check(DecodeDxt1Dds(dds.data(), dds.size()).Empty(), "other formats are not decoded");
}

void TestCannon(const fs::path& root) {
  Cannon::Folders folders{root / "user" / "primedgun" / "cannon_textures",
                          root / "exe" / "primedgun" / "cannon_textures",
                          root / "user" / "user_textures"};
  for (int index = 0; index < 3; ++index) {
    const std::string name(Cannon::kTextureNames[index]);
    WriteFile(folders.shippedLibrary / "slot_1" / (name + ".dds"), "slot1-" + name);
    WriteFile(folders.shippedLibrary / "default" / (name + ".dds"), "default-" + name);
  }
  const std::string shine(Cannon::kTextureNames[Cannon::kShineIndex]);
  WriteFile(folders.shippedLibrary / "presets" / "remove_shine" / (shine + ".dds"), "noshine");

  Cannon::SeedLibrary(folders);
  Check(fs::exists(folders.library / "slot_1" / (shine + ".dds")), "the library is seeded");
  WriteFile(folders.library / "slot_1" / (shine + ".dds"), "edited");
  Cannon::SeedLibrary(folders);
  Check(ReadFile(folders.library / "slot_1" / (shine + ".dds")) == "edited",
        "seeding never overwrites the player's files");

  std::string error;
  const fs::path pack = folders.userTextures / std::string(Cannon::kPackFolder);
  Check(Cannon::ApplySlot(folders, 1, error), "slot 1 applies");
  Check(ReadFile(pack / (shine + ".dds")) == "edited", "the slot's files go to the managed folder");

  const fs::path removed = Cannon::RemoveShine(folders, 1, error);
  Check(ReadFile(removed) == "noshine", "Remove Shine puts the preset in the slot");
  Check(ReadFile(folders.library / "presets" / "restore_shine" / "slot_1" / (shine + ".dds")) ==
            "edited",
        "Remove Shine keeps the slot's own shine mask");
  const fs::path restored = Cannon::RestoreShine(folders, 1, error);
  Check(ReadFile(restored) == "edited", "Restore Shine puts it back");

  // A pack split by device: only the active device's folder loads, so the
  // slot goes into every device folder.
  WriteFile(folders.userTextures / "xbox" / "tex1_32x32_0000000000000000_14.dds", "user");
  Check(Cannon::ApplySlot(folders, 1, error), "slot 1 applies to a split pack");
  Check(!fs::exists(pack), "the root copy is gone once the pack is split");
  Check(fs::exists(folders.userTextures / "keyboard" / std::string(Cannon::kPackFolder) /
                   (shine + ".dds")) &&
            fs::exists(folders.userTextures / "xbox" / std::string(Cannon::kPackFolder) /
                       (shine + ".dds")),
        "every device folder gets the slot");
  Check(Cannon::ApplySlot(folders, 0, error), "Default applies");
  Check(!fs::exists(folders.userTextures / "keyboard"), "device folders made for the slot go");
  Check(fs::exists(folders.userTextures / "xbox" / "tex1_32x32_0000000000000000_14.dds"),
        "the player's own pack files stay");
}

} // namespace

int main(int argc, char** argv) {
  const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "port_launcher_tests";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root);

  TestSettingsFile(root);
  TestModel();
  TestDefaultsMatchGame();
  TestRangesInsideGameClamps();
  TestDiscProbe(root);
  TestImport(root / "import");
  TestDds();
  TestCannon(root / "cannon");

  fs::remove_all(root, ec);
  if (sFailures != 0) {
    std::fprintf(stderr, "port_launcher_tests: %d failure(s)\n", sFailures);
    return 1;
  }
  std::printf("port_launcher_tests: ok\n");
  return 0;
}
