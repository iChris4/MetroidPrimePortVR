#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
home=${HOME:-/nonexistent}
sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$home/android/sdk}}
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}

if [ ! -d "$sdk" ]; then
    echo "error: Android SDK not found; set ANDROID_SDK_ROOT" >&2
    exit 1
fi
# The NDK app/build.gradle pins (ndkVersion) first: ANDROID_NDK_HOME set here
# becomes Gradle's ndkPath, which overrides ndkVersion.
pinned_ndk=$(sed -n 's/^[[:space:]]*ndkVersion[[:space:]]*"\([^"]*\)".*/\1/p' "$root_dir/android/app/build.gradle")
if [ -z "$ndk" ] && [ -n "$pinned_ndk" ] && [ -f "$sdk/ndk/$pinned_ndk/source.properties" ]; then
    ndk="$sdk/ndk/$pinned_ndk"
fi
if [ -z "$ndk" ]; then
    for candidate in "$sdk"/ndk/* "$home"/android-ndk-cache/android-ndk-*; do
        if [ -f "$candidate/source.properties" ]; then
            ndk=$candidate
        fi
    done
    if [ -n "$ndk" ]; then
        echo "note: NDK $pinned_ndk (app/build.gradle) is not installed; using $ndk" >&2
    fi
fi
if [ -z "$ndk" ] || [ ! -f "$ndk/source.properties" ]; then
    echo "error: Android NDK not found; set ANDROID_NDK_HOME" >&2
    exit 1
fi
ANDROID_NDK_HOME=$ndk
export ANDROID_NDK_HOME

gradle=${GRADLE:-}
if [ -z "$gradle" ]; then
    gradle=$(command -v gradle || true)
fi
if [ -z "$gradle" ]; then
    for candidate in "$home"/.gradle/wrapper/dists/gradle-8.*/*/gradle-8.*/bin/gradle; do
        if [ -x "$candidate" ]; then
            gradle=$candidate
        fi
    done
fi
if [ -z "$gradle" ]; then
    echo "error: Gradle 8.x not found; set GRADLE" >&2
    exit 1
fi

repo_cargo="$root_dir/build/android-rust/cargo"
repo_rustup="$root_dir/build/android-rust/rustup"
if [ -x "$repo_cargo/bin/cargo" ]; then
    CARGO_HOME=$repo_cargo
    RUSTUP_HOME=$repo_rustup
    PATH="$repo_cargo/bin:$PATH"
    export CARGO_HOME RUSTUP_HOME PATH
fi

# The NDK names its host directory darwin-x86_64 on Apple Silicon as well.
case "$(uname -s)" in
    Darwin) ndk_host=darwin-x86_64 ;;
    *) ndk_host=linux-x86_64 ;;
esac
ndk_bin="$ndk/toolchains/llvm/prebuilt/$ndk_host/bin"
CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$ndk_bin/aarch64-linux-android28-clang"
CC_aarch64_linux_android=$CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER
CXX_aarch64_linux_android="$ndk_bin/aarch64-linux-android28-clang++"
AR_aarch64_linux_android="$ndk_bin/llvm-ar"
RANLIB_aarch64_linux_android="$ndk_bin/llvm-ranlib"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER CC_aarch64_linux_android
export CXX_aarch64_linux_android AR_aarch64_linux_android RANLIB_aarch64_linux_android

escape_property() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/:/\\:/g; s/ /\\ /g'
}

# Only sdk.dir is ours; anything else in local.properties is kept.
properties="$root_dir/android/local.properties"
{
    printf 'sdk.dir=%s\n' "$(escape_property "$sdk")"
    if [ -f "$properties" ]; then
        grep -v '^sdk\.dir=' "$properties" || true
    fi
} > "$properties.tmp"
mv "$properties.tmp" "$properties"

# A release is normally signed with this project's own key, configured in
# android/keystore.properties (see tools/make_android_keystore.sh). When there
# is no key, :app:assembleRelease stops rather than quietly signing with the
# shared debug key, so this script opts in on the caller's behalf for a build
# that is only going to be sideloaded - and says so, because a debug-signed
# package cannot be replaced by a properly signed one without uninstalling it
# first. A build that must not produce a debug-signed package passes
# --strict-signing and gets the refusal instead.
strict=0
# Consumed here rather than forwarded: Gradle rejects an option it does not
# know, so leaving --strict-signing in the argument list fails the build with a
# usage dump instead of doing what it says. Rotating the positional parameters
# is the POSIX way to drop one without an array.
remaining_count=$#
index=0
while [ "$index" -lt "$remaining_count" ]; do
    argument=$1
    shift
    if [ "$argument" = "--strict-signing" ]; then
        strict=1
    else
        set -- "$@" "$argument"
    fi
    index=$((index + 1))
done

key_configured=0
[ -f "$root_dir/android/keystore.properties" ] && key_configured=1
[ -n "${MP_APK_KEYSTORE:-}" ] && key_configured=1
if [ "$key_configured" -eq 0 ]; then
    if [ "$strict" -eq 1 ]; then
        echo "error: --strict-signing and no release key configured." >&2
        echo "       run tools/make_android_keystore.sh first." >&2
        exit 1
    fi
    echo "note: no release key configured; signing with the shared debug key." >&2
    echo "      This build is only good for sideloading. To sign it properly," >&2
    echo "      run tools/make_android_keystore.sh. Use --strict-signing to refuse instead." >&2
    set -- "$@" -PmpDebugSigning=true
fi

exec "$gradle" --project-dir "$root_dir/android" --no-daemon "$@"
