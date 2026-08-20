param(
    [string]$Executable = "$PSScriptRoot\..\bin\Release\CyberpunkVRPort-Auto-Installer.exe"
)

$ErrorActionPreference = "Stop"
$assembly = [Reflection.Assembly]::LoadFile((Resolve-Path $Executable).Path)
$iniType = $assembly.GetType("CyberpunkVRPort.AutoInstaller.IniDocument", $true)
$loadMethod = $iniType.GetMethod("Load", [Reflection.BindingFlags]"Static,NonPublic", $null, [Type[]]@([IO.Stream]), $null)
$stream = $assembly.GetManifestResourceStream("CyberpunkVRPort.AutoInstaller.config.ini")
try { $ini = $loadMethod.Invoke($null, @($stream)) } finally { $stream.Dispose() }

$formType = $assembly.GetType("CyberpunkVRPort.AutoInstaller.MainForm", $true)
$constructor = $formType.GetConstructor(
    [Reflection.BindingFlags]"Instance,NonPublic", $null,
    [Type[]]@($iniType, [bool], [string[]]), $null)

function Get-Control([object]$form, [string]$name) {
    return $formType.GetField($name, [Reflection.BindingFlags]"Instance,NonPublic").GetValue($form)
}

function Get-FormPoint([object]$control) {
    $x = $control.Left
    $y = $control.Top
    $parent = $control.Parent
    while ($null -ne $parent -and $parent -ne $control.FindForm()) {
        $x += $parent.Left
        $y += $parent.Top
        $parent = $parent.Parent
    }
    return [Drawing.Point]::new($x, $y)
}

function Test-Layout([bool]$devMode) {
    $form = $constructor.Invoke([object[]]@($ini, $devMode, [string[]]@()))
    try {
        $form.CreateControl()
        $form.PerformLayout()
        $title = Get-Control $form "titleLabel"
        $language = Get-Control $form "languageBox"
        $game = Get-Control $form "gamePathBox"
        $source = Get-Control $form "sourceBox"
        $fork = Get-Control $form "forkBox"
        $release = Get-Control $form "releaseBox"
        $local = Get-Control $form "localPathBox"
        $status = Get-Control $form "statusLabel"
        $installerStatus = Get-Control $form "installerStatusLabel"
        $browse = Get-Control $form "browseButton"
        $refresh = Get-Control $form "refreshButton"
        $titlePoint = Get-FormPoint $title
        $languagePoint = Get-FormPoint $language
        $gamePoint = Get-FormPoint $game
        $statusPoint = Get-FormPoint $status
        $installerPoint = Get-FormPoint $installerStatus

        if ($game.Width -lt 320) { throw "Field controls are still too narrow for release and path text." }
        if ($browse.Height -gt 40 -or $refresh.Height -gt 40) {
            throw "Row action buttons expanded vertically and broke the form layout."
        }

        if ([Math]::Abs($titlePoint.Y - $languagePoint.Y) -gt 12 -or $languagePoint.X -le $titlePoint.X) {
            throw "Title and language selector are not aligned on one row."
        }
        if ([Math]::Abs($statusPoint.Y - $installerPoint.Y) -gt 12 -or $installerPoint.X -le $statusPoint.X) {
            throw "Operation and Installer statuses are not aligned in the footer."
        }
        if ($devMode) {
            $sourcePoint = Get-FormPoint $source
            $localPoint = Get-FormPoint $local
            if (-not ($gamePoint.Y -lt $sourcePoint.Y -and $sourcePoint.Y -lt $localPoint.Y)) {
                throw "Dev layout is not game root -> source -> local package."
            }
            if ($gamePoint.X -ne $sourcePoint.X -or $sourcePoint.X -ne $localPoint.X -or
                $game.Width -ne $source.Width -or $source.Width -ne $local.Width) {
                throw "Dev field controls are not horizontally aligned."
            }
        }
        else {
            $forkPoint = Get-FormPoint $fork
            $releasePoint = Get-FormPoint $release
            if (-not ($gamePoint.Y -lt $forkPoint.Y -and $forkPoint.Y -lt $releasePoint.Y)) {
                throw "Normal layout is not game root -> fork -> release."
            }
            if ($gamePoint.X -ne $forkPoint.X -or $forkPoint.X -ne $releasePoint.X -or
                $game.Width -ne $fork.Width -or $fork.Width -ne $release.Width) {
                throw "Normal field controls are not horizontally aligned."
            }
        }
        if ($fork.Items.Count -ne 6 -or $fork.Items[0].ToString() -ne "Tofu Express") {
            throw "Embedded Folks were not appended after the built-in fork."
        }
    }
    finally { $form.Dispose() }
}

Test-Layout $false
Test-Layout $true

$resumeForm = $constructor.Invoke([object[]]@($ini, $false,
    [string[]]@("--resume-install", "123", "C:\Games\Cyberpunk 2077", "zh-TW",
        "dariulone", "cyberpunk-vr-port")))
try {
    $languageBox = Get-Control $resumeForm "languageBox"
    $languageCode = $languageBox.SelectedItem.GetType().GetProperty(
        "Code", [Reflection.BindingFlags]"Instance,NonPublic").GetValue($languageBox.SelectedItem)
    $resumeReleaseId = $formType.GetField(
        "resumeReleaseId", [Reflection.BindingFlags]"Instance,NonPublic").GetValue($resumeForm)
    $resumeFork = (Get-Control $resumeForm "forkBox").SelectedItem.ToString()
    if ($languageCode -ne "zh-TW" -or $resumeReleaseId -ne 123 -or $resumeFork -ne "dariulone (Upstream)") {
        throw "Self-update resume state did not preserve language, release ID, and fork."
    }
}
finally { $resumeForm.Dispose() }

$forkConfigRoot = Join-Path ([IO.Path]::GetTempPath()) ("CyberpunkVRPort-fork-test-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $forkConfigRoot | Out-Null
try {
    Set-Content -LiteralPath (Join-Path $forkConfigRoot "CyberpunkVRPort-Auto-Installer.dev.ini") -Encoding UTF8 -Value @"
[DevForks]
iPowerTech upstream=iPowerTech/cyberpunk-vr-port
Another fork=someone/cyberpunk-vr-port|^custom-.*\.zip$
Duplicate own=dabinn/cyberpunk-vr-port
"@
    $loadForks = $formType.GetMethod("LoadForks", [Reflection.BindingFlags]"Static,NonPublic")
    $forks = $loadForks.Invoke($null, [object[]]@($ini, $true, [string]$forkConfigRoot))
    if ($forks.Count -ne 7 -or $forks[0].ToString() -ne "Tofu Express" -or
        $forks[6].ToString() -ne "Another fork") {
        throw "External DevForks were not appended after embedded Folks in INI order."
    }
}
finally {
    $resolvedForkConfigRoot = (Resolve-Path -LiteralPath $forkConfigRoot).Path
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolvedForkConfigRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe DevForks test cleanup target: $resolvedForkConfigRoot"
    }
    Remove-Item -LiteralPath $resolvedForkConfigRoot -Recurse -Force
}

$normalForm = $constructor.Invoke([object[]]@($ini, $false, [string[]]@()))
$devForm = $constructor.Invoke([object[]]@($ini, $true, [string[]]@()))
try {
    $shouldRefreshInstaller = $formType.GetMethod(
        "ShouldRefreshInstaller", [Reflection.BindingFlags]"Instance,NonPublic")
    if ($shouldRefreshInstaller.Invoke($normalForm, [object[]]@($false, $false))) {
        throw "Normal fork switching unexpectedly refreshes the Installer repository."
    }
    if (-not $shouldRefreshInstaller.Invoke($normalForm, [object[]]@($true, $false)) -or
        -not $shouldRefreshInstaller.Invoke($normalForm, [object[]]@($false, $true))) {
        throw "Startup/Refresh or the own repository no longer refreshes Installer status."
    }
    if ($shouldRefreshInstaller.Invoke($devForm, [object[]]@($true, $true))) {
        throw "Dev Mode unexpectedly refreshes Installer status."
    }
}
finally {
    $normalForm.Dispose()
    $devForm.Dispose()
}

Write-Host "MainForm layout smoke test passed: layout, forks, resume state, and Installer query policy"
