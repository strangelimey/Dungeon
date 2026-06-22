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
# Mixed source formats: Poly Haven / FreePBR sets ship loose PNG/JPG maps the
# importer reads directly. textures.com PBR sets instead ship TIFF (8/16-bit),
# which the C++ importer's stb_image cannot read - so a folder containing any
# TIFF is staged to PNG first (Convert-TiffMaps, WIC-based, bit depth preserved
# so a 16-bit height map stays 16-bit for stbi_load_16) and imported with
# --flip-green (textures.com normals are OpenGL but their filenames lack the
# 'gl' token the importer auto-detects).
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

# Run AssetBaker without letting its stderr abort us. AssetBaker logs warnings
# (e.g. "No height/displacement map found") to stderr; under PS 5.1 a native
# command's stderr is wrapped as a terminating NativeCommandError when
# $ErrorActionPreference is Stop, which would kill the whole batch over a benign
# warning. So merge stderr into stdout as plain text and key success ONLY off the
# real process exit code. Returns $LASTEXITCODE.
function Invoke-Baker {
    param([Parameter(ValueFromRemainingArguments = $true)] $bakerArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $baker @bakerArgs 2>&1 | ForEach-Object { Write-Host "$_" } }
    finally { $ErrorActionPreference = $prev }
    return $LASTEXITCODE
}

# TIFF -> PNG staging for textures.com sets (see header). WIC (PresentationCore)
# decodes the TIFF and re-encodes PNG preserving the source pixel format, so a
# 16-bit grayscale height map round-trips as a 16-bit PNG. Returns the directory
# to import from plus whether the set needs a forced green-channel flip: a folder
# with no TIFFs imports in place (Poly Haven / FreePBR, unchanged); a folder with
# TIFFs is a textures.com set, staged to PNG and flagged for --flip-green.
Add-Type -AssemblyName PresentationCore
function Convert-TiffMaps {
    param([string] $srcDir)

    $tiffs = Get-ChildItem -Path $srcDir -File | Where-Object { $_.Extension -match '^\.tiff?$' }
    if ($tiffs.Count -eq 0) {
        return [pscustomobject]@{ Dir = $srcDir; ForceFlipGreen = $false }
    }

    $stage = Join-Path $env:TEMP ("DungeonTexImport\" + (Split-Path $srcDir -Leaf))
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null

    foreach ($tif in $tiffs) {
        $out = Join-Path $stage ($tif.BaseName + ".png")
        $ins = [System.IO.File]::OpenRead($tif.FullName)
        try {
            $dec = New-Object System.Windows.Media.Imaging.TiffBitmapDecoder($ins,
                [System.Windows.Media.Imaging.BitmapCreateOptions]::PreservePixelFormat,
                [System.Windows.Media.Imaging.BitmapCacheOption]::OnLoad)
            $enc = New-Object System.Windows.Media.Imaging.PngBitmapEncoder
            $enc.Frames.Add([System.Windows.Media.Imaging.BitmapFrame]::Create($dec.Frames[0]))
            $outs = [System.IO.File]::Create($out)
            try { $enc.Save($outs) } finally { $outs.Close() }
        } finally { $ins.Close() }
    }

    # Carry over any maps that already ship as PNG/JPG (mixed-format folders).
    Get-ChildItem -Path $srcDir -File |
        Where-Object { $_.Extension -match '^\.(png|jpe?g)$' } |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $stage $_.Name) -Force }

    Write-Host "  staged $($tiffs.Count) TIFF map(s) -> PNG"
    return [pscustomobject]@{ Dir = $stage; ForceFlipGreen = $true }
}

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
            $conv = Convert-TiffMaps $materialDir.FullName
            $bakerArgs = @('import', $conv.Dir, $assets, $name)
            if ($conv.ForceFlipGreen) { $bakerArgs += '--flip-green' }
            if ((Invoke-Baker @bakerArgs) -ne 0) { throw "Import failed for $name" }
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
        $conv = Convert-TiffMaps $src
        if ((Invoke-Baker import $conv.Dir $assets "$($prop.Name)_2k" --flip-green) -ne 0) {
            throw "Import failed for $($prop.Name)_2k"
        }
        $imported++
    }
}

if ($imported -eq 0) { throw "Nothing imported - check the material names against the archive" }

Write-Host ""
Write-Host "$imported texture sets installed. Build (or robocopy assets) to sync them"
Write-Host "next to the game exe, then pick a quality tier in Settings."
