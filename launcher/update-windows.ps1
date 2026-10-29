# SPDX-License-Identifier: GPL-3.0-or-later
param([Parameter(Mandatory=$true)][string]$Archive,
      [Parameter(Mandatory=$true)][string]$Destination,
      [Parameter(Mandatory=$true)][int]$LauncherPid,
      [Parameter(Mandatory=$true)][string]$ExpectedHash,
      [switch]$NoRestart,
      [switch]$Quiet)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1')
$work = Split-Path -Parent $Archive
$log = Join-Path $work 'update.log'
$created = [System.Collections.Generic.List[string]]::new()
$replaced = [System.Collections.Generic.List[string]]::new()
$backup = Join-Path $work 'backup'
try {
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $ExpectedHash) { throw 'Update checksum mismatch.' }
    $target = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
    if (!(Test-Path -LiteralPath (Join-Path $target 'PrimedGun.exe'))) { throw 'Not a PrimedGun installation.' }
    for ($probe = $target; $probe; $probe = Split-Path -Parent $probe) {
        if ((Get-Item -LiteralPath $probe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Updates through junctions/symlinks are not supported.' }
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    $extract = Join-Path $work 'extracted'
    $prefix = [IO.Path]::GetFullPath($extract) + '\'
    try {
        if ($zip.Entries.Count -gt 20000) { throw 'Too many files in update.' }
        [long]$total = 0
        foreach ($entry in $zip.Entries) {
            foreach ($part in ($entry.FullName -split '[/\\]')) {
                # Win32 normalizes trailing dots/spaces: user. must never alias user/.
                if ($part -and ($part -eq '.' -or $part -eq '..' -or $part.TrimEnd(' ', '.') -ne $part)) {
                    throw 'Ambiguous update archive path.'
                }
            }
            $entryPath = [IO.Path]::GetFullPath((Join-Path $extract $entry.FullName))
            if (!$entryPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or $entry.FullName.Contains(':') -or
                (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw 'Unsafe update archive path.' }
            $total += $entry.Length
            if ($total -gt 4294967296) { throw 'Update archive is too large.' }
        }
    } finally { $zip.Dispose() }
    [IO.Compression.ZipFile]::ExtractToDirectory($Archive, $extract)
    $launchers = @(Get-ChildItem -LiteralPath $extract -Filter PrimedGun.exe -File -Recurse)
    if ($launchers.Count -ne 1) { throw 'Update must contain one PrimedGun launcher.' }
    $source = $launchers[0].DirectoryName
    if (!(Test-Path -LiteralPath (Join-Path $source 'metroid_prime_port.exe'))) { throw 'Game executable missing from update.' }
    $files = @(Get-ChildItem -LiteralPath $source -File -Recurse | Where-Object {
        $relative = $_.FullName.Substring($source.Length + 1)
        $relative.Split('\')[0] -ine 'user'
    })
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($source.Length + 1)
        $dest = Join-Path $target $relative
        for ($probe = $dest; $probe -and $probe.Length -gt $target.Length; $probe = Split-Path -Parent $probe) {
            if ((Test-Path -LiteralPath $probe) -and ((Get-Item -LiteralPath $probe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw 'Update destination contains a junction/symlink.'
            }
        }
    }
    $deadline = [DateTime]::UtcNow.AddMinutes(2)
    do {
        $busy = $null -ne (Get-Process -Id $LauncherPid -ErrorAction SilentlyContinue)
        foreach ($process in @(Get-Process PrimedGun,metroid_prime_port -ErrorAction SilentlyContinue)) {
            if (!$process.Path) { throw 'Cannot check whether a game executable is still open.' }
            if ((Split-Path -Parent $process.Path) -ieq $target) { $busy = $true }
        }
        if (!$busy) { break }
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Close the game and launcher, then check for updates again.' }
        Start-Sleep -Milliseconds 500
    } while ($true)
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($source.Length + 1)
        $dest = Join-Path $target $relative
        if (Test-Path -LiteralPath $dest) {
            $saved = Join-Path $backup $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $saved) -Force | Out-Null
            Copy-Item -LiteralPath $dest -Destination $saved
            $replaced.Add($relative)
        } else { $created.Add($relative) }
        New-Item -ItemType Directory -Path (Split-Path -Parent $dest) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $dest -Force
    }
    'Update installed successfully. Previous binaries kept in: ' + $backup | Set-Content -LiteralPath $log -Encoding UTF8
    if (!$NoRestart) { Start-Process -FilePath (Join-Path $target 'PrimedGun.exe') -WorkingDirectory $target }
} catch {
    $message = $_.Exception.Message
    $rollbackErrors = @()
    foreach ($relative in $replaced) {
        try { Copy-Item -LiteralPath (Join-Path $backup $relative) -Destination (Join-Path $target $relative) -Force } catch { $rollbackErrors += $_.Exception.Message }
    }
    foreach ($relative in $created) {
        try {
            $newFile = Join-Path $target $relative
            if (Test-Path -LiteralPath $newFile) {
                $saved = Join-Path (Join-Path $work 'incomplete') $relative
                New-Item -ItemType Directory -Path (Split-Path -Parent $saved) -Force | Out-Null
                Move-Item -LiteralPath $newFile -Destination $saved
            }
        } catch { $rollbackErrors += $_.Exception.Message }
    }
    $message += "`n`nUpdate log and backup: $work"
    if ($rollbackErrors.Count) { $message += "`nRestore errors: " + ($rollbackErrors -join '; ') }
    $message | Set-Content -LiteralPath $log -Encoding UTF8
    if (!$Quiet) {
        Add-Type -AssemblyName PresentationFramework
        [System.Windows.MessageBox]::Show($message, 'PrimedGun update could not finish') | Out-Null
    }
    exit 1
}
