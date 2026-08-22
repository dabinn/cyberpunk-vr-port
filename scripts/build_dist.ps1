# Assemble a tester package under dist\ with a FOMOD wrapper and a Cyberpunk 2077\ payload.
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
$Payload  = Join-Path $Out "Cyberpunk 2077"

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
New-Item -ItemType Directory -Path $Payload -Force | Out-Null

$manifest = @()
function Add-File($src, $rel) {
    $dst = Join-Path $Payload $rel
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
    $n = Copy-Tree $d.FullName (Join-Path $Payload "bin\x64\plugins\cyber_engine_tweaks\mods\$($d.Name)")
    $manifest += [pscustomobject]@{ Path = "bin\x64\plugins\cyber_engine_tweaks\mods\$($d.Name)\  ($n files)"; Bytes = 0 }
}
foreach ($d in (Get-ChildItem (Join-Path $RepoRoot "mods\redscript") -Directory)) {
    if ($d.Name -eq "logs" -or $SkipMods -contains $d.Name) { continue }
    $n = Copy-Tree $d.FullName (Join-Path $Payload "r6\scripts\$($d.Name)")
    $manifest += [pscustomobject]@{ Path = "r6\scripts\$($d.Name)\  ($n files)"; Bytes = 0 }
}
$tw = Join-Path $RepoRoot "mods\tweaks\vrcigarette"
if (Test-Path $tw) {
    $n = Copy-Tree $tw (Join-Path $Payload "r6\tweaks\vrcigarette")
    $manifest += [pscustomobject]@{ Path = "r6\tweaks\vrcigarette\  ($n files)"; Bytes = 0 }
}

# ---- packed archives ---------------------------------------------------------------------------
foreach ($a in @("cyberpunkvrport.archive","VRCigarette.archive.xl")) {
    $p = Join-Path $RepoRoot "mods\archive\$a"
    if (Test-Path $p) { Add-File $p "archive\pc\mod\$a" }
    else { Write-Host "[!] $a is not in the repo -- run sync_assets.ps1 first" }
}

# ---- Vortex FOMOD ---------------------------------------------------------------------------
$fomodDirectory = Join-Path $Out "fomod"
New-Item -ItemType Directory -Path $fomodDirectory -Force | Out-Null

$moduleConfig = @"
<config xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
        xsi:noNamespaceSchemaLocation="http://qconsulting.ca/fo3/ModConfig5.0.xsd">
  <moduleName>CyberpunkVRPort</moduleName>
  <requiredInstallFiles>
    <folder source="Cyberpunk 2077" destination="" />
  </requiredInstallFiles>
</config>
"@
Set-Content -LiteralPath (Join-Path $fomodDirectory "ModuleConfig.xml") -Value $moduleConfig -Encoding utf8

$fomodInfo = @"
<fomod>
  <Name>CyberpunkVRPort</Name>
  <Version>$Version</Version>
  <Author>dariulone and contributors</Author>
  <Website>https://github.com/dabinn/cyberpunk-vr-port</Website>
</fomod>
"@
Set-Content -LiteralPath (Join-Path $fomodDirectory "info.xml") -Value $fomodInfo -Encoding utf8

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

INSTALL
    Auto Installer:
        Download CyberpunkVRPort-Auto-Installer.exe from GitHub Releases and select Install.

    Vortex:
        Add the original archive to Vortex and install it normally.

UNINSTALL
    Auto Installer:
        Run CyberpunkVRPort-Auto-Installer.exe and select Uninstall.

    Vortex:
        Remove the mod from Vortex.

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
