// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI
import UniformTypeIdentifiers

/// The window the app opens with: the Play tab (the disc images, which one to play, and the
/// button that opens the immersive space), Settings, Controls and About.
struct LauncherView: View {
    @EnvironmentObject private var model: GameModel

    var body: some View {
        TabView {
            PlayView()
                .tabItem { Label("Play", systemImage: "play.fill") }
            NavigationStack {
                SettingsView()
                    .navigationTitle("Settings")
            }
            .tabItem { Label("Settings", systemImage: "gearshape") }
            ControlsView()
                .tabItem { Label("Controls", systemImage: "gamecontroller") }
            AboutView()
                .tabItem { Label("About", systemImage: "info.circle") }
        }
    }
}

private struct PlayView: View {
    @EnvironmentObject private var model: GameModel
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismissWindow) private var dismissWindow
    @State private var picking = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                Text("\(GameInfo.title) for Apple Vision Pro")
                    .font(.largeTitle.bold())
                Text("\(GameInfo.gameName) in VR, native on the headset.")
                    .font(.title3)
                    .foregroundStyle(.secondary)

                discBox
                statusView

                HStack(spacing: 16) {
                    Button {
                        Task { await start() }
                    } label: {
                        Label("Play", systemImage: "play.fill")
                            .frame(minWidth: 140)
                    }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.large)
                    .disabled(!model.canStart)

                    if model.phase == .running {
                        Button("Quit game", role: .destructive) {
                            model.requestQuit()
                            Task { await dismissImmersiveSpace() }
                        }
                        .buttonStyle(.bordered)
                    }
                }
                Text(GameInfo.firstSteps)
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            .padding(28)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .fileImporter(isPresented: $picking, allowedContentTypes: [.item, .data, .diskImage]) { result in
            switch result {
            case .success(let url):
                model.importDisc(from: url)
            case .failure(let error):
                model.openingFailed(error.localizedDescription)
            }
        }
        .onAppear {
            model.refreshDiscs()
            // `xcrun simctl launch ... --autoplay` presses Play for an unattended run on the
            // simulator, where nothing can pinch the button.
            if CommandLine.arguments.contains("--autoplay"), model.canStart {
                Task { await start() }
            }
        }
    }

    private var discBox: some View {
        GroupBox("Disc image") {
            VStack(alignment: .leading, spacing: 12) {
                Text(GameInfo.discDescription)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                if model.discs.isEmpty {
                    Label("No disc image on the headset yet.", systemImage: "opticaldisc")
                        .foregroundStyle(.orange)
                } else {
                    ForEach(model.discs, id: \.self) { disc in
                        DiscRow(disc: disc, selected: disc == model.selectedDisc)
                    }
                }
                if let check = model.selectedCheck {
                    Label(check.gameID.isEmpty ? check.message : "\(check.gameID): \(check.message)",
                          systemImage: check.playable ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                        .foregroundStyle(check.playable ? .green : .orange)
                }
                if let progress = model.importProgress {
                    HStack {
                        ProgressView(value: progress) {
                            Text("Copying the disc image to the headset… \(Int(progress * 100))%")
                        }
                        Button("Cancel") { model.cancelImport() }
                    }
                }
                HStack {
                    Button {
                        picking = true
                    } label: {
                        Label("Import a disc image…", systemImage: "square.and.arrow.down")
                    }
                    .disabled(model.importing || model.phase != .idle)
                    Button("Look again") { model.refreshDiscs() }
                }
                .buttonStyle(.bordered)
                Text("Import copies the image into the app (Files: On My Apple Vision Pro > \(GameInfo.title) > Discs), so the original can be deleted. Images copied there with Finder's file sharing show up here too.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    @ViewBuilder
    private var statusView: some View {
        switch model.phase {
        case .idle:
            if !model.lastError.isEmpty {
                Label(model.lastError, systemImage: "xmark.octagon.fill").foregroundStyle(.red)
            }
        case .opening:
            Label("Opening the immersive space…", systemImage: "hourglass")
        case .running:
            Label("The game is running in the immersive space.", systemImage: "visionpro")
        case .ended(let code, let message):
            VStack(alignment: .leading, spacing: 6) {
                Label(code == 0 ? "The game ended." : "The game ended with code \(code).",
                      systemImage: code == 0 ? "checkmark.circle" : "exclamationmark.triangle")
                if !message.isEmpty {
                    Text(message).foregroundStyle(.red)
                }
                Text("The game can run once per start of the app.")
                    .foregroundStyle(.secondary)
                Button("Close \(GameInfo.title)") { exit(0) }
                    .buttonStyle(.bordered)
            }
        case .failed(let message):
            Label(message.isEmpty ? "The game could not start." : message, systemImage: "xmark.octagon.fill")
                .foregroundStyle(.red)
        }
    }

    private func start() async {
        model.markOpening()
        switch await openImmersiveSpace(id: GameModel.immersiveSpaceID) {
        case .opened:
            // The game starts when the space's CompositorLayer hands over its renderer
            // (GameModel.attach). The launcher steps aside until it ends; the Digital Crown
            // closes the space, and the game with it.
            let open = openWindow
            let dismiss = dismissImmersiveSpace
            model.reopenLauncher = { open(id: GameModel.launcherWindowID) }
            model.closeImmersiveSpace = { await dismiss() }
            dismissWindow(id: GameModel.launcherWindowID)
        case .userCancelled:
            model.openingFailed("The immersive space was not opened.")
        case .error:
            model.openingFailed("visionOS could not open the immersive space.")
        @unknown default:
            model.openingFailed("visionOS could not open the immersive space.")
        }
    }
}

private struct DiscRow: View {
    @EnvironmentObject private var model: GameModel
    let disc: URL
    let selected: Bool
    @State private var confirmDelete = false

    var body: some View {
        HStack {
            Button {
                model.select(disc)
            } label: {
                HStack {
                    Image(systemName: selected ? "largecircle.fill.circle" : "circle")
                    VStack(alignment: .leading) {
                        Text(disc.lastPathComponent)
                        Text(GameStorage.relativePath(of: disc))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                    Text(size)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .buttonStyle(.plain)
            .disabled(model.phase != .idle)
            Button(role: .destructive) {
                confirmDelete = true
            } label: {
                Image(systemName: "trash")
            }
            .buttonStyle(.borderless)
            .disabled(model.phase != .idle)
            .confirmationDialog("Delete \(disc.lastPathComponent) from the headset?", isPresented: $confirmDelete) {
                Button("Delete", role: .destructive) { model.deleteDisc(disc) }
            }
        }
        .padding(10)
        .background(selected ? Color.accentColor.opacity(0.18) : Color.clear,
                    in: RoundedRectangle(cornerRadius: 12))
    }

    private var size: String {
        let bytes = (try? disc.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0
        return ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)
    }
}

private struct ControlsView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                GroupBox("PlayStation VR2 Sense controllers") {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Pair them in Settings > General > Game Controllers before starting the game, and allow accessory tracking when the game asks. They play PrimedGun's full VR controls, as Touch controllers do on Quest: the arm cannon follows the controller in your gun hand.")
                        Text("Right controller: trigger fires, Cross and Circle are the right face buttons, the stick click sets your height. Left controller: Square and Triangle are the left face buttons, the stick moves, the stick click opens the VR menu, Create is the menu button. The grips are the grip buttons.")
                        Text("The in-game VR menu and the PC launcher's Layout tab show and change the full layout; the settings are shared with this app.")
                    }
                    .font(.callout)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                GroupBox("DualSense or another gamepad") {
                    Text("A Bluetooth gamepad plays the original controls: the headset still looks around, the stick turns and moves Samus. Pair it in Settings > General > Game Controllers.")
                        .font(.callout)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                GroupBox("Hands") {
                    Text("With nothing in hand, look at a menu item and pinch to press it. Playing needs controllers or a gamepad.")
                        .font(.callout)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .padding(28)
        }
    }
}

private struct AboutView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                Text(GameInfo.title).font(.largeTitle.bold())
                Text(GameInfo.credits)
                Text("No game content is included: the game reads your own disc image and ships none of its assets. \(GameInfo.gameName) is a trademark of Nintendo; this project is unaffiliated with Nintendo and Retro Studios.")
                Text("Your files: Files app > On My Apple Vision Pro > \(GameInfo.title). The settings, memory card saves and the log (the .log file) are there.")
                    .foregroundStyle(.secondary)
            }
            .font(.callout)
            .padding(28)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}
