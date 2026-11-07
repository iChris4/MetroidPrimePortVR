# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
Builds the Dawn that PC VR on Vulkan needs: aurora's pinned Dawn with PrimedGun's patches, for Windows x64.

.DESCRIPTION
The stock prebuilt Dawn cannot create its Vulkan device through OpenXR, which Vulkan VR on the PC
uses (the runtime creates Dawn's VkInstance and VkDevice through XR_KHR_vulkan_enable2, and the eyes
are drawn straight into the runtime's swapchain images, platform/vr/openxr_vulkan_direct.cpp). This
script downloads the Dawn source aurora pins (extern/aurora/cmake/AuroraDependencyVersions.cmake,
AURORA_DAWN_REF on Odrannnn/dawn), applies quest/dawn/apply.py (the same patches as the Quest's
Dawn), and builds it the way encounter/dawn's release workflow builds dawn-windows-amd64
(.github/workflows/release.yml + dawn-ci.cmake: MSVC, Ninja, a shared webgpu_dawn.dll and the DXC it
builds), so the D3D12 backend behaves as with the stock DLL. The result is a package in the stock
layout (build/wd/dawn-windows-amd64-primedgun-<sha>.tar.gz) to hand to a native build as
-DAURORA_DAWN_PACKAGE_URL=<path, forward slashes> (a file:/// URL fails on Windows); the script
prints the line. A build with the stock Dawn still runs; only Vulkan VR then reports that it needs
this one.

The package is cached: dawn-package.json records the source, the patch hash, the compiler and the
flags, and a later run with the same inputs reuses it. The first build fetches Dawn's dependencies
with git and compiles all of Dawn, Tint and DXC, which takes a while.

Run it from an x64 Visual Studio 2022 developer environment (cl.exe, INCLUDE and LIB set, the Windows
SDK included). Also needed: Git, Python 3, CMake 3.25+ and Ninja 1.11+ (VS's bundled one is fine).

.PARAMETER WorkDirectory
Where the source, the build and the package go (default build/wd under the repository; keep it
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

$repo = Split-Path -Parent $PSScriptRoot
if (-not $WorkDirectory) { $WorkDirectory = Join-Path $repo 'build\wd' }
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
[IO.Directory]::CreateDirectory($WorkDirectory) | Out-Null

# The source revision is aurora's; its archive hash is pinned here, as in quest/Build-QuestDawn.ps1,
# so a Dawn bump in aurora fails loudly until both scripts (and the patches) are checked against it.
$revision = 'bfa890082bf20bc10e0a3bfe122cf0bfa7f4da61'
$archiveHash = 'e0cb2aabea2bde5957ab6bd5e7919c5ed61dd73dd8237027bd5755a9c91bf237'
$versions = [IO.File]::ReadAllText((Join-Path $repo 'extern\aurora\cmake\AuroraDependencyVersions.cmake'))
$pinnedRevision = [regex]::Match($versions, '_aurora_dependency_version\(AURORA_DAWN_REF\s+"([a-f0-9]+)"').Groups[1].Value
if ($pinnedRevision -ne $revision) {
    throw "aurora pins another Dawn revision than $revision; update `$revision, `$archiveHash and quest\dawn\apply.py"
}

# The compiler and the Windows SDK come from the developer environment.
$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cl -or -not $env:INCLUDE -or -not $env:LIB) {
    throw 'Run this from an x64 Visual Studio developer environment (cl.exe, INCLUDE and LIB).'
}
$sdkPath = if ($env:WindowsSdkDir) { $env:WindowsSdkDir } else { '' }
$sdkVersion = if ($env:WindowsSDKVersion) { $env:WindowsSDKVersion.TrimEnd('\') } else { '' }
if (-not $sdkPath -or -not $sdkVersion) {
    throw 'WindowsSdkDir and WindowsSDKVersion are not set: the developer environment has no Windows SDK.'
}
# dawn-ci.cmake reads these (the release workflow's names).
$env:WIN10_SDK_PATH = $sdkPath
$env:WIN10_SDK_VERSION = $sdkVersion
$dxil = Join-Path $sdkPath "bin\$sdkVersion\x64\dxil.dll"
if (-not (Test-Path -LiteralPath $dxil)) { throw "dxil.dll not found in the Windows SDK: $dxil" }
# cl prints its banner on stderr, which Windows PowerShell turns into errors when redirected.
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
$compiler = (& $cl.Source 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -match 'Version' } |
    Select-Object -First 1)
if (-not $compiler) { throw 'Could not read the version of cl.exe.' }
$compiler = $compiler.Trim()
$ErrorActionPreference = $previousPreference

$ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue
if (-not $ninja -or [version]((& $ninja.Source --version).Trim()) -lt [version]'1.11') {
    throw 'Dawn requires Ninja 1.11 or newer on PATH (the developer environment has one).'
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git is not on PATH (Dawn fetches its dependencies with it)' }
$pythonPath = (Get-Command $Python -ErrorAction Stop).Source

# encounter/dawn's release workflow for dawn-windows-amd64 (dawn-ci.cmake's Windows branch builds a
# shared library and DXC), plus no protobuf: only Tint's IR binary format needs it, which is off.
$flags = @(
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_C_COMPILER=cl',
    '-DCMAKE_CXX_COMPILER=cl',
    '-DDAWN_BUILD_PROTOBUF=OFF'
)

function Get-TextSha256([string]$text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text))
        return -join ($bytes | ForEach-Object { $_.ToString('x2') })
    } finally { $sha.Dispose() }
}

$patchDir = Join-Path $repo 'quest\dawn'
$patchScript = Join-Path $patchDir 'apply.py'
# Every file the patches consist of: quest/dawn and the ABI headers apply.py copies in from aurora.
# Line endings vary between checkouts; hash their text with LF only.
$patchFiles = @(Get-ChildItem -LiteralPath $patchDir -File | Sort-Object Name | ForEach-Object { $_.FullName }) +
    @(Join-Path $repo 'extern\aurora\include\aurora\dawn_vulkan_abi.h') +
    @(Join-Path $repo 'extern\aurora\include\aurora\dawn_fdm_abi.h')
$patchHash = Get-TextSha256 (($patchFiles | ForEach-Object {
    (Split-Path -Leaf $_) + "`n" + [IO.File]::ReadAllText($_).Replace("`r`n", "`n")
}) -join "`n")
$cacheKey = Get-TextSha256 "$revision|$archiveHash|$patchHash|$compiler|$sdkVersion|$($flags -join ' ')"

# The package is named after its content, so a native build's FetchContent extracts it again when
# it changes (the URL changes with it).
$manifestPath = Join-Path $WorkDirectory 'dawn-package.json'
if (-not $Force -and (Test-Path $manifestPath)) {
    $manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    $cached = if ($manifest.PSObject.Properties['PackageFile']) { Join-Path $WorkDirectory $manifest.PackageFile } else { '' }
    if ($cached -and (Test-Path $cached) -and $manifest.CacheKey -eq $cacheKey -and
        $manifest.PackageSha256 -eq (Get-FileHash -LiteralPath $cached -Algorithm SHA256).Hash.ToLowerInvariant()) {
        Write-Host "Patched Dawn for Windows is up to date: $cached"
        return
    }
}

$archive = Join-Path $WorkDirectory "dawn-$($revision.Substring(0, 12)).tar.gz"
if (-not (Test-Path -LiteralPath $archive)) {
    Write-Host "Downloading Dawn $revision"
    Invoke-WebRequest "https://github.com/Odrannnn/dawn/archive/$revision.tar.gz" -OutFile $archive -UseBasicParsing
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $archiveHash) {
    throw 'The Dawn source archive does not match the pinned SHA-256.'
}

# Restore every file in the pinned source archive, preserving identical files and fetched
# dependencies, then patch it (apply.py skips edits that are already there).
$source = Join-Path $WorkDirectory 's'
$patchMarker = Join-Path $source 'primedgun-patches.sha256'
$sourceKey = "$revision|$patchHash"
$patched = (Test-Path -LiteralPath $patchMarker) -and ([IO.File]::ReadAllText($patchMarker).Trim() -eq $sourceKey)
if (-not $patched) {
    $extract = @'
from pathlib import Path
import os, sys, tarfile
archive, work, source = sys.argv[1:]
source = Path(source).resolve()
work = Path(work).resolve()
assert source.is_relative_to(work), "Dawn source must stay inside its build work directory"
# A lexical check per member: resolve() costs a file-system round trip each, which made a
# re-extraction over an existing tree take most of an hour on Windows.
for_check = os.path.normcase(str(source)) + os.sep
with tarfile.open(archive, "r|gz") as tar:
    for member in tar:
        parts = Path(member.name).parts[1:]
        if not parts:
            continue
        target = source.joinpath(*parts)
        assert ".." not in parts and os.path.normcase(os.path.normpath(target)).startswith(for_check), \
            "Archive path escapes Dawn source"
        # The submodules Dawn fetches (third_party/abseil-cpp, .../src) are empty directories in
        # the archive, and fetch_dawn_dependencies.py runs git inside them: they must exist.
        if member.isdir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        if not member.isfile():
            continue
        data = tar.extractfile(member).read()
        try:
            same = target.stat().st_size == len(data) and target.read_bytes() == data
        except FileNotFoundError:
            same = False
        if not same:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
'@
    $extractScript = Join-Path $WorkDirectory 'extract.py'
    [IO.File]::WriteAllText($extractScript, $extract)
    Write-Host 'Restoring pinned Dawn source files'
    & $pythonPath $extractScript $archive $WorkDirectory $source
    if ($LASTEXITCODE -ne 0) { throw 'Dawn source extraction failed.' }
    & $pythonPath $patchScript $source
    if ($LASTEXITCODE -ne 0) { throw 'Applying the PrimedGun Dawn patches failed.' }
    [IO.File]::WriteAllText($patchMarker, $sourceKey)
}

$build = Join-Path $WorkDirectory 'b'
$install = Join-Path $WorkDirectory 'p'
$ciCache = Join-Path $source '.github\workflows\dawn-ci.cmake'
& cmake -S $source -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$($ninja.Source)" @flags -C $ciCache `
    "-DPython3_EXECUTABLE=$pythonPath" "-DCMAKE_INSTALL_PREFIX=$install"
if ($LASTEXITCODE -ne 0) { throw 'Dawn configuration failed.' }
& cmake --build $build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Dawn build failed.' }
if (Test-Path -LiteralPath $install) {
    $resolvedInstall = (Resolve-Path -LiteralPath $install).Path
    $workPrefix = $WorkDirectory.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedInstall.StartsWith($workPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Dawn install path escapes its build directory: $resolvedInstall"
    }
    Remove-Item -LiteralPath $resolvedInstall -Recurse -Force
}
& cmake --install $build
if ($LASTEXITCODE -ne 0) { throw 'Dawn installation failed.' }
$library = Join-Path $install 'bin\webgpu_dawn.dll'
if (-not (Test-Path -LiteralPath $library)) { throw "Dawn library missing: $library" }
if (-not (Test-Path -LiteralPath (Join-Path $install 'lib\webgpu_dawn.lib'))) { throw 'webgpu_dawn.lib is missing from the Dawn install.' }
# As the release workflow: the DXC this build made (DAWN_USE_BUILT_DXC), which the install
# leaves out, and its signing library from the Windows SDK.
$dxcompiler = Join-Path $build 'dxcompiler.dll'
if (-not (Test-Path -LiteralPath $dxcompiler)) { throw "dxcompiler.dll is missing from the Dawn build: $dxcompiler" }
Copy-Item -LiteralPath $dxcompiler -Destination (Join-Path $install 'bin\dxcompiler.dll') -Force
Copy-Item -LiteralPath $dxil -Destination (Join-Path $install 'bin\dxil.dll') -Force

$pack = @'
import sys, tarfile
install, out = sys.argv[1], sys.argv[2]
with tarfile.open(out, "w:gz") as tar:
    tar.add(install, arcname=".")
'@
$packScript = Join-Path $WorkDirectory 'pack.py'
[IO.File]::WriteAllText($packScript, $pack)
$packing = Join-Path $WorkDirectory 'dawn-windows-amd64-primedgun.partial.tar.gz'
& $pythonPath $packScript $install $packing
if ($LASTEXITCODE -ne 0) { throw 'Packing Dawn failed.' }
$packageHash = (Get-FileHash -LiteralPath $packing -Algorithm SHA256).Hash.ToLowerInvariant()
$packageFile = "dawn-windows-amd64-primedgun-$($packageHash.Substring(0, 12)).tar.gz"
Get-ChildItem -Path $WorkDirectory -Filter 'dawn-windows-amd64-primedgun*.tar.gz' |
    Where-Object { $_.Name -ne $packageFile -and $_.FullName -ne $packing } | Remove-Item -Force
Move-Item -LiteralPath $packing -Destination (Join-Path $WorkDirectory $packageFile) -Force

$manifestJson = [ordered]@{
    SourceRevision = $revision
    SourceSha256 = $archiveHash
    PatchSha256 = $patchHash
    Compiler = $compiler
    WindowsSdk = $sdkVersion
    Flags = ($flags -join ' ')
    CacheKey = $cacheKey
    PackageFile = $packageFile
    PackageSha256 = $packageHash
} | ConvertTo-Json
[IO.File]::WriteAllText($manifestPath, $manifestJson, (New-Object Text.UTF8Encoding $false))
Write-Host "Patched Dawn for Windows ready: $(Join-Path $WorkDirectory $packageFile)"
Write-Host ("Configure a native build with -DAURORA_DAWN_PROVIDER=package " +
    "-DAURORA_DAWN_PACKAGE_URL=$((Join-Path $WorkDirectory $packageFile).Replace('\', '/')) " +
    "-DAURORA_DAWN_PACKAGE_SHA256=$packageHash")
