// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// What makes this app Metroid Prime rather than Echoes. The two PrimedGun apps share every
/// other Swift file of visionos/App; only this one differs between the repositories.
enum GameInfo {
    static let title = "PrimedGun"
    static let gameName = "Metroid Prime"
    /// The disc the game plays, as the player is told.
    static let discDescription =
        "Your own Metroid Prime GameCube disc image: USA 1.00 (GM8E01), or PAL (GM8P01, experimental)."
    /// Shown under the Play button.
    static let firstSteps =
        "Once in the game, click the right stick to set your height and the left stick to open or close the VR menu. Press the Digital Crown to leave the game."
}
