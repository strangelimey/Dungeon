# Files a textures.com download batch into the OneDrive archive.
#
# textures.com hands every map out as a separate file in Downloads, named
# TCom_<TheirName>_<res>_<map>.tif — the product's name, not the game's. The
# archive instead wants OneDrive\DungeonAssets\<res>\<category>\<name>\
# <name>_<map>.tif, keyed by the name the CATALOG will use. This script is the
# translation, and $sets below is the whole of the judgement: one row per
# purchased set saying which archive category it files under and what the game
# calls it. Everything else is mechanical.
#
# The category folder is organisational (it mirrors the FreePBR pack), but the
# NAME is load-bearing twice over: the worn block mesh is baked per set as
# worn_<name>_<tier>.gltf, so a set belongs to exactly ONE surface kind for the
# life of the project, and LoadPbrSet resolves a catalog's `texture` field to
# <name>_<res>. Renaming later means re-importing and re-baking, so the names
# here are chosen to be final — and to not collide with the batch-1 sets
# (wall_stone, wall_brick, floor_cobble, ceiling_rough, wood_planks, ...).
#
# Unknown files are REPORTED, never guessed at: a set missing from $sets means
# the table is out of date, and silently filing it under a made-up name would
# be worse than stopping. Existing files are never overwritten without -Force.
#
# Usage:  powershell -File tools\SortTextureDownloads.ps1 [-WhatIf]
#                    [-Downloads <dir>] [-Resolution 4k] [-Copy] [-Force]
#
# Then:   powershell -File tools\FetchTextures.ps1 -Materials <names printed at
#                    the end> -Resolutions 4k
#
# ...followed by the worn-block bake and the catalog entries; see CLAUDE.md's
# asset pipeline section and the textures-com-sourcing notes.

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string] $Downloads = (Join-Path $env:USERPROFILE "Downloads"),
    [string] $Resolution = "4k",
    [switch] $Copy,
    [switch] $Force
)

$ErrorActionPreference = "Stop"

$oneDrive = if ($env:OneDrive) { $env:OneDrive } else { Join-Path $env:USERPROFILE "OneDrive" }
$archive = Join-Path $oneDrive "DungeonAssets"

# --- the batch-2 manifest -------------------------------------------------
# TCom source stem (everything before _<res>_) -> archive category + game name.
# 39 sets bought 2026-08-03. Roles were decided at purchase time and are fixed
# by the one-kind rule above: walls stay walls, the three rock surfaces are
# CEILINGS (textures.com sells no vaulted stone), ground/* are floors that
# happen to file under the archive's ground category.
$sets = @{
    # --- walls (19) -------------------------------------------------------
    "TCom_Wall_CobblestoneMixed1_5x2.5"           = @{ Cat = "walls";    Name = "wall_cobble_mixed" }
    "TCom_Wall_CobblestoneMixed4_4x2"             = @{ Cat = "walls";    Name = "wall_cobble_mixed4" }
    "TCom_Wall_CobblestoneMossy1_5x2.5"           = @{ Cat = "walls";    Name = "wall_cobble_mossy" }
    "TCom_Wall_CobblestoneRound1_2x2"             = @{ Cat = "walls";    Name = "wall_cobble_round" }
    "TCom_Wall_Stone1_2x2"                        = @{ Cat = "walls";    Name = "wall_stone_plain" }
    "TCom_Wall_Stone2_3x3"                        = @{ Cat = "walls";    Name = "wall_stone_granite" }
    "TCom_Wall_Stone28_4.4x2.2"                   = @{ Cat = "walls";    Name = "wall_stone_28" }
    "TCom_Wall_Stone30_4x2"                       = @{ Cat = "walls";    Name = "wall_stone_30" }
    "TCom_Wall_Stone34"                           = @{ Cat = "walls";    Name = "wall_stone_34" }
    "TCom_Wall_BrickWeatheredMossy_5x2.5"         = @{ Cat = "walls";    Name = "wall_brick_weathered" }
    # "coarse", not "rough": a set name may not contain a map-type token — see
    # the $reserved check below, which this row is the reason for.
    "TCom_Wall_BrickRough3_4x2"                   = @{ Cat = "walls";    Name = "wall_brick_coarse" }
    "TCom_Wall_BrickOld3B_2x2"                    = @{ Cat = "walls";    Name = "wall_brick_plaster" }
    "TCom_Wall_BrickSloppy_2.5x2.5"               = @{ Cat = "walls";    Name = "wall_brick_distorted" }
    "TCom_Wall_BrickOld2_2.1x2.1"                 = @{ Cat = "walls";    Name = "wall_brick_old" }
    "TCom_Wall_Sandstone_Blocks4"                 = @{ Cat = "walls";    Name = "wall_sandstone_blocks" }
    "TCom_SandstoneBlockWall2"                    = @{ Cat = "walls";    Name = "wall_sandstone_block2" }
    "TCom_SandstoneWall"                          = @{ Cat = "walls";    Name = "wall_temple_sandstone" }
    "TCom_AncientWall"                            = @{ Cat = "walls";    Name = "wall_temple_ancient" }
    "TCom_CarvedSandstoneWallA"                   = @{ Cat = "walls";    Name = "wall_carved" }

    # --- floors (10) ------------------------------------------------------
    "TCom_Pavement_MedievalFloor8_5x5"            = @{ Cat = "floors";   Name = "floor_medieval" }
    "TCom_Pavement_CobblestoneMedieval12Path_6x3" = @{ Cat = "floors";   Name = "floor_cobble_path" }
    "TCom_Pavement_CobblestoneMedieval04_5x5"     = @{ Cat = "floors";   Name = "floor_cobble_medieval" }
    "TCom_Pavement_CobblestoneMossy01B_5x5"       = @{ Cat = "floors";   Name = "floor_cobble_mossy" }
    "TCom_Pavement_Stone1_2x2"                    = @{ Cat = "floors";   Name = "floor_stone_pavement" }
    "TCom_AncientFloor"                           = @{ Cat = "floors";   Name = "floor_temple" }
    "TCom_AncientFloorB"                          = @{ Cat = "floors";   Name = "floor_ancient_stone" }
    "TCom_OldPavingStone"                         = @{ Cat = "floors";   Name = "floor_paving_mossy" }
    "TCom_SlatePavement3"                         = @{ Cat = "floors";   Name = "floor_slate" }
    # A stair TREAD surface, for the stairs mesh rather than a floor cell.
    "TCom_Pavement_CobblestoneMossy03Stairs_4x4"  = @{ Cat = "floors";   Name = "floor_stairs" }

    # --- ground: cave / mine floors (4) -----------------------------------
    "TCom_Ground_RockBedSandstone_2x2"            = @{ Cat = "ground";   Name = "ground_rockbed" }
    "TCom_Ground_SoilDusty_1x1A"                  = @{ Cat = "ground";   Name = "ground_soil_dusty" }
    "TCom_Ground_SoilRocky11"                     = @{ Cat = "ground";   Name = "ground_soil_rocky" }
    "TCom_Road_Gravel_3x3"                        = @{ Cat = "ground";   Name = "ground_gravel" }

    # --- ceilings (3) -----------------------------------------------------
    # Rock surfaces DESIGNATED ceilings, the same trick ceiling_rough already
    # plays: the site sells no vaulted stone ceiling.
    "TCom_Rock_Cliff2"                            = @{ Cat = "ceilings"; Name = "ceiling_rock" }
    "TCom_Rock_Base5_2x2"                         = @{ Cat = "ceilings"; Name = "ceiling_rock_layered" }
    "TCom_Rock_Porous_New"                        = @{ Cat = "ceilings"; Name = "ceiling_rock_porous" }

    # --- wood (3) ---------------------------------------------------------
    "TCom_Wood_Planks1Base_noSeams_2x2"           = @{ Cat = "wood";     Name = "wood_planks_clean" }
    "TCom_Wood_PlanksOld3Base_3.5x3.5"            = @{ Cat = "wood";     Name = "wood_planks_old3" }
    "TCom_Wood_PlanksOld8_2x2"                    = @{ Cat = "wood";     Name = "wood_planks_old8" }
}

# The map tokens the archive (and the C++ importer's DiscoverPbrMaps) expect.
# textures.com already names its files with exactly these, so the token passes
# through untouched — anything else is a map kind we did not mean to buy
# (mask/opacity in particular, which DiscoverPbrMaps would read as an ALPHA
# channel and silently cut holes in the albedo).
$knownMaps = @("albedo", "ao", "height", "normal", "roughness", "metallic")

# A set NAME may not contain any substring the importer uses to identify a map
# KIND, because the two meet in one filename: <name>_<map>.png. DiscoverPbrMaps
# tests those substrings against the whole stem, IN ORDER, and roughness is
# tested before albedo — so a set called wall_brick_rough has its
# wall_brick_rough_albedo.png claimed as the ROUGHNESS map, and the import then
# dies with "No albedo/basecolor map found". This list mirrors PbrMaps.cpp; the
# check is here rather than in review because the failure looks like a bad
# download rather than a bad name, and it only surfaces after the slow bake.
$reserved = @("albedo", "basecolor", "base_color", "diffuse", "normal", "_nor",
              "_nrm", "height", "displacement", "_disp", "bump", "rough",
              "metallic", "metalness", "metal", "_met", "ambientocclusion",
              "ambient_occlusion", "_ao", "occ", "opacity", "opac", "alpha",
              "transp", "mask")
$clashes = @()
foreach ($k in $sets.Keys) {
    $n = $sets[$k].Name
    foreach ($tok in $reserved) {
        if ($n.ToLower().Contains($tok)) { $clashes += "$n  (contains '$tok')" }
    }
}
if ($clashes.Count -gt 0) {
    throw ("Set name(s) collide with a map-type token, which would break the import:`n  " +
           ($clashes -join "`n  "))
}

if (-not (Test-Path $Downloads)) { throw "Downloads folder not found: $Downloads" }
if (-not (Test-Path $archive))   { throw "Archive not found: $archive" }

Write-Host ""
Write-Host "Sorting $Resolution maps" -ForegroundColor Cyan
Write-Host "  from $Downloads"
Write-Host "  into $archive\$Resolution\<category>\<name>\"
Write-Host ""

$files = Get-ChildItem -Path $Downloads -Filter "TCom_*.tif" -File
if ($files.Count -eq 0) { throw "No TCom_*.tif files in $Downloads" }

$moved = 0
$skipped = 0
$unknownSets = @{}
$strays = @()
$touched = @{}

foreach ($f in $files) {
    # TCom_<source>_<res>_<map>.tif — the source stem may itself contain dots
    # and underscores (TCom_Wall_Stone28_4.4x2.2), so anchor on the res token.
    $m = [regex]::Match($f.Name, "^(?<src>.+)_$([regex]::Escape($Resolution))_(?<map>[A-Za-z]+)\.tif$",
                        [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $m.Success) {
        $strays += $f.Name
        continue
    }

    $src = $m.Groups["src"].Value
    $map = $m.Groups["map"].Value.ToLower()

    if ($knownMaps -notcontains $map) {
        Write-Warning "$($f.Name): unexpected map kind '$map' - not filed"
        $skipped++
        continue
    }
    if (-not $sets.ContainsKey($src)) {
        $unknownSets[$src] = $true
        $skipped++
        continue
    }

    $entry = $sets[$src]
    $destDir = Join-Path (Join-Path (Join-Path $archive $Resolution) $entry.Cat) $entry.Name
    $destPath = Join-Path $destDir "$($entry.Name)_$map.tif"

    if ((Test-Path $destPath) -and (-not $Force)) {
        Write-Warning "exists, left alone: $($entry.Cat)\$($entry.Name)\$($entry.Name)_$map.tif"
        $skipped++
        continue
    }

    if ($PSCmdlet.ShouldProcess($destPath, $(if ($Copy) { "Copy" } else { "Move" }))) {
        if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
        if ($Copy) {
            Copy-Item -LiteralPath $f.FullName -Destination $destPath -Force
        } else {
            Move-Item -LiteralPath $f.FullName -Destination $destPath -Force
        }
    }
    $moved++
    if (-not $touched.ContainsKey($entry.Name)) {
        $touched[$entry.Name] = @{ Cat = $entry.Cat; Count = 0 }
    }
    $touched[$entry.Name].Count++
}

# --- report ---------------------------------------------------------------
# Counts what this run FILED rather than what is on disk, so the summary reads
# the same under -WhatIf as for real. A set short of its 5 core maps means the
# download batch itself was incomplete — worth knowing before the BC7 bake.
Write-Host ""
foreach ($name in ($touched.Keys | Sort-Object)) {
    $n = $touched[$name].Count
    $flag = ""
    if ($n -lt 5) { $flag = "  <-- INCOMPLETE, expected 5 or 6" }
    Write-Host ("  {0,-12} {1,-24} {2} maps{3}" -f $touched[$name].Cat, $name, $n, $flag)
}

Write-Host ""
Write-Host "$moved file(s) filed into $($touched.Count) set(s); $skipped skipped." -ForegroundColor Green

if ($strays.Count -gt 0) {
    Write-Host ""
    Write-Host "Not $Resolution maps, left in Downloads:" -ForegroundColor Yellow
    foreach ($s in $strays) { Write-Host "  $s" }
}

if ($unknownSets.Count -gt 0) {
    Write-Host ""
    Write-Host "UNKNOWN sets - add them to `$sets and re-run:" -ForegroundColor Red
    foreach ($u in ($unknownSets.Keys | Sort-Object)) { Write-Host "  $u" }
}

if ($touched.Count -gt 0) {
    $names = ($touched.Keys | Sort-Object) -join ","
    Write-Host ""
    Write-Host "Next - import them (BC7 bake, slow):" -ForegroundColor Cyan
    # -Command, NOT -File: powershell.exe -File passes `a,b,c` as ONE string and
    # binds it to [string[]] as a single element, so every name silently matches
    # nothing and FetchTextures reports "Nothing imported". Only -Command (or a
    # call from inside PowerShell) splits the list into an array.
    Write-Host "  powershell -Command `"& { .\tools\FetchTextures.ps1 -Resolutions $Resolution -Materials $names }`""
}
