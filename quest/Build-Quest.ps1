# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
Builds PrimedGun v2's Meta Quest APK on a Windows host.

.DESCRIPTION
Runs this folder's Gradle project (Gradle 8.13 wrapper, Android Gradle plugin
8.13.2), which builds the game's native library from the repository's
CMakeLists.txt with the OpenXR Vulkan path on.

Prerequisites: JDK 17 (JAVA_HOME), the Android SDK with NDK 29.0.14206865 and a
CMake folder (for its Ninja), a system CMake 3.25+ on PATH, git, and Rust with the
aarch64-linux-android target (nod, the disc reader, is built from source).

A release is signed with this project's own key (quest/keystore.properties, see
README.md). Without one, the build signs it with the shared debug key, which is
fine for sideloading: the switch to a real key later means uninstalling, and the
app's data goes with it.

.PARAMETER Configuration
release (the default) or debug. Both build optimised native code.

.PARAMETER Install
Installs the APK on the connected headset with adb.

.PARAMETER StockDawn
Links aurora's stock prebuilt Dawn instead of PrimedGun's patched one
(Build-QuestDawn.ps1), so the eyes render in a pass each instead of one multiview
pass.

.PARAMETER Unsigned
Builds an unsigned release APK for the release maintainer to sign with the
existing update key. This APK cannot be installed until it has been signed.

.EXAMPLE
quest\Build-Quest.ps1 -Install
#>
[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')] [string]$Configuration = 'release',
    [string]$CMakeDir = '',
    [switch]$Install,
    [switch]$Unsigned,
    [switch]$StockDawn
)
$ErrorActionPreference = 'Stop'
if ($Unsigned -and ($Configuration -ne 'release' -or $Install)) {
    throw '-Unsigned requires release configuration and cannot be used with -Install.'
}

$root = $PSScriptRoot
$repo = Split-Path -Parent $root
$ndkVersion = '29.0.14206865'

$javaHome = if ($env:JAVA_HOME) { $env:JAVA_HOME } else { 'C:\Program Files\Java\jdk-17' }
$sdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
if (-not (Test-Path (Join-Path $javaHome 'bin\java.exe'))) { throw "JDK 17 not found at $javaHome (set JAVA_HOME)" }
if (-not (Test-Path $sdkRoot)) { throw "Android SDK not found at $sdkRoot (set ANDROID_HOME)" }
if (-not (Test-Path (Join-Path $sdkRoot "ndk\$ndkVersion"))) { throw "NDK $ndkVersion not found under $sdkRoot\ndk" }
$env:JAVA_HOME = $javaHome
$env:ANDROID_HOME = $sdkRoot
$env:ANDROID_SDK_ROOT = $sdkRoot

if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) { throw 'cargo is not on PATH (Rust builds nod, the disc reader)' }
$rustTargets = & rustup target list --installed 2>$null
if ($rustTargets -notcontains 'aarch64-linux-android') {
    throw 'Rust target aarch64-linux-android is missing: rustup target add aarch64-linux-android'
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git is not on PATH (CMake fetches nod with it)' }

# The port needs CMake 3.25+, newer than the SDK's bundled CMake: point the Android
# Gradle plugin at a system CMake and put the SDK's Ninja on PATH for it.
if (-not $CMakeDir) {
    $cmakeExe = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmakeExe) { $CMakeDir = Split-Path -Parent (Split-Path -Parent $cmakeExe.Source) }
}
if (-not $CMakeDir -or -not (Test-Path (Join-Path $CMakeDir 'bin\cmake.exe'))) {
    throw 'No CMake 3.25+ found; install one or pass -CMakeDir <install root>'
}
$systemNinja = Get-Command ninja.exe -ErrorAction SilentlyContinue
$ninjaPath = if ($systemNinja) { $systemNinja.Source } else { Get-ChildItem -Path (Join-Path $sdkRoot 'cmake') -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    Where-Object { Test-Path (Join-Path $_.FullName 'bin\ninja.exe') } |
    Select-Object -First 1 | ForEach-Object { Join-Path $_.FullName 'bin\ninja.exe' } }
if (-not $ninjaPath) { throw "No ninja.exe on PATH or under $sdkRoot\cmake\*\bin" }
if ([version](& $ninjaPath --version) -lt [version]'1.11') {
    throw 'The Quest build requires Ninja 1.11 or newer; put a current ninja.exe on PATH.'
}
$env:PATH = (Split-Path -Parent $ninjaPath) + ';' + $env:PATH
# Parenthesised: PowerShell's comma binds tighter than +.
$localProperties = @(
    ('sdk.dir=' + $sdkRoot.Replace('\', '\\')),
    ('cmake.dir=' + $CMakeDir.Replace('\', '\\'))
)
Set-Content -Path (Join-Path $root 'local.properties') -Value $localProperties -Encoding ascii

# app/build.gradle.kts repeats upstream's phone build arguments; say so when upstream changes them.
function Get-DefineNames([string]$text) {
    [regex]::Matches($text, '"-D([A-Za-z0-9_]+)=') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
}
$upstreamGradle = Get-Content -Raw (Join-Path $repo 'android\app\build.gradle')
$upstreamBlock = [regex]::Match($upstreamGradle, '(?s)def cmakeArguments = \[(.*?)\]').Groups[1].Value
$questGradle = Get-Content -Raw (Join-Path $root 'app\build.gradle.kts')
$questBlock = [regex]::Match($questGradle, '(?s)val upstreamCmakeArguments = listOf\((.*?)\)').Groups[1].Value
$drift = Compare-Object @(Get-DefineNames $upstreamBlock) @(Get-DefineNames $questBlock) |
    Where-Object { $_.InputObject -ne 'Rust_CARGO_TARGET' }
if ($drift) {
    Write-Warning ("upstream's android/app/build.gradle CMake arguments differ from quest/app/build.gradle.kts: " +
        (($drift | ForEach-Object { "$($_.InputObject) ($($_.SideIndicator))" }) -join ', '))
}

$gradleArgs = @('--project-dir', $root, '--console=plain')
if (-not $StockDawn) {
    # PrimedGun's patched Dawn (Vulkan multiview), built once and then reused from its cache.
    & (Join-Path $root 'Build-QuestDawn.ps1')
    $dawnManifest = [IO.File]::ReadAllText((Join-Path $repo 'build\qd\dawn-package.json')) | ConvertFrom-Json
    $dawnPackage = Join-Path $repo "build\qd\$($dawnManifest.PackageFile)"
    if (-not (Test-Path $dawnPackage)) { throw "Patched Dawn package missing: $dawnPackage" }
    $gradleArgs += "-PquestDawnPackage=$dawnPackage"
}
if ($Unsigned) {
    $gradleArgs += '-PquestUnsignedRelease=true'
} elseif ($Configuration -eq 'release' -and -not (Test-Path (Join-Path $root 'keystore.properties')) -and
    -not $env:PRIMEDGUN_QUEST_KEYSTORE) {
    Write-Host 'No release key configured: signing with the shared debug key, for sideloading.'
    $gradleArgs += '-PquestDebugSigning=true'
}
$variant = (Get-Culture).TextInfo.ToTitleCase($Configuration)
$gradleArgs += "app:assemble$variant"

# The Gradle daemon can block on a console pipe; its output goes to log files instead.
$logDir = Join-Path $root 'app\build'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir "gradle-$Configuration.log"
$errorLog = Join-Path $logDir "gradle-$Configuration.err.log"
Write-Host "gradlew $($gradleArgs -join ' ')  (log: $log)"
$quotedArgs = $gradleArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
# Not -Wait: that also waits for the Gradle daemon gradlew leaves running.
$gradle = Start-Process -FilePath (Join-Path $root 'gradlew.bat') -ArgumentList $quotedArgs -NoNewWindow -PassThru `
    -RedirectStandardOutput $log -RedirectStandardError $errorLog
$null = $gradle.Handle  # keeps ExitCode readable after the exit
$gradle.WaitForExit()
if ($gradle.ExitCode -ne 0) {
    Get-Content $log -Tail 40
    Get-Content $errorLog -Tail 40
    throw "Gradle failed ($($gradle.ExitCode)); logs: $log, $errorLog"
}

$apkDir = Join-Path $root "app\build\outputs\apk\$Configuration"
$apk = Get-ChildItem -Path $apkDir -Filter '*.apk' | Select-Object -First 1
if (-not $apk) { throw "No APK under $apkDir" }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($apk.FullName)
try {
    $game = @($archive.Entries | Where-Object { $_.FullName -eq 'lib/arm64-v8a/libmetroid_prime_port.so' }) | Select-Object -First 1
    $launcher = @($archive.Entries | Where-Object { $_.FullName -eq 'lib/arm64-v8a/libprimedgun_launcher.so' }) | Select-Object -First 1
} finally { $archive.Dispose() }
if (-not $game) { throw "The APK has no lib/arm64-v8a/libmetroid_prime_port.so" }
if (-not $launcher) { throw "The APK has no lib/arm64-v8a/libprimedgun_launcher.so (the launcher panel's native half)" }
Write-Host ("APK: {0} ({1:N1} MB; game library {2:N1} MB)" -f $apk.FullName, ($apk.Length / 1MB), ($game.Length / 1MB))

$aapt2 = Get-ChildItem -Path (Join-Path $sdkRoot 'build-tools') -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'aapt2.exe' } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1
if ($aapt2) {
    & $aapt2 dump badging $apk.FullName 2>$null |
        Select-String -Pattern '^(package|launchable-activity|uses-feature|uses-permission|sdkVersion|targetSdkVersion)' |
        ForEach-Object { Write-Host "  $_" }
}

if ($Install) {
    $adb = Join-Path $sdkRoot 'platform-tools\adb.exe'
    & $adb install -r -g $apk.FullName
    if ($LASTEXITCODE -ne 0) { throw "adb install failed ($LASTEXITCODE)" }
    Write-Host 'Installed. quest\Run-Quest.ps1 starts it and collects the logs.'
}
