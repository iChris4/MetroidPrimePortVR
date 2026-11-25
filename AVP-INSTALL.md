# Installing PrimedGun 2.0 on Apple Vision Pro

PrimedGun 2.0 is a native VR port of Metroid Prime, built from the [PrimeDecomp](https://github.com/PrimeDecomp/prime) decompilation with Aurora rendering through Dawn on Metal, and a visionOS layer from Wiicompiled VR. It runs on the headset itself, with no emulator.

## Requirements

- An Apple silicon Mac with Xcode 26 or later and the visionOS platform
- Apple Vision Pro on visionOS 26 or later
- A paid Apple Developer Program membership. The app asks for the increased memory limit (`visionos/App/PrimedGunVision.entitlements`), which a free personal team can't sign.
- Your own copy of Metroid Prime for the GameCube, and its disc image (see Game files)
- CMake 3.28 or later, Ninja, Python 3 and git (`brew install cmake ninja`)
- Rust nightly with its sources, for the disc reader:
  ```sh
  curl https://sh.rustup.rs -sSf | sh -s -- -y --default-toolchain nightly --profile minimal
  rustup component add rust-src --toolchain nightly
  ```
- A controller: PlayStation VR2 Sense controllers (full VR controls), a pair of Nintendo Switch Joy-Cons, or a DualSense or other gamepad. Bare hands only work the menus.

## Game files

You need to supply your own game files, legally obtained from a copy you own. This repository contains none.

- A Metroid Prime GameCube disc image: USA 1.00, 1.01 or 1.02 (`GM8E01`), or PAL (`GM8P01`, experimental), as ISO, NKit ISO, GCM, RVZ, CISO or WBFS.
- On the headset, open **PrimedGun**, choose **Import a disc image** under **Disc image**, and pick the file in Files, iCloud Drive or a USB drive. It's copied into the app, in *Files > On My Apple Vision Pro > PrimedGun > Discs*.
- Pick the image in the list: the launcher says whether it's a disc the game plays.

## Build

Start from a checkout of this repository. (The AVP Ports Index gives players a command that clones this repo at the exact commit it scanned, with submodules, so these steps should work from any existing checkout.)

The Vision Pro build lives on the default branch, `OpenXR`; the older `VisionPro` branch is behind it.

1. Find your Team ID in *Xcode > Settings > Accounts*.
2. Pick a bundle ID of your own, such as `com.yourname.primedgun`. The default, `org.primedgun.vision`, belongs to the developers' team and won't register under yours.
3. Pair the headset first (see below), then run:
   ```sh
   visionos/Build-VisionOS.sh --team <TEAMID> --bundle-id com.yourname.primedgun --install
   ```
   The script builds Dawn for visionOS into `build/visionos-dawn` (a few minutes the first time), cross-compiles the game as `MetroidPrimeGame.framework`, then builds the SwiftUI app at `build/visionos-app/Release-xros/PrimedGunVision.app`. Each stage is kept when it's up to date.

`--open` generates the Xcode project and opens it instead, and `--simulator` builds for the visionOS Simulator (head tracking only, no controllers).

## Install on Apple Vision Pro

1. Pair the headset with Xcode: on the headset open *Settings > General > Remote Devices*, then pair it from *Window > Devices and Simulators* in Xcode.
2. Turn on Developer Mode on the headset (*Settings > Privacy & Security > Developer Mode*) and restart it.
3. With `--install`, the script waits about two minutes for the headset: put it on and unlock it.
4. Pair Sense controllers or Joy-Cons in *Settings > General > Game Controllers* before starting, and allow accessory tracking or hand tracking when asked.

## Controls and known issues

- Press **Play** in the launcher; the game opens in an immersive space. Click the right stick to set your height, and the left stick to open or close the VR menu. Press the Digital Crown to leave.
- Sense controllers play the full VR controls: the arm cannon follows the controller in your gun hand. Cross and Circle are A and B, Square and Triangle are X and Y, and Create is the menu button.
- Joy-Cons held upright in each hand play the same VR controls through hand tracking. A gamepad plays the original controls: look at a target and hold L to lock on, or hold R to aim the cannon with the stick.
- Settings, memory card (`USA/Card A/`), log and mods are in *Files > On My Apple Vision Pro > PrimedGun*. **Show my room around the menu screen** shows your room around menus, the map and cutscenes; gameplay stays fully immersive.
- Experimental. Stereo comfort and frame pacing aren't checked on a headset yet, nor are Joy-Con hand tracking and Sense controller calibration; the VR menu's calibration corrects the cannon if it points off.
- The game runs once per start of the app: quit the app before playing again. There's no foveated rendering yet.
