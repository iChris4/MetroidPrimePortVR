// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every user-facing VR setting, in one struct: the headset renderer's (from
// Wiicompiled VR's [vr] table) and PrimedGun's RuntimeSettings (NativeRuntime.h),
// with the same defaults PrimedGun shipped. Persisted in the port's
// port_settings.ini under vr_* keys (see vr_settings.cpp), read by the
// pacing thread, the input layer and the game hooks through a copy, and
// written by the F1 overlay, the headset panel and PrimedGun's own
// one-click calibrations.

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace PortVr {

enum class MirrorView : uint8_t { Normal, Both, Left, Right, None };
enum class ControllerModeSetting : uint8_t { PrimedGun, Gamepad, None };
// Split: the arm cannon on the gun hand, the grapple arm on the off hand, the
// rest on both (vr_rumble.h). The others play everything on their hand(s).
enum class RumbleHand : uint8_t { Both, Left, Right, Split };
// aurora_set_stereo_foveation's levels, in order.
enum class FoveationLevel : uint8_t { Off, Low, Medium, High };

struct PortVrSettings {
    bool wrist_visor_enabled = false;
    float wrist_offset_x = 0.f;
    float wrist_offset_y = 0.f;
    float wrist_offset_z = 0.f;
    float wrist_pitch = 40.f;
    float wrist_yaw = -40.f;
    float wrist_roll = 50.f;

    bool custom_layout_enabled = false;
    bool custom_menu_long_press = false;
    float custom_menu_hold_seconds = 1.0f;
    float custom_trigger_threshold = 0.55f;
    float custom_grip_threshold = 0.55f;
    float custom_touchpad_threshold = 0.55f;
    float custom_stick_threshold = 0.55f;
    int custom_label_profile = 0;
    int custom_move_hand = 0;
    int custom_look_hand = 1;
    int custom_visor_hand = 0;
    int custom_visor_mode = 0; // 0 near-head gesture; 1 hold selector; 2 physical buttons only
#define VR_BIND(name, initial, group, label) int custom_##name = initial;
#include "vr/vr_controller_bindings.inc"
#undef VR_BIND

    // --- headset renderer (Wiicompiled [vr]) ---
    bool enabled = true;
    bool required = false;             // a failed headset start stops the game instead of falling back
    // What the desktop window shows while the headset runs. An eye view spares
    // the flat image's rendering, a third of each frame's encoding work.
    MirrorView mirror_view = MirrorView::Left;
    ControllerModeSetting controller_mode = ControllerModeSetting::PrimedGun;
    uint32_t frame_interpolation_fps = 0; // 0 off, 1 auto, else 72/90/120 (Quest; later)
#if defined(__ANDROID__)
    // Of the runtime's recommended eye size. The Quest's GPU gets PrimedGun Quest's
    // eye size (1428x1496 on a Quest 3); launcher/core/launcher_keys.cpp agrees.
    float render_scale = 0.85f;
#else
    float render_scale = 1.0f;         // of the runtime's recommended eye size
#endif
    float lean_back_degrees = 0.0f;
    bool passthrough = true;           // Quest only
    bool diagnostics_logging = false;  // [xr-diag]
    bool room_scale = true;
    bool immersive_replay = true;      // draw the world per eye; off shows the mono image on the virtual screen
    bool multiview = true;             // Quest: both eyes in one Vulkan multiview render pass
    // Quest: fixed foveated rendering of the eye passes (fragment density maps through
    // PrimedGun's patched Dawn). Low shades fully within 30 degrees of each eye's forward
    // direction and in 2x2 blocks beyond; Medium fully to 25, 2x2 to 40, 4x4 beyond; High 18
    // and 34. The Vulkan device gets the maps, or not, when it is created, so Off to a level
    // takes a restart; between levels and back to Off it is live. Off by default: at the
    // Chozo plaza the maps cost about 0.7 ms of GPU time and save none, the eye pass being
    // bound by its draws rather than its pixels (docs/CHOZO_PERFORMANCE.md).
    FoveationLevel foveation = FoveationLevel::Off;
    // Quest: the session runs on Dawn's own Vulkan device and the eyes are copied straight into
    // its swapchain images (PrimedGun's patched Dawn); off shares them with a second device
    // through AHardwareBuffers. Taken when the headset starts.
    bool direct_present = true;
    // Render-first pacing with one packet of overlap: the game records the next frame
    // while Aurora encodes this one, for more throughput at one frame of latency.
    // The D3D12 backend and the Quest's direct presentation pipeline; the others (PC Vulkan,
    // the Quest's AHardwareBuffer bridge) run render-first.
#if defined(__ANDROID__)
    bool pipelined_rendering = true;
#else
    bool pipelined_rendering = false;
#endif
    // aurora_set_gx_deindex_vertices: the Quest's GPU stalls on the dependent array
    // fetches. With it, static world surfaces are resolved once and stay on the GPU
    // (Aurora's geometry cache), which also lightens the FIFO thread, the PC's limit.
    bool deindex_vertices = true;
    std::string performance_level = "boost"; // XR_EXT_performance_settings
    // XR_FB_display_refresh_rate (Quest, Virtual Desktop): the headset's display rate
    // in Hz, the nearest one it offers; 0 leaves the runtime's own (72 Hz on a Quest).
    float display_refresh_rate = 0.0f;
    // Where a virtual screen (menus, cinematics, pause/map when detached) hangs.
    float screen_distance_meters = 1.5f;  // PrimedGun ScreenDistance
    float screen_width_meters = 2.0f;     // PrimedGun ScreenSize: 1.5 m tall at 4:3
    bool cinematic_screen_enabled = true; // cutscenes on the virtual screen
    // The pause menu, logbook and map on the virtual screen. Off (default), they
    // stay in the immersive view like the message boxes: the world keeps head
    // tracking under the pause blur, the menus sit on the held (or floating)
    // screen and the map becomes a 3D map held in front of the player.
    bool game_menu_screen_enabled = false;
    // Message boxes (item pickups, "Game has been saved", scan log unlocks) and
    // the in-game save prompt stay in the immersive view: the world keeps head
    // tracking under the pause blur, which each eye takes from its own image, and
    // the box sits on a screen placed ahead of the head when it opens. Off, they
    // show on the virtual screen with the TV's frozen picture.
    bool message_box_immersive = true;
    // The morph ball's HUD and those message boxes float: a screen of the virtual
    // screen's size and distance that eases toward the headset's position and
    // facing (AURORA_STEREO_ROUTE_FLOATING_HUD). Off, both are fixed: placed
    // where the head faces when the ball's HUD comes up or a box opens, then held
    // (ANCHORED_SCREEN; the ball's HUD stays with the ball's camera).
    bool floating_hud = false;

    // --- PrimedGun RuntimeSettings (NativeRuntime.h:14-87) ---
    bool builtin_patches_enabled = true;
    bool patch_no_idle_sway = true;
    bool patch_disable_arm_cannon_idle_fidget = true;
    bool patch_cannon_rotation = true;
    bool patch_gun_ray_target = true;
    bool patch_reticle = true;
    bool use_right_hand = true;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float offset_z = 0.0f;
    float model_offset_x = 0.0f;
    float model_offset_y = 0.0f;
    float model_offset_z = 0.0f;
    float rot_offset_x = 0.0f;
    float rot_offset_y = 0.0f;
    float rot_offset_z = 0.0f;
    float world_scale = 1.50f;         // game units per metre (PrimedGun GFX_VR_UNITS_PER_METER)
    bool rumble_enabled = true;
    float rumble_intensity = 1.0f;
    RumbleHand rumble_hand = RumbleHand::Split;
    bool grip_inputs_enabled = true;
    bool grip_inputs_use_trackpad = false;
    float trackpad_press_threshold = 0.5f;
    float index_grip_press_threshold = 0.5f;
    bool combat_jump_use_primary_button = false;
    bool gun_targeting_enabled = true;
    bool cannon_targeting = true;
    float gun_targeting_distance = 60.0f;
    float gun_targeting_radius = 4.0f;
    bool visor_helmet_enabled = false;
    bool vr_overlays_enabled = true;
    bool height_prompt_enabled = true;
    bool vr_menu_hold_left_stick = false;
    bool vr_menu_requires_head_zone = false;
    bool vr_menu_floating = false;
    bool frustum_culling_enabled = true;
    // PrimedGun (DolphinXR) RemoveCinematicBars: the morph ball and visor
    // transitions letterbox by shrinking the 3D viewport, which squeezes the
    // world into a band of each eye; script camera filters draw "cinema bars"
    // over the view. Both are skipped while the headset runs.
    bool remove_cinematic_bars = true;
    // PrimedGun (DolphinXR) "Detect Skybox" without its per-draw scan: the
    // sky dome is drawn with the head's rotation only, so it reads as
    // infinitely far rather than as a ball sixty units away.
    bool sky_at_infinity = true;
    // The space warp (the distortion around a charged shot, Flickerbats,
    // Chozo Ghosts and Metroid Prime's second form) in the headset. Off, it is
    // not drawn while the headset runs; the desktop always draws it.
    bool space_warp = true;
    // The scan visor window's zoom in the headset. The window copies the view
    // behind its centre and stretches it over its pane, which magnifies each
    // eye's view; off, the pane shows exactly the view behind it,
    // clear of the scan dim, inside the window's frame. The desktop always zooms.
    bool scan_zoom = true;
    // The beam wheel's hover shown on the HUD: while the weapon hand's B holds
    // the wheel open, the beam the aim ray points at has its beam box on the
    // HUD lit (CHudVisorBeamMenu), where PrimedGun drew its own panel of four
    // icons with a frame around the hovered one.
    bool beam_wheel_hud_highlight = true;
    // Look to lock-on and look to grapple: outside the scan visor the head's
    // gaze picks the L lock's target among the enemies (and other lockable
    // objects) and among the grapple points, as look to scan
    // (patch_gun_ray_target) does for the scan target. Off, that kind is
    // picked by the body's screen box, as on the TV (platform/vr/vr_look_scan.cpp).
    bool look_to_lock_on = true;
    bool look_to_grapple = true;
    // No camera turn on lock: hold the view's acquisition heading throughout
    // the action. Off, follow subsequent target/swing rotation with the initial
    // view offset preserved. Body physics always keeps its target-facing basis.
    bool look_lock_no_camera_turn = false;
    float frustum_culling_degrees = 115.0f;
    float metroid_hud_distance = 0.10f;
    float metroid_hud_size = 0.75f;
    float metroid_hud_offset_up = 0.0f;
    float metroid_hud_offset_down = 0.0f;
    float metroid_hud_offset_left = 0.0f;
    float metroid_hud_offset_right = 0.0f;
    bool position_marker_enabled = true;
    bool xr_dpad_enabled = true;
    float xr_dpad_head_radius = 0.28f;
    float xr_dpad_head_y_below = 0.02f;
    float xr_dpad_deadzone = 0.45f;
    bool directional_movement_enabled = true;
    bool directional_movement_use_right_stick = false;
    bool directional_movement_use_hmd_direction = false;
    float directional_movement_deadzone = 0.25f;
    float directional_movement_speed = 14.0f;
    float directional_movement_accel = 45.0f;
    float directional_movement_air_accel = 8.0f;
    float look_yaw_sensitivity = 1.0f;
    bool snap_turn_enabled = false;
    int snap_turn_degrees = 45;
    int vr_state_slot = 1;
    int cannon_texture_slot = 0;

    bool operator==(const PortVrSettings&) const = default;

    bool CannonTargetingForVisor(bool scanVisor) const noexcept {
        return cannon_targeting && gun_targeting_enabled && !scanVisor;
    }
};

inline constexpr float kVrRenderScaleMin = 0.25f;
inline constexpr float kVrRenderScaleMax = 2.0f;
inline constexpr float kVrLeanBackDegreesLimit = 45.0f;

// Thread-safe copy of the current settings, and replacement of them. A
// replacement marks the port's settings file dirty (PortDebug::SaveSettingsNow
// writes it; the overlay writes it on exit as it does for every other setting).
PortVrSettings GetVrSettings() noexcept;
void SetVrSettings(const PortVrSettings& settings) noexcept;
// Hands aurora the settings it applies itself (the replay switch, the HUD's
// head-locked size and distance). Called after every change and at startup.
void PushVrSettingsToAurora() noexcept;
void ResetVrCalibrationOffsets() noexcept;
void ApplyVrSamusArmPreset() noexcept;

// The settings file: one vr_<name>=<value> line per field. ApplyVrSetting
// returns false for a key that is not a VR setting, so the overlay's parser can
// fall through to its own table; WriteVrSettings appends every VR line.
bool ApplyVrSetting(const std::string& key, const std::string& value) noexcept;
void WriteVrSettings(std::ostream& out);

// Environment overrides for one run, read once at startup: MP_VR=0|1,
// MP_VR_MIRROR=normal|both|left|right|none, MP_VR_LOG=1,
// MP_FOVEATION=off|low|medium|high (or 0-3); MP_FDM_DEVICE=0|1 forces whether the Vulkan
// device gets fragment density maps (platform/vr/openxr_integration.cpp).
void ApplyVrEnvironmentOverrides() noexcept;

} // namespace PortVr
