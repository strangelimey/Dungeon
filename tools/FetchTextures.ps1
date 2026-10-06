# Installs the game's scanned texture sets from the local OneDrive archive.
#
# The raw PBR downloads (Poly Haven CC0 + FreePBR Premium) live in
# OneDrive\DungeonAssets\<1k|2k|4k>\<category>\<material>\ - the res folder
# is the material's native resolution, the category folders mirror the
# FreePBR pack (walls, floors, rocks, metals, ...) plus ceilings for the
# Poly Haven sets. This script imports materials into assets/textures as
# packed PNG + BC7 DDS pairs (<name>_<res>.png / _n.png / .dds). None of
# those files are committed - run after cloning. The game reads this tree
# directly, so nothing needs copying afterwards.
#
# By default every texture set the CONTENT names is imported: each `texture` /
# `part2_texture` field of every catalog - the worlds' (assets\projects\*\
# catalog), the style library's (assets\library) and the new-world template's
# (assets\templates\*\catalog) - plus the fixed set of prop/creature sets the
# game binds in code ($propSets below: sconce, brazier, monsters), and then the
# rune tablets' sets, carved into the runestone set by `AssetBaker runes`. A name the
# catalogs give that this script cannot install is an ERROR, raised before
# anything is baked, unless another script installs it (FetchModels.ps1's table,
# or an editor import ReplayImports.ps1 replays) - those are listed, not fetched.
# Use -Materials to pull specific sets by name (props are skipped then; a name
# the archive lacks is said, not fatal), or -All for the whole archive
# (hundreds of sets - the BC7 bake takes a while).
#
# The list used to come from the `textures` records of assets\maps\*.map - the
# old one-level format, which nothing had parsed since levels moved to `palette`
# records of catalog ids - so a fresh clone got 2 of the 12 sets the crypts draw,
# drew the rest magenta and still reported success (code-review C402).
#
# Mixed source formats: Poly Haven / FreePBR sets ship loose PNG/JPG maps the
# importer reads directly. textures.com PBR sets instead ship TIFF (8/16-bit),
# which the C++ importer's stb_image cannot read - so a folder containing any
# TIFF is staged to PNG first (Convert-TiffMaps, WIC-based, bit depth preserved
# so a 16-bit height map stays 16-bit for stbi_load_16) and imported with
# --flip-green (textures.com normals are OpenGL but their filenames lack the
# 'gl' token the importer auto-detects).
#
# Usage, from the repo root:
#   powershell -Command "& { .\tools\FetchTextures.ps1 }"
#   powershell -Command "& { .\tools\FetchTextures.ps1 -Materials wall_stone,floor_cobble -Resolutions 2k,4k }"
#   powershell -Command "& { .\tools\FetchTextures.ps1 -All -Resolutions 2k }"
# Add -WhatIf inside the braces to list what would be imported, baking nothing.
#
# -Command, NOT -File, for a comma list: powershell.exe -File passes `a,b,c` as
# ONE string, which binds to [string[]] as a single element, so every name
# matches nothing and the run ends "Nothing imported". tools\UsageLinesTest.ps1
# dry-runs the lines above and fails if one stops selecting what it names.

[CmdletBinding(SupportsShouldProcess = $true)]
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

# Find-AssetBaker and Invoke-Baker: the baker call every asset script shares,
# which keys success off the exit code alone so a warning on stderr cannot abort
# the batch under 'Stop' (tools\Pipeline.ps1). A -WhatIf run bakes nothing, so it
# needs no baker.
. (Join-Path $PSScriptRoot 'Pipeline.ps1')
$baker = $null
try { $baker = Find-AssetBaker $repo } catch { if (-not $WhatIfPreference) { throw } }

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

# Prop / creature PBR sets bound by code (DungeonWorld::LoadPropTextures) or
# named by a catalog after a model rather than a material, so they are listed
# here explicitly: the source archive folder is renamed to the output name the
# game loads by convention (model/monster <name> -> texture set <name>). These
# are 2k-native, and the loader falls back to the 2k set at every quality tier,
# so only the 2k variant is imported - except where `Bare` says the set is read
# by its bare name (RuneBaker carves the rune tablets from textures\runestone.png,
# not runestone_2k). All carry OpenGL normals -> --flip-green. Skipped when the
# caller scopes the fetch with -Materials.
$propSets = @(
    @{ Src = "metals\worn-medieval";        Name = "sconce" }   # iron torch holder
    @{ Src = "metals\bronze";               Name = "brazier" }  # bronze fire bowl
    @{ Src = "rocks\carvedlimestoneground1"; Name = "skeleton" } # bone
    @{ Src = "fabric\burlap-stained1";      Name = "mummy" }    # bandages
    @{ Src = "organic\alien-slime1";        Name = "blob" }     # slime
    @{ Src = "rocks\flaking-limestone1";    Name = "runestone"; Bare = $true } # rune tablets (RuneBaker carves the glyph in)
    # Authored decoration meshes (import-model writes the .gltf, committed; the
    # PBR maps live in the same model folder and are gitignored, so re-pack them
    # here too). Their normals are OGL like the rest.
    @{ Src = "models\sharp-boulder1";       Name = "boulder" }     # rubble
    @{ Src = "models\mossy-rock-model";     Name = "mossy_rock" }  # mossy rock
    @{ Src = "models\primative-handled-pot"; Name = "pot" }        # clay pot
    @{ Src = "models\ancient-pot1";         Name = "ancient_pot" } # the ancient pot
)

# The archive's material folders by name: name -> the resolutions holding it.
$inArchive = @{}
foreach ($res in @("1k", "2k", "4k")) {
    $resDir = Join-Path $archive $res
    if (-not (Test-Path $resDir)) { continue }
    foreach ($categoryDir in Get-ChildItem $resDir -Directory) {
        foreach ($materialDir in Get-ChildItem $categoryDir.FullName -Directory) {
            $key = $materialDir.Name.ToLowerInvariant()
            if (-not $inArchive.ContainsKey($key)) { $inArchive[$key] = @() }
            if ($inArchive[$key] -notcontains $res) { $inArchive[$key] += $res }
        }
    }
}

# The sets OTHER scripts install, which the catalogs may name: FetchModels.ps1's
# table (each entry that imports a set - not a multi-material model, whose maps
# ride inside its .glb - under its TextureSet, else its Name; read from the
# script itself, never copied) and the editor imports a world records in its
# catalog\imports.cat (pool names are <set>_<res>), which ReplayImports.ps1
# replays.
function Get-ElsewhereSets {
    $sets = @{}
    $fetchModels = Join-Path $PSScriptRoot "FetchModels.ps1"
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($fetchModels, [ref]$null, [ref]$null)
    $table = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and
        $n.Left.Extent.Text -eq '$modelSets' }, $true)
    if (-not $table) { throw "FetchModels.ps1 has no `$modelSets table to read" }
    foreach ($m in $table.Right.Expression.SafeGetValue()) {
        if ($m.MultiMaterial) { continue }
        $set = if ($m.TextureSet) { $m.TextureSet } else { $m.Name }
        $sets[$set.ToLowerInvariant()] = "FetchModels.ps1"
    }
    foreach ($manifest in Get-ChildItem (Join-Path $assets "projects\*\catalog\imports.cat") -ErrorAction SilentlyContinue) {
        foreach ($b in Read-Blocks $manifest.FullName) {
            if ($b.Contains("kind") -and $b["kind"] -ne "texture") { continue }
            $set = $b.id -replace '_(1k|2k|4k)$', ''
            if (-not $sets.ContainsKey($set.ToLowerInvariant())) {
                $sets[$set.ToLowerInvariant()] = "ReplayImports.ps1 (" + $manifest.Directory.Parent.Name + ")"
            }
        }
    }
    return $sets
}

# The wanted set: explicit names, or every set the catalogs name.
$wanted = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($name in $Materials) { [void]$wanted.Add($name) }
if (-not $All -and $Materials.Count -eq 0) {
    $catalogs = @(Get-ChildItem (Join-Path $assets "projects\*\catalog\*.cat") -ErrorAction SilentlyContinue) +
                @(Get-ChildItem (Join-Path $assets "library\*.cat") -ErrorAction SilentlyContinue) +
                @(Get-ChildItem (Join-Path $assets "templates\*\catalog\*.cat") -ErrorAction SilentlyContinue)
    $namedBy = @{} # set -> the first catalog naming it, for the error below
    foreach ($cat in $catalogs) {
        foreach ($b in Read-Blocks $cat.FullName) {
            foreach ($field in @("texture", "part2_texture")) {
                if (-not $b.Contains($field)) { continue }
                $name = $b[$field]
                # `none`: a model drawn on its own glTF material, by design.
                if (-not $name -or $name -eq "none") { continue }
                [void]$wanted.Add($name)
                $key = $name.ToLowerInvariant()
                if (-not $namedBy.ContainsKey($key)) {
                    $namedBy[$key] = "$($cat.FullName.Substring($assets.Length + 1)) [$($b.id)]"
                }
            }
        }
    }
    if ($wanted.Count -eq 0) { throw "No catalog names a texture set - is $assets a checkout?" }

    # Route each name: a prop set this script renames, a set another script
    # installs, an archive folder - or nothing, which is the error.
    $propNames = @($propSets | ForEach-Object { $_.Name.ToLowerInvariant() })
    $elsewhere = Get-ElsewhereSets
    $leftTo = @{}
    $unknown = @()
    foreach ($name in @($wanted)) {
        $key = $name.ToLowerInvariant()
        if ($propNames -contains $key) { [void]$wanted.Remove($name); continue }
        if ($inArchive.ContainsKey($key)) { continue }
        [void]$wanted.Remove($name)
        if ($elsewhere.ContainsKey($key)) {
            $by = $elsewhere[$key]
            if (-not $leftTo.ContainsKey($by)) { $leftTo[$by] = @() }
            $leftTo[$by] += $name
            continue
        }
        $unknown += "$name (named by $($namedBy[$key]))"
    }
    if ($unknown.Count) {
        throw ("The catalogs name $($unknown.Count) texture set(s) that no archive folder holds and no " +
               "fetch script installs:`n  " + ($unknown -join "`n  ") + "`nAdd each to `$propSets here " +
               "(a renamed archive folder) or to FetchModels.ps1's table, or fix the catalog.")
    }
    Write-Host "Importing the $($wanted.Count) texture sets the catalogs name (plus the prop sets below)."
    foreach ($by in $leftTo.Keys | Sort-Object) {
        Write-Host "  left to $($by): $(($leftTo[$by] | Sort-Object) -join ', ')"
    }
} elseif (-not $All) {
    foreach ($name in $Materials) {
        $have = $inArchive[$name.ToLowerInvariant()]
        if (-not $have) { Write-Host "  $name is not in the archive - skipped" -ForegroundColor Yellow }
        else {
            $none = @($Resolutions | Where-Object { $have -notcontains $_ })
            if ($none.Count) { Write-Host "  $name has no $($none -join '/') set in the archive" -ForegroundColor Yellow }
        }
    }
}

$imported = 0
foreach ($res in $Resolutions) {
    $resDir = Join-Path $archive $res
    if (-not (Test-Path $resDir)) { Write-Host "No $res sets in archive - skipped"; continue }
    foreach ($categoryDir in Get-ChildItem $resDir -Directory) {
        foreach ($materialDir in Get-ChildItem $categoryDir.FullName -Directory) {
            if (-not $All -and -not $wanted.Contains($materialDir.Name)) { continue }
            $name = "$($materialDir.Name)_$res"
            # Counted either way, so a -WhatIf run ends with the same verdict.
            $imported++
            if (-not $PSCmdlet.ShouldProcess($name, "Import from $($materialDir.FullName)")) { continue }
            Write-Host "Importing $name..."
            $conv = Convert-TiffMaps $materialDir.FullName
            $bakerArgs = @('import', $conv.Dir, $assets, $name)
            if ($conv.ForceFlipGreen) { $bakerArgs += '--flip-green' }
            if ((Invoke-Baker @bakerArgs) -ne 0) { throw "Import failed for $name" }
        }
    }
}
# The prop sets ($propSets, above). A missing source is an error, not a skip:
# the table names exact folders, so one that is gone leaves a set the game asks
# for uninstalled - the fetch must not report success over it.
if ($Materials.Count -eq 0) {
    Write-Host "Importing the $($propSets.Count) code-bound prop/creature sets (2k)."
    foreach ($prop in $propSets) {
        $src = Join-Path (Join-Path $archive "2k") $prop.Src
        if (-not (Test-Path $src)) { throw "The archive has no 2k\$($prop.Src), which `$propSets installs as $($prop.Name)" }
        $out = if ($prop.Bare) { $prop.Name } else { "$($prop.Name)_2k" }
        $imported++
        if (-not $PSCmdlet.ShouldProcess($out, "Import from $src")) { continue }
        Write-Host "Importing $out..."
        $conv = Convert-TiffMaps $src
        if ((Invoke-Baker import $conv.Dir $assets $out --flip-green) -ne 0) {
            throw "Import failed for $out"
        }
    }
    # The rune tablets' sets (rune_<symbol>_2k, bound in code by RuneItemId) are
    # not in the archive: `AssetBaker runes` CARVES them into the runestone set
    # just installed, mips and all. Without this a fresh clone drew every rune
    # flat ("texture set 'rune_fire' not found"), and the runestone it installed
    # was under a name RuneBaker never read, so a later `runes` carved plain
    # procedural stone instead.
    if ($PSCmdlet.ShouldProcess("rune_<symbol>_2k", "Carve the rune sets (AssetBaker runes)")) {
        Write-Host "Carving the rune sets (AssetBaker runes)..."
        if ((Invoke-Baker runes $assets) -ne 0) { throw "AssetBaker runes failed - see the baker's log above" }
    }
}

if ($imported -eq 0) { throw "Nothing imported - check the material names against the archive" }

Write-Host ""
if ($WhatIfPreference) {
    Write-Host "$imported texture sets would be imported (-WhatIf: nothing baked)."
} else {
    Write-Host "$imported texture sets installed. The game reads this tree directly,"
    Write-Host "so just relaunch, then pick a quality tier in Settings."
}
