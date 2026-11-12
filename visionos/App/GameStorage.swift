// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// Where the game's files live on the headset, and the disc images among them.
///
/// The app's Documents folder is the game's user folder (settings, memory card saves, mods, the
/// log) and is shown by the Files app (On My Apple Vision Pro > PrimedGun) and by Finder's file
/// sharing. Disc images are copied into its Discs folder by the launcher's import, or dropped
/// anywhere in Documents by hand. The shader caches stay in Library/Caches, out of sight.
enum GameStorage {
    static var documents: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    static var caches: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
    }

    static var discsFolder: URL { documents.appendingPathComponent("Discs", isDirectory: true) }

    /// The game's settings file (the PrimedGun launcher core edits it line by line).
    static var settingsFile: URL {
        documents.appendingPathComponent(String(cString: mp_visionos_settings_file_name()))
    }

    /// The extensions the port opens (launcher/core/disc_probe.cpp).
    static var discExtensions: Set<String> { GameInfo.discExtensions }

    static func prepare() -> String? {
        do {
            try FileManager.default.createDirectory(at: discsFolder, withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: caches, withIntermediateDirectories: true)
            return nil
        } catch {
            return "Could not create the game's folders: \(error.localizedDescription)"
        }
    }

    /// Every disc image in Documents and its subfolders, by name.
    static func discImages() -> [URL] {
        guard let walker = FileManager.default.enumerator(at: documents,
                                                          includingPropertiesForKeys: [.isRegularFileKey],
                                                          options: [.skipsHiddenFiles]) else {
            return []
        }
        var found: [URL] = []
        for case let url as URL in walker where discExtensions.contains(url.pathExtension.lowercased()) {
            if (try? url.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile == true {
                found.append(url)
            }
        }
        return found.sorted { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
    }

    /// A disc path kept relative to Documents: the app's container moves when it is reinstalled.
    static func relativePath(of url: URL) -> String {
        let base = documents.standardizedFileURL.path
        let path = url.standardizedFileURL.path
        if path.hasPrefix(base + "/") {
            return String(path.dropFirst(base.count + 1))
        }
        return path
    }

    static func url(forRelativePath path: String) -> URL {
        path.hasPrefix("/") ? URL(fileURLWithPath: path) : documents.appendingPathComponent(path)
    }

    struct DiscCheck {
        let code: Int32
        let gameID: String
        let revision: Int32
        var playable: Bool { code == Int32(MP_DISC_OK) || code == Int32(MP_DISC_UNVERIFIED) }
        var message: String { String(cString: mp_launcher_disc_check_message(code)) }
    }

    static func check(_ url: URL) -> DiscCheck {
        let size = 16
        var id = [CChar](repeating: 0, count: size)
        var revision: Int32 = -1
        let code = url.path.withCString { path in
            mp_launcher_probe_disc(path, &id, size, &revision)
        }
        let gameID = id.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
        return DiscCheck(code: code, gameID: gameID, revision: revision)
    }

    /// Copies a picked file into Discs, reporting the fraction done. The picker hands a file the
    /// app may only read while it holds the security scope, so the copy is the app's own.
    static func importDisc(from source: URL, progress: @escaping @Sendable (Double) -> Void) async throws -> URL {
        try await Task.detached(priority: .userInitiated) {
            let scoped = source.startAccessingSecurityScopedResource()
            defer { if scoped { source.stopAccessingSecurityScopedResource() } }
            try FileManager.default.createDirectory(at: discsFolder, withIntermediateDirectories: true)
            let target = discsFolder.appendingPathComponent(source.lastPathComponent)
            if source.standardizedFileURL == target.standardizedFileURL {
                return target
            }
            let partial = target.appendingPathExtension("part")
            try? FileManager.default.removeItem(at: partial)
            FileManager.default.createFile(atPath: partial.path, contents: nil)
            let input = try FileHandle(forReadingFrom: source)
            defer { try? input.close() }
            let output = try FileHandle(forWritingTo: partial)
            defer { try? output.close() }
            let total = Double((try? source.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0)
            var copied = 0.0
            while true {
                try Task.checkCancellation()
                let chunk = try input.read(upToCount: 8 << 20) ?? Data()
                if chunk.isEmpty { break }
                try output.write(contentsOf: chunk)
                copied += Double(chunk.count)
                if total > 0 { progress(min(copied / total, 1)) }
            }
            try output.synchronize()
            try? FileManager.default.removeItem(at: target)
            try FileManager.default.moveItem(at: partial, to: target)
            return target
        }.value
    }
}
