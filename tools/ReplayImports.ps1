# Rebuilds the assets a project's editor-created types depend on.
#
# Creating a type in the editor ("+ New" -> Import new) runs AssetBaker over a
# download folder and writes the result into assets/textures + assets/models.
# Those directories are gitignored, so the catalog entry reaches git but its
# asset does not - a fresh clone would render magenta (textures) or abort at
# level load (models).
#
# The editor therefore records every import in the project's provenance
# manifest, assets\projects\<project>\catalog\imports.cat:
#
#     [mywall_2k]              ; the POOL asset name, not the catalog id
#     kind = texture           ; texture | model
#     source = C:\Users\...\OneDrive\DungeonAssets\2k\walls\foo
#     flip_green = 1           ; textures: 1 = the green was flipped, 0 = it was
#                              ; not (absent, as in an older record: by the
#                              ; normal map's name)
#     surface = wall           ; a surface set: the kind its worn meshes bake as
#
# This script replays them: for each entry whose asset is missing, it re-runs
# the same AssetBaker commands the editor ran - including the worn-block bake
# at the RELIEF and WEAR the surface type carries in its catalog, as the
# editor's type save passes them. Without those a replayed surface would bake at
# the set's own record and overwrite the worn meshes its type was tuned to
# (code-review C438). Sets that are already installed are skipped unless -Force.
#
# `source` is an absolute path from whichever machine did the import. If it no
# longer exists, a path under the asset archive is RE-ROOTED onto this machine's
# archive (the tail from "DungeonAssets\" onwards), which is how the same import
# replays on a second machine or after the OneDrive root moves.
#
# Usage:  powershell -File tools\ReplayImports.ps1 [-Project dungeon-demo]
#                    [-Force] [-WhatIf]
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.

param(
    [string] $Project = "dungeon-demo",
    [switch] $Force,
    [switch] $WhatIf
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$assets = Join-Path $repo "assets"
$catalogDir = Join-Path $assets "projects\$Project\catalog"
$manifest = Join-Path $catalogDir "imports.cat"

# Find-AssetBaker, Invoke-Baker (the stderr-safe call every asset script uses)
# and Read-Blocks (the block format).
. (Join-Path $PSScriptRoot 'Pipeline.ps1')

if (-not (Test-Path $manifest)) {
    Write-Host "No imports manifest for project '$Project' - nothing to replay."
    exit 0
}

$oneDrive = if ($env:OneDrive) { $env:OneDrive } else { Join-Path $env:USERPROFILE "OneDrive" }
$archive = Join-Path $oneDrive "DungeonAssets"

$baker = Find-AssetBaker $repo

$entries = @(Read-Blocks $manifest)
if (-not $entries) {
    Write-Host "Manifest is empty - nothing to replay."
    exit 0
}

# Re-roots a stale absolute path onto this machine's asset archive.
function Resolve-Source([string] $path) {
    if (Test-Path $path) { return $path }
    $marker = "DungeonAssets\"
    $at = $path.IndexOf($marker, [StringComparison]::OrdinalIgnoreCase)
    if ($at -ge 0) {
        $tail = $path.Substring($at + $marker.Length)
        $candidate = Join-Path $archive $tail
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

# The worn-block knobs a surface set's TYPE carries: `relief` and `wear` from
# the first entry of the kind's catalog that paints with the set (its `texture`,
# or its own id when it names none - as the editor's save does). An absent field
# stays absent, so the baker falls back to the set's own record
# (Assets/WornSets.h) exactly as the editor's bake would. Types that disagree
# are reported: the worn mesh is ONE per set, so only one of them can win.
function Get-WornKnobs([string] $surface, [string] $set) {
    $catalog = Join-Path $catalogDir "$($surface)s.cat"
    $users = @(Read-Blocks $catalog | Where-Object {
        $tex = if ($_.Contains('texture')) { $_['texture'] } else { $_['id'] }
        $tex -eq $set
    })
    $knobs = @()
    if (-not $users) { return $knobs }
    $first = $users[0]
    foreach ($u in @($users | Select-Object -Skip 1)) {
        if ($u['relief'] -ne $first['relief'] -or $u['wear'] -ne $first['wear']) {
            Write-Host ("  warning: types $($first.id) and $($u.id) bake '$set' differently " +
                        "(relief/wear) - replaying $($first.id)'s") -ForegroundColor Yellow
        }
    }
    if ($first['relief']) { $knobs += @('--relief', $first['relief']) }
    if ($first['wear']) { $knobs += @('--wear', $first['wear']) }
    return $knobs
}

$replayed = 0
$skipped = 0
$missing = @()

foreach ($e in $entries) {
    $id = $e.id
    $kind = if ($e.kind) { $e.kind } else { "texture" }

    # Already installed? A texture set is its albedo PNG, a model its .gltf.
    $installed = if ($kind -eq "model") {
        Test-Path (Join-Path $assets "models\$id.gltf")
    } else {
        Test-Path (Join-Path $assets "textures\$id.png")
    }
    if ($installed -and -not $Force) {
        $skipped++
        continue
    }

    $source = Resolve-Source $e.source
    if (-not $source) {
        $missing += "$id (source gone: $($e.source))"
        continue
    }

    if ($kind -eq "model") {
        $bakerArgs = @("import-model", $source, $assets, $id)
    } else {
        $bakerArgs = @("import", $source, $assets, $id)
        # The flip the import was made with, either way (code-review C393): the
        # editor always tells the baker, so a replay must not guess again.
        if ($e.flip_green -eq "1") { $bakerArgs += "--flip-green" }
        elseif ($e.flip_green -eq "0") { $bakerArgs += "--no-flip-green" }
    }

    # A surface set also needs its worn block meshes - the editor's second bake
    # step. Only for the kind the set was imported as: the mesh geometry differs
    # per kind but the FILE NAME does not (worn_<set>_<tier>.gltf, one per set),
    # so baking "all three" would just overwrite twice and leave the wrong shape.
    $wornArgs = $null
    if ($kind -eq "texture" -and $e.surface) {
        $base = $id -replace '_(1k|2k|4k)$', ''
        $wornArgs = @("wornblock", $e.surface, $base, $assets) + @(Get-WornKnobs $e.surface $base)
    }

    Write-Host "Replaying $kind '$id' from $source"
    if ($WhatIf) {
        Write-Host "  would run: AssetBaker $($bakerArgs -join ' ')"
        if ($wornArgs) { Write-Host "  would run: AssetBaker $($wornArgs -join ' ')" }
        continue
    }
    $code = Invoke-Baker @bakerArgs
    if ($code -ne 0) { throw "AssetBaker failed for '$id' (exit $code)" }
    if ($wornArgs) {
        $code = Invoke-Baker @wornArgs
        if ($code -ne 0) { throw "wornblock $($e.surface) failed for '$($wornArgs[2])' (exit $code)" }
    }
    $replayed++
}

Write-Host ""
Write-Host "Replayed $replayed, skipped $skipped already installed."
if ($missing) {
    Write-Host "Could not replay (source unavailable):" -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    exit 1
}
