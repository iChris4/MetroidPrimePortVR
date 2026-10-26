// SPDX-License-Identifier: GPL-3.0-or-later

#include "launcher_keys.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <sstream>

namespace PrimedGunLauncher {
namespace {

using K = KeyKind;

// Defaults: PortVrSettings (platform/include/vr/vr_settings.h) for vr_*, the
// overlay's statics (platform/debug_ui.cpp) for the display keys. Slider
// ranges and steps are PrimedGun's (DolphinQt/MainWindow.cpp ConnectStack).
// tests/port_launcher.cpp checks the defaults against PortVrSettings{} and the
// ranges against the game's clamps.
constexpr KeyInfo kKeys[] = {
    {"vr_wrist_visor_enabled", K::Bool, "0"},
    {"vr_wrist_offset_x", K::Float, "0", -0.50f, 0.50f, 0.005f},
    {"vr_wrist_offset_y", K::Float, "0", -0.50f, 0.50f, 0.005f},
    {"vr_wrist_offset_z", K::Float, "0", -0.50f, 0.50f, 0.005f},
    {"vr_wrist_pitch", K::Float, "40", -180.00f, 180.00f, 1.000f},
    {"vr_wrist_yaw", K::Float, "-40", -180.00f, 180.00f, 1.000f},
    {"vr_wrist_roll", K::Float, "50", -180.00f, 180.00f, 1.000f},

    {"vr_custom_layout_enabled", K::Bool, "0"},
    {"vr_custom_menu_long_press", K::Bool, "0"},
    {"vr_custom_menu_hold_seconds", K::Float, "1", 0.01f, 5.0f, 0.01f},
    {"vr_custom_trigger_threshold", K::Float, "0.55", 0.05f, 0.95f, 0.01f},
    {"vr_custom_grip_threshold", K::Float, "0.55", 0.05f, 0.95f, 0.01f},
    {"vr_custom_touchpad_threshold", K::Float, "0.55", 0.05f, 0.95f, 0.01f},
    {"vr_custom_stick_threshold", K::Float, "0.55", 0.05f, 0.95f, 0.01f},
    {"vr_custom_label_profile", K::Int, "0", 0, 6, 1},
    {"vr_custom_move_hand", K::Int, "0", 0, 1, 1},
    {"vr_custom_look_hand", K::Int, "1", 0, 1, 1},
    {"vr_custom_visor_hand", K::Int, "0", 0, 1, 1},
    {"vr_custom_visor_mode", K::Int, "0", 0, 2, 1},
#define VR_BIND(name, initial, group, label) {"vr_custom_" #name, K::Int, #initial, 0, 36, 1},
#include "vr/vr_controller_bindings.inc"
#undef VR_BIND

    {"quick_resume_enabled", K::Bool, "0"},

    // Port Config: headset
    {"vr_enabled", K::Bool, "1"},
    {"vr_controller_mode", K::Choice, "primedgun", 0, 0, 0, true, true, "primedgun,gamepad,none"},
    {"vr_mirror_view", K::Choice, "left", 0, 0, 0, false, true, "normal,both,left,right,none"},
#if defined(__ANDROID__)
    // The Quest's default eye size (platform/include/vr/vr_settings.h).
    {"vr_render_scale", K::Float, "0.85", 0.25f, 2.0f, 0.05f},
#else
    {"vr_render_scale", K::Float, "1", 0.25f, 2.0f, 0.05f},
#endif
    {"vr_world_scale", K::Float, "1.5", 0.5f, 4.0f, 0.05f},
    {"vr_immersive_replay", K::Bool, "1"},
    {"vr_room_scale", K::Bool, "1"},
    {"vr_multiview", K::Bool, "1"},
    {"vr_foveation", K::Choice, "off", 0, 0, 0, true, true, "off,low,medium,high"},
    {"vr_direct_present", K::Bool, "1"},
#if defined(__ANDROID__)
    {"vr_pipelined_rendering", K::Bool, "1"},
#else
    {"vr_pipelined_rendering", K::Bool, "0"},
#endif
    {"vr_deindex_vertices", K::Bool, "1"},
    {"vr_remove_cinematic_bars", K::Bool, "1"},
    {"vr_sky_at_infinity", K::Bool, "1"},
    {"vr_space_warp", K::Bool, "1"},
    {"vr_scan_zoom", K::Bool, "1"},
    {"vr_screen_distance_meters", K::Float, "1.5", 0.5f, 5.0f, 0.05f},
    {"vr_screen_width_meters", K::Float, "2", 0.5f, 5.0f, 0.05f},
    {"vr_lean_back_degrees", K::Float, "0", -45.0f, 45.0f, 1.0f},
    // Headset keys the Quest launcher shows (passthrough and the levels are standalone only).
    {"vr_passthrough", K::Bool, "1"},
    {"vr_performance_level", K::Choice, "boost", 0, 0, 0, true, true,
     "default,boost,sustained_high,sustained_low,power_savings"},
    {"vr_display_refresh_rate", K::Float, "0", 0.0f, 120.0f, 1.0f},
    {"vr_diagnostics_logging", K::Bool, "0"},
    // Controller
    {"vr_use_right_hand", K::Bool, "1"},
    {"vr_vr_menu_hold_left_stick", K::Bool, "0"},
    {"vr_vr_menu_requires_head_zone", K::Bool, "0"},
    {"vr_combat_jump_use_primary_button", K::Bool, "0"},
    {"vr_beam_wheel_hud_highlight", K::Bool, "1"},
    {"vr_rumble_enabled", K::Bool, "1"},
    {"vr_rumble_hand", K::Choice, "split", 0, 0, 0, true, true, "both,left,right,split"},
    {"vr_grip_inputs_enabled", K::Bool, "1"},
    {"vr_grip_inputs_use_trackpad", K::Bool, "0"},
    {"vr_trackpad_press_threshold", K::Float, "0.5", 0.05f, 1.0f, 0.05f},
    {"vr_index_grip_press_threshold", K::Float, "0.5", 0.05f, 1.0f, 0.05f},
    {"vr_rumble_intensity", K::Float, "1", 0.0f, 1.0f, 0.05f},
    {"vr_xr_dpad_enabled", K::Bool, "1"},
    {"vr_xr_dpad_head_radius", K::Float, "0.28", 0.08f, 0.28f, 0.01f},
    {"vr_xr_dpad_head_y_below", K::Float, "0.02", 0.02f, 0.25f, 0.01f},
    {"vr_xr_dpad_deadzone", K::Float, "0.45", 0.20f, 0.80f, 0.01f},
    {"vr_directional_movement_enabled", K::Bool, "1"},
    {"vr_directional_movement_use_right_stick", K::Bool, "0"},
    {"vr_directional_movement_use_hmd_direction", K::Bool, "0"},
    {"vr_directional_movement_deadzone", K::Float, "0.25", 0.05f, 0.80f, 0.01f},
    {"vr_directional_movement_speed", K::Float, "14", 4.0f, 30.0f, 0.25f},
    {"vr_directional_movement_accel", K::Float, "45", 5.0f, 120.0f, 1.0f},
    {"vr_directional_movement_air_accel", K::Float, "8", 0.0f, 60.0f, 0.5f},
    {"vr_look_yaw_sensitivity", K::Float, "1", 0.20f, 3.00f, 0.05f},
    {"vr_snap_turn_enabled", K::Bool, "0"},
    {"vr_snap_turn_degrees", K::Int, "45", 30.0f, 90.0f, 15.0f},
    // Calibration
    {"vr_vr_overlays_enabled", K::Bool, "1"},
    {"vr_height_prompt_enabled", K::Bool, "1", 0, 0, 0, false},
    {"vr_cinematic_screen_enabled", K::Bool, "1"},
    {"vr_vr_menu_floating", K::Bool, "0"},
    {"vr_game_menu_screen_enabled", K::Bool, "0"},
    {"vr_message_box_immersive", K::Bool, "1"},
    {"vr_floating_hud", K::Bool, "0"},
    {"vr_visor_helmet_enabled", K::Bool, "0", 0, 0, 0, false},
    {"vr_position_marker_enabled", K::Bool, "1", 0, 0, 0, false},
    {"vr_frustum_culling_enabled", K::Bool, "1"},
    {"vr_frustum_culling_degrees", K::Float, "115", 70.0f, 175.0f, 1.0f},
    {"vr_metroid_hud_distance", K::Float, "0.1", 0.10f, 3.00f, 0.05f},
    {"vr_metroid_hud_size", K::Float, "0.75", 0.10f, 3.00f, 0.05f},
    {"vr_metroid_hud_offset_up", K::Float, "0", 0.0f, 1.0f, 0.01f, false},
    {"vr_metroid_hud_offset_down", K::Float, "0", 0.0f, 1.0f, 0.01f, false},
    {"vr_metroid_hud_offset_left", K::Float, "0", 0.0f, 1.0f, 0.01f, false},
    {"vr_metroid_hud_offset_right", K::Float, "0", 0.0f, 1.0f, 0.01f, false},
    {"vr_gun_targeting_enabled", K::Bool, "1", 0, 0, 0, false},
    // Look to scan reads both (platform/vr/vr_look_scan.cpp).
    {"vr_gun_targeting_distance", K::Float, "60", 10.0f, 120.0f, 1.0f},
    {"vr_gun_targeting_radius", K::Float, "4", 0.5f, 8.0f, 0.1f},
    {"vr_offset_x", K::Float, "0", -2.0f, 2.0f, 0.01f},
    {"vr_offset_y", K::Float, "0", -2.0f, 2.0f, 0.01f},
    {"vr_offset_z", K::Float, "0", -2.0f, 2.0f, 0.01f},
    {"vr_model_offset_x", K::Float, "0", -2.0f, 2.0f, 0.01f},
    {"vr_model_offset_y", K::Float, "0", -2.0f, 2.0f, 0.01f},
    {"vr_model_offset_z", K::Float, "0", -2.0f, 2.0f, 0.01f},
    {"vr_rot_offset_x", K::Float, "0", -180.0f, 180.0f, 0.5f},
    {"vr_rot_offset_y", K::Float, "0", -180.0f, 180.0f, 0.5f},
    {"vr_rot_offset_z", K::Float, "0", -180.0f, 180.0f, 0.5f},
    // Cannon Textures: the launcher applies it by copying files, so Reset All
    // leaves it with the files it describes.
    {"vr_cannon_texture_slot", K::Int, "0", 0.0f, 5.0f, 1.0f, true, false},
    // Port Config: display (not PrimedGun settings, so Reset All leaves them)
    {"fullscreen", K::Bool, "0", 0, 0, 0, true, false},
    {"vsync", K::Bool, "0", 0, 0, 0, true, false},
    {"msaa", K::Int, "1", 1.0f, 4.0f, 3.0f, true, false},
#if defined(__ANDROID__)
    {"anisotropy", K::Int, "4", 1.0f, 16.0f, 1.0f, true, false},
#else
    {"anisotropy", K::Int, "16", 1.0f, 16.0f, 1.0f, true, false},
#endif
    {"render_scale", K::Float, "1", 1.0f, 2.0f, 0.25f, true, false},
};

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool InChoices(std::string_view choices, std::string_view name) {
  size_t start = 0;
  while (start <= choices.size()) {
    size_t end = choices.find(',', start);
    if (end == std::string_view::npos) {
      end = choices.size();
    }
    if (choices.substr(start, end - start) == name) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

} // namespace

std::span<const KeyInfo> LauncherKeys() { return kKeys; }

const KeyInfo* FindKey(std::string_view key) {
  for (const KeyInfo& info : kKeys) {
    if (info.key == key) {
      return &info;
    }
  }
  return nullptr;
}

bool ParseBool(std::string_view value) {
  // platform/debug_ui.cpp and platform/vr/vr_settings.cpp ParseBool.
  return value == "1" || value == "true" || value == "on" || value == "yes";
}

std::string FormatBool(bool value) { return value ? "1" : "0"; }

std::string FormatFloat(float value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << value;
  return out.str();
}

std::string FormatInt(int value) { return std::to_string(value); }

std::string Canonical(const KeyInfo& info, std::string_view value) {
  const std::string text(value);
  switch (info.kind) {
  case KeyKind::Bool:
    return FormatBool(ParseBool(value));
  case KeyKind::Float: {
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(parsed)) {
      return std::string(info.defaultValue);
    }
    return FormatFloat(static_cast<float>(parsed));
  }
  case KeyKind::Int: {
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str()) {
      return std::string(info.defaultValue);
    }
    return FormatInt(static_cast<int>(parsed));
  }
  case KeyKind::Choice: {
    const std::string name = Lower(value);
    return InChoices(info.choices, name) ? name : std::string(info.defaultValue);
  }
  }
  return std::string(info.defaultValue);
}

} // namespace PrimedGunLauncher
