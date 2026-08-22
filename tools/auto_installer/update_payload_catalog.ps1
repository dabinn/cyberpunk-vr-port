param(
    [Parameter(Mandatory = $true)]
    [string]$PayloadRoot,
    [string]$CatalogPath = (Join-Path $PSScriptRoot "CyberpunkVRPort-Auto-Installer.ini"),
    [switch]$Update
)

$ErrorActionPreference = "Stop"

$payloadRootPath = (Resolve-Path -LiteralPath $PayloadRoot).Path.TrimEnd('\', '/')
$catalogFile = (Resolve-Path -LiteralPath $CatalogPath).Path
$lines = [IO.File]::ReadAllLines($catalogFile)
$sectionStart = [Array]::IndexOf($lines, "[PayloadFilesEver]")
if ($sectionStart -lt 0) {
    throw "The catalog does not contain [PayloadFilesEver]: $catalogFile"
}

$sectionEnd = $lines.Length
for ($index = $sectionStart + 1; $index -lt $lines.Length; $index++) {
    if ($lines[$index].TrimStart().StartsWith('[')) {
        $sectionEnd = $index
        break
    }
}

$knownPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$highestKey = 0
for ($index = $sectionStart + 1; $index -lt $sectionEnd; $index++) {
    $line = $lines[$index].Trim()
    if (-not $line -or $line.StartsWith(';')) { continue }
    if ($line -notmatch '^([^=]+)=(.+)$') {
        throw "Invalid [PayloadFilesEver] entry: $line"
    }
    $key = 0
    if (-not [int]::TryParse($Matches[1], [ref]$key)) {
        throw "Non-numeric [PayloadFilesEver] key: $($Matches[1])"
    }
    if (-not $knownPaths.Add($Matches[2].Replace('\', '/'))) {
        throw "Duplicate [PayloadFilesEver] path: $($Matches[2])"
    }
    $highestKey = [Math]::Max($highestKey, $key)
}

$payloadFiles = @(Get-ChildItem -LiteralPath $payloadRootPath -Recurse -File | ForEach-Object {
    $_.FullName.Substring($payloadRootPath.Length + 1).Replace('\', '/')
} | Sort-Object -Unique)
if ($payloadFiles.Count -eq 0) {
    throw "The payload root contains no files: $payloadRootPath"
}

$missingPaths = @($payloadFiles | Where-Object { -not $knownPaths.Contains($_) })
if ($missingPaths.Count -eq 0) {
    Write-Host "Payload catalog coverage passed: $($payloadFiles.Count) file(s), 0 missing."
    exit 0
}

if (-not $Update) {
    Write-Error ("Payload catalog is missing {0} exact path(s):`n{1}" -f
        $missingPaths.Count, ($missingPaths -join "`n"))
    exit 1
}

$newEntries = [Collections.Generic.List[string]]::new()
foreach ($path in $missingPaths) {
    $highestKey++
    $newEntries.Add(('{0:D4}={1}' -f $highestKey, $path))
}

$updatedLines = [Collections.Generic.List[string]]::new()
for ($index = 0; $index -lt $sectionEnd; $index++) { $updatedLines.Add($lines[$index]) }
$updatedLines.AddRange($newEntries)
for ($index = $sectionEnd; $index -lt $lines.Length; $index++) { $updatedLines.Add($lines[$index]) }
[IO.File]::WriteAllLines($catalogFile, $updatedLines, [Text.UTF8Encoding]::new($false))
Write-Host "Added $($newEntries.Count) exact path(s) to [PayloadFilesEver]."
