# Fetches the 4K versions of the game's Poly Haven material sets (CC0) and
# imports them as the Ultra quality tier (<name>_4k.png/.dds pairs).
#
# The 4K files are deliberately NOT committed to the repo (~600 MB); run this
# script once to enable the Ultra setting. Requires a built AssetBaker
# (release preferred: build.cmd release).
#
# Usage:  powershell -File tools\FetchUltraTextures.ps1

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$assets = Join-Path $repo "assets"

$baker = Join-Path $repo "build\release\bin\AssetBaker.exe"
if (-not (Test-Path $baker)) { $baker = Join-Path $repo "build\debug\bin\AssetBaker.exe" }
if (-not (Test-Path $baker)) { throw "Build AssetBaker first (build.cmd release)" }

# Material slot -> Poly Haven asset slug (must match assets/textures/SOURCES.txt).
$sets = [ordered]@{
    "wall_brick"      = "castle_brick_broken_06"
    "wall_stone"      = "medieval_blocks_03"
    "wall_moss"       = "mossy_brick"
    "floor_slabs"     = "seaworn_stone_tiles"
    "floor_cobble"    = "patterned_cobblestone"
    "ceiling_rough"   = "rock_face"
    "ceiling_cracked" = "rock_boulder_cracked"
}

$dl = Join-Path $env:TEMP "pbr_dl_4k"
New-Item -ItemType Directory -Force $dl | Out-Null

foreach ($target in $sets.Keys) {
    $slug = $sets[$target]
    $dir = Join-Path $dl $target
    New-Item -ItemType Directory -Force $dir | Out-Null
    Write-Host "Downloading $slug (4K)..."
    $files = curl.exe -s "https://api.polyhaven.com/files/$slug" | ConvertFrom-Json
    foreach ($map in @("Diffuse", "nor_dx", "Displacement", "AO")) {
        $entry = $files.$map
        if ($null -eq $entry) { Write-Host "  $map missing (importer will cope)"; continue }
        $url = $entry.'4k'.png.url
        if ($null -eq $url) { Write-Host "  $map has no 4k png"; continue }
        $name = ($map -replace "Diffuse","diff" -replace "Displacement","disp" -replace "AO","ao")
        $out = Join-Path $dir "${slug}_${name}_4k.png"
        if (-not (Test-Path $out)) { curl.exe -sL -o $out $url }
    }
    Write-Host "Importing ${target}_4k..."
    & $baker import $dir $assets "${target}_4k"
    if ($LASTEXITCODE -ne 0) { throw "Import failed for $target" }
}

Write-Host ""
Write-Host "Ultra textures installed. Re-sync assets next to the game exe"
Write-Host "(a build does this automatically) and pick Quality: Ultra in Settings."
