param(
    [string]$Executable = "$PSScriptRoot\..\bin\Release\CyberpunkVRPort-Auto-Installer.exe"
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path "$PSScriptRoot\..\..\..").Path
$BuildRoot = (Resolve-Path (Join-Path $RepoRoot "build")).Path
$TestRoot = Join-Path $BuildRoot "auto_installer-engine-test"

function Remove-TestRoot {
    if (-not (Test-Path -LiteralPath $TestRoot)) { return }
    $resolved = (Resolve-Path -LiteralPath $TestRoot).Path
    $expectedPrefix = $BuildRoot.TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe test cleanup target: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

try {
    Remove-TestRoot
    $package = Join-Path $TestRoot "package"
    $game = Join-Path $TestRoot "game"
    New-Item -ItemType Directory -Path (Join-Path $package "Cyberpunk 2077\bin\x64") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $game "bin\x64") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $game "archive\pc\mod") -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $package "Cyberpunk 2077\bin\x64\openvr_api.dll") -Value "new-payload" -NoNewline
    Set-Content -LiteralPath (Join-Path $game "bin\x64\Cyberpunk2077.exe") -Value "marker" -NoNewline
    Set-Content -LiteralPath (Join-Path $game "bin\x64\vrport.ini") -Value "player-setting" -NoNewline
    Set-Content -LiteralPath (Join-Path $game "archive\pc\mod\cyberpunkvrport.archive") -Value "old-version" -NoNewline

    $assembly = [Reflection.Assembly]::LoadFile((Resolve-Path $Executable).Path)
    $iniType = $assembly.GetType("CyberpunkVRPort.AutoInstaller.IniDocument", $true)
    $loadMethod = $iniType.GetMethod("Load", [Reflection.BindingFlags]"Static,NonPublic", $null, [Type[]]@([IO.Stream]), $null)
    $stream = $assembly.GetManifestResourceStream("CyberpunkVRPort.AutoInstaller.config.ini")
    try { $ini = $loadMethod.Invoke($null, @($stream)) } finally { $stream.Dispose() }

    $engineType = $assembly.GetType("CyberpunkVRPort.AutoInstaller.InstallerEngine", $true)
    $constructor = $engineType.GetConstructor([Reflection.BindingFlags]"Instance,NonPublic", $null, [Type[]]@($iniType), $null)
    $engine = $constructor.Invoke(@($ini))
    $installMethod = $engineType.GetMethod("Install", [Reflection.BindingFlags]"Instance,NonPublic")
    $uninstallMethod = $engineType.GetMethod("Uninstall", [Reflection.BindingFlags]"Instance,NonPublic")

    $installArguments = [object[]]@([string](Join-Path $package "Cyberpunk 2077"), [bool]$false, [string]$game, $null)
    $installed = $installMethod.Invoke($engine, $installArguments)
    if ($installed -ne 1) { throw "Expected 1 copied file, got $installed" }
    if (Test-Path -LiteralPath (Join-Path $game "archive\pc\mod\cyberpunkvrport.archive")) {
        throw "Historical payload was not removed before install."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $game "bin\x64\vrport.ini"))) {
        throw "Generated player setting was removed during pre-install cleanup."
    }
    if ((Get-Content -LiteralPath (Join-Path $game "bin\x64\openvr_api.dll") -Raw) -ne "new-payload") {
        throw "New payload was not installed."
    }
    $statePath = Join-Path $game "bin\x64\CyberpunkVRPort-Auto-Installer.state.ini"
    if (-not (Test-Path -LiteralPath $statePath) -or
        (Get-Content -LiteralPath $statePath -Raw) -notmatch "bin/x64/openvr_api.dll") {
        throw "Installed payload was not recorded in the external cumulative state."
    }

    New-Item -ItemType Directory -Path (Join-Path $game "red4ext\plugins\CyberpunkVR_Stereo") -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $game "red4ext\plugins\CyberpunkVR_Stereo\runtime.tmp") -Value "runtime" -NoNewline
    $removed = $uninstallMethod.Invoke($engine, [object[]]@([string]$game))
    if (Test-Path -LiteralPath (Join-Path $game "bin\x64\openvr_api.dll")) { throw "Installed payload survived uninstall." }
    if (-not (Test-Path -LiteralPath (Join-Path $game "bin\x64\vrport.ini"))) {
        throw "State-backed uninstall unexpectedly used the embedded generated-file catalog."
    }
    if (-not (Test-Path -LiteralPath (Join-Path $game "red4ext\plugins\CyberpunkVR_Stereo"))) {
        throw "State-backed uninstall unexpectedly used the embedded owned-directory catalog."
    }
    $fallbackRemoved = $uninstallMethod.Invoke($engine, [object[]]@([string]$game))
    if (Test-Path -LiteralPath (Join-Path $game "bin\x64\vrport.ini")) { throw "Fallback uninstall left a generated player setting." }
    if (Test-Path -LiteralPath (Join-Path $game "red4ext\plugins\CyberpunkVR_Stereo")) {
        throw "Fallback uninstall left an owned directory."
    }

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $legacyPackage = Join-Path $TestRoot "legacy-package"
    New-Item -ItemType Directory -Path (Join-Path $legacyPackage "bin\x64") -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $legacyPackage "bin\x64\openvr_api.dll") -Value "legacy-payload" -NoNewline
    Set-Content -LiteralPath (Join-Path $legacyPackage "bin\x64\future-upstream.dll") -Value "future-payload" -NoNewline
    Set-Content -LiteralPath (Join-Path $legacyPackage "bin\x64\shared-future.dll") -Value "fork-payload" -NoNewline
    Set-Content -LiteralPath (Join-Path $legacyPackage "INSTALL.txt") -Value "documentation" -NoNewline
    Set-Content -LiteralPath (Join-Path $game "bin\x64\shared-future.dll") -Value "preexisting-third-party" -NoNewline
    $packageSourceType = $assembly.GetType("CyberpunkVRPort.AutoInstaller.PackageSource", $true)
    $openFolder = $packageSourceType.GetMethod("OpenFolder", [Reflection.BindingFlags]"Static,NonPublic")
    $folderSource = $openFolder.Invoke($null, [object[]]@([string]$legacyPackage, "Cyberpunk 2077"))
    try {
        $directRootLayout = $packageSourceType.GetProperty("DirectRootLayout", [Reflection.BindingFlags]"Instance,NonPublic").GetValue($folderSource)
        if (-not $directRootLayout) { throw "Legacy package was not detected as a direct-root layout." }
    }
    finally { $folderSource.Dispose() }

    $legacyZip = Join-Path $TestRoot "legacy.zip"
    [IO.Compression.ZipFile]::CreateFromDirectory($legacyPackage, $legacyZip)
    $openZip = $packageSourceType.GetMethod("OpenZip", [Reflection.BindingFlags]"Static,NonPublic")
    $legacySource = $openZip.Invoke($null, [object[]]@([string]$legacyZip, "Cyberpunk 2077"))
    try {
        $payloadRoot = $packageSourceType.GetProperty("PayloadRoot", [Reflection.BindingFlags]"Instance,NonPublic").GetValue($legacySource)
        $directRootLayout = $packageSourceType.GetProperty("DirectRootLayout", [Reflection.BindingFlags]"Instance,NonPublic").GetValue($legacySource)
        $legacyInstalled = $installMethod.Invoke($engine, [object[]]@([string]$payloadRoot, [bool]$directRootLayout, [string]$game, $null))
    }
    finally { $legacySource.Dispose() }
    if ($legacyInstalled -ne 3) { throw "Expected 3 legacy payload files, got $legacyInstalled" }
    if ((Get-Content -LiteralPath (Join-Path $game "bin\x64\openvr_api.dll") -Raw) -ne "legacy-payload") {
        throw "Legacy direct-root payload was not installed."
    }
    if ((Get-Content -LiteralPath (Join-Path $game "bin\x64\future-upstream.dll") -Raw) -ne "future-payload") {
        throw "Uncatalogued direct-root payload was not installed."
    }
    if (Test-Path -LiteralPath (Join-Path $game "INSTALL.txt")) {
        throw "Uncatalogued legacy documentation was copied into the game root."
    }
    if ((Get-Content -LiteralPath $statePath -Raw) -notmatch "bin/x64/future-upstream.dll") {
        throw "Uncatalogued fork payload was not added to the external cumulative state."
    }
    $backupRoot = Join-Path $game "bin\x64\CyberpunkVRPort-Auto-Installer.backups"
    if (@(Get-ChildItem -LiteralPath $backupRoot -Filter *.bak).Count -ne 1) {
        throw "The preexisting third-party file was not backed up exactly once."
    }
    $upgradePackage = Join-Path $TestRoot "upgrade-package"
    New-Item -ItemType Directory -Path (Join-Path $upgradePackage "Cyberpunk 2077\bin\x64") -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $upgradePackage "Cyberpunk 2077\bin\x64\openvr_api.dll") -Value "upgrade-payload" -NoNewline
    $upgraded = $installMethod.Invoke($engine, [object[]]@(
        [string](Join-Path $upgradePackage "Cyberpunk 2077"), [bool]$false, [string]$game, $null))
    if ($upgraded -ne 1) { throw "Expected 1 upgraded payload file, got $upgraded" }
    if (Test-Path -LiteralPath (Join-Path $game "bin\x64\future-upstream.dll")) {
        throw "A file omitted by the upgraded fork package survived state-backed pre-install cleanup."
    }
    if ((Get-Content -LiteralPath $statePath -Raw) -notmatch "bin/x64/future-upstream.dll") {
        throw "External ownership history was not cumulative across upgrades."
    }
    $removedLegacy = $uninstallMethod.Invoke($engine, [object[]]@([string]$game))
    if (Test-Path -LiteralPath (Join-Path $game "bin\x64\future-upstream.dll")) {
        throw "Externally recorded fork payload survived uninstall."
    }
    if ((Get-Content -LiteralPath (Join-Path $game "bin\x64\shared-future.dll") -Raw) -ne "preexisting-third-party") {
        throw "The preexisting third-party file was not restored during uninstall."
    }
    if ((Test-Path -LiteralPath $statePath) -or (Test-Path -LiteralPath $backupRoot)) {
        throw "Installer state survived a successful uninstall."
    }

    $unsafeZip = Join-Path $TestRoot "unsafe.zip"
    $archive = [IO.Compression.ZipFile]::Open($unsafeZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $entry = $archive.CreateEntry("../escaped.txt")
        $writer = New-Object IO.StreamWriter($entry.Open())
        try { $writer.Write("escape") } finally { $writer.Dispose() }
    }
    finally { $archive.Dispose() }
    $zipRejected = $false
    try { $openZip.Invoke($null, [object[]]@([string]$unsafeZip, "Cyberpunk 2077")) | Out-Null }
    catch { $zipRejected = $_.Exception.Message -match "escapes its root" }
    if (-not $zipRejected -or (Test-Path -LiteralPath (Join-Path $TestRoot "escaped.txt"))) {
        throw "ZIP path traversal was not rejected."
    }

    Write-Host "InstallerEngine smoke test passed: wrapped=$installed stateRemoved=$removed fallbackRemoved=$fallbackRemoved legacy=$legacyInstalled upgraded=$upgraded removed=$removedLegacy backup=restored zip-slip=rejected"
}
finally {
    Remove-TestRoot
}
