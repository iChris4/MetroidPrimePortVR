// SPDX-License-Identifier: GPL-3.0-or-later

import AVFAudio
import CompositorServices
import Foundation
import SwiftUI

/// The game's state as the launcher sees it: the disc images (GameStorage), the chosen one, and
/// the game itself through its C bridge (platform/include/visionos/visionos_host.h).
@MainActor
final class GameModel: ObservableObject {
    static let launcherWindowID = "launcher"
    static let immersiveSpaceID = "game"

    enum Phase: Equatable {
        /// Nothing started yet.
        case idle
        /// The immersive space is being opened; the game starts once its layer exists.
        case opening
        case running
        /// The game returned. Its engine keeps process-wide state, so another run needs the app
        /// to be closed and opened again.
        case ended(exitCode: Int32, message: String)
        case failed(message: String)
    }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var discs: [URL] = []
    @Published private(set) var selectedDisc: URL?
    @Published private(set) var selectedCheck: GameStorage.DiscCheck?
    @Published private(set) var importProgress: Double?
    @Published private(set) var lastError = ""
    @Published var immersionStyle: any ImmersionStyle = .mixed

    private var watchdog: Timer?
    private var importTask: Task<Void, Never>?
    /// Set by the launcher as the space opens: its window closes while the game runs (in a full
    /// space it would float inside the game's world) and comes back when the game ends.
    var reopenLauncher: (() -> Void)?
    var closeImmersiveSpace: (() async -> Void)?
    private static let selectedDiscKey = "selectedDisc"
    private static let wantsRoomKey = "showRoom"

    init() {
        // visionOS anchors an app's audio to its window scene by default, so the game would fall
        // silent when the launcher window closes. Its sound belongs to the player: anchor it to
        // the listener's front, which outlives every scene (Wiicompiled VR found this).
        do {
            try AVAudioSession.sharedInstance().setIntendedSpatialExperience(
                .headTracked(soundStageSize: .automatic, anchoringStrategy: .front))
        } catch {
            lastError = "Could not set up the game's audio: \(error.localizedDescription)"
        }
        if let message = GameStorage.prepare() {
            lastError = message
        }
        let wantsRoom = UserDefaults.standard.object(forKey: GameModel.wantsRoomKey) as? Bool ?? true
        immersionStyle = wantsRoom ? .mixed : .full
        refreshDiscs()
    }

    // MARK: Discs

    func refreshDiscs() {
        discs = GameStorage.discImages()
        var chosen: URL?
        if let remembered = UserDefaults.standard.string(forKey: GameModel.selectedDiscKey) {
            let url = GameStorage.url(forRelativePath: remembered)
            if FileManager.default.fileExists(atPath: url.path) {
                chosen = url
            }
        }
        if chosen == nil, discs.count == 1 {
            chosen = discs[0]
        }
        select(chosen)
    }

    func select(_ disc: URL?) {
        selectedDisc = disc
        selectedCheck = disc.map(GameStorage.check)
        if let disc {
            UserDefaults.standard.set(GameStorage.relativePath(of: disc), forKey: GameModel.selectedDiscKey)
        }
    }

    func importDisc(from url: URL) {
        guard importTask == nil else { return }
        lastError = ""
        importProgress = 0
        importTask = Task {
            do {
                let copied = try await GameStorage.importDisc(from: url) { fraction in
                    Task { @MainActor in self.importProgress = fraction }
                }
                refreshDiscs()
                select(copied)
            } catch is CancellationError {
            } catch {
                lastError = "Could not copy \(url.lastPathComponent): \(error.localizedDescription)"
            }
            importProgress = nil
            importTask = nil
        }
    }

    func cancelImport() {
        importTask?.cancel()
    }

    func deleteDisc(_ disc: URL) {
        do {
            try FileManager.default.removeItem(at: disc)
        } catch {
            lastError = "Could not delete \(disc.lastPathComponent): \(error.localizedDescription)"
        }
        if disc == selectedDisc {
            UserDefaults.standard.removeObject(forKey: GameModel.selectedDiscKey)
        }
        refreshDiscs()
    }

    var importing: Bool { importTask != nil }

    var canStart: Bool {
        guard case .idle = phase, selectedDisc != nil, !importing else { return false }
        return selectedCheck?.playable ?? false
    }

    // MARK: The game

    func markOpening() {
        if case .idle = phase {
            phase = .opening
        }
    }

    func openingFailed(_ message: String) {
        phase = .failed(message: message)
    }

    /// Called from the CompositorLayer closure once the immersive space has a layer renderer. The
    /// provider keeps the renderer; the game creates its OpenXR session against it.
    nonisolated func attach(_ layerRenderer: LayerRenderer) {
        // An OS object: the pointer the provider bridges back to cp_layer_renderer_t and retains.
        mp_visionos_set_layer_renderer(Unmanaged.passUnretained(layerRenderer).toOpaque())
        // visionOS's look-and-pinch selection. Apps never see the gaze itself, but a pinch carries
        // the ray from the eyes to where the wearer looked as it began, which the provider turns
        // into the menu pointer and its press when no controller is in hand.
        nonisolated(unsafe) let renderer = layerRenderer
        Task { @MainActor in
            renderer.onSpatialEvent = { events in
                for event in events {
                    Self.forward(event)
                }
            }
            self.startGame()
        }
    }

    private nonisolated static func forward(_ event: SpatialEventCollection.Event) {
        let phase: Int32
        switch event.phase {
        case .active: phase = 0
        case .ended: phase = 1
        case .cancelled: phase = 2
        @unknown default: phase = 2
        }
        let chirality: Int32
        switch event.chirality {
        case .left?: chirality = 1
        case .right?: chirality = 2
        default: chirality = 0
        }
        // Stable per pinch: the same event id arrives for every phase of one gesture.
        let id = UInt64(bitPattern: Int64(event.id.hashValue))
        let ray = event.selectionRay
        let pose = event.inputDevicePose?.pose3D.position
        mp_visionos_spatial_event(id, phase, chirality, ray != nil,
                                  Float(ray?.origin.x ?? 0), Float(ray?.origin.y ?? 0), Float(ray?.origin.z ?? 0),
                                  Float(ray?.direction.x ?? 0), Float(ray?.direction.y ?? 0),
                                  Float(ray?.direction.z ?? -1), pose != nil,
                                  Float(pose?.x ?? 0), Float(pose?.y ?? 0), Float(pose?.z ?? 0))
    }

    private func startGame() {
        guard phase == .opening || phase == .idle, let disc = selectedDisc else { return }
        GameStorage.documents.path.withCString { user in
            GameStorage.caches.path.withCString { cache in
                mp_visionos_set_directories(user, cache)
            }
        }
        let started = disc.path.withCString { mp_visionos_start_game($0) }
        if started {
            phase = .running
            startWatchdog()
            // `--autoquit` (with --autoplay) quits after a minute: an unattended simulator run of the
            // whole path, quit included.
            if CommandLine.arguments.contains("--autoquit") {
                Task {
                    try? await Task.sleep(for: .seconds(60))
                    self.requestQuit()
                }
            }
        } else {
            phase = .failed(message: String(cString: mp_visionos_last_error()))
        }
    }

    /// The game runs on its own thread: the launcher polls the bridge for its end, and for a
    /// dismissed immersive space, after which the game would carry on unseen.
    private func startWatchdog() {
        watchdog?.invalidate()
        watchdog = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.poll()
            }
        }
    }

    private func poll() {
        if !mp_visionos_game_running() {
            watchdog?.invalidate()
            watchdog = nil
            phase = .ended(exitCode: mp_visionos_exit_code(), message: String(cString: mp_visionos_last_error()))
            Task {
                await closeImmersiveSpace?()
                reopenLauncher?()
            }
            return
        }
        if mp_visionos_layer_invalidated() {
            mp_visionos_request_quit()
        }
    }

    func requestQuit() {
        mp_visionos_request_quit()
    }

    // MARK: Settings

    /// Whether the immersive space opens in the room (mixed) or fully immersive, remembered across
    /// launches. The game's passthrough setting follows it: the room shows around the menu screen
    /// only in a mixed space.
    var wantsRoom: Bool {
        get { immersionStyle is MixedImmersionStyle }
        set {
            objectWillChange.send()
            immersionStyle = newValue ? .mixed : .full
            UserDefaults.standard.set(newValue, forKey: GameModel.wantsRoomKey)
            _ = setSetting("vr_passthrough", newValue ? "1" : "0")
        }
    }

    func setting(_ key: String) -> String {
        let size = 256
        var buffer = [CChar](repeating: 0, count: size)
        let found = GameStorage.settingsFile.path.withCString { path in
            key.withCString { mp_launcher_setting_get(path, $0, &buffer, size) }
        }
        return found ? buffer.withUnsafeBufferPointer { String(cString: $0.baseAddress!) } : ""
    }

    @discardableResult
    func setSetting(_ key: String, _ value: String) -> Bool {
        let saved = GameStorage.settingsFile.path.withCString { path in
            key.withCString { k in value.withCString { mp_launcher_setting_set(path, k, $0) } }
        }
        if !saved {
            lastError = "Could not save \(key): \(String(cString: mp_launcher_last_error()))"
        }
        return saved
    }
}
