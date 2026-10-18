# Native port 0.29 integration for PrimedGun VR

This branch merges Odrannnn/MetroidPrimePort `port` at
`2dca62bb3b90484e940a31757dd0273a99f27178` into the OpenXR tree at
`28ea69d4fc73063f0cd11eb628cb8a14fb2be51b`.

The upstream update brings moving-platform collision fixes, beam-switch and
save-state fixes, persistent beam-door unlocks, particle/HDR corrections,
Archipelago item-ID fixes, HUD opacity controls, and an optional Remastered
crosshair, along with the upstream code and build refactoring.

## VR integration decisions

- Preserve the fork's README, launchers, Quest package, signing configuration,
  and VR defaults. This merge does not bump the PrimedGun release version.
- Retain all VR test targets using upstream's new `mp_port_test` helper.
- Move the fork's settings read/write hooks, atomic settings writes, live
  launcher polling, option-save debounce, VR page, and shader-progress display
  into the refactored settings UI's corresponding translation units.
- Preserve headset frame pacing and vsync behavior, the Original Experience
  comfort exceptions, hidden helmet and non-widescreen HUD defaults, and the
  Quest storage-clamp default.
- Keep desktop mouse/twin-stick aim from taking over the PrimedGun VR pad.
- Preserve cannon targeting, tracked-weapon FOV bypass, model mirroring,
  roomscale stepping, lock/grapple camera behavior, and Morph Ball transitions.
  The optional Remastered crosshair uses the same cannon-targeting orientation.
- Retain case-insensitive memory-card imports while adopting upstream's safe
  directory enumeration. Keep the cannon texture slot when resetting settings.
- Restore the audio getter used by the headset menu. Update the renderer test
  stubs to cover both stereo uniform reuse and upstream's HDR blend dependency.
- Keep native-port update checks disabled so they cannot redirect VR players to
  the flat port's releases.

The settings self-test exercises representative VR options through the same
reader/writer as the new native settings table. Automated checks cannot replace
in-headset testing of visuals, movement, and camera comfort before release.
## Validation

- Windows x64 game and Qt launcher built with OpenXR enabled.
- Quest ARM64 release APK built with the existing patched Dawn package; its
  signer matches iChris4's certificate.
- All 130 native-port tests passed, including the VR suite and settings round-trip.
- All 482 Aurora renderer tests passed.
- The map-side-file test skips its symlink assertion only when Windows refuses
  symlink creation; its other identity, file and persistence checks still run.
- Fixed renderer test isolation: unique I/O fixture folders for parallel CTest
  processes, and explicit device-limit initialization for standalone HDR copy tests.
- In-headset play testing remains required before a release. No release or
  runtime installation is replaced by this integration branch.
