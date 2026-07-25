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
#     flip_green = 1           ; optional, textures only
#
# This script replays them: for each entry whose asset is missing, it re-runs
# the same AssetBaker command the editor ran. Sets that are already installed
# are skipped unless -Force.
#
# `source` is an absolute path from whichever machine did the import. If it no
# longer exists, a path under the asset archive is RE-ROOTED onto this machine's
# archive (the tail from "DungeonAssets\" onwards), which is how the same import
# replays on a second machine or after the OneDrive root moves.
#
# Usage:  powershell -File tools\ReplayImports.ps1 [-Project dungeon-demo]
#                    [-Force] [-WhatIf]

param(
    [string] $Project = "dungeon-demo",
    [switch] $Force,
    [switch] $WhatIf
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$assets = Join-Path $repo "assets"
$manifest = Join-Path $assets "projects\$Project\catalog\imports.cat"

if (-not (Test-Path $manifest)) {
    Write-Host "No imports manifest for project '$Project' - nothing to replay."
    exit 0
}

$oneDrive = if ($env:OneDrive) { $env:OneDrive } else { Join-Path $env:USERPROFILE "OneDrive" }
$archive = Join-Path $oneDrive "DungeonAssets"

$baker = Join-Path $repo "build\release\bin\AssetBaker.exe"
if (-not (Test-Path $baker)) { $baker = Join-Path $repo "build\debug\bin\AssetBaker.exe" }
if (-not (Test-Path $baker)) { throw "Build AssetBaker first (build.cmd release)" }

# --- parse the block format (see Game/Serialize.h) ---------------------------
# [id] headers with "key = value" lines; ';' starts a comment on its own line.
$entries = @()
$current = $null
foreach ($line in Get-Content $manifest) {
    $t = $line.Trim()
    if (-not $t -or $t.StartsWith(";")) { continue }
    if ($t.StartsWith("[")) {
        if ($current) { $entries += $current }
        $current = [ordered]@{ id = $t.Trim('[', ']') }
        continue
    }
    if (-not $current) { continue }
    $eq = $t.IndexOf("=")
    if ($eq -lt 0) { continue }
    $current[$t.Substring(0, $eq).Trim()] = $t.Substring($eq + 1).Trim()
}
if ($current) { $entries += $current }

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
        if ($e.flip_green -eq "1") { $bakerArgs += "--flip-green" }
    }

    Write-Host "Replaying $kind '$id' from $source"
    if ($WhatIf) { Write-Host "  would run: AssetBaker $($bakerArgs -join ' ')"; continue }
    & $baker @bakerArgs
    if ($LASTEXITCODE -ne 0) { throw "AssetBaker failed for '$id' (exit $LASTEXITCODE)" }
    $replayed++

    # A surface set also needs its worn block meshes - the editor's second bake
    # step. Only for the kind the set was imported as: the mesh geometry differs
    # per kind but the FILE NAME does not (worn_<set>_<tier>.gltf, one per set),
    # so baking "all three" would just overwrite twice and leave the wrong shape.
    if ($kind -eq "texture" -and $e.surface) {
        $base = $id -replace '_(1k|2k|4k)$', ''
        & $baker wornblock $e.surface $base $assets
        if ($LASTEXITCODE -ne 0) { throw "wornblock $($e.surface) failed for '$base'" }
    }
}

Write-Host ""
Write-Host "Replayed $replayed, skipped $skipped already installed."
if ($missing) {
    Write-Host "Could not replay (source unavailable):" -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
    exit 1
}
