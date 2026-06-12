# Installs the game's scanned texture sets from the local OneDrive archive.
#
# The raw Poly Haven downloads (CC0) for every material live in
# OneDrive\DungeonAssets\<1k|2k|4k>\<material>\, copied there once from the
# original site. This script imports them all into assets/textures as packed
# PNG + BC7 DDS pairs (<name>_<res>.png / _n.png / .dds). None of those files
# are committed - run this once after cloning, then build (the build copies
# assets next to the exe).
#
# Usage:  powershell -File tools\FetchTextures.ps1 [-Resolutions 1k,2k,4k]

param([string[]] $Resolutions = @("1k", "2k", "4k"))

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$assets = Join-Path $repo "assets"

$oneDrive = if ($env:OneDrive) { $env:OneDrive } else { Join-Path $env:USERPROFILE "OneDrive" }
$archive = Join-Path $oneDrive "DungeonAssets"
if (-not (Test-Path $archive)) { throw "Asset archive not found: $archive" }

$baker = Join-Path $repo "build\release\bin\AssetBaker.exe"
if (-not (Test-Path $baker)) { $baker = Join-Path $repo "build\debug\bin\AssetBaker.exe" }
if (-not (Test-Path $baker)) { throw "Build AssetBaker first (build.cmd release)" }

foreach ($res in $Resolutions) {
    $resDir = Join-Path $archive $res
    if (-not (Test-Path $resDir)) { Write-Host "No $res sets in archive - skipped"; continue }
    foreach ($materialDir in Get-ChildItem $resDir -Directory) {
        $name = "$($materialDir.Name)_$res"
        Write-Host "Importing $name..."
        & $baker import $materialDir.FullName $assets $name
        if ($LASTEXITCODE -ne 0) { throw "Import failed for $name" }
    }
}

Write-Host ""
Write-Host "Texture sets installed. Build (or robocopy assets) to sync them next"
Write-Host "to the game exe, then pick a quality tier in Settings."
