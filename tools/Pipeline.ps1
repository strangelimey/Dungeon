# ============================================================================
# tools\Pipeline.ps1 - what the asset scripts share. Dot-source it:
#
#   . (Join-Path $PSScriptRoot 'Pipeline.ps1')
#   $baker = Find-AssetBaker $repo                  # release, else debug
#   if ((Invoke-Baker import $src $assets $name) -ne 0) { throw "..." }
#
# FetchTextures, FetchModels and ReplayImports each carried their own baker
# call, and the copies drifted: ReplayImports ran AssetBaker bare under
# $ErrorActionPreference = 'Stop', where one harmless warning on stderr aborts
# the whole batch - the exact failure FetchTextures' wrapper was written for
# (code-review C438). One copy now. (Phase 2's C405 adds the archive root and
# the Blender search here.)
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================

# The AssetBaker to run: the RELEASE build's, else the debug one's (the release
# baker is the one to use - its BC7 encoder is many times faster).
function Find-AssetBaker([string] $repo) {
    foreach ($config in @('release', 'debug')) {
        $exe = Join-Path $repo "build\$config\bin\AssetBaker.exe"
        if (Test-Path $exe) { return $exe }
    }
    throw "Build AssetBaker first (build.cmd release)"
}

# Run AssetBaker without letting its stderr abort us. AssetBaker logs warnings
# (e.g. "No height/displacement map found") to stderr; under PS 5.1 a native
# command's stderr is wrapped as a terminating NativeCommandError when
# $ErrorActionPreference is Stop, which would kill the whole batch over a benign
# warning. So merge stderr into stdout as plain text and key success ONLY off the
# real process exit code. Returns $LASTEXITCODE.
#
# Runs the `$baker` of the script that dot-sourced this file (Find-AssetBaker's
# answer), so every call site reads as the command it runs.
function Invoke-Baker {
    param([Parameter(ValueFromRemainingArguments = $true)] $bakerArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $baker @bakerArgs 2>&1 | ForEach-Object { Write-Host "$_" } }
    finally { $ErrorActionPreference = $prev }
    return $LASTEXITCODE
}

# The project block format (see Game/Serialize.h): `[id]` headers with
# `key = value` lines; ';' starts a comment on its own line. Returns one ordered
# dictionary per block, its header under `id`. A missing file is no blocks.
function Read-Blocks([string] $path) {
    $blocks = @()
    if (-not (Test-Path $path)) { return $blocks }
    $current = $null
    foreach ($line in Get-Content -Encoding UTF8 $path) {
        $t = $line.Trim()
        if (-not $t -or $t.StartsWith(";")) { continue }
        if ($t.StartsWith("[")) {
            if ($current) { $blocks += $current }
            $current = [ordered]@{ id = $t.Trim('[', ']') }
            continue
        }
        if (-not $current) { continue }
        $eq = $t.IndexOf("=")
        if ($eq -lt 0) { continue }
        $current[$t.Substring(0, $eq).Trim()] = $t.Substring($eq + 1).Trim()
    }
    if ($current) { $blocks += $current }
    return $blocks
}
