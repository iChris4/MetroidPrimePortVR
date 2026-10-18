#!/usr/bin/env bash
# Collects every notice a shipped build has to carry into one directory: the
# port's own grant and notice, the vendored snapshots' licences, the licences
# of everything FetchContent fetched into <build-dir>/_deps, and the texts in
# packaging/licenses/ for prebuilt packages that ship none (Dawn, nod) and for
# generated data tables whose licence lives only in the project they came from.
#
# Usage: tools/collect_licenses.sh <build-dir> <dest-dir>
#
# Each fetched package's first licence file is filed as <dep>.txt (e.g.
# sdl-src.txt); further ones (zstd's COPYING, a dual licence) as
# <dep>-<file>, and notices further down a package's tree under
# <dep>/<their path>. Run it after the build: the fetched sources exist only
# then. Every release package uses it (the AppImage and tarball, the Windows
# zip and the macOS app), so they all carry the same set.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <build-dir> <dest-dir>" >&2
    exit 2
fi
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$1
DEST=$2
mkdir -p "$DEST"

copy() {
    # $1 = source file, $2 = name it is filed under. Missing sources are skipped.
    [[ -f "$1" ]] || return 0
    cp "$1" "$DEST/$2"
}

copy "$ROOT/LICENSE" port-license.txt
copy "$ROOT/NOTICE" port-notice.txt
copy "$ROOT/extern/aurora/LICENSE" aurora.txt
copy "$ROOT/extern/musyx/LICENSE" musyx.txt
copy "$ROOT/extern/astcenc/LICENSE.txt" astcenc.txt
copy "$ROOT/extern/GamepadMotionHelpers/LICENSE" gamepadmotionhelpers.txt
# freetype's LICENSE.TXT only points at the licence it is used under, and the
# fetched loop below only looks at the package's top level.
copy "$BUILD"/_deps/freetype-src/docs/FTL.TXT freetype-FTL.txt
for notice in "$ROOT"/packaging/licenses/*.txt; do
    copy "$notice" "$(basename "$notice")"
done

found=0
shopt -s nullglob nocaseglob
for src in "$BUILD"/_deps/*-src; do
    dep=$(basename "$src")
    first=1
    for notice in "$src"/LICENSE* "$src"/COPYING* "$src"/NOTICE*; do
        [[ -f "$notice" ]] || continue
        if [[ $first = 1 ]]; then
            copy "$notice" "$dep.txt"
            first=0
        else
            copy "$notice" "$dep-$(basename "$notice")"
        fi
        found=$((found + 1))
    done
done
shopt -u nullglob nocaseglob

# Notices deeper inside a fetched package cover code it bundles (SDL's hidapi
# and yuv2rgb, Tracy's libbacktrace, ...). They keep their path, under
# <dep>/, since their names repeat; a package's own build/ trees (copies made
# while building it) are skipped. find rather than globstar: macOS ships bash 3.2.
for src in "$BUILD"/_deps/*-src; do
    [[ -d "$src" ]] || continue
    dep=$(basename "$src")
    (cd "$src" && find . -mindepth 2 -type f -not -path '*/build/*' \
        \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' \) -print) |
        while IFS= read -r notice; do
            notice=${notice#./}
            mkdir -p "$DEST/$dep/$(dirname "$notice")"
            copy "$src/$notice" "$dep/$notice"
        done
done

if [[ $found = 0 ]]; then
    echo "warning: no fetched package notices under $BUILD/_deps" >&2
fi
echo "collected $(find "$DEST" -maxdepth 1 -type f | wc -l) notice file(s) into $DEST"
