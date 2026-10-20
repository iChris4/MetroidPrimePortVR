// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from PrimedGun's Quest launcher (features/primedgun/ui/PrimedGunTabs.kt,
// Copyright 2026 PrimedGun Project, GPL-2.0-or-later).
package org.primedgun.v2.launcher

import android.content.Context
import org.primedgun.v2.R

/**
 * The launcher's tabs, in the order PrimedGun's MainWindow::ConnectStack() adds them,
 * and the row lists for the tabs that are plain settings lists.
 *
 * Rows, labels, ranges and reset groups follow the PC launcher's Qt tabs
 * (launcher/ui/{controller,calibration,port_config}_tab.cpp), which already map
 * PrimedGun's settings onto the port's vr_* keys; the ranges come from the shared key
 * table. Port Config takes the place of PrimedGun's Dolphin Config tab and holds the
 * Quest's own headset settings: the PC-only ones (VR on/off, the desktop mirror,
 * fullscreen, VSync) are left out, and the renderer switches the PC reaches through
 * the F1 overlay are here, since the Quest has none.
 */
object PrimedGunTabs {

    enum class Tab(val titleId: Int) {
        SETUP(R.string.primedgun_tab_setup),
        CONTROLLER(R.string.primedgun_tab_controller),
        CALIBRATION(R.string.primedgun_tab_calibration),
        CANNON_TEXTURES(R.string.primedgun_tab_cannon_textures),
        LAYOUT(R.string.primedgun_tab_layout),
        PORT_CONFIG(R.string.primedgun_tab_port_config),
        ABOUT(R.string.primedgun_tab_about)
    }

    /** The tabs whose rows edit settings, which lock while the game runs. */
    val SETTINGS_TABS = setOf(Tab.CONTROLLER, Tab.CALIBRATION, Tab.CANNON_TEXTURES, Tab.PORT_CONFIG)

    /** Rows for the list-based tabs. The other tabs have their own fragments. */
    fun itemsFor(context: Context, tab: Tab): List<PrimedGunItem> = when (tab) {
        Tab.CONTROLLER -> controllerItems(context)
        Tab.CALIBRATION -> calibrationItems(context)
        Tab.PORT_CONFIG -> portConfigItems(context)
        Tab.SETUP, Tab.CANNON_TEXTURES, Tab.LAYOUT, Tab.ABOUT -> emptyList()
    }

    // PrimedGun's Reset Controller (launcher/ui/controller_tab.cpp).
    private val CONTROLLER_KEYS = arrayOf(
        "vr_use_right_hand", "vr_vr_overlays_enabled", "vr_vr_menu_hold_left_stick",
        "vr_vr_menu_requires_head_zone", "vr_vr_menu_floating", "vr_cinematic_screen_enabled",
        "vr_game_menu_screen_enabled", "vr_message_box_immersive", "vr_floating_hud",
        "vr_rumble_enabled",
        "vr_rumble_intensity",
        "vr_rumble_hand", "vr_xr_dpad_enabled", "vr_combat_jump_use_primary_button",
        "vr_beam_wheel_hud_highlight",
        "vr_grip_inputs_enabled", "vr_grip_inputs_use_trackpad", "vr_trackpad_press_threshold",
        "vr_index_grip_press_threshold", "vr_directional_movement_enabled",
        "vr_directional_movement_use_right_stick", "vr_directional_movement_use_hmd_direction",
        "vr_xr_dpad_head_radius", "vr_xr_dpad_head_y_below", "vr_xr_dpad_deadzone",
        "vr_directional_movement_deadzone", "vr_directional_movement_speed",
        "vr_directional_movement_accel", "vr_directional_movement_air_accel",
        "vr_look_yaw_sensitivity", "vr_snap_turn_enabled", "vr_snap_turn_degrees",
    )
    private val HUD_KEYS = arrayOf(
        "vr_metroid_hud_distance", "vr_metroid_hud_size", "vr_metroid_hud_offset_up",
        "vr_metroid_hud_offset_down", "vr_metroid_hud_offset_left", "vr_metroid_hud_offset_right",
    )
    private val TARGETING_KEYS = arrayOf(
        "vr_gun_targeting_enabled", "vr_gun_targeting_distance", "vr_gun_targeting_radius",
        "vr_visor_helmet_enabled",
    )
    private val OFFSET_KEYS = arrayOf(
        "vr_model_offset_x", "vr_model_offset_y", "vr_model_offset_z",
        "vr_rot_offset_x", "vr_rot_offset_y", "vr_rot_offset_z",
    )

    private fun controllerItems(context: Context): List<PrimedGunItem> = buildList {
        add(PrimedGunItem.Action("Customize controller layout") { ControllerLayoutDialog.show(context) })
        add(PrimedGunItem.Switch("Use custom controller layout", refreshes = true,
            get = { LauncherSettings.bool("vr_custom_layout_enabled") },
            set = { LauncherSettings.setBool("vr_custom_layout_enabled", it) }))
        add(header(context, R.string.primedgun_section_controller_mapping))
        add(PrimedGunItem.Action(context.getString(R.string.primedgun_reset_controller)) {
            LauncherSettings.reset(*CONTROLLER_KEYS)
            LauncherNative.keyTable().map { it.substringBefore('\t') }.filter { it.startsWith("vr_custom_") }.forEach { LauncherSettings.reset(it) }
        })
        // Stored as use_right_hand, so the "left hand" option is the inverted case.
        add(
            PrimedGunItem.Toggle2(
                labelA = context.getString(R.string.primedgun_right_hand),
                labelB = context.getString(R.string.primedgun_left_hand),
                inactive = inactive("vr_use_right_hand"),
                get = { !LauncherSettings.bool("vr_use_right_hand") },
                set = { LauncherSettings.setBool("vr_use_right_hand", !it) }
            )
        )
        add(
            switch(
                context, R.string.primedgun_beam_wheel_hud_highlight, "vr_beam_wheel_hud_highlight",
                R.string.primedgun_beam_wheel_hud_highlight_note
            )
        )

        add(PrimedGunItem.Header("Rumble"))
        add(switch(context, R.string.primedgun_rumble, "vr_rumble_enabled"))
        add(
            choice(
                context, R.string.primedgun_rumble_target, "vr_rumble_hand",
                listOf(
                    R.string.primedgun_rumble_split to "split",
                    R.string.primedgun_rumble_both to "both",
                    R.string.primedgun_rumble_left_only to "left",
                    R.string.primedgun_rumble_right_only to "right",
                )
            )
        )
        add(PrimedGunItem.Note(context.getString(R.string.primedgun_rumble_split_note)))
        add(slider(context, R.string.primedgun_rumble_intensity, "vr_rumble_intensity"))

        add(PrimedGunItem.Header("Visor gesture tuning"))
        add(slider(context, R.string.primedgun_dpad_head_radius, "vr_xr_dpad_head_radius"))
        add(slider(context, R.string.primedgun_dpad_head_y_below, "vr_xr_dpad_head_y_below"))
        add(slider(context, R.string.primedgun_dpad_deadzone, "vr_xr_dpad_deadzone"))

        add(header(context, R.string.primedgun_section_directional_movement))
        add(switch(context, R.string.primedgun_movement_enabled, "vr_directional_movement_enabled"))
        add(
            PrimedGunItem.Toggle2(
                labelA = context.getString(R.string.primedgun_controller_direction),
                labelB = context.getString(R.string.primedgun_hmd_direction),
                inactive = inactive("vr_directional_movement_use_hmd_direction"),
                get = { LauncherSettings.bool("vr_directional_movement_use_hmd_direction") },
                set = { LauncherSettings.setBool("vr_directional_movement_use_hmd_direction", it) }
            )
        )
        add(slider(context, R.string.primedgun_movement_deadzone, "vr_directional_movement_deadzone"))
        add(slider(context, R.string.primedgun_movement_speed, "vr_directional_movement_speed"))
        add(slider(context, R.string.primedgun_movement_accel, "vr_directional_movement_accel"))
        add(slider(context, R.string.primedgun_movement_air_accel, "vr_directional_movement_air_accel"))
        add(
            slider(
                context, R.string.primedgun_look_yaw_sensitivity, "vr_look_yaw_sensitivity",
                R.string.primedgun_look_yaw_sensitivity_note
            )
        )
        add(switch(context, R.string.primedgun_snap_turn, "vr_snap_turn_enabled"))
        add(
            choice(
                context, R.string.primedgun_snap_turn_angle, "vr_snap_turn_degrees",
                listOf(
                    R.string.primedgun_snap_turn_30 to "30",
                    R.string.primedgun_snap_turn_45 to "45",
                    R.string.primedgun_snap_turn_60 to "60",
                    R.string.primedgun_snap_turn_90 to "90",
                )
            )
        )
    }

    private fun calibrationItems(context: Context): List<PrimedGunItem> = buildList {
        add(header(context, R.string.primedgun_section_in_headset_display))
        add(switch(context, R.string.primedgun_vr_overlays_enabled, "vr_vr_overlays_enabled"))
        add(switch(context, R.string.primedgun_height_prompt_enabled, "vr_height_prompt_enabled"))
        add(switch(context, R.string.primedgun_cinematic_screen_enabled, "vr_cinematic_screen_enabled"))
        add(switch(context, R.string.primedgun_vr_menu_floating, "vr_vr_menu_floating"))
        add(switch(context, R.string.primedgun_game_menu_screen_enabled, "vr_game_menu_screen_enabled"))
        add(
            switch(
                context, R.string.primedgun_message_box_immersive, "vr_message_box_immersive",
                R.string.primedgun_message_box_immersive_note
            )
        )
        add(
            switch(
                context, R.string.primedgun_floating_hud, "vr_floating_hud",
                R.string.primedgun_floating_hud_note
            )
        )
        add(switch(context, R.string.primedgun_visor_helmet_enabled, "vr_visor_helmet_enabled"))
        add(PrimedGunItem.Note(context.getString(R.string.primedgun_visor_helmet_note)))
        add(switch(context, R.string.primedgun_position_marker_enabled, "vr_position_marker_enabled"))

        add(header(context, R.string.primedgun_section_culling))
        add(switch(context, R.string.primedgun_frustum_culling_enabled, "vr_frustum_culling_enabled"))
        add(slider(context, R.string.primedgun_frustum_culling_degrees, "vr_frustum_culling_degrees"))

        add(header(context, R.string.primedgun_section_hud))
        add(PrimedGunItem.Action(context.getString(R.string.primedgun_reset_hud)) {
            LauncherSettings.reset(*HUD_KEYS)
        })
        add(slider(context, R.string.primedgun_hud_distance, "vr_metroid_hud_distance"))
        add(slider(context, R.string.primedgun_hud_size, "vr_metroid_hud_size"))
        add(hudAxis(context, R.string.primedgun_hud_vertical, "vr_metroid_hud_offset_up", "vr_metroid_hud_offset_down"))
        add(hudAxis(context, R.string.primedgun_hud_horizontal, "vr_metroid_hud_offset_right", "vr_metroid_hud_offset_left"))

        add(header(context, R.string.primedgun_section_targeting))
        add(PrimedGunItem.Action(context.getString(R.string.primedgun_reset_targeting)) {
            LauncherSettings.reset(*TARGETING_KEYS)
        })
        add(slider(context, R.string.primedgun_target_distance, "vr_gun_targeting_distance"))
        add(slider(context, R.string.primedgun_target_radius, "vr_gun_targeting_radius"))

        add(header(context, R.string.primedgun_section_offset_tuning))
        add(PrimedGunItem.Action(context.getString(R.string.primedgun_reset_calibration)) {
            LauncherSettings.reset(*OFFSET_KEYS)
        })

        add(header(context, R.string.primedgun_section_position))
        add(slider(context, R.string.primedgun_offset_left_right, "vr_model_offset_x"))
        add(slider(context, R.string.primedgun_offset_forward_back, "vr_model_offset_y"))
        add(slider(context, R.string.primedgun_offset_up_down, "vr_model_offset_z"))

        add(header(context, R.string.primedgun_section_rotation))
        add(slider(context, R.string.primedgun_pitch_offset, "vr_rot_offset_x"))
        add(slider(context, R.string.primedgun_yaw_offset, "vr_rot_offset_y"))
        add(slider(context, R.string.primedgun_roll_offset, "vr_rot_offset_z"))

        add(header(context, R.string.primedgun_section_presets))
        add(PrimedGunItem.Action(context.getString(R.string.primedgun_preset_default_arm)) {
            LauncherSettings.reset(*OFFSET_KEYS)
        })
        add(PrimedGunItem.Action(context.getString(R.string.primedgun_preset_samus_arm)) {
            // PrimedGun's Samus arm (platform/vr/vr_settings.cpp ApplyVrSamusArmPreset).
            LauncherSettings.setFloat("vr_model_offset_x", 0.0f)
            LauncherSettings.setFloat("vr_model_offset_y", -0.30f)
            LauncherSettings.setFloat("vr_model_offset_z", 0.0f)
            LauncherSettings.setFloat("vr_rot_offset_x", 0.0f)
            LauncherSettings.setFloat("vr_rot_offset_y", 20.0f)
            LauncherSettings.setFloat("vr_rot_offset_z", -90.0f)
        })
    }

    private fun portConfigItems(context: Context): List<PrimedGunItem> = buildList {
        add(header(context, R.string.primedgun_section_port_config))
        add(PrimedGunItem.Note(context.getString(R.string.primedgun_port_config_note)))

        add(header(context, R.string.primedgun_section_vr_headset))
        add(
            choice(
                context, R.string.primedgun_controller_mode, "vr_controller_mode",
                listOf(
                    R.string.primedgun_controller_mode_primedgun to "primedgun",
                    R.string.primedgun_controller_mode_gamepad to "gamepad",
                    R.string.primedgun_controller_mode_none to "none",
                ),
                R.string.primedgun_controller_mode_note
            )
        )
        add(slider(context, R.string.primedgun_render_scale, "vr_render_scale", R.string.primedgun_render_scale_note))
        add(
            choice(
                context, R.string.primedgun_refresh_rate, "vr_display_refresh_rate",
                listOf(
                    R.string.primedgun_refresh_rate_default to "0",
                    R.string.primedgun_refresh_rate_72 to "72",
                    R.string.primedgun_refresh_rate_80 to "80",
                    R.string.primedgun_refresh_rate_90 to "90",
                    R.string.primedgun_refresh_rate_120 to "120",
                ),
                R.string.primedgun_refresh_rate_note
            )
        )
        add(
            choice(
                context, R.string.primedgun_performance_level, "vr_performance_level",
                listOf(
                    R.string.primedgun_performance_boost to "boost",
                    R.string.primedgun_performance_sustained_high to "sustained_high",
                    R.string.primedgun_performance_sustained_low to "sustained_low",
                    R.string.primedgun_performance_power_savings to "power_savings",
                    R.string.primedgun_performance_default to "default",
                ),
                R.string.primedgun_performance_level_note
            )
        )
        add(
            choice(
                context, R.string.primedgun_foveation, "vr_foveation",
                listOf(
                    R.string.primedgun_foveation_off to "off",
                    R.string.primedgun_foveation_low to "low",
                    R.string.primedgun_foveation_medium to "medium",
                    R.string.primedgun_foveation_high to "high",
                ),
                R.string.primedgun_foveation_note
            )
        )
        add(switch(context, R.string.primedgun_passthrough, "vr_passthrough", R.string.primedgun_passthrough_note))
        add(slider(context, R.string.primedgun_world_scale, "vr_world_scale", R.string.primedgun_world_scale_note))
        add(switch(context, R.string.primedgun_immersive_replay, "vr_immersive_replay", R.string.primedgun_immersive_replay_note))
        add(switch(context, R.string.primedgun_remove_cinematic_bars, "vr_remove_cinematic_bars", R.string.primedgun_remove_cinematic_bars_note))
        add(switch(context, R.string.primedgun_sky_at_infinity, "vr_sky_at_infinity", R.string.primedgun_sky_at_infinity_note))
        add(switch(context, R.string.primedgun_space_warp, "vr_space_warp", R.string.primedgun_space_warp_note))
        add(switch(context, R.string.primedgun_scan_zoom, "vr_scan_zoom", R.string.primedgun_scan_zoom_note))
        add(slider(context, R.string.primedgun_screen_distance, "vr_screen_distance_meters", R.string.primedgun_screen_distance_note))
        add(slider(context, R.string.primedgun_screen_width, "vr_screen_width_meters", R.string.primedgun_screen_width_note))
        add(slider(context, R.string.primedgun_lean_back, "vr_lean_back_degrees", R.string.primedgun_lean_back_note))

        add(header(context, R.string.primedgun_section_display))
        add(
            choice(
                context, R.string.primedgun_msaa, "msaa",
                listOf(R.string.primedgun_msaa_off to "1", R.string.primedgun_msaa_4x to "4")
            )
        )
        add(
            choice(
                context, R.string.primedgun_anisotropy, "anisotropy",
                listOf(
                    R.string.primedgun_anisotropy_1x to "1",
                    R.string.primedgun_anisotropy_2x to "2",
                    R.string.primedgun_anisotropy_4x to "4",
                    R.string.primedgun_anisotropy_8x to "8",
                    R.string.primedgun_anisotropy_16x to "16",
                )
            )
        )
        // render_scale 0 is the overlay's "Auto render scale (native)".
        add(
            PrimedGunItem.Switch(
                title = context.getString(R.string.primedgun_auto_render_scale),
                description = context.getString(R.string.primedgun_auto_render_scale_note),
                inactive = inactive("render_scale"),
                refreshes = true,
                get = { LauncherSettings.float("render_scale") <= 0f },
                set = { LauncherSettings.setFloat("render_scale", if (it) 0f else 1f) }
            )
        )
        val efb = LauncherSettings.key("render_scale")
        add(
            PrimedGunItem.Slider(
                title = context.getString(R.string.primedgun_efb_scale),
                min = efb.min, max = efb.max, step = efb.step,
                inactive = !efb.active,
                description = context.getString(R.string.primedgun_efb_scale_note),
                enabled = { LauncherSettings.float("render_scale") > 0f },
                get = { LauncherSettings.float("render_scale").coerceAtLeast(1f) },
                set = { LauncherSettings.setFloat("render_scale", it) }
            )
        )

        add(header(context, R.string.primedgun_section_advanced))
        add(PrimedGunItem.Note(context.getString(R.string.primedgun_advanced_note)))
        add(switch(context, R.string.primedgun_multiview, "vr_multiview", R.string.primedgun_multiview_note))
        add(switch(context, R.string.primedgun_direct_present, "vr_direct_present", R.string.primedgun_direct_present_note))
        add(switch(context, R.string.primedgun_pipelined_rendering, "vr_pipelined_rendering", R.string.primedgun_pipelined_rendering_note))
        add(switch(context, R.string.primedgun_deindex_vertices, "vr_deindex_vertices", R.string.primedgun_deindex_vertices_note))
        add(switch(context, R.string.primedgun_diagnostics_logging, "vr_diagnostics_logging", R.string.primedgun_diagnostics_logging_note))
    }

    private fun inactive(key: String) = !LauncherSettings.isActive(key)

    private fun header(context: Context, titleId: Int) = PrimedGunItem.Header(context.getString(titleId))

    private fun switch(context: Context, titleId: Int, key: String, noteId: Int? = null) =
        PrimedGunItem.Switch(
            title = context.getString(titleId),
            description = noteId?.let(context::getString),
            inactive = inactive(key),
            get = { LauncherSettings.bool(key) },
            set = { LauncherSettings.setBool(key, it) }
        )

    // A slider over the key's range from the shared key table.
    private fun slider(context: Context, titleId: Int, key: String, noteId: Int? = null): PrimedGunItem.Slider {
        val info = LauncherSettings.key(key)
        return PrimedGunItem.Slider(
            title = context.getString(titleId),
            min = info.min,
            max = info.max,
            step = info.step,
            inactive = !info.active,
            description = noteId?.let(context::getString),
            get = { LauncherSettings.float(key) },
            set = { LauncherSettings.setFloat(key, it) }
        )
    }

    private fun choice(
        context: Context,
        titleId: Int,
        key: String,
        options: List<Pair<Int, String>>,
        noteId: Int? = null,
    ) = PrimedGunItem.Choice(
        title = context.getString(titleId),
        labels = options.map { context.getString(it.first) },
        values = options.map { it.second },
        inactive = inactive(key),
        description = noteId?.let(context::getString),
        get = { LauncherSettings.string(key) },
        set = { LauncherSettings.set(key, it) }
    )

    // The HUD offsets are four non-negative keys; PrimedGun shows each axis as one
    // signed slider.
    private fun hudAxis(context: Context, titleId: Int, positive: String, negative: String) =
        PrimedGunItem.Slider(
            title = context.getString(titleId),
            min = -1f, max = 1f, step = 0.01f,
            inactive = inactive(positive),
            get = { LauncherSettings.float(positive) - LauncherSettings.float(negative) },
            set = {
                LauncherSettings.setFloat(positive, it.coerceAtLeast(0f))
                LauncherSettings.setFloat(negative, (-it).coerceAtLeast(0f))
            }
        )
}
