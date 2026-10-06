# ============================================================================
# tools\StaleTest.ps1 - a harness refuses a stale exe; CheckAll builds first.
#
#   .\tools\StaleTest.ps1          # exit 0 = PASS
#
# No harness used to ask whether the exe it launched was current (code-review
# C426): CheckAll's -Only skipped the build rows, the Python judges read
# build\debug and never built, and a PASS on yesterday's binary read as a PASS
# on today's change. Now every harness asks the build system first
# (Assert-ExeCurrent / refuse_if_stale) and CheckAll brings the build rows a
# selection needs. This checks both, and cheaply - nothing here launches a game:
#
#   1. with the debug tree current, `CheckAll -Plan` puts the right build row
#      FIRST for a lone check (alloc, profile, bc7, spells), in self-test too;
#   2. touching a source the game links makes the stale check see it, and
#      AllocTest, SpellTest and Eval refuse with exit 4 - not 1, which is FAIL;
#   3. DN_HARNESS_ALLOW_STALE turns the refusal into a warning;
#   4. putting the file's time back makes the tree current again.
#
# The file is touched by its TIMESTAMP only, and step 4 restores it whatever
# happened (a finally), so the run leaves no rebuild behind.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root 'tools'
$exe = Join-Path $root 'build\debug\bin\Dungeon.exe'
. (Join-Path $tools 'HarnessGame.ps1')

$checks = 0
$failed = 0
function Expect($what, $ok, $detail) {
	$script:checks++
	if (-not $ok) { $script:failed++ }
	Write-Host ("  {0,-62} {1,-16} {2}" -f $what, $detail, $(if ($ok) { 'ok' } else { 'FAIL' }))
}

if (-not (Test-Path $exe)) { Write-Host "stale: no exe at $exe - build debug first"; exit 2 }
if ((Get-StaleSteps $exe) -ne 0) {
	Write-Host 'stale: the debug tree must be CURRENT to start - run .\build.cmd debug' -ForegroundColor Red
	exit 2
}

# The rows `CheckAll -Plan` would run, in order.
function Plan([hashtable]$with) {
	$out = @(& (Join-Path $tools 'CheckAll.ps1') -Plan @with 6>&1 | ForEach-Object { "$_" })
	return @($out | Where-Object { $_ -match '^  [a-z][a-z0-9-]*(\s|$)' } | ForEach-Object { ($_.Trim() -split '\s+')[0] })
}

Write-Host ''
Write-Host '=== CheckAll brings the build a selection needs, first ==='
$cases = @(
	@{ with = @{ Only = @('alloc') };                       build = 'build-debug';   check = 'alloc' },
	@{ with = @{ Only = @('alloc'); SelfTest = $true };     build = 'build-debug';   check = 'alloc' },
	@{ with = @{ Only = @('profile') };                     build = 'build-profile'; check = 'profile' },
	@{ with = @{ Only = @('bc7'); SelfTest = $true };       build = 'build-release'; check = 'bc7' },
	@{ with = @{ Only = @('spells'); Config = 'release' };  build = 'build-debug';   check = 'spells' },
	@{ with = @{ Only = @('pipeline'); Config = 'release' }; build = 'build-release'; check = 'pipeline' }
)
foreach ($c in $cases) {
	$rows = Plan $c.with
	$at = [array]::IndexOf($rows, $c.check)
	$said = ($c.with.GetEnumerator() | Sort-Object Key | ForEach-Object { "-$($_.Key) $($_.Value -join ',')" }) -join ' '
	Expect ("CheckAll {0}: {1} first" -f $said, $c.build) `
		($rows.Count -ge 2 -and $rows[0] -eq $c.build -and $at -gt 0) ($rows -join ',')
}

Write-Host ''
Write-Host '=== a harness on its own refuses a stale exe (exit 4) ==='
$source = Join-Path $root 'src\Main\Main.cpp'
$was = (Get-Item $source).LastWriteTimeUtc
try {
	(Get-Item $source).LastWriteTimeUtc = [DateTime]::UtcNow
	$steps = Get-StaleSteps $exe
	Expect 'touching Main.cpp makes the build system see work' ($steps -ge 1) "$steps step(s)"

	& (Join-Path $tools 'AllocTest.ps1') *> $null
	Expect 'AllocTest refuses, with the stale code' ($LASTEXITCODE -eq $HarnessExitStale) "exit $LASTEXITCODE"
	& python (Join-Path $tools 'SpellTest.py') *> $null
	Expect 'SpellTest refuses, with the stale code' ($LASTEXITCODE -eq $HarnessExitStale) "exit $LASTEXITCODE"
	& (Join-Path $tools 'Eval.ps1') -Only reset *> $null
	Expect 'Eval refuses, with the stale code' ($LASTEXITCODE -eq $HarnessExitStale) "exit $LASTEXITCODE"
	& (Join-Path $tools 'PipelineTest.ps1') *> $null
	Expect 'PipelineTest refuses, with the stale code' ($LASTEXITCODE -eq $HarnessExitStale) "exit $LASTEXITCODE"

	# The escape hatch: a warning, and the run goes on. A dot-sourced call, so
	# nothing launches.
	$env:DN_HARNESS_ALLOW_STALE = '1'
	$said = @(& { Assert-ExeCurrent $exe } 6>&1 | ForEach-Object { "$_" })
	Expect 'DN_HARNESS_ALLOW_STALE warns instead of refusing' (($said -join ' ') -match 'WARNING: .*STALE') ($said.Count)
} finally {
	Remove-Item Env:DN_HARNESS_ALLOW_STALE -ErrorAction SilentlyContinue
	(Get-Item $source).LastWriteTimeUtc = $was
}
$steps = Get-StaleSteps $exe
Expect 'with the time put back, the tree is current again' ($steps -eq 0) "$steps step(s)"

Write-Host ''
Write-Host ("stale RESULT={0} checks={1} failures={2}" -f $(if ($failed -eq 0) { 'PASS' } else { 'FAIL' }), $checks, $failed)
exit $(if ($failed -eq 0) { 0 } else { 1 })
