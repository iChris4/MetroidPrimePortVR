// SPDX-License-Identifier: GPL-3.0-or-later

import CompositorServices
import SwiftUI

/// The app: a launcher window (the disc, Play, Settings) and the immersive space the game renders
/// into. The game is a framework the app embeds and links (visionos/CMakeLists.txt), driven through
/// GameModel. Ported from Wiicompiled VR's visionOS app.
@main
struct PrimedGunVisionApp: App {
    @StateObject private var model = GameModel()

    var body: some Scene {
        WindowGroup(id: GameModel.launcherWindowID) {
            LauncherView()
                .environmentObject(model)
        }
        .windowResizability(.contentMinSize)
        .defaultSize(width: 860, height: 760)

        ImmersiveSpace(id: GameModel.immersiveSpaceID) {
            ImmersiveGame.layer(for: model)
        }
        // Mixed immersion shows the room around the virtual screen (the menus, the map, cutscenes)
        // when the game's passthrough setting asks for it: the provider clears the frame
        // transparent there. Full immersion is the headset with nothing else. Settings offers it.
        .immersionStyle(selection: $model.immersionStyle, in: .mixed, .full)
        .upperLimbVisibility(.visible)
    }
}
