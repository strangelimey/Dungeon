# Launch Blender with the Claude bridge listening (tools\blender_bridge.py).
# Open a .blend or start empty, then Claude drives it while you watch.
#
# The Blender version is DISCOVERED, never pinned: Blender self-updates, and a
# hardcoded version silently stops matching the day it does (5.1 -> 5.2 once made
# every FetchModels import skip rather than fail). The sort is by [version], not
# by name — a string sort would rank "Blender 5.2" above a future "Blender 10.0".
param(
    [string]$Blender,                       # explicit blender.exe, skips discovery
    [int]$Port = 4242,                      # must match tools\bsend.py -p
    [string]$Open,                          # optional .blend to open
    [switch]$Background                     # headless (for testing the bridge)
)

$ErrorActionPreference = 'Stop'

if (-not $Blender) {
    $root = Join-Path $env:ProgramFiles 'Blender Foundation'
    if (-not (Test-Path $root)) { throw "No Blender install found: $root does not exist." }
    $newest = Get-ChildItem -LiteralPath $root -Directory |
        Where-Object { $_.Name -match '^Blender (\d+)\.(\d+)' } |
        Sort-Object { [version]($_.Name -replace '^Blender ', '') } |
        Select-Object -Last 1
    if (-not $newest) { throw "No 'Blender <ver>' directory under $root." }
    $Blender = Join-Path $newest.FullName 'blender.exe'
}
if (-not (Test-Path $Blender)) { throw "blender.exe not found: $Blender" }

$script = Join-Path $PSScriptRoot 'blender_bridge.py'
if (-not (Test-Path $script)) { throw "Bridge script missing: $script" }

# The bridge reads its port from the environment so the .py has no hardcoded
# copy to drift from this one.
$env:DUNGEON_BRIDGE_PORT = $Port

$blenderArgs = @()
if ($Background) { $blenderArgs += '--background' }
if ($Open) {
    if (-not (Test-Path $Open)) { throw "Blend file not found: $Open" }
    $blenderArgs += (Resolve-Path $Open).Path
}
$blenderArgs += @('--python', $script)

Write-Host "Blender : $Blender"
Write-Host "Bridge  : 127.0.0.1:$Port   (test: python tools\bsend.py -c ""print(1+1)"")"
& $Blender @blenderArgs
