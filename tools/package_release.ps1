param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$DistDir = "dist\DatPlayer",
    [switch]$Zip
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildRoot = Join-Path $repoRoot $BuildDir
$exePath = Join-Path $buildRoot (Join-Path $Configuration "DatPlayer.exe")
$distPath = Join-Path $repoRoot $DistDir
$distParent = Split-Path -Parent $distPath

if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "DatPlayer.exe was not found at '$exePath'. Build Release first."
}

if (-not (Test-Path -LiteralPath $distParent -PathType Container)) {
    New-Item -ItemType Directory -Path $distParent | Out-Null
}

$resolvedRepo = (Resolve-Path $repoRoot).Path
$fullDistPath = [System.IO.Path]::GetFullPath($distPath)
if (-not $fullDistPath.StartsWith($resolvedRepo, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to write outside the repository: '$fullDistPath'"
}

if (Test-Path -LiteralPath $fullDistPath) {
    Remove-Item -LiteralPath $fullDistPath -Recurse -Force
}
New-Item -ItemType Directory -Path $fullDistPath | Out-Null

Copy-Item -LiteralPath $exePath -Destination (Join-Path $fullDistPath "DatPlayer.exe")
Copy-Item -LiteralPath (Join-Path $repoRoot "README.md") -Destination (Join-Path $fullDistPath "README.md")

$unexpectedLocalDlls = Get-ChildItem -LiteralPath $fullDistPath -Filter *.dll -File -ErrorAction SilentlyContinue
if ($unexpectedLocalDlls) {
    Write-Warning "Unexpected DLLs found in release folder:"
    $unexpectedLocalDlls | ForEach-Object { Write-Warning "  $($_.Name)" }
}

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($dumpbin) {
    Write-Host "Dependency preflight via dumpbin:"
    & $dumpbin.Source /DEPENDENTS (Join-Path $fullDistPath "DatPlayer.exe")
} else {
    Write-Host "dumpbin.exe was not found on PATH; dependency listing skipped."
    Write-Host "Expected runtime dependencies are Windows system DLLs such as Media Foundation, GDI+, COM, and Win32 libraries."
}

$packagedFiles = Get-ChildItem -LiteralPath $fullDistPath -File | Select-Object -ExpandProperty Name
Write-Host "Packaged release:"
Write-Host "  $fullDistPath"
Write-Host "Files:"
$packagedFiles | ForEach-Object { Write-Host "  $_" }

if ($packagedFiles -contains "dat_decode_smoke_test.exe" -or $packagedFiles -contains "dat_indexer_tests.exe") {
    throw "Developer test executables should not be in the end-user release folder."
}

if (Get-ChildItem -LiteralPath $fullDistPath -Filter *.dat -File -ErrorAction SilentlyContinue) {
    throw "Sample DAT files should not be included in the release folder."
}

if ($Zip) {
    $zipPath = Join-Path (Split-Path -Parent $fullDistPath) "DatPlayer-portable.zip"
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path $fullDistPath -DestinationPath $zipPath
    Write-Host "Portable archive:"
    Write-Host "  $zipPath"
}
