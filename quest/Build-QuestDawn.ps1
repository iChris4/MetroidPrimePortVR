# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
Builds the Dawn that the Quest APK links: aurora's pinned Dawn with PrimedGun's patches.

.DESCRIPTION
The stock prebuilt Dawn has no Vulkan multiview, which the headset's eye rendering uses (one render
pass draws both eyes), cannot create its Vulkan device through OpenXR, which direct presentation
uses (the eyes are copied straight into the runtime's swapchain images), and has no fragment density
maps, which foveated rendering attaches to the eye passes. This script downloads the
Dawn source aurora pins
(extern/aurora/cmake/AuroraDependencyVersions.cmake, AURORA_DAWN_REF on encounter/dawn), applies
quest/dawn/apply.py, and builds it for Android arm64 with the NDK the way encounter/dawn's own
release workflow does (.github/workflows/release.yml + dawn-ci.cmake). The result is a package in
the stock layout (build/qd/dawn-android-aarch64-primedgun.tar.gz) that Build-Quest.ps1 hands to the
native build as AURORA_DAWN_PACKAGE_URL.

The package is cached: dawn-package.json records the source, the patch hash, the NDK and the flags,
and a later run with the same inputs reuses it. The first build fetches Dawn's dependencies with git
and compiles all of Dawn and Tint, which takes a while.

Prerequisites: Git, Python 3, CMake 3.25+, and the Android SDK with NDK 29.0.14206865 and a CMake
folder (for its Ninja).

.PARAMETER WorkDirectory
Where the source, the build and the package go (default build/qd under the repository; keep it
short, Dawn's paths are deep).

.PARAMETER Force
Rebuilds even when the cached package matches.
#>
[CmdletBinding()]
param(
    [string]$WorkDirectory = '',
    [string]$Python = 'python',
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$root = $PSScriptRoot
$repo = Split-Path -Parent $root
if (-not $WorkDirectory) { $WorkDirectory = Join-Path $repo 'build\qd' }
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
[IO.Directory]::CreateDirectory($WorkDirectory) | Out-Null

# The source revision is aurora's; its archive hash is pinned here, so a Dawn bump in aurora fails
# loudly until this script (and the patches) are checked against the new revision.
$revision = '1155e0ed531126f33a1279afa029349651ca1c93'
$archiveHash = 'd0d291936d02a56b3b7e92e84e8b8c71db04a9c9e2b3a41cc6d7540cf13b4167'
$versions = [IO.File]::ReadAllText((Join-Path $repo 'extern\aurora\cmake\AuroraDependencyVersions.cmake'))
if ($versions -notmatch [regex]::Escape($revision)) {
    throw "aurora pins another Dawn revision than $revision; update `$revision, `$archiveHash and quest\dawn\apply.py"
}

$ndkVersion = '29.0.14206865'
$platform = 'android-28'
# encounter/dawn's release workflow for Android, plus no protobuf (only the IR binary format needs it,
# and building it would run a protoc cross-compiled for Android).
$flags = @(
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_SYSTEM_NAME=Android',
    '-DANDROID_ABI=arm64-v8a',
    "-DANDROID_PLATFORM=$platform",
    '-DCMAKE_SYSTEM_PROCESSOR=aarch64',
    '-DDAWN_BUILD_PROTOBUF=OFF'
)

$sdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
$ndk = Join-Path $sdkRoot "ndk\$ndkVersion"
$toolchainFile = Join-Path $ndk 'build\cmake\android.toolchain.cmake'
if (-not (Test-Path $toolchainFile)) { throw "NDK $ndkVersion not found under $sdkRoot (set ANDROID_HOME)" }
$llvmBin = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$systemNinja = Get-Command ninja.exe -ErrorAction SilentlyContinue
$ninja = if ($systemNinja) { $systemNinja.Source } else { Get-ChildItem -Path (Join-Path $sdkRoot 'cmake') -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'bin\ninja.exe' } |
    Where-Object { Test-Path $_ } | Select-Object -First 1 }
if (-not $ninja) { throw "No Ninja under $sdkRoot\cmake; install the SDK's CMake 3.22.1" }
if ([version](& $ninja --version) -lt [version]'1.11') {
    throw 'Dawn requires Ninja 1.11 or newer; put a current ninja.exe on PATH.'
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git is not on PATH (Dawn fetches its dependencies with it)' }
$pythonPath = (Get-Command $Python -ErrorAction Stop).Source

function Get-TextSha256([string]$text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))
        return -join ($bytes | ForEach-Object { $_.ToString('x2') })
    } finally { $sha.Dispose() }
}

$patchScript = Join-Path $root 'dawn\apply.py'
# Every file the patches consist of: quest/dawn and the ABI header apply.py copies in from aurora.
# Line endings vary between checkouts; hash their text with LF only.
$patchFiles = @(Get-ChildItem -LiteralPath (Join-Path $root 'dawn') -File | Sort-Object Name | ForEach-Object { $_.FullName }) +
    @(Join-Path $repo 'extern\aurora\include\aurora\dawn_vulkan_abi.h') +
    @(Join-Path $repo 'extern\aurora\include\aurora\dawn_fdm_abi.h')
$patchHash = Get-TextSha256 (($patchFiles | ForEach-Object {
    (Split-Path -Leaf $_) + "`n" + [IO.File]::ReadAllText($_).Replace("`r`n", "`n")
}) -join "`n")
$cacheKey = Get-TextSha256 "$revision|$archiveHash|$patchHash|$ndkVersion|$($flags -join ' ')"

# The package is named after its content (dawn-android-aarch64-primedgun-<sha>.tar.gz, recorded in
# dawn-package.json as PackageFile): the native build's FetchContent only extracts a URL again
# when the URL changes.
$manifestPath = Join-Path $WorkDirectory 'dawn-package.json'
if (-not $Force -and (Test-Path $manifestPath)) {
    $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    $cached = if ($manifest.PSObject.Properties['PackageFile']) { Join-Path $WorkDirectory $manifest.PackageFile } else { '' }
    if ($cached -and (Test-Path $cached) -and $manifest.CacheKey -eq $cacheKey -and
        $manifest.PackageSha256 -eq (Get-FileHash -LiteralPath $cached -Algorithm SHA256).Hash.ToLowerInvariant()) {
        Write-Host "Patched Dawn for the Quest is up to date: $cached"
        return
    }
}

$archive = Join-Path $WorkDirectory 'dawn-src.tar.gz'
if (-not (Test-Path -LiteralPath $archive)) {
    Write-Host "Downloading Dawn $revision"
    Invoke-WebRequest "https://github.com/encounter/dawn/archive/$revision.tar.gz" -OutFile $archive -UseBasicParsing
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $archiveHash) {
    throw 'The Dawn source archive does not match the pinned SHA-256.'
}

# The patches go onto pristine sources: src/ is extracted again whenever they changed, while
# third_party/ (which DAWN_FETCH_DEPENDENCIES fills) is kept.
$source = Join-Path $WorkDirectory 's'
$patchMarker = Join-Path $source 'primedgun-patches.sha256'
$patched = (Test-Path -LiteralPath $patchMarker) -and ([IO.File]::ReadAllText($patchMarker).Trim() -eq $patchHash)
if (-not $patched) {
    $extract = @'
import os, shutil, sys, tarfile
archive, work, source, only_src = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] == "1"
with tarfile.open(archive) as tar:
    members = tar.getmembers()
    top = members[0].name.split("/")[0]
    if only_src:
        members = [m for m in members if m.name.split("/", 2)[1:2] == ["src"]]
    tar.extractall(work, members=members, filter="data")
extracted = os.path.join(work, top)
if only_src:
    shutil.rmtree(os.path.join(source, "src"), ignore_errors=True)
    shutil.move(os.path.join(extracted, "src"), os.path.join(source, "src"))
    shutil.rmtree(extracted, ignore_errors=True)
else:
    shutil.rmtree(source, ignore_errors=True)
    os.rename(extracted, source)
'@
    $extractScript = Join-Path $WorkDirectory 'extract.py'
    [IO.File]::WriteAllText($extractScript, $extract)
    $onlySource = Test-Path -LiteralPath (Join-Path $source 'CMakeLists.txt')
    Write-Host ("Extracting Dawn " + $(if ($onlySource) { 'src/' } else { 'sources' }))
    & $pythonPath $extractScript $archive $WorkDirectory $source $(if ($onlySource) { '1' } else { '0' })
    if ($LASTEXITCODE -ne 0) { throw 'Dawn source extraction failed.' }
    & $pythonPath $patchScript $source
    if ($LASTEXITCODE -ne 0) { throw 'Applying the PrimedGun Dawn patches failed.' }
    [IO.File]::WriteAllText($patchMarker, $patchHash)
}

$build = Join-Path $WorkDirectory 'b'
$install = Join-Path $WorkDirectory 'p'
$ciCache = Join-Path $source '.github\workflows\dawn-ci.cmake'
# The flags go before -C, as in encounter's workflow: dawn-ci.cmake branches on CMAKE_SYSTEM_NAME,
# and without it takes the host's (Windows) branch, which forces a shared library and builds DXC.
& cmake -S $source -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile" `
    @flags -C $ciCache "-DPython3_EXECUTABLE=$pythonPath" "-DCMAKE_INSTALL_PREFIX=$install"
if ($LASTEXITCODE -ne 0) { throw 'Dawn configuration failed.' }
& cmake --build $build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Dawn build failed.' }
if (Test-Path -LiteralPath $install) { Remove-Item -LiteralPath $install -Recurse -Force }
& cmake --install $build
if ($LASTEXITCODE -ne 0) { throw 'Dawn installation failed.' }
$library = Join-Path $install 'lib\libwebgpu_dawn.a'
if (-not (Test-Path -LiteralPath $library)) { throw "Dawn archive missing: $library" }
# As the stock package: debug info would multiply the archive's size.
& (Join-Path $llvmBin 'llvm-strip.exe') --strip-debug $library
if ($LASTEXITCODE -ne 0) { throw 'Stripping the Dawn archive failed.' }

$pack = @'
import sys, tarfile
install, out = sys.argv[1], sys.argv[2]
with tarfile.open(out, "w:gz") as tar:
    tar.add(install, arcname=".")
'@
$packScript = Join-Path $WorkDirectory 'pack.py'
[IO.File]::WriteAllText($packScript, $pack)
$packing = Join-Path $WorkDirectory 'dawn-android-aarch64-primedgun.partial.tar.gz'
& $pythonPath $packScript $install $packing
if ($LASTEXITCODE -ne 0) { throw 'Packing Dawn failed.' }
$packageHash = (Get-FileHash -LiteralPath $packing -Algorithm SHA256).Hash.ToLowerInvariant()
$packageFile = "dawn-android-aarch64-primedgun-$($packageHash.Substring(0, 12)).tar.gz"
Get-ChildItem -Path $WorkDirectory -Filter 'dawn-android-aarch64-primedgun*.tar.gz' |
    Where-Object { $_.Name -ne $packageFile -and $_.FullName -ne $packing } | Remove-Item -Force
Move-Item -LiteralPath $packing -Destination (Join-Path $WorkDirectory $packageFile) -Force

$manifestJson = [ordered]@{
    SourceRevision = $revision
    SourceSha256 = $archiveHash
    PatchSha256 = $patchHash
    Ndk = $ndkVersion
    Flags = ($flags -join ' ')
    CacheKey = $cacheKey
    PackageFile = $packageFile
    PackageSha256 = $packageHash
} | ConvertTo-Json
[IO.File]::WriteAllText($manifestPath, $manifestJson, (New-Object Text.UTF8Encoding $false))
Write-Host "Patched Dawn for the Quest ready: $(Join-Path $WorkDirectory $packageFile)"
