# Vendored runtime snapshots

Aurora and MusyX are tracked source snapshots, not submodules. A normal clone
contains the port patches; no private or unpublished dependency commits need to
be fetched. Keep their license files and source notices when updating them.

| Directory | Upstream | Snapshot imported into this repository |
| --- | --- | --- |
| `aurora` | <https://github.com/encounter/aurora> | Local port fork `d39404c803d28657c94159a8f44addffdd85a781`, based on port snapshot `32c926ca0f59994249ba4dae5d606a66f8bbd87f` (upstream `011e33109`, 2026-09-17), plus upstream fixes ported by hand up to `8e356df6e` (2026-10-20): `26371ce`, `2bf0825`, `e938637`, `0857ad8`, `3094e04`, `afa388d`, `8e0ff96`, `f394fc4`, `323e66f`, `cb0e279`, `3840bf9`, `9c0bf66`. Not taken: readback API, `LruCache`, `aurora::input`/`binding`, RmlUi |
| `astcenc` | <https://github.com/ARM-software/astc-encoder> | Release `5.7.0` (`baff485b0ff36d2f95d28961605106502c653966`), Apache-2.0 |
| `adrenotools` | <https://github.com/bylaws/libadrenotools> | `8fae8ce254dfc1344527e05301e43f37dea2df80`, with `lib/linkernsbypass` at `aa3975893d83ef1bc84c321ec60c65fbf1287887`, BSD-2-Clause |
| `sdk` | PrimeDecomp (decompiled) | Dolphin SDK, Metrowerks libc headers and CodeWarrior runtime, decompiled from the retail executable; **not** open source and not covered by `LICENSE` (see `NOTICE`) |
| `musyx` | <https://github.com/AxioDL/musyx> | Local SDL3 port fork `dc8bed3d6b253113a9f0c7bed57378f91b8dc788`, based on port snapshot `ab6d648668f2c6871eef2c079e8edccf51d7f8ec` |
| `GamepadMotionHelpers` | <https://github.com/JibbSmart/GamepadMotionHelpers> | `f1a76007ec058122a4bb6f19c816c6c0c7b7f12a` (`GamepadMotion.hpp` only), MIT |

These commit IDs record provenance; they are not download requirements. Existing
developer checkouts retain their old submodule object databases under
`.git/modules/extern/` for historical comparisons.

Both snapshots carry MIT top-level license files. The earlier recompilation
project's DolRecomp/ModernGekko dependencies are separate and are not linked here.
Retain and review individual source notices as well as the top-level licenses.

Port-specific changes include deferred ARQ callbacks, FIFO draw-sync tokens,
bounded/validated ARAM copies, paused event-pump servicing, Windows runtime DLL
packaging, DVD base-file reads for overlays (`aurora_dvd_base_*`), PC audio stream buffers, pointer-width corrections, atomic muting,
and synchronous voice retirement before releasing group resources.

Aurora still obtains its transitive dependencies through its provider system;
versions are pinned in `aurora/cmake/AuroraDependencyVersions.cmake`. Native CI
builds the vendored source and does not run `git submodule update`.

`astcenc` is a snapshot of the encoder core only: `Source/astcenc.h` and
`Source/astcenc_*.{cpp,h}` (no CLI, tests, fuzzers, ThirdParty or images), with
`LICENSE.txt`. `extern/astcenc/CMakeLists.txt` is ours: a static library, SSE2 on
x86-64, NEON on arm64, plain C++ elsewhere, with no GCC-only flags on MSVC. It
writes the Remastered importer's ASTC textures (`platform/port_remastered_image.cpp`).

`adrenotools` is unmodified apart from the dropped `.git` folders. It is
built only for Android arm64 (`add_subdirectory(... EXCLUDE_FROM_ALL)` in the root
`CMakeLists.txt`): `platform/port_gpu_driver.cpp` uses it to load custom Vulkan
drivers such as Mesa Turnip, and Gradle packages its four hook libraries.
