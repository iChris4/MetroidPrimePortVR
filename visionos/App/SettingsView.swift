// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// A few of the game's VR settings, edited in its settings file line by line through the PrimedGun
/// launcher core (the PC launcher and the Quest panel use the same code), with the defaults and
/// ranges it knows. Everything else is in the VR menu inside the game (left stick click), which
/// writes the same file; a change made here while the game runs reaches it within a second.
struct SettingsView: View {
    @EnvironmentObject private var model: GameModel

    var body: some View {
        Form {
            Section {
                Toggle("Show my room around the menu screen", isOn: Binding(
                    get: { model.wantsRoom },
                    set: { model.wantsRoom = $0 }))
                    .disabled(model.phase != .idle)
                SettingRow(key: "vr_screen_distance_meters", title: "Menu screen distance", unit: "m")
            } header: {
                Text("Headset")
            } footer: {
                Text("The room shows around the menus, the map and cutscenes; gameplay is always fully immersive. Takes effect the next time the game starts.")
            }
            Section("Rendering") {
                SettingRow(key: "vr_render_scale", title: "Render scale", unit: "×")
                SettingRow(key: "vr_world_scale", title: "World scale", unit: "")
            }
            Section("Controls") {
                SettingRow(key: "vr_use_right_hand", title: "Arm cannon in the right hand")
                SettingRow(key: "vr_directional_movement_enabled", title: "Move where the controller points")
                SettingRow(key: "vr_snap_turn_enabled", title: "Snap turning")
                SettingRow(key: "vr_snap_turn_degrees", title: "Snap turn angle", unit: "°")
                SettingRow(key: "vr_rumble_enabled", title: "Rumble")
                SettingRow(key: "vr_rumble_intensity", title: "Rumble strength", unit: "")
            }
            Section {
                SettingRow(key: "vr_diagnostics_logging", title: "Log VR frame timing")
            } header: {
                Text("Diagnostics")
            } footer: {
                Text("Written to the game's log, in the Files app with the saves.")
            }
            if !model.lastError.isEmpty {
                Section {
                    Text(model.lastError).foregroundStyle(.red)
                }
            }
        }
    }
}

/// One key of the settings file, as a toggle, a slider or a picker depending on its kind.
private struct SettingRow: View {
    @EnvironmentObject private var model: GameModel
    let key: String
    let title: String
    var unit: String = ""

    @State private var info = MpLauncherKeyInfo()
    @State private var known = false
    @State private var value = ""

    var body: some View {
        Group {
            if !known {
                LabeledContent(title, value: "not available")
            } else {
                switch Int(info.kind) {
                case Int(MP_LAUNCHER_KEY_BOOL):
                    Toggle(title, isOn: Binding(
                        get: { value == "1" || value.lowercased() == "true" },
                        set: { store($0 ? "1" : "0") }))
                case Int(MP_LAUNCHER_KEY_CHOICE):
                    Picker(title, selection: Binding(get: { value }, set: { store($0) })) {
                        ForEach(choices, id: \.self) { Text($0).tag($0) }
                    }
                default:
                    VStack(alignment: .leading) {
                        LabeledContent(title, value: "\(formatted)\(unit)")
                        Slider(value: Binding(
                                   get: { Double(value) ?? Double(info.minimum) },
                                   set: { store(format($0)) }),
                               in: Double(info.minimum)...Double(max(info.maximum, info.minimum + 0.01)),
                               step: Double(info.step > 0 ? info.step : 0.01))
                    }
                }
            }
        }
        .onAppear(perform: load)
    }

    private var choices: [String] {
        guard let raw = info.choices else { return [] }
        return String(cString: raw).split(separator: ",").map(String.init)
    }

    private var formatted: String {
        guard let number = Double(value) else { return value }
        return Int(info.kind) == Int(MP_LAUNCHER_KEY_INT) ? String(Int(number.rounded())) : String(format: "%.2f", number)
    }

    private func format(_ number: Double) -> String {
        Int(info.kind) == Int(MP_LAUNCHER_KEY_INT) ? String(Int(number.rounded())) : String(format: "%.2f", number)
    }

    private func load() {
        var details = MpLauncherKeyInfo()
        known = key.withCString { mp_launcher_key_info($0, &details) }
        info = details
        value = model.setting(key)
        if value.isEmpty, let fallback = details.default_value {
            value = String(cString: fallback)
        }
    }

    private func store(_ newValue: String) {
        value = newValue
        model.setSetting(key, newValue)
    }
}
