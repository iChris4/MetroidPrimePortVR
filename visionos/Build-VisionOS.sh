#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds PrimedGun for Apple Vision Pro (docs/VISIONOS.md), and installs it with --install.
#
#   visionos/Build-VisionOS.sh [--simulator] [--team TEAMID] [--bundle-id ID] [--install]
#                              [--device UDID] [--jobs N] [--open]
#
# Three stages, each skipped when up to date:
#   1. Dawn for the SDK (tools/Build-VisionOSDawn.sh, cached in build/visionos-dawn).
#   2. The game, cross-compiled with Ninja as MetroidPrimeGame.framework (build/visionos or
#      build/visionos-sim). nod, the disc reader, is Rust: a nightly toolchain with rust-src
#      builds its standard library for visionOS (-Zbuild-std).
#   3. The SwiftUI app, an Xcode project that links and embeds the framework (build/visionos-app or
#      build/visionos-app-sim): PrimedGunVision.app.
# --install puts the app on the booted simulator, or on the paired headset (put it on and unlock
# it). --open stops after generating the Xcode project and opens it.
#
# Signing: a device build needs a team; pass --team, set MP_VISIONOS_TEAM, or leave both out to use
# the only team Xcode knows. The simulator build is signed ad hoc without one.
#
# Needs Xcode with the visionOS platform, CMake 3.28+, Ninja, Python 3, git, and rustup with a
# nightly default toolchain and rust-src:
#   curl https://sh.rustup.rs -sSf | sh -s -- -y --default-toolchain nightly --profile minimal
#   rustup component add rust-src --toolchain nightly
set -euo pipefail

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
simulator=0
team="${MP_VISIONOS_TEAM:-}"
bundle_id="${MP_VISIONOS_BUNDLE_ID:-}"
install=0
device=""
jobs="$(sysctl -n hw.ncpu)"
open_project=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --simulator) simulator=1; shift ;;
        --team) team="${2:?}"; shift 2 ;;
        --bundle-id) bundle_id="${2:?}"; shift 2 ;;
        --install) install=1; shift ;;
        --device) device="${2:?}"; shift 2 ;;
        --jobs) jobs="${2:?}"; shift 2 ;;
        --open) open_project=1; shift ;;
        -h|--help) sed -n '3,25p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
    # xcrun follows xcode-select, which may point at the Command Line Tools; those have no visionOS SDK.
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
export PATH="${HOME}/.cargo/bin:${PATH}"
for tool in cmake ninja python3 git cargo rustup; do
    command -v "${tool}" >/dev/null || { echo "ERROR: ${tool} not found (see the header of this script)" >&2; exit 1; }
done
if ! rustup component list --toolchain nightly --installed 2>/dev/null | grep -q '^rust-src'; then
    echo "ERROR: Rust nightly with rust-src is needed: rustup component add rust-src --toolchain nightly" >&2
    exit 1
fi

if [[ ${simulator} -eq 1 ]]; then
    sysroot="xrsimulator"
    dawn_args=(--simulator)
    game_dir="${repo_root}/build/visionos-sim"
    app_dir="${repo_root}/build/visionos-app-sim"
else
    sysroot="xros"
    dawn_args=()
    game_dir="${repo_root}/build/visionos"
    app_dir="${repo_root}/build/visionos-app"
fi

# The team, when none was given: the only one Xcode is signed in with.
if [[ -z "${team}" && ${simulator} -eq 0 ]]; then
    teams="$({ defaults read com.apple.dt.Xcode IDEProvisioningTeamByIdentifier 2>/dev/null || true
               defaults read com.apple.dt.Xcode IDEProvisioningTeams 2>/dev/null || true; } |
             sed -n 's/.*teamID = \([A-Z0-9]*\);.*/\1/p' | sort -u)"
    if [[ "$(printf '%s\n' "${teams}" | sed '/^$/d' | wc -l | tr -d ' ')" == "1" ]]; then
        team="${teams}"
        echo "Signing with team ${team} (the one Xcode knows)" >&2
    else
        echo "ERROR: pass --team TEAMID (Xcode > Settings > Accounts shows it); teams found: ${teams:-none}" >&2
        exit 1
    fi
fi

# 1. Dawn.
dawn_output="$("${repo_root}/tools/Build-VisionOSDawn.sh" "${dawn_args[@]+"${dawn_args[@]}"}" --jobs "${jobs}")"
dawn_package="$(printf '%s\n' "${dawn_output}" | tail -n 1)"
dawn_sha256="$(printf '%s\n' "${dawn_output}" | tail -n 2 | head -n 1)"

# 2. The game framework.
if [[ ! -f "${game_dir}/CMakeCache.txt" ]]; then
    cmake -S "${repo_root}" -B "${game_dir}" -G Ninja \
        -DCMAKE_SYSTEM_NAME=visionOS "-DCMAKE_OSX_SYSROOT=${sysroot}" -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_SYSTEM_PROCESSOR=arm64 \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DMP_BUILD_TESTS=OFF -DAURORA_ENABLE_TESTS=OFF -DBUILD_SHARED_LIBS=OFF \
        -DAURORA_DAWN_PROVIDER=package "-DAURORA_DAWN_PACKAGE_URL=file://${dawn_package}" \
        "-DAURORA_DAWN_PACKAGE_SHA256=${dawn_sha256}" -DAURORA_DAWN_LINKAGE=static \
        -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=static \
        -DAURORA_NOD_PROVIDER=vendor -DAURORA_NOD_LINKAGE=static \
        -DTINT_BUILD_IR_BINARY=OFF -DDAWN_BUILD_PROTOBUF=OFF \
        -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON -DCMAKE_DISABLE_FIND_PACKAGE_xxHash=ON \
        -DCMAKE_DISABLE_FIND_PACKAGE_zstd=ON -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
else
    # A rebuilt Dawn package has another hash; FetchContent then extracts it again.
    cmake "${game_dir}" "-DAURORA_DAWN_PACKAGE_URL=file://${dawn_package}" "-DAURORA_DAWN_PACKAGE_SHA256=${dawn_sha256}"
fi
cmake --build "${game_dir}" --target metroid_prime_port --parallel "${jobs}"
framework="${game_dir}/MetroidPrimeGame.framework"
[[ -d "${framework}" ]] || { echo "ERROR: no game framework at ${framework}" >&2; exit 1; }

# 3. The app.
app_args=(
    -S "${repo_root}/visionos" -B "${app_dir}" -G Xcode
    -DCMAKE_SYSTEM_NAME=visionOS "-DCMAKE_OSX_SYSROOT=${sysroot}" -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 "-DMP_VISIONOS_GAME_FRAMEWORK=${framework}"
    "-DMP_VISIONOS_TEAM=${team}"
)
if [[ -n "${bundle_id}" ]]; then app_args+=("-DMP_VISIONOS_BUNDLE_ID=${bundle_id}"); fi
cmake "${app_args[@]}"
if [[ ${open_project} -eq 1 ]]; then
    open "${app_dir}/PrimedGunVision.xcodeproj"
    exit 0
fi
build_extra=()
if [[ ${simulator} -eq 0 ]]; then
    # Lets automatic signing register the headset and refresh the provisioning profile.
    build_extra=(-- -allowProvisioningUpdates)
fi
cmake --build "${app_dir}" --config Release --target PrimedGunVision --parallel "${jobs}" "${build_extra[@]+"${build_extra[@]}"}"
app="${app_dir}/Release-${sysroot}/PrimedGunVision.app"
if [[ ! -d "${app}" ]]; then
    app="$(find "${app_dir}" -maxdepth 2 -name PrimedGunVision.app -type d | head -n 1)"
fi
echo "App: ${app}"

if [[ ${install} -eq 1 ]]; then
    if [[ ${simulator} -eq 1 ]]; then
        xcrun simctl install booted "${app}"
        echo "Installed on the booted visionOS simulator"
    else
        # The first paired physical Apple Vision Pro (the list holds simulators too).
        if [[ -z "${device}" ]]; then
            device="$(xcrun devicectl list devices --hide-headers 2>/dev/null |
                awk 'tolower($0) ~ /vision/ && $0 ~ /physical/ { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9A-Fa-f-]{25,}$/) { print $i; exit } }')"
        fi
        if [[ -z "${device}" ]]; then
            echo "ERROR: no paired Apple Vision Pro; pair it in Xcode > Window > Devices and Simulators (or pass --device UDID)" >&2
            exit 1
        fi
        # A headset off the head is locked and devicectl cannot reach it: retry for about two
        # minutes, long enough to put it on (Wiicompiled VR's installer does the same).
        install_log="$(mktemp -t primedgun-install)"
        attempts=24
        attempt=1
        until xcrun devicectl device install app --device "${device}" "${app}" > "${install_log}" 2>&1; do
            if [[ ${attempt} -ge ${attempts} ]]; then
                cat "${install_log}" >&2
                rm -f "${install_log}"
                echo "ERROR: could not install on the Apple Vision Pro (${device}): put it on, unlock it, and run again." >&2
                exit 1
            fi
            [[ ${attempt} -eq 1 ]] && echo ">>> Put the Apple Vision Pro on and unlock it; retrying every 5 seconds." >&2
            attempt=$((attempt + 1))
            sleep 5
        done
        cat "${install_log}"
        rm -f "${install_log}"
        echo "Installed on ${device}"
    fi
fi
