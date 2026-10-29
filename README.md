# PrimedGun 2.0

**Created by Nobbie and iChris4.**

PrimedGun 2.0 brings **Metroid Prime** to native VR on Windows PC and standalone
Meta Quest. It builds on the controls and VR experience of
[the original PrimedGun](https://github.com/Nobbie248/PrimedGun), with a native
game engine based on [PrimeDecomp](https://github.com/PrimeDecomp/prime),
Odran's MetroidPrimePort, and Aurora rendering.

This fork targets Windows PC VR and standalone Meta Quest.

**No game content is included.** You need your own copy of the game: the port
reads it from a disc image and never ships any of its assets. This repository
ships no disc image, and neither does any package or release built from it.

## Features

- Native OpenXR VR on PC and standalone Meta Quest.
- Roomscale movement with a modern VR control scheme.
- 6DOF arm cannon tracking for targeting and grapple controls.
- One-click height calibration and adjustable cannon position and rotation.
- Left-handed support.
- Easy texture swapping tools.
- In-headset settings for controls, calibration, graphics, and more.
- Randomizer, and Archipelago multiworld support.

## Setup Notes

1. Launch **PrimedGun** and use **Select Game** to choose your own Metroid Prime
   disc image. USA 1.00/1.01/1.02 (`GM8E01`, revisions 0-2) is supported; PAL (`GM8P01`) is
   experimental.
2. On PC, start your OpenXR runtime and enable **VR** in **Port Config**. On
   Quest, select the game in the launcher panel, then press **Play** to enter VR.
3. Check the **Layout** tab for controller bindings and the **Controller** tab
   for input options.
4. Once in game, click the right stick to calibrate your height. Click the left
   stick to open or close the in-headset settings menu.
5. Use **Calibration** to adjust the arm cannon and **Cannon Textures** to select
   its appearance.
6. Use **Save Settings** after changing launcher options. **Play** also saves
   pending changes. On Quest, change launcher settings before starting the game.

To bring over existing progress, use **Transfer PrimedGun Memory Card / Settings**.
On PC, select the old installation or user folder. On Quest, select a memory
card, `PrimedGun.ini`, or an exported user-data ZIP. Select your game first so
the correct regional memory card is used. Dolphin save states cannot be
transferred; save normally in the game before moving your progress.

Roomscale is optional. With it disabled, use stick movement and keep near your
calibrated position. Physical walking with roomscale enabled applies during
first-person gameplay.

## Supported discs and saves

### Disc images

The launcher accepts these formats for your own **Metroid Prime GameCube** disc:

| Format | File types |
|---|---|
| Standard disc image | `.iso`, `.gcm` |
| NKit ISO | `.nkit.iso` (or `.nkit`) |
| Dolphin RVZ | `.rvz` |
| CISO | `.ciso` |
| WBFS | `.wbfs` |

Supported game versions are **USA 1.00, 1.01 and 1.02** (`GM8E01`, revisions 0–2).
**PAL revision 0** (`GM8P01`) is experimental. Extract downloaded archives before
selecting an image; ZIP/7z files are not disc images.

The native disc reader also supports **GCZ** (`.gcz`) and **WIA** (`.wia`), but
these extensions are not currently accepted by the PrimedGun launcher. Use a
supported launcher format such as ISO or RVZ when selecting a game there.
Wii Trilogy disc images and Metroid Prime Remastered images are not playable
replacement discs for the GameCube game; optional asset imports are separate.

### Saves and memory cards

| Type | Support |
|---|---|
| Individual GameCube save | Dolphin-compatible `.gci` files |
| GameCube memory-card folder | Dolphin/PrimeHack/older PrimedGun **GCI folders**, containing individual `.gci` files |
| Complete GameCube memory-card image | `.raw` and `.gcp` images; the importer extracts Metroid Prime saves rather than replacing the card with unrelated games |
| Wii Metroid Prime Trilogy save | `save.bin` or Wii-exported `data.bin`, through the full F1 memory-card importer; Metroid Prime progress is converted to the selected GameCube release |
| Native save state / Quick resume | PrimedGun `.mpss` files in the user's `savestates` folder; separate from normal memory-card saves |
| Dolphin/PrimeHack emulator save state | **Not compatible**; load it in the original emulator and make a normal in-game save first |

Use **Transfer PrimedGun Memory Card / Settings** in the launcher to bring over
an older installation. On Windows, select its installation or user folder. On
Quest, choose a `.gci`, `.raw`, `.gcp`, or an exported PrimedGun/Dolphin user-data
ZIP. Select your game first so the correct **USA** or **EUR** card is used.
For individual save imports and conversion, the full options panel also has
**System > Memory card > Import file...**; return to the title screen before
importing. The launcher's Quest transfer button does not accept Trilogy `.bin`
files directly.

PrimedGun normally stores its active card as individual `.gci` files in
`user/USA/Card A/` on Windows, or
`/sdcard/Android/data/org.primedgun.v2/files/USA/Card A/` on Quest. PAL uses
`EUR/Card A/` instead. Save import supports conversion between the USA and PAL
GameCube releases and Trilogy saves; only Metroid Prime saves are imported.
Existing saves replaced by the native importer are kept in a `_replaced` folder.

When updating on PC, keep the existing **`user` folder**. On Quest, install a
compatible APK over the existing app to keep its saves and settings; do not
uninstall it first. Keeping a separate backup is recommended.

## Launcher updates

Open **Setup > Check for updates** in the Windows or Quest launcher. The check
uses the public releases on `iChris4/MetroidPrimePortVR`; no GitHub login is
needed. **Include alpha, beta and release candidate versions** is enabled by
default for preview builds. Stable builds default to stable releases only.
The launcher checks when you press the button, shows the release notes and
selects the newest published version with a package for your platform.

Choose **Download update** to download and verify its SHA-256 checksum. Nothing
is installed automatically. A release must provide a GitHub SHA-256 asset digest
or a matching entry in `SHA256SUMS.txt`; unverified packages are not installed.

- **Windows:** Close the game and Port Options, then choose **Install and restart
  launcher**. The updater saves launcher settings, waits for the programs to exit,
  replaces the program files and opens the launcher again. It never replaces the
  `user` folder. Previous program files and an `update.log` are kept in the
  temporary `PrimedGun-update-*` folder; failed copies attempt to restore those
  files. Use a writable installation folder. System `curl` and Windows PowerShell
  are used; they are included in supported Windows versions.
- **Quest:** Close the game, then choose **Install update**. If asked, allow the
  launcher to install apps and return to approve Quest's system installation
  prompt. The APK must have the same application ID and signing identity as your
  installed build, and a higher Android version code. Compatible updates preserve
  saves and settings. The launcher rejects mismatched keys; it never uninstalls
  your app. If your Horizon OS version does not offer the permission or installer
  panel, use the release page to sideload the same APK through your usual tool.

This update button first appears in beta.6; older versions need one manual update.
Release publishers must keep the Quest signing key and package name unchanged,
increase `versionCode` for each APK, and upload the normal Windows x64 ZIP and
Quest APK with SHA-256 digests/checksums. Downloaded data and backups may be removed
from the temporary/cache folders once an update has been checked successfully.

## Installing HD texture packs

Use a Metroid Prime replacement texture pack containing `.png` or `.dds` files
with their original texture filenames (usually `tex1_...`). Extract the ZIP/7z
first; copying the archive itself does not install it. No ISO patch is needed.

### Windows

1. Open **Port Config > Open Texture Pack Folder** in the PrimedGun launcher.
   Normally this is `user/user_textures/` beside `PrimedGun.exe`; the button
   opens the folder your installation actually uses.
2. Copy the extracted textures into that folder. A named pack subfolder is fine;
   subfolders are scanned automatically. Keep the original filenames.
3. Restart the game to load the pack. While playing on PC, you can also use
   **F1 > Mods > Texture pack > Reload texture pack** after copying files.

Example: `user/user_textures/My HD Pack/tex1_...png`.

### Meta Quest

1. Launch PrimedGun once, then open **Port Config > Show Texture Pack Folder**.
   This creates the `user_textures` folder if it is missing and shows its path.
2. Connect the headset to your computer and allow file access, then copy the
   extracted textures into:
   `/sdcard/Android/data/org.primedgun.v2/files/user_textures/`.
   In a USB file browser this is usually **Internal shared storage > Android >
   data > org.primedgun.v2 > files > user_textures**. If copying manually and the
   folder is missing, create a folder named exactly `user_textures` inside `files`.
3. Keep the pack in a named subfolder if desired, for example
   `files/user_textures/My HD Pack/tex1_...png`. Restart the game after copying.

Use one pack/version at a time to avoid conflicting replacements. Larger packs
use more memory, especially on Quest; choose a lower-resolution version if needed.
**Texture Pack Help** in Port Config repeats these steps and shows the local path.
See [the texture-pack reference](docs/MODS.md) for advanced setup.

## Controller Settings

The **Layout** tab shows the recommended controls. Use **Controller** to adjust
the PrimedGun input options, **Calibration** for tracking and cannon alignment,
and the in-headset menu to make changes while playing. The native build uses
its own launcher and OpenXR controls; see [the launcher guide](launcher/README.md)
for settings and transfer details.

## Links

- [PrimedGun 2.0 — iChris4](https://github.com/iChris4/MetroidPrimePortVR)
- [PrimedGun Legacy — Nobbie](https://github.com/Nobbie248/PrimedGun)
- [Dolphin VR Discord](https://discord.gg/GdmffzCTrh)
- [Flat2VR Modding Discord](https://discord.gg/flat2vr)

## Randomizer and Archipelago Multiworld

- **Randomizer** — Generate your own randomizer games
- **Archipelago multiworld** — Generate a game in archipelago with the [Metroid Prime APWorld](https://github.com/UltiNaruto/MetroidAPrime) and host it. Open F1 > Archipelago, enter the server (`host:port`), your
  slot name and the password if any, and connect; no seed file is needed. Each seed and slot gets its own separate save, checks made
  offline are sent on the next connect, chat with command support, DeathLink.

## Downloads

Check the [PrimedGun 2.0 releases page](https://github.com/iChris4/MetroidPrimePortVR/releases)
for available builds. Every build needs your own disc image.

The underlying native port supports the following package formats; consult
each release for the files provided.

## Building

### Windows VR and Launcher

Use an MSVC developer shell with CMake 3.25+, Ninja, a C++20 toolchain, and
Qt 6.5+. Replace the Qt path with your local installation:

```bat
cmake -S . -B build/vr -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  -DMP_ENABLE_OPENXR=ON -DMP_BUILD_LAUNCHER=ON ^
  -DCMAKE_PREFIX_PATH=C:/path/to/Qt/6.5.1/msvc2019_64
cmake --build build/vr --parallel
```

The launcher, `PrimedGun.exe`, is built beside `metroid_prime_port.exe`.
See [launcher/README.md](launcher/README.md) for deployment details.

### Standalone Meta Quest

On Windows, install JDK 17, the Android SDK with NDK 29.0.14206865, CMake 3.25+,
Ninja, Git, and Rust configured for Android ARM64. Then run:

```powershell
.\quest\Build-Quest.ps1
```

Add `-Install` to install on an ADB-connected headset. The script builds the
Quest Gradle project and its OpenXR native library. See the prerequisite and
signing notes in [quest/Build-Quest.ps1](quest/Build-Quest.ps1).

### Desktop Native Port

For a desktop build, use an MSVC developer shell with CMake 3.25+, Ninja,
clang-cl and a C++20 toolchain. Aurora and MusyX are vendored; Aurora fetches
its pinned dependencies on the first configure.

```bat
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  -DAURORA_ENABLE_TESTS=OFF -DMP_BUILD_TESTS=ON
cmake --build build/native --parallel 4
ctest --test-dir build/native -L port --output-on-failure
```

### Flat Android Build

```sh
tools/make_android_keystore.sh          # once; writes android/keystore.properties
tools/android_apk.sh :app:assembleRelease
```

## Running

```bat
build\native\metroid_prime_port.exe "C:\Games\Metroid Prime (USA) (v1.00).iso"
```

## Credits

### Created by

Nobbie and iChris4

### Quest Port & Early VR Development

**iChris4** — creator of Dolphin ReduX for PC and Quest. Ported PrimedGun to
Quest, contributed early visor effect fixes, and ported the Metroid Prime
override from Dolphin Hydra.

### Native Port & Rendering

PrimeDecomp, Odran's MetroidPrimePort, and Aurora. OpenXR stereo rendering,
head tracking, and PrimedGun controls build on the OpenXR work from Wiicompiled VR.

### Development References

Metroid Prime decompilation project, Metaforce, and PrimeHack.

### Development Assistance

Codex — debugging, system integration, and iterative development.

### Early Playtesting

GeekyGami, Lucaspec72, TorchRing, detective_yoshi, PHA3ESH1FTGAMES,
retrovideogamer, Samevi, Mochu, VideoGameEsoterica, and VRified Games.

### Quest Playtesting

budwheizzah

### Special Thanks

The Metroid Prime modding community, for the resources and research that helped
make PrimedGun possible, and the Dolphin team.

## Licensing

This is a fork of the [PrimeDecomp/prime](https://github.com/PrimeDecomp/prime)
decompilation, which the game logic is built from. Aurora (`extern/aurora`) and
MusyX (`extern/musyx`) are MIT-licensed vendored snapshots, and the button
prompt icons come from Kenney's Input Prompts pack (CC0), vendored under
`tools/prompt_icons`. Their licences are kept alongside them and are copied into
release packages.

`LICENSE` covers this project's own work — the platform layer, the build and
packaging scripts, and the Android integration. It does **not** cover
the game code under `src/` and `include/`, which comes from the decompilation
and remains Nintendo's. See `NOTICE`.

Metroid Prime is a trademark of Nintendo. This project is unaffiliated with
Nintendo and Retro Studios, and ships no game data.

## Documentation

| Document | What it covers |
|---|---|
| `docs/SETTINGS.md` | Every control, setting and environment variable |
| `docs/NATIVE_PORT.md` | The detailed reference: build options, smoke-test hooks, platform notes |
| `docs/MODS.md` | HD texture packs and the mods folder |
| `docs/DREAD_SAMUS.md` | The Metroid Dread Samus model import |
| `docs/RANDOMIZER.md` | The built-in randomizer |
| `docs/ARCHIPELAGO.md` | The Archipelago multiworld client |
| `docs/CONSOLE.md` | The debug console (`MP_CONSOLE`) |
| `docs/DEBUGGING.md` | Headless debugging with `tools/mprig.py` |
| `tools/README.md` | What each script in `tools/` does and what runs it |
| `docs/RELEASING.md` | What a release contains and must carry |
| `docs/ANDROID.md` | The Android port's status and build |
| `docs/REMASTERED_EFFECTS.md` | Importing Remastered particle effects |
| `docs/FRAME_INTERPOLATION.md`, `docs/HIGH_FPS_AUDIT.md` | High frame rate design notes |
| `decomp/` | The original decompilation's GameCube build and notes (not used by the port) |
| `PORT_NOTES.md` | Open follow-ups and non-obvious facts |

## AI assistance

Parts of this port were written with AI coding assistants, under my oversight.
