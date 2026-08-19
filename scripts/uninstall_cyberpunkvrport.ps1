# Remove CyberpunkVRPort files from a Cyberpunk 2077 installation.
# Run the packaged copy from the game root, or pass -GameRoot when running from a source tree.
# Use -WhatIf to preview all removals.

[CmdletBinding(SupportsShouldProcess=$true, ConfirmImpact="Medium")]
param(
    [string]$GameRoot,
    [switch]$KeepSettings
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($GameRoot)) {
    $GameRoot = $PSScriptRoot
}

if (-not (Test-Path -LiteralPath $GameRoot -PathType Container)) {
    throw "GameRoot not found: $GameRoot"
}

$Root = (Resolve-Path -LiteralPath $GameRoot).Path.TrimEnd([IO.Path]::DirectorySeparatorChar)
$RootPrefix = $Root + [IO.Path]::DirectorySeparatorChar

if (-not (Test-Path -LiteralPath (Join-Path $Root "bin\x64") -PathType Container)) {
    throw "GameRoot does not contain bin\x64: $Root"
}

function Resolve-RemovalTarget([string]$RelativePath) {
    if ([string]::IsNullOrWhiteSpace($RelativePath) -or [IO.Path]::IsPathRooted($RelativePath)) {
        throw "Unsafe uninstall path: '$RelativePath'"
    }

    $target = [IO.Path]::GetFullPath((Join-Path $Root $RelativePath))
    if (-not $target.StartsWith($RootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Uninstall path escapes GameRoot: '$RelativePath'"
    }
    return $target
}

function Assert-NoReparsePointInPath([string]$Target) {
    $fullTarget = [IO.Path]::GetFullPath($Target)
    if (-not $fullTarget.StartsWith($RootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Uninstall path escapes GameRoot: '$Target'"
    }
    $relative = $fullTarget.Substring($RootPrefix.Length)
    $current = $Root
    foreach ($part in ($relative -split '[\\/]' | Where-Object { $_.Length -gt 0 })) {
        $current = Join-Path $current $part
        if (-not (Test-Path -LiteralPath $current)) { return }
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to remove through a reparse point: $current"
        }
    }
}

function Remove-OwnedDirectory([string]$RelativePath) {
    $target = Resolve-RemovalTarget $RelativePath
    if (-not (Test-Path -LiteralPath $target -PathType Container)) { return }
    Assert-NoReparsePointInPath $target
    if ($PSCmdlet.ShouldProcess($target, "Remove CyberpunkVRPort directory")) {
        Remove-Item -LiteralPath $target -Recurse -Force
        Write-Host "[removed] $RelativePath\"
        $script:Removed++
    }
}

function Remove-OwnedFile([string]$RelativePath) {
    $target = Resolve-RemovalTarget $RelativePath
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { return }
    Assert-NoReparsePointInPath $target
    if ($PSCmdlet.ShouldProcess($target, "Remove CyberpunkVRPort file")) {
        Remove-Item -LiteralPath $target -Force
        Write-Host "[removed] $RelativePath"
        $script:Removed++
    }
}

$proc = Get-Process Cyberpunk2077 -ErrorAction SilentlyContinue
if ($proc) { throw "Cyberpunk2077.exe is running -- close it before uninstalling." }

$Removed = 0

# Native plugin directories are dedicated to this mod. Removing the complete directories also
# handles locally rebuilt DLLs and runtime files that differ from a packaged release.
foreach ($relative in @(
    "red4ext\plugins\CyberpunkVR_Stereo",
    "red4ext\plugins\CyberpunkVR_Hands",
    "r6\tweaks\vrcigarette"
)) {
    Remove-OwnedDirectory $relative
}

# Every directory with this prefix under these two loader locations belongs to CyberpunkVRPort.
# Enumerate only the immediate children; never recurse through the shared parent directories.
foreach ($parentRelative in @(
    "bin\x64\plugins\cyber_engine_tweaks\mods",
    "r6\scripts"
)) {
    $parent = Resolve-RemovalTarget $parentRelative
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) { continue }
    Assert-NoReparsePointInPath $parent
    foreach ($directory in (Get-ChildItem -LiteralPath $parent -Directory -Filter "CyberpunkVRPort_*")) {
        $relative = $directory.FullName.Substring($RootPrefix.Length)
        Remove-OwnedDirectory $relative
    }
}

# These files are installed into shared game directories, so remove the exact filenames only.
foreach ($relative in @(
    "bin\x64\CyberpunkVR_SmokeGrip_right.ini",
    "bin\x64\CyberpunkVR_SmokeGrip_Left.ini",
    "bin\x64\CyberpunkVR_LighterGrip_Left.ini",
    "bin\x64\openvr_api.dll",
    "engine\config\platform\pc\vrcam_cpu_tweaks.ini",
    "archive\pc\mod\cyberpunkvrport.archive",
    "archive\pc\mod\VRCigarette.archive.xl"
)) {
    Remove-OwnedFile $relative
}

if (-not $KeepSettings) {
    foreach ($relative in @(
        "bin\x64\vrport.ini",
        "bin\x64\vrport-launcher.ini",
        "bin\x64\vrik_calibration.ini",
        "bin\x64\vrport_nostereo.txt",
        "bin\x64\cyberpunkvr_snapwin.log",
        "bin\x64\cyberpunkvrport.log"
    )) {
        Remove-OwnedFile $relative
    }
}

Remove-OwnedFile "INSTALL.txt"

Write-Host ""
Write-Host ("[ok] removed {0} item(s)" -f $Removed)
Write-Host "UserSettings.json is not restored automatically. A first-launch backup, if created, is under:"
Write-Host "  $env:LOCALAPPDATA\CD Projekt Red\Cyberpunk 2077\UserSettings.pre-vr-*.json"

# PowerShell has already loaded this script, so the packaged copy can remove itself safely. Never
# delete a source-tree copy invoked with an explicit -GameRoot.
$scriptPath = [IO.Path]::GetFullPath($MyInvocation.MyCommand.Path)
if ($scriptPath.StartsWith($RootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    Assert-NoReparsePointInPath $scriptPath
    if ($PSCmdlet.ShouldProcess($scriptPath, "Remove CyberpunkVRPort uninstaller")) {
        Remove-Item -LiteralPath $scriptPath -Force
    }
}
