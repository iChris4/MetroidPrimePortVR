#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the Dawn that PC VR on Linux needs: aurora's pinned Dawn with PrimedGun's patches, for
# x86_64 Linux. The Linux counterpart of tools/Build-WindowsDawn.ps1.
#
# The stock prebuilt Dawn cannot create its Vulkan device through OpenXR, which Linux VR uses (the
# runtime creates Dawn's VkInstance and VkDevice through XR_KHR_vulkan_enable2, and the eyes are
# drawn straight into the runtime's swapchain images, platform/vr/openxr_vulkan_direct.cpp). This
# script downloads the Dawn source aurora pins (extern/aurora/cmake/AuroraDependencyVersions.cmake,
# AURORA_DAWN_REF on Odrannnn/dawn), applies quest/dawn/apply.py (the same patches as the Quest's
# Dawn), and builds it the way encounter/dawn's release workflow builds dawn-linux-x86_64
# (.github/workflows/release.yml + dawn-ci.cmake: clang, lld, a static libwebgpu_dawn.a). The
# result is a package in the stock layout to hand to a native build as AURORA_DAWN_PACKAGE_URL;
# the script prints the configure flags. A build with the stock Dawn still runs; only VR then
# reports that it needs this one.
#
# The package is cached: dawn-package.txt records the source, the patch hash, the compiler and the
# flags, and a later run with the same inputs reuses it. The first build fetches Dawn's
# dependencies with git and compiles all of Dawn and Tint, which takes a while.
#
# Needs: git, python3, cmake 3.25+, ninja 1.11+, clang and lld (Ubuntu: apt install clang lld),
# and the X11/Wayland headers Dawn's release workflow installs (libwayland-dev libxkbcommon-dev
# libxrandr-dev libxinerama-dev libxcursor-dev mesa-common-dev libx11-xcb-dev).
#
# Usage: tools/build-linux-dawn.sh [--force] [work-dir]
#   work-dir defaults to build/ld under the repository. Both games' ports apply the same patches,
#   so one work directory can serve both.
set -euo pipefail

force=0
if [[ ${1:-} == --force ]]; then
    force=1
    shift
fi
repo=$(cd "$(dirname "$0")/.." && pwd)
work=${1:-"$repo/build/ld"}
mkdir -p "$work"
work=$(cd "$work" && pwd)
jobs=${JOBS:-$(nproc)}

# The source revision is aurora's; its archive hash is pinned here, as in quest/Build-QuestDawn.ps1,
# so a Dawn bump in aurora fails loudly until this script (and the patches) are checked against it.
revision=118436b3f8f68583eb86e811ca605dc41637e8cc
archive_sha256=033b1a2eca2dddc67fd9c1ca88a54b42d727f8a9f765ba669b84d9a77bb02576
pinned=$(sed -nE 's/.*_aurora_dependency_version\(AURORA_DAWN_REF[[:space:]]+"([a-f0-9]+)".*/\1/p' \
    "$repo/extern/aurora/cmake/AuroraDependencyVersions.cmake")
if [[ $pinned != "$revision" ]]; then
    echo "aurora pins Dawn $pinned, not $revision: update revision, archive_sha256 and quest/dawn/apply.py" >&2
    exit 1
fi

for tool in git python3 cmake ninja clang clang++ ld.lld; do
    command -v "$tool" >/dev/null || { echo "$tool is not on PATH" >&2; exit 1; }
done
compiler=$(clang --version | head -n1)

# dawn-ci.cmake's Linux branch: clang, lld, Wayland. No protobuf: only Tint's IR binary format
# needs it, which dawn-ci.cmake turns off.
flags=(-DCMAKE_BUILD_TYPE=Release -DDAWN_BUILD_PROTOBUF=OFF)

# Every file the patches consist of: quest/dawn and the ABI headers apply.py copies in from aurora.
# The SPDX line is left out, so the two games' copies (GPL here, MIT in Echoes) hash the same.
patch_hash=$(
    {
        for f in $(cd "$repo/quest/dawn" && ls | sort); do
            [[ -f "$repo/quest/dawn/$f" ]] || continue
            printf '%s\n' "$f"
            grep -v 'SPDX-License-Identifier' "$repo/quest/dawn/$f" | tr -d '\r'
        done
        for f in dawn_vulkan_abi.h dawn_fdm_abi.h; do
            printf '%s\n' "$f"
            grep -v 'SPDX-License-Identifier' "$repo/extern/aurora/include/aurora/$f" | tr -d '\r'
        done
    } | sha256sum | cut -d' ' -f1
)
cache_key=$(printf '%s|%s|%s|%s|%s' "$revision" "$archive_sha256" "$patch_hash" "$compiler" \
    "${flags[*]}" | sha256sum | cut -d' ' -f1)

manifest="$work/dawn-package.txt"
if [[ $force = 0 && -f $manifest ]]; then
    cached_key=$(sed -n 's/^cache_key=//p' "$manifest")
    cached_file=$(sed -n 's/^package_file=//p' "$manifest")
    cached_sha=$(sed -n 's/^package_sha256=//p' "$manifest")
    if [[ $cached_key = "$cache_key" && -f "$work/$cached_file" ]] &&
        [[ $(sha256sum "$work/$cached_file" | cut -d' ' -f1) = "$cached_sha" ]]; then
        echo "Patched Dawn for Linux is up to date: $work/$cached_file"
        echo "Configure a native build with -DAURORA_DAWN_PROVIDER=package" \
            "-DAURORA_DAWN_PACKAGE_URL=$work/$cached_file -DAURORA_DAWN_PACKAGE_SHA256=$cached_sha"
        exit 0
    fi
fi

archive="$work/dawn-${revision:0:12}.tar.gz"
if [[ ! -f $archive ]]; then
    echo "Downloading Dawn $revision"
    curl -fL --retry 3 -o "$archive.partial" "https://github.com/Odrannnn/dawn/archive/$revision.tar.gz"
    mv "$archive.partial" "$archive"
fi
if [[ $(sha256sum "$archive" | cut -d' ' -f1) != "$archive_sha256" ]]; then
    echo "The Dawn source archive does not match the pinned SHA-256: $archive" >&2
    exit 1
fi

# Restore every file of the pinned archive over the source tree, keeping the dependencies Dawn
# fetched into it, then patch it (apply.py skips edits that are already there). Extracting over the
# tree puts back the pristine text of every file a changed patch may have touched.
source="$work/s"
marker="$source/primedgun-patches.sha256"
source_key="$revision|$patch_hash"
if [[ ! -f $marker || $(cat "$marker") != "$source_key" ]]; then
    echo "Restoring pinned Dawn source files"
    mkdir -p "$source"
    tar -xzf "$archive" -C "$source" --strip-components=1
    python3 "$repo/quest/dawn/apply.py" "$source"
    printf '%s' "$source_key" >"$marker"
fi

build="$work/b"
install="$work/p"
cmake -S "$source" -B "$build" -G Ninja -C "$source/.github/workflows/dawn-ci.cmake" "${flags[@]}" \
    "-DPython3_EXECUTABLE=$(command -v python3)" "-DCMAKE_INSTALL_PREFIX=$install"
cmake --build "$build" --parallel "$jobs"
case "$install" in
    "$work"/*) rm -rf "$install" ;;
    *) echo "Dawn install path escapes its work directory: $install" >&2; exit 1 ;;
esac
cmake --install "$build"
library="$install/lib/libwebgpu_dawn.a"
if [[ ! -f $library ]]; then
    echo "Dawn library missing: $library" >&2
    exit 1
fi
# The direct-presentation entry points must be in the archive, or Linux VR has nothing to call.
# (No grep -q: it would stop reading at the first match, and pipefail would count nm's SIGPIPE.)
if ! nm --defined-only "$library" 2>/dev/null | grep ' T AuroraDawnVulkanConfigure$' >/dev/null; then
    echo "The built Dawn lacks PrimedGun's entry points (AuroraDawnVulkanConfigure)" >&2
    exit 1
fi

packing="$work/dawn-linux-x86_64-primedgun.partial.tar.gz"
tar -czf "$packing" -C "$install" .
package_sha=$(sha256sum "$packing" | cut -d' ' -f1)
package_file="dawn-linux-x86_64-primedgun-${package_sha:0:12}.tar.gz"
find "$work" -maxdepth 1 -name 'dawn-linux-x86_64-primedgun*.tar.gz' ! -name "$package_file" \
    ! -name "$(basename "$packing")" -delete
mv -f "$packing" "$work/$package_file"

cat >"$manifest" <<EOF
source_revision=$revision
source_sha256=$archive_sha256
patch_sha256=$patch_hash
compiler=$compiler
flags=${flags[*]}
cache_key=$cache_key
package_file=$package_file
package_sha256=$package_sha
EOF
echo "Patched Dawn for Linux ready: $work/$package_file"
echo "Configure a native build with -DAURORA_DAWN_PROVIDER=package" \
    "-DAURORA_DAWN_PACKAGE_URL=$work/$package_file -DAURORA_DAWN_PACKAGE_SHA256=$package_sha"
