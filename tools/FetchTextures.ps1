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
# By default the materials referenced by the levels' "textures" records
# (assets\maps\*.map) are imported, plus the fixed set of prop/creature sets
# the game binds in code ($propSets below: sconce, brazier, pillar, monsters).
# Use -Materials to pull specific sets by name (props are skipped then), or
# -All for the whole archive (hundreds of sets - the BC7 bake takes a while).
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
# Prop / creature PBR sets bound by code (DungeonWorld::LoadPropTextures), not
# by a map "textures" record, so they are listed here explicitly: the source
# archive folder is renamed to the output name the game loads by convention
# (model/monster <name> -> texture set <name>). These are 2k-native, and the
# loader falls back to the 2k set at every quality tier, so only the 2k variant
# is imported. All carry OpenGL normals -> --flip-green. Skipped when the caller
# scopes the fetch with -Materials.
$propSets = @(
    @{ Src = "metals\worn-medieval";        Name = "sconce" }   # iron torch holder
    @{ Src = "metals\bronze";               Name = "brazier" }  # bronze fire bowl
    @{ Src = "rocks\peacock-ore";           Name = "pillar" }   # serpent pillar
    @{ Src = "rocks\carvedlimestoneground1"; Name = "skeleton" } # bone
    @{ Src = "fabric\burlap-stained1";      Name = "mummy" }    # bandages
    @{ Src = "organic\alien-slime1";        Name = "blob" }     # slime
    @{ Src = "rocks\flaking-limestone1";    Name = "runestone" } # rune tablets (RuneBaker carves the glyph in)
    # Authored decoration meshes (import-model writes the .gltf, committed; the
    # PBR maps live in the same model folder and are gitignored, so re-pack them
    # here too). Their normals are OGL like the rest.
    @{ Src = "models\sharp-boulder1";       Name = "boulder" }     # rubble
    @{ Src = "models\mossy-rock-model";     Name = "mossy_rock" }  # mossy rock
    @{ Src = "models\primative-handled-pot"; Name = "pot" }        # clay pot
)
if ($Materials.Count -eq 0) {
    Write-Host "Importing the $($propSets.Count) code-bound prop/creature sets (2k)."
    foreach ($prop in $propSets) {
        $src = Join-Path (Join-Path $archive "2k") $prop.Src
        if (-not (Test-Path $src)) { Write-Host "  $($prop.Src) missing - skipped"; continue }
        Write-Host "Importing $($prop.Name)_2k..."
        & $baker import $src $assets "$($prop.Name)_2k" --flip-green
        if ($LASTEXITCODE -ne 0) { throw "Import failed for $($prop.Name)_2k" }
        $imported++
    }
}

if ($imported -eq 0) { throw "Nothing imported - check the material names against the archive" }

Write-Host ""
Write-Host "$imported texture sets installed. Build (or robocopy assets) to sync them"
Write-Host "next to the game exe, then pick a quality tier in Settings."
