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
| Launcher, disc import, disc check | Works in the simulator. |
| Game start, OpenXR session, title screen on the virtual screen | Works in the simulator (head tracking only). |
| Quit and return to the launcher | Works in the simulator. |
| Stereo gameplay, frame pacing, comfort | Not yet checked on a headset. |
| PlayStation VR2 Sense controllers | Written against the visionOS 26 SDK; not yet checked on a headset. |
| DualSense and other gamepads | Through SDL as on iPhone; not yet checked on a headset. |

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
  around.
- **Hands.** With nothing in hand, look at a menu item and pinch to press it. Playing needs
  controllers or a gamepad.

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
