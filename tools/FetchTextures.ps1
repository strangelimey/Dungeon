# Installs the game's scanned texture sets from the local OneDrive archive.
#
# The raw PBR downloads (Poly Haven CC0 + FreePBR Premium) live in
# OneDrive\DungeonAssets\<1k|2k|4k>\<category>\<material>\ — the res folder
# is the material's native resolution, the category folders mirror the
# FreePBR pack (walls, floors, rocks, metals, ...) plus ceilings for the
# Poly Haven sets. This script imports materials into assets/textures as
# packed PNG + BC7 DDS pairs (<name>_<res>.png / _n.png / .dds). None of
# those files are committed - run after cloning, then build (the build
# copies assets next to the exe).
#
# By default only the materials referenced by the levels' "textures" records
# (assets\maps\*.map) are imported - the game never loads anything else.
# Use -Materials to pull extra sets by name, or -All for the whole archive
# (hundreds of sets - the BC7 bake will take a long time).
#
# Usage:  powershell -File tools\FetchTextures.ps1 [-Resolutions 1k,2k,4k]
#                    [-Materials name1,name2,...] [-All]

param(
    [string[]] $Resolutions = @("1k", "2k", "4k"),
    [string[]] $Materials = @(),
    [switch] $All
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$assets = Join-Path $repo "assets"

$oneDrive = if ($env:OneDrive) { $env:OneDrive } else { Join-Path $env:USERPROFILE "OneDrive" }
$archive = Join-Path $oneDrive "DungeonAssets"
if (-not (Test-Path $archive)) { throw "Asset archive not found: $archive" }

$baker = Join-Path $repo "build\release\bin\AssetBaker.exe"
if (-not (Test-Path $baker)) { $baker = Join-Path $repo "build\debug\bin\AssetBaker.exe" }
if (-not (Test-Path $baker)) { throw "Build AssetBaker first (build.cmd release)" }

# The wanted set: explicit names, plus every set the levels reference.
$wanted = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($name in $Materials) { [void]$wanted.Add($name) }
if (-not $All -and $Materials.Count -eq 0) {
    foreach ($map in Get-ChildItem (Join-Path $assets "maps") -Filter *.map) {
        foreach ($line in Get-Content $map.FullName) {
            $tokens = -split $line
            if ($tokens.Count -ge 3 -and $tokens[0] -eq "textures") {
                foreach ($name in $tokens[2..($tokens.Count - 1)]) { [void]$wanted.Add($name) }
            }
        }
    }
    if ($wanted.Count -eq 0) { throw "No 'textures' records found in assets\maps\*.map" }
    Write-Host "Importing the $($wanted.Count) materials referenced by the maps."
}

$imported = 0
foreach ($res in $Resolutions) {
    $resDir = Join-Path $archive $res
    if (-not (Test-Path $resDir)) { Write-Host "No $res sets in archive - skipped"; continue }
    foreach ($categoryDir in Get-ChildItem $resDir -Directory) {
        foreach ($materialDir in Get-ChildItem $categoryDir.FullName -Directory) {
            if (-not $All -and -not $wanted.Contains($materialDir.Name)) { continue }
            $name = "$($materialDir.Name)_$res"
            Write-Host "Importing $name..."
            & $baker import $materialDir.FullName $assets $name
            if ($LASTEXITCODE -ne 0) { throw "Import failed for $name" }
            $imported++
        }
    }
}
if ($imported -eq 0) { throw "Nothing imported - check the material names against the archive" }

Write-Host ""
Write-Host "$imported texture sets installed. Build (or robocopy assets) to sync them"
Write-Host "next to the game exe, then pick a quality tier in Settings."
