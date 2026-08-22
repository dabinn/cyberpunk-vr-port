param(
    [string]$Executable = "$PSScriptRoot\..\bin\Release\CyberpunkVRPort-Auto-Installer.exe"
)

$ErrorActionPreference = "Stop"
$assembly = [Reflection.Assembly]::LoadFile((Resolve-Path $Executable).Path)
$iniType = $assembly.GetType("CyberpunkVRPort.AutoInstaller.IniDocument", $true)
$loadMethod = $iniType.GetMethod("Load", [Reflection.BindingFlags]"Static,NonPublic", $null, [Type[]]@([IO.Stream]), $null)
$stream = $assembly.GetManifestResourceStream("CyberpunkVRPort.AutoInstaller.config.ini")
try { $ini = $loadMethod.Invoke($null, @($stream)) } finally { $stream.Dispose() }

$clientType = $assembly.GetType("CyberpunkVRPort.AutoInstaller.GitHubReleaseClient", $true)
$constructor = $clientType.GetConstructor([Reflection.BindingFlags]"Instance,NonPublic", $null, [Type[]]@($iniType), $null)
$client = $constructor.Invoke(@($ini))
try {
    $method = $clientType.GetMethod("GetReleasesAsync", [Reflection.BindingFlags]"Instance,NonPublic")
    $task = $method.Invoke($client, @())
    $null = $task.GetAwaiter().GetResult()
    $releases = @($task.Result)
    if ($releases.Count -eq 0) { throw "No compatible GitHub release assets were parsed." }
    $modReleaseCount = 0
    $installerReleaseCount = 0
    $releaseNotesCount = 0
    foreach ($release in $releases) {
        $releaseType = $release.GetType()
        $zip = $release.GetType().GetProperty("ZipAsset", [Reflection.BindingFlags]"Instance,NonPublic").GetValue($release)
        $installer = $release.GetType().GetProperty("InstallerAsset", [Reflection.BindingFlags]"Instance,NonPublic").GetValue($release)
        $htmlUrl = $releaseType.GetProperty("HtmlUrl", [Reflection.BindingFlags]"Instance,NonPublic").GetValue($release)
        $hasReleaseNotes = $releaseType.GetProperty("HasReleaseNotes", [Reflection.BindingFlags]"Instance,NonPublic").GetValue($release)
        if (-not [string]::IsNullOrWhiteSpace($htmlUrl) -and $hasReleaseNotes) { $releaseNotesCount++ }
        if ($null -ne $zip) {
            $name = $zip.GetType().GetProperty("Name", [Reflection.BindingFlags]"Instance,NonPublic").GetValue($zip)
            if (-not $name.EndsWith(".zip", [StringComparison]::OrdinalIgnoreCase)) { throw "Invalid ZIP asset: $name" }
            $modReleaseCount++
        }
        if ($null -ne $installer) { $installerReleaseCount++ }
    }
    if ($modReleaseCount -lt 3) { throw "Expected all three mod ZIP releases, got $modReleaseCount." }
    if ($installerReleaseCount -lt 1) { throw "No standalone Installer release was parsed." }
    if ($releaseNotesCount -lt 1) { throw "No release page with non-empty notes was parsed." }
    Write-Host "GitHub release smoke test passed: mod-releases=$modReleaseCount installer-releases=$installerReleaseCount notes=$releaseNotesCount"
}
finally {
    $client.Dispose()
}
