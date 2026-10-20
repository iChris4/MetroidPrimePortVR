// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/vr_settings.h"

#include "port_debug.h"
#if defined(MP_ENABLE_OPENXR)
#include "vr/openxr_diagnostics.h"
#endif
#include "vr/openxr_integration.h"
#include "vr/prime_vr_policy.h"

#include <aurora/gfx.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

namespace PortVr {
namespace {

std::mutex& Mutex() {
    static std::mutex mutex;
    return mutex;
}

PortVrSettings& Stored() {
    static PortVrSettings settings;
    return settings;
}

bool ParseBool(const std::string& value) {
    return value == "1" || value == "true" || value == "on" || value == "yes";
}

float ParseFloat(const std::string& value, float fallback, float low, float high) {
    const float f = static_cast<float>(std::atof(value.c_str()));
    if (!std::isfinite(f)) {
        return fallback;
    }
    return std::clamp(f, low, high);
}

int ParseInt(const std::string& value, int fallback, int low, int high) {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str()) {
        return fallback;
    }
    return static_cast<int>(std::clamp<long>(parsed, low, high));
}

std::string Lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

const char* MirrorViewName(MirrorView view) {
    switch (view) {
    case MirrorView::Both: return "both";
    case MirrorView::Left: return "left";
    case MirrorView::Right: return "right";
    case MirrorView::None: return "none";
    case MirrorView::Normal: break;
    }
    return "normal";
}

bool ParseMirrorView(const std::string& value, MirrorView& out) {
    const std::string name = Lower(value);
    if (name == "normal") out = MirrorView::Normal;
    else if (name == "both") out = MirrorView::Both;
    else if (name == "left") out = MirrorView::Left;
    else if (name == "right") out = MirrorView::Right;
    else if (name == "none") out = MirrorView::None;
    else return false;
    return true;
}

const char* ControllerModeName(ControllerModeSetting mode) {
    switch (mode) {
    case ControllerModeSetting::Gamepad: return "gamepad";
    case ControllerModeSetting::None: return "none";
    case ControllerModeSetting::PrimedGun: break;
    }
    return "primedgun";
}

bool ParseControllerMode(const std::string& value, ControllerModeSetting& out) {
    const std::string name = Lower(value);
    if (name == "primedgun") out = ControllerModeSetting::PrimedGun;
    else if (name == "gamepad") out = ControllerModeSetting::Gamepad;
    else if (name == "none") out = ControllerModeSetting::None;
    else return false;
    return true;
}

const char* RumbleHandName(RumbleHand hand) {
    switch (hand) {
    case RumbleHand::Both: return "both";
    case RumbleHand::Left: return "left";
    case RumbleHand::Split: return "split";
    case RumbleHand::Right: break;
    }
    return "right";
}

bool ParseRumbleHand(const std::string& value, RumbleHand& out) {
    const std::string name = Lower(value);
    if (name == "both") out = RumbleHand::Both;
    else if (name == "left") out = RumbleHand::Left;
    else if (name == "right") out = RumbleHand::Right;
    else if (name == "split") out = RumbleHand::Split;
    else return false;
    return true;
}

const char* FoveationName(FoveationLevel level) {
    switch (level) {
    case FoveationLevel::Low: return "low";
    case FoveationLevel::Medium: return "medium";
    case FoveationLevel::High: return "high";
    case FoveationLevel::Off: break;
    }
    return "off";
}

// Names, or aurora's level numbers.
bool ParseFoveation(const std::string& value, FoveationLevel& out) {
    const std::string name = Lower(value);
    if (name == "off" || name == "0") out = FoveationLevel::Off;
    else if (name == "low" || name == "1") out = FoveationLevel::Low;
    else if (name == "medium" || name == "2") out = FoveationLevel::Medium;
    else if (name == "high" || name == "3") out = FoveationLevel::High;
    else return false;
    return true;
}

// One row per field: its key in port_settings.ini (vr_<field>), how to parse a
// value into the struct and how to print the struct's value.
struct Field {
    const char* key;
    void (*apply)(PortVrSettings&, const std::string&);
    void (*write)(const PortVrSettings&, std::ostream&);
};

#define VR_BOOL(name)                                                                               \
    Field{"vr_" #name, [](PortVrSettings& s, const std::string& v) { s.name = ParseBool(v); },    \
          [](const PortVrSettings& s, std::ostream& o) { o << (s.name ? 1 : 0); }}
#define VR_FLOAT(name, low, high)                                                                   \
    Field{"vr_" #name,                                                                              \
          [](PortVrSettings& s, const std::string& v) { s.name = ParseFloat(v, s.name, low, high); }, \
          [](const PortVrSettings& s, std::ostream& o) { o << s.name; }}
#define VR_INT(name, low, high)                                                                     \
    Field{"vr_" #name,                                                                              \
          [](PortVrSettings& s, const std::string& v) { s.name = ParseInt(v, s.name, low, high); }, \
          [](const PortVrSettings& s, std::ostream& o) { o << s.name; }}
#define VR_UINT(name, low, high)                                                                    \
    Field{"vr_" #name,                                                                              \
          [](PortVrSettings& s, const std::string& v) {                                             \
              s.name = static_cast<uint32_t>(ParseInt(v, static_cast<int>(s.name), low, high));     \
          },                                                                                        \
          [](const PortVrSettings& s, std::ostream& o) { o << s.name; }}

const Field kFields[] = {
    VR_BOOL(custom_layout_enabled),
    VR_INT(custom_move_hand, 0, 1), VR_INT(custom_look_hand, 0, 1),
    VR_INT(custom_visor_hand, 0, 1), VR_INT(custom_visor_mode, 0, 2),
#define VR_BIND(name, initial, group, label) VR_INT(custom_##name, 0, 36),
#include "vr/vr_controller_bindings.inc"
#undef VR_BIND

    // headset renderer
    VR_BOOL(enabled),
    VR_BOOL(required),
    Field{"vr_mirror_view", [](PortVrSettings& s, const std::string& v) { ParseMirrorView(v, s.mirror_view); },
          [](const PortVrSettings& s, std::ostream& o) { o << MirrorViewName(s.mirror_view); }},
    Field{"vr_controller_mode",
          [](PortVrSettings& s, const std::string& v) { ParseControllerMode(v, s.controller_mode); },
          [](const PortVrSettings& s, std::ostream& o) { o << ControllerModeName(s.controller_mode); }},
    VR_UINT(frame_interpolation_fps, 0, 240),
    VR_FLOAT(render_scale, kVrRenderScaleMin, kVrRenderScaleMax),
    VR_FLOAT(lean_back_degrees, -kVrLeanBackDegreesLimit, kVrLeanBackDegreesLimit),
    VR_BOOL(passthrough),
    VR_BOOL(diagnostics_logging),
    VR_BOOL(immersive_replay),
    VR_BOOL(multiview),
    Field{"vr_foveation", [](PortVrSettings& s, const std::string& v) { ParseFoveation(v, s.foveation); },
          [](const PortVrSettings& s, std::ostream& o) { o << FoveationName(s.foveation); }},
    VR_BOOL(direct_present),
    VR_BOOL(pipelined_rendering),
    VR_BOOL(deindex_vertices),
    Field{"vr_performance_level",
          [](PortVrSettings& s, const std::string& v) {
              const std::string name = Lower(v);
              if (name == "default" || name == "boost" || name == "sustained_high" ||
                  name == "sustained_low" || name == "power_savings") {
                  s.performance_level = name;
              }
          },
          [](const PortVrSettings& s, std::ostream& o) { o << s.performance_level; }},
    VR_FLOAT(display_refresh_rate, 0.0f, 144.0f),
    VR_FLOAT(screen_distance_meters, 0.25f, 10.0f),
    VR_FLOAT(screen_width_meters, 0.25f, 10.0f),
    VR_BOOL(cinematic_screen_enabled),
    VR_BOOL(game_menu_screen_enabled),
    VR_BOOL(message_box_immersive),
    VR_BOOL(floating_hud),
    // PrimedGun runtime settings
    VR_BOOL(builtin_patches_enabled),
    VR_BOOL(patch_no_idle_sway),
    VR_BOOL(patch_disable_arm_cannon_idle_fidget),
    VR_BOOL(patch_cannon_rotation),
    VR_BOOL(patch_gun_ray_target),
    VR_BOOL(patch_reticle),
    VR_BOOL(use_right_hand),
    VR_FLOAT(offset_x, -2.0f, 2.0f),
    VR_FLOAT(offset_y, -2.0f, 2.0f),
    VR_FLOAT(offset_z, -2.0f, 2.0f),
    VR_FLOAT(model_offset_x, -2.0f, 2.0f),
    VR_FLOAT(model_offset_y, -2.0f, 2.0f),
    VR_FLOAT(model_offset_z, -2.0f, 2.0f),
    VR_FLOAT(rot_offset_x, -180.0f, 180.0f),
    VR_FLOAT(rot_offset_y, -180.0f, 180.0f),
    VR_FLOAT(rot_offset_z, -180.0f, 180.0f),
    VR_FLOAT(world_scale, 0.1f, 50.0f),
    VR_BOOL(rumble_enabled),
    VR_FLOAT(rumble_intensity, 0.0f, 1.0f),
    Field{"vr_rumble_hand", [](PortVrSettings& s, const std::string& v) { ParseRumbleHand(v, s.rumble_hand); },
          [](const PortVrSettings& s, std::ostream& o) { o << RumbleHandName(s.rumble_hand); }},
    VR_BOOL(grip_inputs_enabled),
    VR_BOOL(grip_inputs_use_trackpad),
    VR_FLOAT(trackpad_press_threshold, 0.05f, 1.0f),
    VR_FLOAT(index_grip_press_threshold, 0.05f, 1.0f),
    VR_BOOL(combat_jump_use_primary_button),
    VR_BOOL(gun_targeting_enabled),
    VR_BOOL(cannon_targeting),
    VR_BOOL(room_scale),
    VR_FLOAT(gun_targeting_distance, 1.0f, 200.0f),
    VR_FLOAT(gun_targeting_radius, 0.1f, 25.0f),
    VR_BOOL(visor_helmet_enabled),
    VR_BOOL(vr_overlays_enabled),
    VR_BOOL(height_prompt_enabled),
    VR_BOOL(vr_menu_hold_left_stick),
    VR_BOOL(vr_menu_requires_head_zone),
    VR_BOOL(vr_menu_floating),
    VR_BOOL(frustum_culling_enabled),
    VR_BOOL(remove_cinematic_bars),
    VR_BOOL(sky_at_infinity),
    VR_BOOL(space_warp),
    VR_BOOL(scan_zoom),
    VR_BOOL(beam_wheel_hud_highlight),
    VR_BOOL(look_to_lock_on),
    VR_BOOL(look_to_grapple),
    VR_BOOL(look_lock_no_camera_turn),
    VR_FLOAT(frustum_culling_degrees, 70.0f, 175.0f),
    VR_FLOAT(metroid_hud_distance, 0.1f, 3.0f),
    VR_FLOAT(metroid_hud_size, 0.1f, 3.0f),
    VR_FLOAT(metroid_hud_offset_up, -1.0f, 1.0f),
    VR_FLOAT(metroid_hud_offset_down, -1.0f, 1.0f),
    VR_FLOAT(metroid_hud_offset_left, -1.0f, 1.0f),
    VR_FLOAT(metroid_hud_offset_right, -1.0f, 1.0f),
    VR_BOOL(position_marker_enabled),
    VR_BOOL(xr_dpad_enabled),
    VR_FLOAT(xr_dpad_head_radius, 0.05f, 0.6f),
    VR_FLOAT(xr_dpad_head_y_below, 0.0f, 0.6f),
    VR_FLOAT(xr_dpad_deadzone, 0.05f, 0.95f),
    VR_BOOL(directional_movement_enabled),
    VR_BOOL(directional_movement_use_right_stick),
    VR_BOOL(directional_movement_use_hmd_direction),
    VR_FLOAT(directional_movement_deadzone, 0.0f, 0.95f),
    VR_FLOAT(directional_movement_speed, 1.0f, 60.0f),
    VR_FLOAT(directional_movement_accel, 1.0f, 120.0f),
    VR_FLOAT(directional_movement_air_accel, 0.0f, 60.0f),
    VR_FLOAT(look_yaw_sensitivity, 0.2f, 3.0f),
    VR_BOOL(snap_turn_enabled),
    VR_INT(snap_turn_degrees, 15, 90),
    VR_INT(vr_state_slot, 1, 10),
    VR_INT(cannon_texture_slot, 0, 5),
};

#undef VR_BOOL
#undef VR_FLOAT
#undef VR_INT
#undef VR_UINT

} // namespace

PortVrSettings GetVrSettings() noexcept {
    std::lock_guard lock(Mutex());
    return Stored();
}

void SetVrSettings(const PortVrSettings& settings) noexcept {
    {
        std::lock_guard lock(Mutex());
        Stored() = settings;
    }
    PushVrSettingsToAurora();
    PortDebug::MarkVrSettingsDirty();
}

namespace {
AuroraStereoMirrorView AuroraMirrorView(MirrorView view) noexcept {
    switch (view) {
    case MirrorView::Both: return AURORA_STEREO_MIRROR_BOTH;
    case MirrorView::Left: return AURORA_STEREO_MIRROR_LEFT;
    case MirrorView::Right: return AURORA_STEREO_MIRROR_RIGHT;
    case MirrorView::None: return AURORA_STEREO_MIRROR_NONE;
    case MirrorView::Normal: break;
    }
    return AURORA_STEREO_MIRROR_NORMAL;
}
} // namespace

void PushVrSettingsToAurora() noexcept {
    const PortVrSettings settings = GetVrSettings();
    aurora_set_stereo_immersive_replay(settings.immersive_replay);
    aurora_set_stereo_multiview(settings.multiview);
    aurora_set_stereo_foveation(static_cast<uint32_t>(settings.foveation));
    aurora_set_gx_deindex_vertices(settings.deindex_vertices);
    aurora_set_stereo_mirror_view(AuroraMirrorView(settings.mirror_view));
#if defined(MP_ENABLE_OPENXR)
    // [xr-diag] follows the toggle at once, with Aurora's stereo frame statistics.
    PortVr::diagnostics::SetEnabled(settings.diagnostics_logging);
    aurora_set_stereo_motion_logging(settings.diagnostics_logging);
#endif
    // The presentation policy's half of the same settings; whether the headset
    // is enabled stays whatever the session set.
    PrimeVRPolicyConfig policy = PrimeVRPolicyGetSnapshot().config;
    policy.immersive = settings.immersive_replay;
    policy.cinematic_screen = settings.cinematic_screen_enabled;
    policy.game_menu_screen = settings.game_menu_screen_enabled;
    policy.message_box_immersive = settings.message_box_immersive;
    policy.world_units_per_meter = settings.world_scale;
    policy.screen_distance_meters = settings.screen_distance_meters;
    policy.screen_width_meters = settings.screen_width_meters;
    PrimeVRPolicyConfigure(policy);
    // PrimedGun's HUD distance and size: in the native build both are factors
    // of the HUD frame's authored distance and size (0.75 each by default).
    aurora_set_stereo_head_locked(settings.metroid_hud_size, settings.metroid_hud_distance);
    // The floating morph-ball HUD uses the menu screen's size and distance,
    // with its own delayed headset pose (AURORA_STEREO_ROUTE_FLOATING_HUD).
    aurora_set_stereo_screen_2d(settings.screen_width_meters, settings.screen_distance_meters, settings.world_scale);
    // The pacing thread asks the runtime again when this changes.
    OpenXRSetDisplayRefreshRate(settings.display_refresh_rate);
    OpenXRSetLeanBackDegrees(settings.lean_back_degrees);
    // The eyes are rebuilt at a new scale as the backend next prepares them, so the
    // headset's VR menu can change it in play.
    OpenXRSetRenderScale(settings.render_scale);
    {
        // Asked for again only when it changed, not with every other setting.
        static std::string pushed_level;
        std::lock_guard lock(Mutex());
        if (settings.performance_level != pushed_level) {
            const bool first = pushed_level.empty();
            pushed_level = settings.performance_level;
            if (!first) {
                OpenXRReapplyPerformanceLevel();
            }
        }
    }
    // The session's copy was taken when it was built, which can be before the
    // settings file's vr_passthrough line was read; nothing else passes it on.
    OpenXRSetPassthrough(settings.passthrough);
}

void ResetVrCalibrationOffsets() noexcept {
    PortVrSettings settings = GetVrSettings();
    settings.offset_x = settings.offset_y = settings.offset_z = 0.0f;
    settings.model_offset_x = settings.model_offset_y = settings.model_offset_z = 0.0f;
    settings.rot_offset_x = settings.rot_offset_y = settings.rot_offset_z = 0.0f;
    settings.world_scale = 1.50f;
    SetVrSettings(settings);
}

void ApplyVrSamusArmPreset() noexcept {
    PortVrSettings settings = GetVrSettings();
    settings.model_offset_x = 0.0f;
    settings.model_offset_y = -0.30f;
    settings.model_offset_z = 0.0f;
    settings.rot_offset_x = 0.0f;
    settings.rot_offset_y = 20.0f;
    settings.rot_offset_z = -90.0f;
    SetVrSettings(settings);
}

bool ApplyVrSetting(const std::string& key, const std::string& value) noexcept {
    if (key.compare(0, 3, "vr_") != 0) {
        return false;
    }
    for (const Field& field : kFields) {
        if (key == field.key) {
            {
                std::lock_guard lock(Mutex());
                field.apply(Stored(), value);
            }
            PushVrSettingsToAurora();
            return true;
        }
    }
    return false;
}

void WriteVrSettings(std::ostream& out) {
    const PortVrSettings settings = GetVrSettings();
    out << "# VR (PrimedGun)\n";
    for (const Field& field : kFields) {
        out << field.key << '=';
        field.write(settings, out);
        out << '\n';
    }
}

void ApplyVrEnvironmentOverrides() noexcept {
    std::lock_guard lock(Mutex());
    PortVrSettings& settings = Stored();
    if (const char* enabled = std::getenv("MP_VR"); enabled != nullptr && *enabled != '\0') {
        settings.enabled = ParseBool(enabled);
    }
    if (const char* mirror = std::getenv("MP_VR_MIRROR"); mirror != nullptr && *mirror != '\0') {
        ParseMirrorView(mirror, settings.mirror_view);
    }
    if (const char* log = std::getenv("MP_VR_LOG"); log != nullptr && *log != '\0') {
        settings.diagnostics_logging = ParseBool(log);
    }
    if (const char* foveation = std::getenv("MP_FOVEATION"); foveation != nullptr && *foveation != '\0') {
        ParseFoveation(foveation, settings.foveation);
    }
}

} // namespace PortVr
