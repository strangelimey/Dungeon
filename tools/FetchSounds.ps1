# Installs the game's audio from the local OneDrive archive.
#
# Bought/downloaded sound libraries live OUTSIDE the repo, in
# OneDrive\DungeonAssets\audio\<library>\... — raw and pristine, exactly as
# they were downloaded. This script normalizes the ones the game actually uses
# into assets\sounds\ via `AssetBaker import-sound`. Those outputs are
# gitignored, so run this after cloning (and after adding a worktree — see
# CLAUDE.md's provisioning checklist, where audio is the third step alongside
# textures and models).
#
# THE TABLE BELOW IS THE DECISION RECORD, like SortTextureDownloads' $sets.
# One row per sound the game names, saying where it came from and what it IS —
# because "what it is" decides how it gets imported:
#
#   Role = 'positional'  (default) The sound belongs to a place or a body: a
#                        drip, a door, a footstep, a sword landing. DOWNMIXED
#                        TO MONO, because X3DAudio cannot place a stereo file —
#                        it has already committed its channels. A stereo drip
#                        would simply refuse to move as you walk past it.
#   Role = 'stereo'      The sound is never positional: the level's ambient
#                        bed, UI. Keeps its stereo image, plays to the master.
#
#   Loop = $true         A bed, or a projectile's flight sound. Seam-checked
#                        (a bad loop ticks once per cycle, forever) and NEVER
#                        trimmed, since the tail is half of the seam.
#
# Source may be a FILE (one sound) or a FOLDER (several takes of the same
# thing, imported as <name>_1..N — the variants that stop a footstep sounding
# like a machine gun).
#
# Usage:  powershell -File tools\FetchSounds.ps1            # everything
#         powershell -File tools\FetchSounds.ps1 -Sounds a,b
#         powershell -File tools\FetchSounds.ps1 -List

param(
    [string[]] $Sounds = @(),
    [switch] $List
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$assets = Join-Path $repo "assets"

# ---------------------------------------------------------------------------
# The sound table. Paths are relative to OneDrive\DungeonAssets\audio\.
#
# EMPTY until the first library is bought — see docs/sound.md's sourcing
# section (Sonniss GDC bundle first; it is free, commercially licensed, and
# vendor-organized rather than sorted by what the sound is, so filing it into
# rows here IS the curation step).
#
# The commented rows are the shape, not real files:
#
#   @{ Name = 'amb_dungeon';   Source = 'gravity\dungeon\drip_hall_01.wav'
#      Role = 'stereo';        Loop = $true }
#   @{ Name = 'amb_drip';      Source = 'sonniss\water\single_drip.wav' }
#   @{ Name = 'step_stone';    Source = 'sonniss\foley\steps_stone\' }
#   @{ Name = 'door_open';     Source = 'zapsplat\doors\wood_heavy_open.wav' }
# ---------------------------------------------------------------------------
$soundSets = @(
)

# ---------------------------------------------------------------------------

if ($List) {
    if ($soundSets.Count -eq 0) { Write-Host "No sounds in the table yet." }
    else { $soundSets | ForEach-Object { [pscustomobject]$_ } | Format-Table -AutoSize }
    return
}

$oneDrive = if ($env:OneDrive) { $env:OneDrive } else { Join-Path $env:USERPROFILE "OneDrive" }
$archive = Join-Path $oneDrive "DungeonAssets\audio"

$baker = Join-Path $repo "build\release\bin\AssetBaker.exe"
if (-not (Test-Path $baker)) { $baker = Join-Path $repo "build\debug\bin\AssetBaker.exe" }
if (-not (Test-Path $baker)) { throw "Build AssetBaker first (build.cmd release)" }

if ($soundSets.Count -eq 0) {
    Write-Host "Nothing to import: the `$soundSets table in this script is empty."
    Write-Host "Drop a library into $archive and add a row per sound (see the header)."
    return
}
if (-not (Test-Path $archive)) {
    throw "Audio archive not found: $archive`nCreate it and put the raw downloads there (see docs/sound.md)."
}

# Run AssetBaker without letting its stderr abort the batch. The importer logs
# warnings (a wide stereo fold, a suspect loop seam) to stderr, and under PS 5.1
# a native command's stderr becomes a terminating NativeCommandError while
# $ErrorActionPreference is Stop — which would kill the whole run over a benign
# warning. So merge stderr into stdout as text and key success ONLY off the exit
# code. (Same trap, same fix, as FetchTextures.ps1.)
function Invoke-Baker {
    param([Parameter(ValueFromRemainingArguments = $true)] $bakerArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $baker @bakerArgs 2>&1 | ForEach-Object { Write-Host "  $_" } }
    finally { $ErrorActionPreference = $prev }
    return $LASTEXITCODE
}

$wanted = if ($Sounds.Count -gt 0) {
    $soundSets | Where-Object { $Sounds -contains $_.Name }
} else { $soundSets }

if ($wanted.Count -eq 0) { throw "No table rows matched: $($Sounds -join ', ')" }

$imported = 0
$failed = @()
$missing = @()

foreach ($set in $wanted) {
    $src = Join-Path $archive $set.Source
    if (-not (Test-Path $src)) {
        # Report, never guess — a missing source is a table row that has drifted
        # from the archive, and silently skipping it produces a game that runs
        # silent in one spot for no visible reason.
        $missing += "$($set.Name)  <- $($set.Source)"
        continue
    }

    $bakerArgs = @('import-sound', $src, $assets, $set.Name)
    if ($set.Role -eq 'stereo') { $bakerArgs += '--stereo' }
    if ($set.Loop) { $bakerArgs += '--loop' }
    if ($set.Rate) { $bakerArgs += @('--rate', $set.Rate) }

    Write-Host "$($set.Name)  <- $($set.Source)"
    if ((Invoke-Baker @bakerArgs) -eq 0) { $imported++ } else { $failed += $set.Name }
}

Write-Host ""
Write-Host "Imported $imported of $($wanted.Count)."
if ($missing.Count -gt 0) {
    Write-Host "Not found in the archive ($($missing.Count)):"
    $missing | ForEach-Object { Write-Host "  $_" }
}
if ($failed.Count -gt 0) {
    Write-Host "FAILED ($($failed.Count)): $($failed -join ', ')"
    exit 1
}
