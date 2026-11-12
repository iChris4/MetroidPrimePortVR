# Apple Vision Pro

PrimedGun runs natively on Apple Vision Pro. The game is the same build as on PC and Quest, with
Aurora rendering through Dawn on Metal. A SwiftUI app launches it and an OpenXR provider built on
CompositorServices and ARKit drives the headset. The visionOS layer comes from Wiicompiled VR
(Mario Kart Wii, branch `vision-pro`).

## Status

| Part | State |
|---|---|
| Build for the device and the simulator | Works. |
| Signing, installing on the headset, copying a disc image to it | Works (`--install`, `devicectl device copy to`). |
| Game start on the headset | Works: the disc mounts, the session reaches FOCUSED, the first screens run at the headset's 90 fps, and a quit is clean (read from the log of an unattended run). |
| Launcher, disc import, disc check | Works in the simulator. |
| Game start, OpenXR session, title screen on the virtual screen | Works in the simulator (head tracking only). |
| Quit and return to the launcher | Works in the simulator. |
| Stereo gameplay, frame pacing, comfort | Not yet checked on a headset. |
| PlayStation VR2 Sense controllers | Played on a headset (the Frigate and beyond); calibration not checked. |
| DualSense and other gamepads | Played on a headset. Look to lock-on and the lock's aim (below) work there; R's free aim is not yet checked. |

The simulator has no hand tracking and no accessory tracking, so everything about hands and
controllers is checked on a headset only.

## Requirements

- Apple Vision Pro with **visionOS 26** or later. The Sense controllers and ARKit's accessory
  tracking start there.
- Your own Metroid Prime GameCube disc image: USA 1.00 (`GM8E01`), or PAL (`GM8P01`,
  experimental), as ISO, NKit ISO, GCM, RVZ, CISO or WBFS.
- To build: a Mac with Apple silicon and Xcode with the visionOS platform, CMake 3.28 or later,
  Ninja, Python 3, git, and Rust (below).

## Building and installing

The disc reader (nod) is written in Rust, and visionOS is a Rust target without a prebuilt
standard library. A nightly toolchain with its sources builds one. Install it once:

```sh
curl https://sh.rustup.rs -sSf | sh -s -- -y --default-toolchain nightly --profile minimal
rustup component add rust-src --toolchain nightly
```

Then build, sign with your team and install on the paired headset:

```sh
visionos/Build-VisionOS.sh --team <TEAMID> --install
```

- The team ID is in Xcode > Settings > Accounts. Without `--team`, the script uses the only team
  Xcode knows.
- `--install` waits about two minutes for the headset; put it on and unlock it.
- Pair the headset first in Xcode > Window > Devices and Simulators.
- `--simulator` builds for the visionOS simulator instead, signed ad hoc; with `--install` it
  installs on the booted simulator.
- `--bundle-id` changes the app's identifier (default `org.primedgun.vision`).
- `--open` generates the Xcode project and opens it instead of building.

The script runs three stages, each kept when up to date:

1. `tools/Build-VisionOSDawn.sh` builds the Dawn revision Aurora pins (`Odrannnn/dawn`) for the
   SDK, Metal only, into `build/visionos-dawn`. The first build takes a few minutes.
2. The game is cross-compiled with Ninja as `MetroidPrimeGame.framework`, in `build/visionos` or
   `build/visionos-sim`.
3. `visionos/CMakeLists.txt` makes the Xcode project of the SwiftUI app, which links and embeds
   that framework: `build/visionos-app/Release-xros/PrimedGunVision.app`.

## Playing

1. Open **PrimedGun** on the headset.
2. Under **Disc image**, choose **Import a disc image** and pick the image in Files, iCloud
   Drive or a USB drive. It is copied into the app, so the original can be deleted. An image
   copied into the app's folder by Finder's file sharing also appears in the list.
3. Pick the image; the launcher shows whether it is a disc the game plays.
4. Press **Play**. The launcher window steps aside and the game opens in an immersive space.
5. Click the right stick to set your height. Click the left stick to open or close the VR menu.
6. Press the Digital Crown to leave. The launcher comes back; the game runs once per start of
   the app.

### Controllers

- **PlayStation VR2 Sense controllers.** Pair them in Settings > General > Game Controllers
  before starting, and allow accessory tracking when asked. They play PrimedGun's full VR
  controls, as Touch controllers do on Quest: the arm cannon follows the controller in the gun
  hand. Cross and Circle are A and B; Square and Triangle are X and Y; Create is the menu
  button.
- **DualSense or another gamepad.** It plays the original controls; the headset still looks
  around. The view stays level, so the head picks what L locks on to (look to lock-on and look to
  grapple, on by default), and while L holds a target the arm cannon turns to it, as the TV
  camera did, so the shots go to it. Holding R aims the cannon with the stick (free aim), up and
  down as on the TV, for missiles and anything that cannot be locked; the view does not move, and
  the crosshair marks where the cannon points.
- **Hands.** With nothing in hand, look at a menu item and pinch to press it. Playing needs
  controllers or a gamepad. Hands holding a gamepad are not controllers to the game: they never
  move the cannon or press its buttons.

### Settings and files

- The launcher's **Settings** tab edits a few VR settings in the game's `port_settings.ini`.
  The VR menu inside the game (left stick click) has all of them and writes the same file.
- **Show my room around the menu screen** opens the space in mixed immersion, so the room shows
  around the menus, the map and cutscenes. Gameplay is always fully immersive.
- Everything the game writes is in Files > On My Apple Vision Pro > PrimedGun:
  - `port_settings.ini`: the settings;
  - `USA/Card A/`: the memory card (`EUR/` for PAL);
  - `metroid_prime_port.log`: the log, `.old.log` for the previous run;
  - `Discs/`: the imported disc images;
  - `mods/`: mods and texture packs.
- The shader caches stay in the app's Library folder, out of sight.

## How it works

| Piece | Where | What it does |
|---|---|---|
| OpenXR provider | `platform/vr/visionos/` | The `xr*` functions the VR layer calls, on CompositorServices (frames, the compositor) and ARKit (head, hands). Linked in place of the Khronos loader as `mp_openxr_visionos`. |
| Spatial controllers | `platform/vr/visionos/xr_visionos_accessories.mm` | The Sense controllers: buttons from GameController, poses from ARKit's accessory tracking in an ARKit session of its own, rumble through Core Haptics. They answer the Touch profile. |
| Metal backend | `platform/vr/openxr_metal.mm` | The VR layer's graphics backend: IOSurface swapchains, the release and acquire fences, the virtual screen and settings panel layers. No pipelined pacing. |
| Aurora bridge | `extern/aurora/lib/webgpu/metal_interop.cpp` | Aurora's stereo sink: each eye copied into an IOSurface on Dawn's queue, ordered with MTLSharedEvents. |
| Host glue | `platform/visionos/visionos_host.mm` | Runs the game's `aurora_main` on a thread of its own once the immersive space exists. Sets its folders and switches, and asks it to quit. |
| Launcher bridge | `platform/visionos/launcher_bridge.cpp` | The launcher core's disc check and line-by-line settings edits, in C for Swift. |
| App | `visionos/App/` | The launcher window and the immersive space whose `CompositorLayer` the provider drives. |

Other platform details:

- SDL runs on its offscreen video driver and Dawn gets a CAMetalLayer of its own: no UIKit window
  exists, and nothing ever shows the desktop picture.
- SDL defines `SDL_PLATFORM_IOS` on visionOS, so the iPhone code paths of `platform/main.cpp`
  (Documents as the user folder, memory caps) apply. CMake sets no `IOS` there: the build uses
  `MP_VISIONOS`, and the code `MP_PLATFORM_VISIONOS` or `TARGET_OS_VISION`.
- Aurora does not link its SDL `main` on visionOS: SwiftUI owns the process.

## Fixed on the first headset run

- **Crash at Play.** This Dawn reports BC texture compression on iOS-family GPUs that have it, the
  headset's included, but mapped the BC formats to Metal only on macOS, so the first BC texture
  (the game's compressed textures) aborted in `MetalPixelFormat`. The simulator's GPU reports no
  BC, which hid it. `tools/Build-VisionOSDawn.sh` patches the mapping for the iOS family.
- **Empty log.** The log file was filled by a forked copier process; the iOS family forbids
  `fork()`, so only its header was written. There the copier is a thread.
- **The room through the game.** The provider drew the eye images with their alpha, which keeps the
  GameCube framebuffer's (Metroid Prime's effects leave it below 1), and a mixed-immersion drawable
  takes alpha as coverage: the room showed through immersive play at about half opacity. Layers
  without OpenXR's alpha-blend flag (the game, the virtual screen) are now written opaque; the VR
  menu keeps its transparency. Mario Kart's alpha stays at 1, so Wiicompiled never showed it.
- **Crash on a pause.** When the layer paused between a frame's start and its drawable (the Digital
  Crown, the headset taken off), the provider still ended the frame's submission, which
  CompositorServices aborts on ("BUG IN CLIENT"). Such a frame is now dropped.

## Fixed after the first play sessions

- **No lock-on aim with a gamepad.** The cannon follows the camera when no controller tracks it,
  and the headset's camera stays level and on the heading a lock started from, so the cannon
  pointed past a locked target, further than the game's aim assist turns a shot, and a target
  up high (the Frigate's first room) could not be hit. While L holds an object, the cannon now
  turns about the camera toward it, at up to 540 degrees a second, as the TV camera turned to the
  target, and the assist finishes the aim (`PortVr::VrPadAimTurn`, `platform/vr/vr_view.cpp`).
  This applies to every headset, whenever no controller tracks the cannon.
- **No free aim with a gamepad.** R's free look pitched the camera, which the headset keeps level,
  and the cannon follows the camera, so it never tilted. The same turn (`PortVr::VrPadAimTurn`)
  now tilts the cannon by the free look's angle while the view stays level (the headset ignores
  free look in `CFirstPersonCamera`); a lock still comes first, as on the TV. The crosshair sits
  on the cannon's ray (`VrPadAimRay`, `VrCannonTargetPoint`).
- **Look to lock-on did nothing with a gamepad.** With cannon targeting on (the default), the
  cannon's ray picked the lock target and nothing did without a tracked cannon. The head's gaze
  now picks then, by the look to lock-on and look to grapple settings
  (`platform/vr/vr_look_scan.cpp`).
- **Bare hands as controllers.** The provider answers the action bindings from bare hands too
  (pinches, the look-and-pinch ray), for the VR menu. The game read hands holding a gamepad as
  Touch controllers: a pinch would have aimed the cannon along a fixed ray, and a curled hand
  could press grip buttons. The game's controller snapshot now counts a hand as connected only
  while a Sense controller is in it (`xr_visionos_hand_has_controller`); the VR menu still reads
  the hands.

## Known gaps

- No foveated rendering. The provider draws full-view quads of the eye images.
- Reprojection is planar. Projection layers are composited at a fixed depth (3 m), as in
  Wiicompiled.
- The game runs once per start of the app; its engine keeps process-wide state.
- The Sense controllers' aim and grip frames are ARKit's `aim` and `grip` locations, used as
  OpenXR's. If the cannon points off, the VR menu's calibration corrects it until the axes are
  checked on a headset.
- Connecting a controller after the game started restarts the accessory session; it may take a
  moment before its pose is tracked.
