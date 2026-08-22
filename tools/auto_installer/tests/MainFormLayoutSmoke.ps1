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
        $installationStatus = Get-Control $form "installationStatusLabel"
        $vrportIniPrefix = Get-Control $form "vrportIniPrefixLabel"
        $vrportIniStatus = Get-Control $form "vrportIniStatusLabel"
        $releaseNotes = Get-Control $form "releaseNotesLink"
        $install = Get-Control $form "installButton"
        $uninstall = Get-Control $form "uninstallButton"
        $browse = Get-Control $form "browseButton"
        $refresh = Get-Control $form "refreshButton"
        $titlePoint = Get-FormPoint $title
        $languagePoint = Get-FormPoint $language
        $gamePoint = Get-FormPoint $game
        $statusPoint = Get-FormPoint $status
        $installerPoint = Get-FormPoint $installerStatus
        $installPoint = Get-FormPoint $install
        $installationPoint = Get-FormPoint $installationStatus
        $vrportPrefixPoint = Get-FormPoint $vrportIniPrefix
        $vrportPoint = Get-FormPoint $vrportIniStatus

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
        if (-not $installerStatus.Text.StartsWith('[v1.1] ')) {
            throw "Installer version was not prefixed to the existing update status."
        }
        $setInstallerStatus = $formType.GetMethod('SetInstallerStatus',
            [Reflection.BindingFlags]'Instance,NonPublic')
        $setInstallerStatus.Invoke($form, [object[]]@('InstallerStatusUpdateAvailable'))
        if (-not $installerStatus.Enabled -or -not $installerStatus.Text.StartsWith('[v1.1] ')) {
            throw "Available Installer update status is not an enabled versioned text link."
        }
        $setInstallerStatus.Invoke($form, [object[]]@('InstallerStatusCurrent'))
        if ($installerStatus.Enabled) { throw "Current Installer status unexpectedly remained clickable." }
        if (-not ($installPoint.Y -lt $installationPoint.Y -and $installationPoint.Y -lt $vrportPoint.Y -and
            $vrportPoint.Y -lt $statusPoint.Y)) {
            throw "Install and vrport.ini status lines are not between the action buttons and footer."
        }
        if ($vrportPrefixPoint.X - $installationPoint.X -ne 12) {
            throw "vrport.ini secondary status row does not retain its 12-pixel indent."
        }
        if ($vrportIniStatus.GetType() -ne [Windows.Forms.LinkLabel]) {
            throw "vrport.ini status is not a text link."
        }
        if ($vrportIniPrefix.GetType() -ne [Windows.Forms.Label] -or
            $vrportIniPrefix.GetType() -eq [Windows.Forms.LinkLabel]) {
            throw "vrport.ini prefix unexpectedly became part of the text link."
        }
        $refreshInstallationStatus = $formType.GetMethod('RefreshInstallationStatus',
            [Reflection.BindingFlags]'Instance,NonPublic')
        $game.Text = Join-Path ([IO.Path]::GetTempPath()) ("CyberpunkVRPort-missing-ini-" + [Guid]::NewGuid().ToString("N"))
        $refreshInstallationStatus.Invoke($form, @())
        if ($uninstall.Text -ne 'Cleanup') {
            throw "Uninstall button did not show Cleanup without Installer state."
        }
        $managedRoot = Join-Path ([IO.Path]::GetTempPath()) ("CyberpunkVRPort-managed-state-" + [Guid]::NewGuid().ToString("N"))
        try {
            New-Item -ItemType Directory -Path (Join-Path $managedRoot 'bin\x64') -Force | Out-Null
            Set-Content -LiteralPath (Join-Path $managedRoot 'bin\x64\Cyberpunk2077.exe') -Value 'marker' -NoNewline
            Set-Content -LiteralPath (Join-Path $managedRoot 'bin\x64\CyberpunkVRPort-Auto-Installer.state.ini') -Value @"
[InstalledFilesEver]
"@
            $game.Text = $managedRoot
            $refreshInstallationStatus.Invoke($form, @())
            if ($uninstall.Text -ne 'Uninstall') {
                throw "Uninstall button did not show Uninstall with Installer state."
            }
        }
        finally {
            if (Test-Path -LiteralPath $managedRoot) {
                $resolvedManagedRoot = (Resolve-Path -LiteralPath $managedRoot).Path
                $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
                if (-not $resolvedManagedRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                    throw "Unsafe managed-state test cleanup target: $resolvedManagedRoot"
                }
                Remove-Item -LiteralPath $resolvedManagedRoot -Recurse -Force
            }
        }
        $expectedVrportPrefix = [string]([char]0x2514) + [string]([char]0x2500) + ' vrport.ini:'
        if (-not $vrportIniStatus.Enabled -or $vrportIniPrefix.Text -ne $expectedVrportPrefix -or
            $vrportIniStatus.Text -ne 'Not present' -or
            $vrportIniStatus.LinkColor -ne [Drawing.Color]::Gainsboro) {
            throw "Missing vrport.ini state is not displayed as a white clickable status-only link."
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
            $releaseType = $assembly.GetType('CyberpunkVRPort.AutoInstaller.GitHubRelease', $true)
            $testRelease = [Activator]::CreateInstance($releaseType, $true)
            $propertyFlags = [Reflection.BindingFlags]'Instance,NonPublic'
            $releaseType.GetProperty('Name', $propertyFlags).SetValue($testRelease, 'Test release')
            $releaseType.GetProperty('HtmlUrl', $propertyFlags).SetValue($testRelease, 'https://github.com/example/repo/releases/tag/test')
            $releaseType.GetProperty('HasReleaseNotes', $propertyFlags).SetValue($testRelease, $true)
            $release.Items.Add($testRelease) | Out-Null
            $release.SelectedItem = $testRelease
            $form.PerformLayout()
            $getControlState = [Windows.Forms.Control].GetMethod('GetState',
                [Reflection.BindingFlags]'Instance,NonPublic')
            $releaseNotesVisible = $getControlState.Invoke($releaseNotes, [object[]]@(2))
            if (-not $releaseNotesVisible -or $releaseNotes.Text -ne 'Notes') {
                throw "Release notes text link was not shown for a selected release with notes."
            }
            $releaseNotesPoint = Get-FormPoint $releaseNotes
            $releasePoint = Get-FormPoint $release
            if (-not ($gamePoint.Y -lt $forkPoint.Y -and $forkPoint.Y -lt $releasePoint.Y) -or
                [Math]::Abs($forkPoint.Y - $releaseNotesPoint.Y) -gt 12 -or
                $releaseNotesPoint.X -le $forkPoint.X) {
                throw "Release notes link is not in the Fork row action column before the Version row."
            }
            if ($gamePoint.X -ne $forkPoint.X -or $forkPoint.X -ne $releasePoint.X -or
                $game.Width -ne $fork.Width -or $fork.Width -ne $release.Width) {
                throw "Normal field controls are not horizontally aligned."
            }
            $releaseType.GetProperty('HasReleaseNotes', $propertyFlags).SetValue($testRelease, $false)
            $updateReleaseNotes = $formType.GetMethod('UpdateReleaseNotesLink', [Reflection.BindingFlags]'Instance,NonPublic')
            $updateReleaseNotes.Invoke($form, @())
            if ($getControlState.Invoke($releaseNotes, [object[]]@(2))) {
                throw "Release notes link remained visible for an empty release body."
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

$localPackageRoot = Join-Path ([IO.Path]::GetTempPath()) ("CyberpunkVRPort-local-package-test-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $localPackageRoot | Out-Null
try {
    $olderFolder = Join-Path $localPackageRoot "CyberpunkVR-older"
    $newerZip = Join-Path $localPackageRoot "CyberpunkVR-newer.zip"
    $ignoredFolder = Join-Path $localPackageRoot "unrelated-folder"
    New-Item -ItemType Directory -Path $olderFolder | Out-Null
    New-Item -ItemType Directory -Path $ignoredFolder | Out-Null
    Set-Content -LiteralPath $newerZip -Value "zip-marker" -NoNewline
    Set-Content -LiteralPath (Join-Path $localPackageRoot "ignored.txt") -Value "not-a-package" -NoNewline
    [IO.Directory]::SetLastWriteTimeUtc($olderFolder, [DateTime]::UtcNow.AddMinutes(-2))
    [IO.File]::SetLastWriteTimeUtc($newerZip, [DateTime]::UtcNow.AddMinutes(-1))
    $findLocalPackages = $formType.GetMethod(
        "FindLocalPackages", [Reflection.BindingFlags]"Static,NonPublic")
    $packages = $findLocalPackages.Invoke($null, [object[]]@(
        [string]$localPackageRoot, [string]'^CyberpunkVR-.*\.zip$'))
    if ($packages.Count -ne 2 -or $packages[0].ToString() -ne "CyberpunkVR-newer.zip" -or
        $packages[1].ToString() -ne "CyberpunkVR-older") {
        throw "Local package discovery did not filter immediate folders and ZIPs by pattern in newest-first order."
    }
}
finally {
    $resolvedLocalPackageRoot = (Resolve-Path -LiteralPath $localPackageRoot).Path
    $tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolvedLocalPackageRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe local package test cleanup target: $resolvedLocalPackageRoot"
    }
    Remove-Item -LiteralPath $resolvedLocalPackageRoot -Recurse -Force
}

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
    $shouldRequireLatestInstaller = $formType.GetMethod(
        "ShouldRequireLatestInstaller", [Reflection.BindingFlags]"Instance,NonPublic")
    if (-not $shouldRequireLatestInstaller.Invoke($normalForm, [object[]]@($true)) -or
        $shouldRequireLatestInstaller.Invoke($normalForm, [object[]]@($false))) {
        throw "Normal mode did not restrict the mandatory Installer update gate to Install."
    }
    if ($shouldRequireLatestInstaller.Invoke($devForm, [object[]]@($true))) {
        throw "Dev Mode unexpectedly enabled the mandatory Installer update gate."
    }
}
finally {
    $normalForm.Dispose()
    $devForm.Dispose()
}

Write-Host "MainForm layout smoke test passed: layout, local packages, forks, resume state, and Installer query policy"
