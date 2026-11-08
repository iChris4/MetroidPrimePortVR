#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Dawn Aurora pins for Apple Vision Pro (or its simulator), as the install tree Aurora's
# package provider consumes (extern/aurora/cmake/AuroraDawnProvider.cmake).
#
# No prebuilt Dawn exists for visionOS, and the ios-arm64 package is a Mach-O for the iOS platform,
# which the visionOS linker refuses. So Dawn is built from source here: the revision
# extern/aurora/cmake/AuroraDependencyVersions.cmake pins (AURORA_DAWN_REF on Odrannnn/dawn, the same
# source quest/Build-QuestDawn.ps1 builds), Metal only, one monolithic static library. The Quest's
# Dawn patches (quest/dawn) are Vulkan-only and are not applied. The flags are Wiicompiled VR's
# (visionos/Build-VisionOSDawn.sh there), which were proven on the headset.
#
# One fix is applied (apply_visionos_fixes below): this Dawn reports BC texture compression on any
# iOS-family GPU that has it, Apple Vision Pro's included, but maps the BC formats to Metal only on
# macOS, so the first BC texture Aurora made (the game's CMPR textures) aborted in MetalPixelFormat.
# The simulator's GPU reports no BC, which is why it ran there. The BC formats exist on visionOS.
#
#   tools/Build-VisionOSDawn.sh [--simulator] [--work DIR] [--jobs N] [--force]
#
# Prints the package path last, and its SHA-256 on the line before (for AURORA_DAWN_PACKAGE_SHA256,
# which makes FetchContent extract a rebuilt package again). Cached: a package built from the same
# revision, SDK and flags is reused. Needs Xcode with the visionOS SDK, CMake 3.28+, Ninja, Python 3
# and git (DAWN_FETCH_DEPENDENCIES clones Dawn's third_party).
set -euo pipefail

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
# The source revision is aurora's; its archive hash is pinned here and in quest/Build-QuestDawn.ps1, so
# a Dawn bump in aurora fails loudly until both scripts are checked against it.
dawn_revision="118436b3f8f68583eb86e811ca605dc41637e8cc"
dawn_source_sha256="033b1a2eca2dddc67fd9c1ca88a54b42d727f8a9f765ba669b84d9a77bb02576"
work_dir="${repo_root}/build/visionos-dawn"
jobs="$(sysctl -n hw.ncpu)"
sysroot="xros"
flavour="visionos-arm64"
force=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --simulator) sysroot="xrsimulator"; flavour="xrsimulator-arm64"; shift ;;
        --work) work_dir="${2:?}"; shift 2 ;;
        --jobs) jobs="${2:?}"; shift 2 ;;
        --force) force=1; shift ;;
        -h|--help) sed -n '3,19p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

pinned="$(sed -n 's/^_aurora_dependency_version(AURORA_DAWN_REF "\([0-9a-f]*\)".*/\1/p' \
    "${repo_root}/extern/aurora/cmake/AuroraDependencyVersions.cmake")"
if [[ "${pinned}" != "${dawn_revision}" ]]; then
    echo "ERROR: aurora pins Dawn ${pinned}, this script builds ${dawn_revision}; update the revision and its hash" >&2
    exit 1
fi
if [[ "$(uname -s)" != "Darwin" || "$(uname -m)" != "arm64" ]]; then
    echo "ERROR: Dawn for visionOS is built on an Apple Silicon Mac" >&2
    exit 1
fi
if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
    # xcrun follows xcode-select, which may point at the Command Line Tools; those have no visionOS SDK.
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
sdk_path="$(xcrun --sdk "${sysroot}" --show-sdk-path 2>/dev/null || true)"
if [[ -z "${sdk_path}" ]]; then
    echo "ERROR: no ${sysroot} SDK; install Xcode with the visionOS platform" >&2
    exit 1
fi
for tool in cmake ninja python3 git; do
    command -v "${tool}" >/dev/null || { echo "ERROR: ${tool} not found" >&2; exit 1; }
done

flags=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_SYSTEM_NAME=visionOS
    "-DCMAKE_OSX_SYSROOT=${sysroot}"
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0
    -DCMAKE_SYSTEM_PROCESSOR=arm64
    -DDAWN_FETCH_DEPENDENCIES=ON
    -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC
    -DBUILD_SHARED_LIBS=OFF
    -DDAWN_ENABLE_INSTALL=ON
    -DDAWN_BUILD_SAMPLES=OFF
    -DDAWN_BUILD_TESTS=OFF
    -DDAWN_BUILD_BENCHMARKS=OFF
    -DDAWN_USE_GLFW=OFF
    -DDAWN_SUPPORTS_GLFW_FOR_WINDOWING=OFF
    -DDAWN_ENABLE_METAL=ON
    -DDAWN_ENABLE_VULKAN=OFF
    -DDAWN_ENABLE_NULL=ON
    -DDAWN_ENABLE_DESKTOP_GL=OFF
    -DDAWN_ENABLE_OPENGLES=OFF
    -DTINT_BUILD_TESTS=OFF
    -DTINT_BUILD_CMD_TOOLS=OFF
    -DTINT_BUILD_IR_BINARY=OFF
    -DTINT_BUILD_MSL_WRITER=ON
    -DTINT_BUILD_HLSL_WRITER=OFF
    -DTINT_BUILD_GLSL_WRITER=OFF
    -DTINT_BUILD_SPV_WRITER=OFF
    -DTINT_BUILD_SPV_READER=OFF
    # Only the IR binary format needs protobuf, and building it would run a protoc
    # cross-compiled for the headset.
    -DDAWN_BUILD_PROTOBUF=OFF
)

# Dawn source fixes for the iOS family's Metal backend; bump fixes_level when they change so cached
# sources and packages are made again. Each edit is anchored on Dawn's text and must match once.
fixes_level=1
apply_visionos_fixes() {
    python3 -I - "$1" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
def widen(rel, anchor):
    path = root / rel
    text = path.read_text()
    old = "#if DAWN_PLATFORM_IS(MACOS)\n" + anchor
    new = "#if DAWN_PLATFORM_IS(MACOS) || DAWN_PLATFORM_IS(IOS)\n" + anchor
    if new in text:
        return
    assert text.count(old) == 1, f"{rel}: anchor not found once"
    path.write_text(text.replace(old, new))
# BC formats to MTLPixelFormat, and their sRGB view reinterpretations (iOS 16.4+, visionOS).
widen("src/dawn/native/metal/UtilsMetal.mm", "        case wgpu::TextureFormat::BC1RGBAUnorm:\n")
widen("src/dawn/native/metal/TextureMTL.mm", "            SRGB_PAIR(MTLPixelFormatBC1_RGBA, MTLPixelFormatBC1_RGBA_sRGB);\n")
PY
}

mkdir -p "${work_dir}"
package_dir="${work_dir}/package-${flavour}"
package_path="${work_dir}/dawn-${flavour}.tar.gz"
manifest="${package_dir}/aurora-dawn.json"
cache_key="$(printf '%s|%s|%s|%s\n' "${dawn_revision}" "${fixes_level}" "${sdk_path##*/}" "${flags[*]}" | shasum -a 256 | awk '{print $1}')"

if [[ ${force} -eq 0 && -f "${manifest}" && -f "${package_path}" ]] &&
   grep -q "\"CacheKey\": \"${cache_key}\"" "${manifest}"; then
    echo "Dawn for ${flavour} is up to date: ${package_path}" >&2
    shasum -a 256 "${package_path}" | awk '{print $1}'
    echo "${package_path}"
    exit 0
fi

source_archive="${work_dir}/dawn-${dawn_revision:0:12}.tar.gz"
if [[ ! -f "${source_archive}" ]]; then
    curl --fail --location --silent --show-error \
        "https://github.com/Odrannnn/dawn/archive/${dawn_revision}.tar.gz" -o "${source_archive}.part"
    mv "${source_archive}.part" "${source_archive}"
fi
actual_sha256="$(shasum -a 256 "${source_archive}" | awk '{print $1}')"
if [[ "${actual_sha256}" != "${dawn_source_sha256}" ]]; then
    echo "ERROR: Dawn source archive hash mismatch: ${actual_sha256}" >&2
    exit 1
fi

source_dir="${work_dir}/src"
source_marker="${dawn_revision}+fixes${fixes_level}"
if [[ ! -f "${source_dir}/.primedgun-revision" ]] ||
   [[ "$(cat "${source_dir}/.primedgun-revision")" != "${source_marker}" ]]; then
    rm -rf "${source_dir}"
    mkdir -p "${source_dir}"
    tar -xzf "${source_archive}" --strip-components=1 -C "${source_dir}"
    apply_visionos_fixes "${source_dir}"
    echo "${source_marker}" > "${source_dir}/.primedgun-revision"
fi

build_dir="${work_dir}/build-${flavour}"
cmake -S "${source_dir}" -B "${build_dir}" -G Ninja "-DPython3_EXECUTABLE=$(command -v python3)" \
    "${flags[@]}" "-DCMAKE_INSTALL_PREFIX=${package_dir}"
cmake --build "${build_dir}" --parallel "${jobs}"
rm -rf "${package_dir}"
cmake --install "${build_dir}"

library="${package_dir}/lib/libwebgpu_dawn.a"
if [[ ! -f "${library}" || ! -f "${package_dir}/lib/cmake/Dawn/DawnConfig.cmake" ]]; then
    echo "ERROR: the Dawn install tree is incomplete: ${package_dir}" >&2
    exit 1
fi
# Debug info would multiply the archive the app links; the stock packages are stripped the same way.
xcrun strip -S "${library}"
ZERO_AR_DATE=1 xcrun ranlib "${library}"

cat > "${manifest}" <<EOF
{
  "SourceRevision": "${dawn_revision}",
  "SourceSha256": "${dawn_source_sha256}",
  "Fixes": ${fixes_level},
  "Sdk": "${sdk_path##*/}",
  "Flags": "${flags[*]}",
  "CacheKey": "${cache_key}",
  "ArchiveSha256": "$(shasum -a 256 "${library}" | awk '{print $1}')"
}
EOF

# A flat archive of the install tree, normalised so the same inputs give the same digest.
find "${package_dir}" -exec touch -h -t 198001010000 {} +
(
    cd "${package_dir}"
    find . -print | LC_ALL=C sort |
        COPYFILE_DISABLE=1 tar -cf - --no-recursion --uid 0 --gid 0 \
            --uname root --gname root --format=ustar -T -
) | gzip -n -9 > "${package_path}"
echo "Dawn for ${flavour}: ${package_path}" >&2
shasum -a 256 "${package_path}" | awk '{print $1}'
echo "${package_path}"
