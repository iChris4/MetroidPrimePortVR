// SPDX-License-Identifier: GPL-3.0-or-later

#include "vr/vr_debug_tab.h"
#include <cstdio>

#include "vr/openxr_integration.h"
#if defined(MP_ENABLE_OPENXR)
#include "vr/openxr_settings_panel.h"
#endif
#include "vr/prime_vr_policy.h"
#include "vr/vr_pad.h"
#include "vr/vr_settings.h"
#include "vr/vr_log.h"

#include <aurora/gfx.h>
#include <imgui.h>

#include <atomic>

namespace PortVr {
namespace {

const char* PresentationName(VRPresentationMode mode) {
  switch (mode) {
  case VRPresentationMode::Desktop:
    return "desktop";
  case VRPresentationMode::VirtualScreen:
    return "virtual screen";
  case VRPresentationMode::Immersive:
    return "immersive";
  default:
    return "unknown";
  }
}

bool SliderMetres(const char* label, float& value, float limit) {
  return ImGui::SliderFloat(label, &value, -limit, limit, "%.3f m");
}

bool SliderDegrees(const char* label, float& value, float limit) {
  return ImGui::SliderFloat(label, &value, -limit, limit, "%.1f deg");
}

const char* VisorDirectionName(int direction) {
  switch (direction) {
  case 0:
    return "up (combat)";
  case 1:
    return "right (X-ray)";
  case 2:
    return "down (thermal)";
  case 3:
    return "left (scan)";
  default:
    return "centred";
  }
}

const char* BeamName(int beam) {
  switch (beam) {
  case 0:
    return "Power";
  case 1:
    return "Wave";
  case 2:
    return "Ice";
  case 3:
    return "Plasma";
  default:
    return "none";
  }
}

} // namespace

namespace {
std::atomic<uint32_t> sHudDrawsVisor{0};
std::atomic<uint32_t> sHudDrawsHud{0};
std::atomic<uint32_t> sHudDrawsMap{0};
std::atomic<uint32_t> sHudDrawsHelmet{0};
} // namespace

void PortVrNoteHudDraws(uint32_t visor, uint32_t hud, uint32_t map, uint32_t helmet) {
  sHudDrawsVisor.store(visor, std::memory_order_relaxed);
  sHudDrawsHud.store(hud, std::memory_order_relaxed);
  sHudDrawsMap.store(map, std::memory_order_relaxed);
  sHudDrawsHelmet.store(helmet, std::memory_order_relaxed);
  // To the log with the stereo statistics' cadence while [xr-diag] is on.
  static uint32_t sCalls = 0;
  if ((++sCalls % 600) == 0 && GetVrSettings().diagnostics_logging) {
    PORTVR_LOG() << "[vr] HUD draw commands per frame: visor " << visor << ", HUD " << hud << ", minimap " << map
                 << ", helmet " << helmet << std::endl;
  }
}

void DrawVrDebugTab() {
  PortVrSettings s = GetVrSettings();
  bool changed = false;

  const bool running = OpenXRIsRunning();
  const PrimeVRPolicySnapshot policy = PrimeVRPolicyGetSnapshot();
  ImGui::Text("Headset: %s   presentation: %s", running ? "running" : "not running",
              PresentationName(policy.presentation));
  if (running) {
    // What the headset gets, as opposed to the Performance tab's mirror rate.
    const OpenXRFrameTiming timing = OpenXRGetFrameTiming();
    ImGui::Text("Display %.0f Hz   new frames to the headset: %.1f/s", timing.headset_hz, timing.rendered_fps);
  }
  ImGui::Text("HUD draw commands per frame: visor %u, HUD %u, minimap %u, helmet %u",
              sHudDrawsVisor.load(std::memory_order_relaxed), sHudDrawsHud.load(std::memory_order_relaxed),
              sHudDrawsMap.load(std::memory_order_relaxed), sHudDrawsHelmet.load(std::memory_order_relaxed));

  ImGui::SeparatorText("Headset");
  changed |= ImGui::Checkbox("Enable the headset (takes effect at the next start)", &s.enabled);
  changed |= ImGui::Checkbox("Immersive replay", &s.immersive_replay);
  changed |= ImGui::Checkbox("Pipelined rendering (more throughput, one frame of latency)", &s.pipelined_rendering);
  changed |= ImGui::Checkbox("Resolve indexed vertices on the CPU (and keep world geometry on the GPU)", &s.deindex_vertices);
  {
    // In MirrorView's order: Normal, Both, Left, Right, None.
    static constexpr const char* kWindowViews[] = {"the flat image", "both eyes", "left eye", "right eye",
                                                   "nothing"};
    int view = static_cast<int>(s.mirror_view);
    if (ImGui::Combo("Window shows", &view, kWindowViews, 5)) {
      s.mirror_view = static_cast<MirrorView>(view);
      changed = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("An eye view spares the flat image's rendering, a third of each frame's encoding work.");
    }
  }
  {
    const bool available = aurora_get_stereo_multiview_available();
    ImGui::BeginDisabled(!available);
    changed |= ImGui::Checkbox("Multiview eyes (both eyes in one pass)", &s.multiview);
    ImGui::EndDisabled();
    if (!available) {
      ImGui::SameLine();
      ImGui::TextDisabled("(Quest only)");
    }
  }
  {
    // In FoveationLevel's order. Never disabled: a session started with it off has no density
    // maps, yet the level chosen here is the next start's.
    static constexpr const char* kFoveationLevels[] = {"Off", "Low", "Medium", "High"};
    int level = static_cast<int>(s.foveation);
    if (ImGui::Combo("Foveated rendering (Quest)", &level, kFoveationLevels, 4)) {
      s.foveation = static_cast<FoveationLevel>(level);
      changed = true;
    }
    ImGui::SameLine();
    if (aurora_stereo_foveation_available()) {
      ImGui::TextDisabled("(live: the edges of each eye shade in 2x2 or 4x4 blocks)");
    } else if (s.foveation == FoveationLevel::Off) {
      ImGui::TextDisabled("(a level takes effect at the next start)");
    } else {
      ImGui::TextDisabled("(unavailable: not a Quest, or this session started with it off)");
    }
  }
#if defined(__ANDROID__)
  changed |= ImGui::Checkbox("Direct to headset (takes effect at the next start)", &s.direct_present);
#endif
  {
    // XR_FB_display_refresh_rate (Quest, Virtual Desktop); the nearest rate offered.
    static constexpr float kRates[] = {0.0f, 72.0f, 80.0f, 90.0f, 120.0f};
    int rate = 0;
    for (int i = 0; i < static_cast<int>(sizeof(kRates) / sizeof(kRates[0])); ++i) {
      if (s.display_refresh_rate == kRates[i]) {
        rate = i;
      }
    }
    if (ImGui::Combo("Display refresh rate", &rate, "Headset default\0" "72 Hz\0" "80 Hz\0" "90 Hz\0"
                                                    "120 Hz\0")) {
      s.display_refresh_rate = kRates[rate];
      changed = true;
    }
  }
  ImGui::TextWrapped(
      "On: the world is drawn per eye. Off: the headset shows the flat image on the virtual screen, "
      "which is also what happens for menus and cinematics.");
  changed |= ImGui::SliderFloat("World scale", &s.world_scale, 0.5f, 3.0f, "%.2f units per metre");
  changed |= ImGui::SliderFloat("Lean back", &s.lean_back_degrees, -kVrLeanBackDegreesLimit,
                                kVrLeanBackDegreesLimit, "%.0f deg");
  changed |= ImGui::Checkbox("Cull against where the head looks", &s.frustum_culling_enabled);
  changed |= ImGui::SliderFloat("Culling cone", &s.frustum_culling_degrees, 60.f, 179.f, "%.0f deg");

  ImGui::SeparatorText("HUD (in front of the head)");
  changed |= ImGui::SliderFloat("HUD size", &s.metroid_hud_size, 0.1f, 3.0f, "%.2f x");
  changed |= ImGui::SliderFloat("HUD distance", &s.metroid_hud_distance, 0.1f, 3.0f, "%.2f x");
  ImGui::TextWrapped("Factors of the HUD frame's authored size and distance (PrimedGun: 0.75 and 0.75).");

  ImGui::SeparatorText("Arm cannon");
  changed |= ImGui::Checkbox("Tracked cannon (follows the controller)", &s.patch_cannon_rotation);
  int hand = s.use_right_hand ? 1 : 0;
  if (ImGui::Combo("Hand", &hand, "Left\0Right\0")) {
    s.use_right_hand = hand == 1;
    changed = true;
  }
  changed |= SliderMetres("Position right", s.offset_x, 0.5f);
  changed |= SliderMetres("Position up", s.offset_y, 0.5f);
  changed |= SliderMetres("Position back", s.offset_z, 0.5f);
  changed |= SliderDegrees("Pitch offset", s.rot_offset_x, 90.f);
  changed |= SliderDegrees("Yaw offset", s.rot_offset_y, 90.f);
  changed |= SliderDegrees("Roll offset", s.rot_offset_z, 180.f);
  changed |= SliderMetres("Model right", s.model_offset_x, 0.5f);
  changed |= SliderMetres("Model forward", s.model_offset_y, 0.5f);
  changed |= SliderMetres("Model up", s.model_offset_z, 0.5f);
  if (ImGui::Button("Samus arm preset")) {
    ApplyVrSamusArmPreset();
    s = GetVrSettings();
    changed = false;
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset calibration")) {
    ResetVrCalibrationOffsets();
    s = GetVrSettings();
    changed = false;
  }

  ImGui::SeparatorText("Scan visor");
  changed |= ImGui::Checkbox("Scan window zoom", &s.scan_zoom);
  ImGui::TextWrapped("Off, the scan window shows the view behind it unmagnified, keeping its frame.");

  ImGui::SeparatorText("Look targeting");
  changed |= ImGui::Checkbox("Room Scale", &s.room_scale);
  ImGui::TextWrapped("Physical walking moves Samus with collision checks in first person. "
                     "On by default. Morph ball and cinematics retain their camera movement.");
  changed |= ImGui::Checkbox("Cannon targeting", &s.cannon_targeting);
  ImGui::TextWrapped("Aim the cannon to select combat lock-on and grapple targets and place the combat reticle. "
                     "The scan visor keeps head targeting. Off uses the existing camera/head targeting. "
                     "Requires a tracked cannon controller.");
  changed |= ImGui::Checkbox("Look to scan", &s.patch_gun_ray_target);
  ImGui::TextWrapped("The scan target and the scan icons follow where the head looks, and a scan lock keeps the "
                     "body's facing (PrimedGun's gun ray / scan target hook).");
  changed |= ImGui::Checkbox("Look to lock-on", &s.look_to_lock_on);
  changed |= ImGui::Checkbox("Look to grapple", &s.look_to_grapple);
  ImGui::TextWrapped("In the other visors, L locks on to the enemy or the grapple point the head looks at. Off, "
                     "that kind is picked in front of the body.");
  changed |= ImGui::Checkbox("No camera turn on lock", &s.look_lock_no_camera_turn);
  ImGui::TextWrapped("During a lock the head picked, the body and the camera keep their facing instead of turning "
                     "to the target. Off, they turn as on the TV, which keeps a boss ahead while strafing.");
  changed |= ImGui::SliderFloat("Look reach", &s.gun_targeting_distance, 15.f, 100.f, "%.0f units");
  changed |= ImGui::SliderFloat("Look radius", &s.gun_targeting_radius, 0.5f, 12.f, "%.1f units");
  ImGui::TextWrapped("The radius widens the cones around the gaze that pick the target and show icons "
                     "(PrimedGun: 60 and 4).");

  ImGui::SeparatorText("Camera");
  changed |= ImGui::Checkbox("No camera bob or idle sway", &s.patch_no_idle_sway);
  changed |= ImGui::Checkbox("No arm cannon idle fidget", &s.patch_disable_arm_cannon_idle_fidget);
  changed |= ImGui::Checkbox("Remove cinematic bars", &s.remove_cinematic_bars);
  ImGui::TextWrapped("The morph ball and visor letterbox shrink the 3D view, which distorts it in the headset; "
                     "this keeps the view full and skips the cinema-bar overlay.");
  changed |= ImGui::Checkbox("Sky at infinity", &s.sky_at_infinity);
  ImGui::TextWrapped("The sky dome is drawn with the head's rotation only, so the planet and the stars read as "
                     "far away rather than as a ball sixty units out (PrimedGun's Detect Skybox, without the scan).");
  changed |= ImGui::Checkbox("Space warp", &s.space_warp);
  ImGui::TextWrapped("The distortion around charged shots (also Flickerbats, Chozo Ghosts and Metroid Prime). "
                     "Off, it is not drawn in the headset.");

  ImGui::SeparatorText("Controls");
  int mode = static_cast< int >(s.controller_mode);
  if (ImGui::Combo("Controller mode", &mode, "PrimedGun\0Gamepad\0None\0")) {
    s.controller_mode = static_cast< ControllerModeSetting >(mode);
    changed = true;
  }
  ImGui::TextWrapped("PrimedGun: the headset controllers drive pad 1 with PrimedGun's layout. "
                     "Gamepad: they act as a plain gamepad. None: a real pad plays.");
  changed |= ImGui::Checkbox("Rumble", &s.rumble_enabled);
  int rumbleHand = static_cast< int >(s.rumble_hand);
  if (ImGui::Combo("Rumble target", &rumbleHand, "Both\0Left only\0Right only\0Split\0")) {
    s.rumble_hand = static_cast< RumbleHand >(rumbleHand);
    changed = true;
  }
  ImGui::TextWrapped("Split: the arm cannon (charge, missiles, recoil) rumbles the gun hand, the grapple "
                     "the off hand, everything else both. PrimedGun controller mode only.");
  changed |= ImGui::SliderFloat("Rumble intensity", &s.rumble_intensity, 0.f, 1.f, "%.2f");
  changed |= ImGui::Checkbox("Grip inputs", &s.grip_inputs_enabled);
  changed |= ImGui::Checkbox("Jump with the primary button", &s.combat_jump_use_primary_button);
  changed |= ImGui::Checkbox("Beam wheel lights the HUD beam box", &s.beam_wheel_hud_highlight);
  ImGui::TextWrapped("While the weapon hand's B holds the beam wheel open, the beam the cannon points at "
                     "has its box on the HUD lit (PrimedGun drew its own panel of icons instead).");
  const VrPadState pad = GetVrPadState();
  ImGui::Text("Beam wheel: %s   hover: %s", pad.weapon_panel ? "open" : "closed",
              pad.weapon_panel ? BeamName(pad.weapon_selected) : "-");

  ImGui::SeparatorText("Movement");
  changed |= ImGui::Checkbox("Directional movement (the move stick strafes)", &s.directional_movement_enabled);
  ImGui::TextWrapped("Outside a lock, the move stick walks and strafes toward where its controller points and only "
                     "the look stick turns (PrimedGun's left stick strafe). Off, pushing it sideways turns Samus "
                     "as on the TV.");
  {
    int stick = s.directional_movement_use_right_stick ? 1 : 0;
    if (ImGui::Combo("Movement stick", &stick, "Left\0Right\0")) {
      s.directional_movement_use_right_stick = stick == 1;
      changed = true;
    }
    int direction = s.directional_movement_use_hmd_direction ? 1 : 0;
    if (ImGui::Combo("Movement direction", &direction, "Controller\0Headset\0")) {
      s.directional_movement_use_hmd_direction = direction == 1;
      changed = true;
    }
  }
  changed |= ImGui::SliderFloat("Movement deadzone", &s.directional_movement_deadzone, 0.f, 0.95f, "%.2f");
  changed |= ImGui::SliderFloat("Movement speed", &s.directional_movement_speed, 1.f, 60.f, "%.1f units/s");
  changed |= ImGui::SliderFloat("Movement acceleration", &s.directional_movement_accel, 1.f, 120.f,
                                "%.0f units/s/s");
  changed |= ImGui::SliderFloat("Air acceleration", &s.directional_movement_air_accel, 0.f, 60.f, "%.1f units/s/s");
  ImGui::TextWrapped("PrimedGun: deadzone 0.25, speed 14, acceleration 45, air acceleration 8.");
  if (pad.move_owned) {
    ImGui::Text("Move stick: %s   heading: %+.0f deg", pad.move_moving ? "walking" : "centred",
                pad.move_heading_degrees);
  } else {
    ImGui::Text("Move stick: the game's walk");
  }

  ImGui::SeparatorText("Visor selection");
  changed |= ImGui::Checkbox("Off-hand visor wristband", &s.wrist_visor_enabled);
  changed |= ImGui::Checkbox("Pick visors with the off hand next to the head", &s.xr_dpad_enabled);
  ImGui::TextWrapped("Hold the off-hand controller beside the headset and push its stick: up combat, left scan, "
                     "down thermal, right X-ray. Meanwhile that stick does not move Samus.");
  changed |= ImGui::SliderFloat("Head zone radius", &s.xr_dpad_head_radius, 0.05f, 0.6f, "%.2f m");
  changed |= ImGui::SliderFloat("Head zone below", &s.xr_dpad_head_y_below, 0.0f, 0.6f, "%.2f m");
  changed |= ImGui::SliderFloat("Gesture stick deadzone", &s.xr_dpad_deadzone, 0.05f, 0.95f, "%.2f");
  ImGui::TextWrapped("The zone reaches 6 cm past the radius, down to 4 cm past the 'below' distance and 28 cm "
                     "above the head; the deadzone counts up to 0.25 (all as in PrimedGun).");
  ImGui::Text("Off hand: %s   stick: %s", pad.visor_zone ? "at the head" : "away",
              pad.visor_zone ? VisorDirectionName(pad.visor_direction) : "-");

  ImGui::SeparatorText("Virtual screen (menus, cinematics, morph ball HUD)");
  changed |= ImGui::SliderFloat("Screen distance", &s.screen_distance_meters, 0.5f, 5.f, "%.2f m");
  changed |= ImGui::SliderFloat("Screen width", &s.screen_width_meters, 0.5f, 6.f, "%.2f m");
  changed |= ImGui::Checkbox("Floating HUD", &s.floating_hud);
  ImGui::TextWrapped("The morph-ball HUD and the message boxes use this size and distance. Floating, they "
                     "ease toward the headset's position and facing with a gentle delay; fixed, they stay "
                     "where the head faced when the morph-ball HUD came up or the box opened.");
  changed |= ImGui::Checkbox("Cinematics on the screen", &s.cinematic_screen_enabled);
  changed |= ImGui::Checkbox("Pause, map and logbook on the screen", &s.game_menu_screen_enabled);
  ImGui::TextWrapped("Off, they stay in the headset view like the message boxes: the world keeps "
                     "following the head under the pause blur, the menus sit on the held screen and the "
                     "map is a 3D map held in front of you.");
  changed |= ImGui::Checkbox("Message boxes in the headset view", &s.message_box_immersive);
  ImGui::TextWrapped("Item pickup and save messages, and the save prompt: the world keeps following the head "
                     "under the pause blur, drawn per eye, and the box stays on a screen placed ahead of the "
                     "head when it opens (or floats, with Floating HUD). "
                     "Off, they show on the screen with the frozen picture.");

  ImGui::SeparatorText("VR menu (PrimedGun's settings in the headset)");
  changed |= ImGui::Checkbox("VR menu", &s.vr_overlays_enabled);
  ImGui::TextWrapped("Click the off hand's thumbstick (the left one unless left-handed) or its menu button to open "
                     "or close it. The cannon hand's laser points and its trigger or A clicks; the game keeps "
                     "running, without the controllers, while it is open.");
  changed |= ImGui::Checkbox("Detach it from the hand (floats 2.7 m ahead)", &s.vr_menu_floating);
  changed |= ImGui::Checkbox("Open it with a one-second press", &s.vr_menu_hold_left_stick);
  changed |= ImGui::Checkbox("Open it only with the hand next to the head", &s.vr_menu_requires_head_zone);
#if defined(MP_ENABLE_OPENXR)
  {
    bool menu_open = OpenXRSettingsPanelOpen();
    ImGui::BeginDisabled(!running || !s.vr_overlays_enabled);
    if (ImGui::Checkbox("Show it in the headset now", &menu_open)) {
      OpenXRSetSettingsPanelOpen(menu_open);
    }
    ImGui::EndDisabled();
  }
#endif

  ImGui::SeparatorText("Diagnostics");
  changed |= ImGui::Checkbox("[xr-diag] logging", &s.diagnostics_logging);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Includes detailed CPU timers in the graphics command processor. This can reduce throughput, "
                      "especially in draw-heavy rooms. Disable it when comparing normal performance.");
  }

  if (changed) {
    SetVrSettings(s);
  }
}

} // namespace PortVr
