// SPDX-License-Identifier: GPL-3.0-or-later
package org.primedgun.v2.launcher

import android.app.AlertDialog
import android.content.Context
import android.view.View
import android.widget.*

/** The same bindings/schema as the desktop editor; edits apply only on Apply. */
object ControllerLayoutDialog {
    fun show(context: Context) {
        data class Binding(val key: String, val initial: Int, val group: String, val label: String)
        val pattern = Regex("""VR_BIND\((\w+), (\d+), "([^"]+)", "([^"]+)"\)""")
        val bindings = context.assets.open("vr_controller_bindings.inc").bufferedReader().useLines { lines ->
            lines.mapNotNull { pattern.matchEntire(it.trim()) }.map {
                Binding("vr_custom_" + it.groupValues[1], it.groupValues[2].toInt(), it.groupValues[3], it.groupValues[4])
            }.toList()
        }
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL; setPadding(24, 12, 24, 12) }
        val enabled = CheckBox(context).apply { text = "Use custom controller layout"; isChecked = true }
        body.addView(enabled)
        body.addView(TextView(context).apply { text = "Overrides standard VR bindings and other remaps. Keep stick jump and assign an additional jump button independently. Beam select is held while swinging the weapon hand. Only bind controls your controllers have." })
        val groups = bindings.groupBy { it.group }
        val sectionPicker = Spinner(context).apply { adapter = ArrayAdapter(context, android.R.layout.simple_spinner_dropdown_item, groups.keys.toList()) }
        body.addView(sectionPicker)
        val pages = mutableMapOf<String, LinearLayout>()
        val rows = mutableMapOf<String, Spinner>()
        val names = mutableListOf("Not assigned")
        for (hand in listOf("Left", "Right")) {
            names.addAll(listOf("Primary (A / X / Square)", "Secondary (B / Y / Triangle)", "Trigger", "Grip / squeeze", "Stick / trackpad click", "Menu / View (if available)", "Touchpad press", "Stick / trackpad up", "Stick / trackpad down", "Stick / trackpad left", "Stick / trackpad right", "Physical D-pad up", "Physical D-pad down", "Physical D-pad left", "Physical D-pad right", "Shoulder (if available)").map { "$hand — $it" })
        }
        names.addAll(listOf("Right — X (Steam Frame)", "Right — Y (Steam Frame)", "Left — extra face button 3", "Left — extra face button 4"))
        fun row(parent: LinearLayout, key: String, label: String, items: List<String>): Spinner {
            parent.addView(TextView(context).apply { text = label; setPadding(0, 12, 0, 0) })
            return Spinner(context).apply {
                adapter = ArrayAdapter(context, android.R.layout.simple_spinner_dropdown_item, items)
                setSelection(LauncherSettings.int(key)); parent.addView(this); rows[key] = this
            }
        }
        for ((name, entries) in groups) {
            val page = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
            pages[name] = page; body.addView(page)
            entries.forEach { row(page, it.key, it.label, names) }
        }
        val move = row(pages.getValue("On foot"), "vr_custom_move_hand", "Movement stick / trackpad", listOf("Left", "Right"))
        val look = row(pages.getValue("On foot"), "vr_custom_look_hand", "Look / turn stick / trackpad", listOf("Left", "Right"))
        row(pages.getValue("Visors"), "vr_custom_visor_hand", "Visor selector stick / trackpad", listOf("Left", "Right"))
        val mode = row(pages.getValue("Visors"), "vr_custom_visor_mode", "Visor selection method", listOf("Controller near head + visor stick", "Hold visor selector button + visor stick", "Physical D-pad / assigned visor buttons only"))
        pages.getValue("Visors").addView(TextView(context).apply { text = "Hold mode suppresses movement, turning and jumping from the visor stick while its button is held. Direct visor buttons work in all modes." })
        sectionPicker.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: View?, position: Int, id: Long) {
                val active = groups.keys.toList()[position]; pages.forEach { (name, page) -> page.visibility = if (name == active) View.VISIBLE else View.GONE }
            }
            override fun onNothingSelected(parent: android.widget.AdapterView<*>?) {}
        }
        fun defaults() {
            bindings.forEach { rows.getValue(it.key).setSelection(it.initial) }
            move.setSelection(0); look.setSelection(1); mode.setSelection(0); rows.getValue("vr_custom_visor_hand").setSelection(0)
            if (!LauncherSettings.bool("vr_use_right_hand")) {
                for (name in listOf("shoot", "lock", "missile", "map", "morph", "pause", "beam", "bomb", "unmorph")) {
                    val spinner = rows.getValue("vr_custom_$name"); val value = spinner.selectedItemPosition
                    spinner.setSelection(if (value <= 16) value + 16 else value - 16)
                }
                rows.getValue("vr_custom_visor_hand").setSelection(1)
            }
        }
        body.addView(Button(context).apply { text = "Load default layout"; setOnClickListener { defaults() } })
        if (!LauncherSettings.bool("vr_custom_layout_enabled")) defaults()
        val dialog = AlertDialog.Builder(context).setTitle("Customize controller layout")
            .setView(ScrollView(context).apply { addView(body) })
            .setPositiveButton("Apply", null).setNegativeButton("Cancel", null).create()
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                if (enabled.isChecked && (move.selectedItemPosition == look.selectedItemPosition ||
                        rows.getValue("vr_custom_menu").selectedItemPosition == 0 || rows.getValue("vr_custom_confirm").selectedItemPosition == 0 ||
                        rows.getValue("vr_custom_back").selectedItemPosition == 0 ||
                        (mode.selectedItemPosition == 1 && rows.getValue("vr_custom_visor_hold").selectedItemPosition == 0))) {
                    Toast.makeText(context, "Use different move/look sticks, assign VR settings, Confirm and Back, and a visor selector for Hold mode.", Toast.LENGTH_LONG).show()
                } else {
                    rows.forEach { (key, spinner) -> LauncherSettings.setInt(key, spinner.selectedItemPosition) }
                    LauncherSettings.setBool("vr_custom_layout_enabled", enabled.isChecked)
                    (context as? LauncherActivity)?.let { activity ->
                        activity.onSettingsEdited()
                        activity.supportFragmentManager.fragments.filterIsInstance<PrimedGunRefreshable>().forEach { it.refresh() }
                    }
                    dialog.dismiss()
                }
            }
        }
        dialog.show()
    }
}
