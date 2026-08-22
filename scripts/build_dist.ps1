# Assemble a tester package under dist\, laid out exactly as it must land in the game root.
#
# Everything comes from the repo or from a build output -- nothing is read out of the installed
# game -- so what a tester gets is what is committed. Run scripts\sync_assets.ps1 first if the
# yamls, archives or grip poses have been touched game-side since the last pull.
#
# Usage:
#   pwsh scripts\build_dist.ps1
#   pwsh scripts\build_dist.ps1 -Version 0.1.1 -Zip

param(
    [string]$Version = "0.1.0",
    [string]$BuildDir = "build",
    [switch]$Zip,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$Out      = Join-Path $RepoRoot "dist\CyberpunkVRPort-$Version"

# Folders that exist for development and have no business in a tester's game.
$SkipMods  = @("CyberpunkVRPort_WorldMapDiag")
# *.md too: the notes beside a script are for whoever maintains it, not for a tester's game folder.
$SkipFiles = @("db.sqlite3", "*.log", "*.bak", "*.orig", "*.rar", "*.zip", "*.md")

function Copy-Tree($src, $dst) {
    New-Item -ItemType Directory -Path $dst -Force | Out-Null
    $n = 0
    foreach ($f in (Get-ChildItem -LiteralPath $src -File)) {
        $skip = $false
        foreach ($p in $SkipFiles) { if ($f.Name -like $p) { $skip = $true; break } }
        if ($skip) { continue }
        Copy-Item $f.FullName (Join-Path $dst $f.Name) -Force
        $n++
    }
    foreach ($d in (Get-ChildItem -LiteralPath $src -Directory)) {
        $n += Copy-Tree $d.FullName (Join-Path $dst $d.Name)
    }
    return $n
}

function Need($p, $what) {
    if (-not (Test-Path -LiteralPath $p)) { throw "$what not found: $p" }
    return $p
}

if (Test-Path $Out) {
    if (-not $Force) { Remove-Item $Out -Recurse -Force } else { Remove-Item $Out -Recurse -Force }
}
New-Item -ItemType Directory -Path $Out -Force | Out-Null

$manifest = @()
function Add-File($src, $rel) {
    $dst = Join-Path $Out $rel
    New-Item -ItemType Directory -Path (Split-Path $dst -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst -Force
    $script:manifest += [pscustomobject]@{ Path = $rel; Bytes = (Get-Item -LiteralPath $dst).Length }
}

# ---- the two plugins ------------------------------------------------------------------------
$stereoDll = Need (Join-Path $RepoRoot "$BuildDir\bin\red4ext\plugins\CyberpunkVR_Stereo\Release\CyberpunkVR_Stereo.dll") "CyberpunkVR_Stereo.dll"
$handsDll  = Need (Join-Path $RepoRoot "src\red4ext_plugin\build\RelWithDebInfo\CyberpunkVR_Hands.dll") "CyberpunkVR_Hands.dll"
Add-File $stereoDll "red4ext\plugins\CyberpunkVR_Stereo\CyberpunkVR_Stereo.dll"
Add-File $handsDll  "red4ext\plugins\CyberpunkVR_Hands\CyberpunkVR_Hands.dll"

# The sight shaders are loaded by name at PSO-replacement time; without both, the replacement is
# skipped and the only symptom is one line in the log.
Add-File (Need (Join-Path $RepoRoot "src\vr\shaders\sight_reflex_ps.dxil") "sight PS") "red4ext\plugins\CyberpunkVR_Stereo\CyberpunkVR_SightPs.dxil"
Add-File (Need (Join-Path $RepoRoot "src\vr\shaders\sight_reflex_vs.dxil") "sight VS") "red4ext\plugins\CyberpunkVR_Stereo\CyberpunkVR_SightVs.dxil"

# Installed over the player's own settings ONCE, on the first launch with first_launch=0 in
# vrport.ini, keeping a timestamped copy of what was there. See INSTALL.txt.
Add-File (Need (Join-Path $RepoRoot "mods\config\UserSettings.json") "UserSettings.json") "red4ext\plugins\CyberpunkVR_Stereo\UserSettings.json"

# ---- captured grip poses, read from beside the exe -------------------------------------------
foreach ($g in @("CyberpunkVR_SmokeGrip_right.ini","CyberpunkVR_SmokeGrip_Left.ini","CyberpunkVR_LighterGrip_Left.ini")) {
    Add-File (Need (Join-Path $RepoRoot "mods\config\$g") $g) "bin\x64\$g"
}

# ---- engine-side tuning + the OpenVR shim ------------------------------------------------------
Add-File (Need (Join-Path $RepoRoot "mods\config\vrcam_cpu_tweaks.ini") "vrcam_cpu_tweaks.ini") "engine\config\platform\pc\vrcam_cpu_tweaks.ini"
Add-File (Need (Join-Path $RepoRoot "mods\config\openvr_api.dll") "openvr_api.dll") "bin\x64\openvr_api.dll"

# ---- CET mods, redscript, tweaks --------------------------------------------------------------
foreach ($d in (Get-ChildItem (Join-Path $RepoRoot "mods\cet") -Directory)) {
    if ($SkipMods -contains $d.Name) { continue }
    $n = Copy-Tree $d.FullName (Join-Path $Out "bin\x64\plugins\cyber_engine_tweaks\mods\$($d.Name)")
    $manifest += [pscustomobject]@{ Path = "bin\x64\plugins\cyber_engine_tweaks\mods\$($d.Name)\  ($n files)"; Bytes = 0 }
}
foreach ($d in (Get-ChildItem (Join-Path $RepoRoot "mods\redscript") -Directory)) {
    if ($d.Name -eq "logs" -or $SkipMods -contains $d.Name) { continue }
    $n = Copy-Tree $d.FullName (Join-Path $Out "r6\scripts\$($d.Name)")
    $manifest += [pscustomobject]@{ Path = "r6\scripts\$($d.Name)\  ($n files)"; Bytes = 0 }
}
$tw = Join-Path $RepoRoot "mods\tweaks\vrcigarette"
if (Test-Path $tw) {
    $n = Copy-Tree $tw (Join-Path $Out "r6\tweaks\vrcigarette")
    $manifest += [pscustomobject]@{ Path = "r6\tweaks\vrcigarette\  ($n files)"; Bytes = 0 }
}

# ---- packed archives ---------------------------------------------------------------------------
foreach ($a in @("cyberpunkvrport.archive","VRCigarette.archive.xl")) {
    $p = Join-Path $RepoRoot "mods\archive\$a"
    if (Test-Path $p) { Add-File $p "archive\pc\mod\$a" }
    else { Write-Host "[!] $a is not in the repo -- run sync_assets.ps1 first" }
}

# Include a standalone uninstaller. It uses the mod's fixed names rather than build-time hashes,
# so locally rebuilt or manually replaced plugin DLLs are still removed.
$uninstallScriptPath = Join-Path $Out "uninstall_cyberpunkvrport.ps1"
Copy-Item -LiteralPath (Need (Join-Path $PSScriptRoot "uninstall_cyberpunkvrport.ps1") "uninstaller") -Destination $uninstallScriptPath
$manifest += [pscustomobject]@{ Path = "uninstall_cyberpunkvrport.ps1"; Bytes = (Get-Item $uninstallScriptPath).Length }

# ---- the OpenXR probe is NOT packaged ---------------------------------------------------------
# It stays in tools\xr_probe\ and goes to a tester by hand, when there is something to measure.
# Registering a MACHINE-WIDE OpenXR API layer is not a thing to ship to everyone who installs a
# mod: it is not dropped in a folder, it is written into a registry key, and one left unregistered
# records every VR application on the box. Build it with the xr_probe_layer target and hand over
# that folder when it is actually needed.

# ---- the note a tester actually reads ----------------------------------------------------------
$readme = @"
CyberpunkVRPort $Version
========================

WHAT THIS IS
    A VR mod for Cyberpunk 2077: stereo rendering through OpenXR, 6DoF head tracking, motion
    controllers merged into the game's own gamepad input, VRIK arms, and a set of gameplay mods
    (HUD placement, holsters, melee, weapon handling, smoking).

BEFORE YOU INSTALL -- READ THIS ONE
    The first time the plugin starts it REPLACES your Cyberpunk settings with the ones this mod
    was tuned against:

        %LOCALAPPDATA%\CD Projekt Red\Cyberpunk 2077\UserSettings.json

    Your own file is copied to UserSettings.pre-vr-<date>-<time>.json in the same folder first,
    and if that copy fails the install is abandoned rather than forced. It happens exactly once:
    afterwards the file is yours and nothing here looks at it again. Everything you change in the
    game's own menus sticks.

    To skip it entirely: after the first launch creates bin\x64\vrport.ini, set first_launch=1 in
    it BEFORE starting the game a second time. To ask for it again later, set first_launch=0.

REQUIREMENTS
    Cyberpunk 2077 2.31 (this build's engine offsets are matched to it)
    RED4ext, Cyber Engine Tweaks, redscript, TweakXL, ArchiveXL, Codeware
    An OpenXR runtime, started BEFORE the game

    Nothing else may proxy dxgi. If bin\x64\dxgi.dll exists (R.E.A.L. VR installs one), move it
    out of the folder -- two VR paths in one process fight over the same engine hooks.

INSTALL
    Extract the contents of this folder into your Cyberpunk 2077 game root -- the folder that
    contains bin\, r6\, red4ext\ and archive\. The paths inside already match.

    Then start your OpenXR runtime, then the game. A small launcher window appears first: pick
    your headset and per-eye render resolution there.

WHAT LANDS WHERE
    red4ext\plugins\CyberpunkVR_Stereo\   the VR plugin, its shaders, the settings template
    red4ext\plugins\CyberpunkVR_Hands\    avatar / VRIK / weapon / smoking natives
    bin\x64\CyberpunkVR_*Grip*.ini        captured hand poses for holding a cigarette and lighter
    bin\x64\plugins\cyber_engine_tweaks\mods\CyberpunkVRPort_*\
    r6\scripts\CyberpunkVRPort_*\
    r6\tweaks\vrcigarette\
    archive\pc\mod\                       packed assets + the ArchiveXL manifest

    The player entity assets in cyberpunkvrport.archive carry one render-to-texture camera per
    supported resolution. The launcher offers exactly the ones that exist.

IF SOMETHING IS WRONG
    bin\x64\cyberpunkvrport.log            the plugin's own log, start here
    red4ext\logs\                          script validation errors land here
    bin\x64\plugins\cyber_engine_tweaks\   per-mod CET logs

UNINSTALL
    Close the game, open PowerShell in the Cyberpunk 2077 game root, and run:

        pwsh -File .\uninstall_cyberpunkvrport.ps1

    Preview without deleting anything by adding -WhatIf. Add -KeepSettings to retain runtime
    settings, opt-out markers, calibration data, and plugin logs.

    The uninstaller does not restore UserSettings.json automatically. Nothing else is written
    outside the game folder; the first-launch backup described above sits next to that file.

Built from commit $(git -C $RepoRoot rev-parse --short HEAD 2>$null) on $(Get-Date -Format "yyyy-MM-dd").
"@
Set-Content (Join-Path $Out "INSTALL.txt") $readme -Encoding utf8

# ---- report -------------------------------------------------------------------------------------
Write-Host "dist\CyberpunkVRPort-$Version"
foreach ($m in $manifest) {
    if ($m.Bytes -gt 0) { Write-Host ("  {0,-62} {1,10:N0}" -f $m.Path, $m.Bytes) }
    else                { Write-Host ("  {0}" -f $m.Path) }
}
$all = Get-ChildItem $Out -Recurse -File
Write-Host ""
Write-Host ("  {0} files, {1:N0} bytes total" -f $all.Count, ($all | Measure-Object Length -Sum).Sum)

if ($Zip) {
    # Not $zip: PowerShell variable names are case-insensitive, so that would be the -Zip switch.
    $archivePath = "$Out.zip"
    if (Test-Path $archivePath) { Remove-Item $archivePath -Force }
    Compress-Archive -Path (Join-Path $Out "*") -DestinationPath $archivePath
    Write-Host ("  packaged -> {0}  ({1:N0} bytes)" -f $archivePath, (Get-Item $archivePath).Length)
}
