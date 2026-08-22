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

# ---- the plugin -----------------------------------------------------------------------------
# ONE plugin. This used to package CyberpunkVR_Hands from src\red4ext_plugin\ as well, and
# that directory no longer exists: every script-callable native (VRBodyBonePos, VRPalmModelPos,
# SetVRSprintActive, all of them) is compiled into CyberpunkVR_Stereo now, out of src\Natives\.
# Shipping both was also the recipe for the read at 0xFFFFFFFFFFFFFFFF -- two RED4ext plugins
# detouring one address -- so a stale CyberpunkVR_Hands.dll left in red4ext\plugins is worth deleting.
$stereoDll = Need (Join-Path $RepoRoot "$BuildDir\bin\red4ext\plugins\CyberpunkVR_Stereo\Release\CyberpunkVR_Stereo.dll") "CyberpunkVR_Stereo.dll"
Add-File $stereoDll "red4ext\plugins\CyberpunkVR_Stereo\CyberpunkVR_Stereo.dll"

# The sight shaders are loaded by name at PSO-replacement time; without both, the replacement is
# skipped and the only symptom is one line in the log.
Add-File (Need (Join-Path $RepoRoot "src\Shaders\sight_reflex_ps.dxil") "sight PS") "red4ext\plugins\CyberpunkVR_Stereo\CyberpunkVR_SightPs.dxil"
Add-File (Need (Join-Path $RepoRoot "src\Shaders\sight_reflex_vs.dxil") "sight VS") "red4ext\plugins\CyberpunkVR_Stereo\CyberpunkVR_SightVs.dxil"

# Installed over the player's own settings ONCE, on a launch that reads first_launch=1 from
# vrport.ini -- 1 means "not installed yet", and FirstLaunch clears it to 0 only after the copy
# succeeds. A timestamped copy of what was there is kept. See INSTALL.txt.
Add-File (Need (Join-Path $RepoRoot "mods\config\UserSettings.json") "UserSettings.json") "red4ext\plugins\CyberpunkVR_Stereo\UserSettings.json"

# ---- captured grip poses, read from beside the exe -------------------------------------------
# ENUMERATED, never listed. This was a hardcoded three (both smoke grips and the lighter) while
# mods\config\ holds fourteen: it also has RestGrip_Left -- the relaxed left hand worn
# while a weapon is out -- and one TwoHandGrip_<weapon> per weapon anyone has recorded a hold for.
# Both absences are silent by design (an uncaptured weapon just has no two-hand hold), so the
# package quietly shipped no two-handed grip at all. Enumerating means a newly captured pose ships
# by existing, without anyone remembering to add it here.
foreach ($g in (Get-ChildItem (Join-Path $RepoRoot "mods\config") -Filter "CyberpunkVR_*.ini" -File | Sort-Object Name)) {
    Add-File $g.FullName "bin\x64\$($g.Name)"
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
foreach ($a in @("cyberpunkvrport.archive","VRCigarette.archive.xl","vrport_basketball.archive")) {
    $p = Join-Path $RepoRoot "mods\archive\$a"
    if (Test-Path $p) { Add-File $p "archive\pc\mod\$a" }
    else { Write-Host "[!] $a is not in the repo -- run sync_assets.ps1 first" }
}

# ---- HUDitor: the port's setup, on the paths the mod actually uses ----------------------------
# HUD placement is not the port's job -- its own HUD mod was removed on 2026-08-20 because it
# scaled the shared HUD root around screen centre and fought a real editor. What ships instead is
# the port's HUDitor setup: the editor moved off F7 to F11, and a layout tuned in VR.
#
# The binding lives in r6\input\ because that is the only place it works: the game merges
# r6\input\*.xml into r6\cache\inputUserMappings.xml every launch and reads the merged
# result. That merge is RED4ext's input_loader, so without it this file is inert.
#
# persistency.json REPLACES whatever HUDitor layout is installed -- said out loud in INSTALL.txt
# rather than hidden, because for a tester getting the tuned layout is the whole point.
$hud = Join-Path $RepoRoot "mods\config\huditor"
if (Test-Path $hud) {
    Add-File (Need (Join-Path $hud "HUDitor.xml") "HUDitor.xml") "r6\input\HUDitor.xml"
    Add-File (Need (Join-Path $hud "persistency.json") "persistency.json") "bin\x64\plugins\cyber_engine_tweaks\mods\HUDitor\persistency.json"
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
    (holsters, physical reload, melee, weapon handling, smoking). HUD placement is HUDitor's job
    now, and this package carries the port's HUDitor setup -- see WHAT LANDS WHERE.

BEFORE YOU INSTALL -- READ THIS ONE
    The first time the plugin starts it REPLACES your Cyberpunk settings with the ones this mod
    was tuned against:

        %LOCALAPPDATA%\CD Projekt Red\Cyberpunk 2077\UserSettings.json

    Your own file is copied to UserSettings.pre-vr-<date>-<time>.json in the same folder first,
    and if that copy fails the install is abandoned rather than forced. It happens exactly once:
    afterwards the file is yours and nothing here looks at it again. Everything you change in the
    game's own menus sticks.

    first_launch=1 in bin\x64\vrport.ini means "not installed yet"; the plugin
    clears it to 0 once the copy has succeeded, and never looks again.

    To SKIP it entirely, including on the very first launch: create bin\x64\vrport.ini yourself,
    containing the single line first_launch=0, before you start the game. Creating it afterwards
    is too late -- the plugin writes that file and reads it in the same breath, so a fresh
    install has already been offered the settings by the time you can edit it.

    To ask for the settings later, set first_launch=1 and start the game once.

REQUIREMENTS
    Cyberpunk 2077 2.31 (this build's engine offsets are matched to it)
    RED4ext, Cyber Engine Tweaks, redscript, TweakXL, ArchiveXL, Codeware
    An OpenXR runtime, started BEFORE the game

    HUDitor, plus RED4ext's input_loader -- ONLY if you want HUD placement. This package carries
    the port's HUDitor setup (the editor on F11, and a VR layout), and input_loader is the plugin
    that merges r6\input\*.xml, so without it the F11 binding is inert. Note that
    persistency.json REPLACES any HUDitor layout you already have -- back yours up first if you
    care about it. The port needs neither: with no HUD editor the flat-screen HUD is used
    unchanged, and the port still composites it into the second eye either way.

    Nothing else may proxy dxgi. If bin\x64\dxgi.dll exists (R.E.A.L. VR installs one), move it
    out of the folder -- two VR paths in one process fight over the same engine hooks.

INSTALL
    Extract the contents of this folder into your Cyberpunk 2077 game root -- the folder that
    contains bin\, r6\, red4ext\ and archive\. The paths inside already match.

    Then start your OpenXR runtime, then the game. A small launcher window appears first: pick
    your headset and per-eye render resolution there.

WHAT LANDS WHERE
    red4ext\plugins\CyberpunkVR_Stereo\   the VR plugin, its shaders, the settings template
    bin\x64\CyberpunkVR_*Grip*.ini        captured hand poses for holding a cigarette and lighter
    bin\x64\plugins\cyber_engine_tweaks\mods\CyberpunkVRPort_*\
    r6\scripts\CyberpunkVRPort_*\
    r6\tweaks\vrcigarette\
    archive\pc\mod\                       packed assets + the ArchiveXL manifest
    r6\input\HUDitor.xml               HUDitor's editor moved to F11 (needs input_loader)
    bin\x64\plugins\...\mods\HUDitor\persistency.json   the VR HUD layout -- REPLACES yours

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
