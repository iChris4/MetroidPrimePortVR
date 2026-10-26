# PrimedGun launcher

`PrimedGun.exe` is PrimedGun's launcher window, ported from its Dolphin build
(`DolphinQt/MainWindow.cpp` there) to the native port. It is a separate Qt 6
program that sits next to `metroid_prime_port.exe`, edits the game's
`port_settings.ini`, and starts the game. The tabs are the same as in PrimedGun:
Setup, Controller, Calibration, Cannon Textures, Layout, Port Config (in place
of Dolphin Config) and About (from the Quest build).

## Building

The launcher is off by default. To build it, turn it on and point CMake at a
Qt 6.5+ install, for example the prebuilt Qt that PrimedGun's Dolphin tree
carries:

```
cmake -S . -B build/vr -DMP_BUILD_LAUNCHER=ON ^
  -DCMAKE_PREFIX_PATH=C:/path/to/PrimedGun/Externals/Qt/Qt6.5.1/x64
cmake --build build/vr
```

`PrimedGun.exe` is built next to the game. The build then runs `windeployqt`,
which copies Qt's DLLs and `platforms/`, and copies the cannon texture slots to
`primedgun/cannon_textures`. `cmake --install` installs the same files.

Qt's prebuilt `Qt6EntryPoint.lib` is compiled with `/GL`, which lld-link cannot
read. The target therefore sets `qt_no_entrypoint` and links with
`/ENTRY:mainCRTStartup`.

The Qt-free half, `core/`, is always built with the tests, and
`port_launcher_tests` checks it. It covers settings-file editing, the key table
against `PortVrSettings` and the game's clamps, the disc check, the PrimedGun
import, and the cannon pack.

## How it behaves

- **Settings.** The launcher finds `port_settings.ini` the same way the game
  does (`platform/include/port_paths.h`). A save re-reads the file and rewrites
  only the keys the launcher changed. Every other line, including comments and
  line endings, stays as the game wrote it.
  - While the game runs, every edit is written as it is made (at most every
    150 ms). The game looks at the file a few times a second and applies the
    keys that changed, as the F1 overlay would (`platform/debug_ui.cpp`
    `PollSettingsFile`). Enable VR waits for the next start.
  - What the game saves while it runs (F1, the headset's menu) shows in the
    launcher, keeping any edit not written yet.
  - A cannon slot applied while the game runs also writes
    `cannon_textures_applied`, a request the game answers by reloading the
    user texture pack. The game drops that line at its next save.
  - Settings the game saves but does not use yet carry a "not active yet" tag
    (`active` in `core/launcher_keys.cpp`).
  - The launcher's own state, such as the selected disc and the window position,
    is kept in `primedgun_launcher.ini` beside it.
- **Play** saves pending edits, then starts the game with the disc as its first
  argument. The game's output goes to `primedgun_last_run.log` in the user
  folder. Select Game and Transfer wait until the game closes.
  - **Stop** closes the game's window, so the game exits normally. **Force
    Stop** is offered after 10 seconds.
- **Cannon Textures.** PrimedGun's Dolphin-named DDS files are used as they
  are; Aurora reads the same names and hashes.
  - Applying a slot copies it into `user_textures/000_primedgun_cannon`. If the
    pack is split by device, the slot goes into every device folder.
  - Aurora uses the first file for each texture in path order, so this folder
    wins over any other pack file for the cannon.
  - The applied slot is stored as `vr_cannon_texture_slot`.
- **Transfer PrimedGun Memory Card / Settings** looks for an old PrimedGun
  install around the launcher, the same way PrimedGun did, or in a folder you
  pick, or in Dolphin's own folder.
  - It imports the Metroid Prime saves into the game's card (`USA/Card A`) with
    `platform/port_gci.cpp`. When the selected disc is PAL, it looks for
    Dolphin's EUR card instead and imports into `EUR/Card A`, as the game reads
    it for that disc.
  - It loads the old `PrimedGun.ini` settings as unsaved changes.

## The Quest launcher

The Quest APK (`quest/`) opens on the same launcher, as a 2D Horizon OS panel:
PrimedGun's Quest launcher (its Kotlin widgets, layouts and palette) with the
tabs of this one. The panel is `quest/app/src/main/java/org/primedgun/v2/launcher/`.
Its native half, `jni/` (`libprimedgun_launcher.so`, built with
`-DMP_BUILD_QUEST_LAUNCHER=ON`), puts `core/` behind JNI. Both launchers therefore
edit `port_settings.ini` with the same key table and line-preserving editor,
and both apply cannon slots with the same code.

Where the Quest panel differs:

- **Game.** It runs in its own process (`PrimedGunVrActivity`, `:game`).
  - Play saves pending edits and starts it.
  - The tabs lock while that process lives. The settings are read again once
    it is gone.
  - Stop sends a broadcast that finishes the game. Pressing Stop again after
    10 seconds ends the process.
- **Select Game** copies the picked image to `disc.iso` in the user folder,
  `/sdcard/Android/data/org.primedgun.v2/files`. It does this in a foreground
  service, because the game process cannot open the picker's document. The
  disc check runs on the image's first bytes before the copy.
- **Transfer** takes a picked file: a card (`.raw`, `.gcp`, `.gci`),
  `PrimedGun.ini`, or the zip that PrimedGun's Export User Data writes.
  - The panel cannot read a raw card itself; `port_gci.cpp` needs SDL. So the
    card waits in `primedgun/pending_import`.
  - The game imports it at its next start, before the card mounts
    (`PortGci::ImportPending`). It writes `primedgun/import_report.txt`, which
    the panel shows afterwards.
- **Port Config** leaves out the PC-only keys: VR on/off, the mirror,
  fullscreen and VSync. It shows the Quest headset keys instead:
  - refresh rate, performance level, passthrough and foveation;
  - the renderer switches the PC reaches through F1.
- Settings the game does not read yet carry the same "not active yet" tag.

The Port Config tab has **Open Full Port Options (F1)...**. With a game running,
it opens its existing F1 panel. Otherwise it opens a desktop options window
without loading a disc or starting OpenXR. Close that window before pressing Play.

On Quest, **Port Config > Open Full Port Options (F1)** opens the same settings
in a normal 2D system panel, without requiring a disc. It has its own SDL process
and GPU cache, so it can open while the immersive game process exists. Close the
options panel to return to the launcher; settings are saved to the shared user folder.

In **Customize controller layout > System**, enable **Long press to open VR menu**
and set **Hold time (seconds)** from 0.01 to 5.00 in 0.01-second steps. This applies
to your assigned VR settings button on Windows and Quest. Closing remains a single
press; with long press disabled, opening is a single press too.
