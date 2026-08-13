# ============================================================================
# tools\Eval.ps1 - run the eval suites and surface what they measured.
#
#   .\tools\Eval.ps1                    # every suite
#   .\tools\Eval.ps1 -Only ladder       # one of them
#   .\tools\Eval.ps1 -List
#   .\tools\Eval.ps1 -SelfTest          # the runner must FAIL on purpose
#   .\tools\Eval.ps1 -Table             # print ONLY the measurements
#
# Exit 0 = every suite's script ran to completion with no unknown lines.
#
# THIS IS A MEASUREMENT HARNESS, NOT A PASS/FAIL ONE, and the distinction is
# the whole reason it is not in CheckAll's tiers. A green verdict here means the
# scripts RAN - not that the numbers are good. The numbers are the artefact:
# TALLY lines and blast tables, printed so a knob change can be diffed against a
# previous run. Nothing here asserts a balance number is correct, because
# nobody has decided what correct is (see docs/eval-harness.md).
#
# What IS checked is that the runner still works: a suite that silently stopped
# measuring would otherwise read exactly like one whose numbers had not changed.
# -SelfTest is the guard - it hands the runner a script containing a line that
# is not a command and requires exit 1.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[string[]]$Only = @(),
	[switch]$List,
	[switch]$SelfTest,
	[switch]$Table,
	[ValidateSet('debug', 'release')][string]$Config = 'debug'
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"
$scripts = Join-Path $root 'tools\EvalScripts'
$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'

# ---------------------------------------------------------------------------
# THE SUITES. `measure` is the regex whose matching log lines ARE the result -
# what a reader compares across runs. A suite with no measure line is a smoke
# test: it proves the machinery, and has nothing to say about balance.
#
# Only top-level scripts appear here. rungs\ and presets\ are FRAGMENTS pulled
# in by `include`/`sweep`; running one on its own would start from whatever the
# world happened to be in.
# ---------------------------------------------------------------------------
$suites = @(
	@{
		name = 'smoke'; script = 'smoke.eval'
		what = 'the runner drives the game unattended, start to finish'
		measure = $null
	},
	@{
		name = 'arena'; script = 'arena.eval'
		what = 'arenas carve, monsters spawn where asked, a fight resolves'
		measure = $null
	},
	@{
		name = 'tiers'; script = 'tiers.eval'
		what = 'the same encounter at two preset tiers, same seed'
		measure = '===|  \[[0-9]\]'
	},
	@{
		name = 'ladder'; script = 'ladder.eval'
		what = 'the progression ladder: walk in, fight, tp away, repeat'
		measure = '=== RUNG|TALLY '
	},
	@{
		name = 'blast'; script = 'blast-geometry.eval'
		what = 'one detonation in four geometries'
		measure = '===|  skel_warrior|TALLY '
	},
	@{
		name = 'sweep'; script = 'sweep-novice.eval'
		what = 'one rung over twelve seeds - a distribution, not an anecdote'
		measure = 'TALLY '
	},
	@{
		name = 'resources'; script = 'resources.eval'
		what = 'the three pools: rates, the state gate, and what recovery trains'
		measure = '===|  \[[0-9]\]|  ref |    skill |    creep '
	}
)

if ($List) {
	Write-Host ''
	Write-Host 'eval suites:'
	foreach ($s in $suites) {
		Write-Host ("  {0,-8} {1,-24} {2}" -f $s.name, $s.script, $s.what)
	}
	Write-Host ''
	Write-Host 'fragments (pulled in by include/sweep, not run directly):'
	Get-ChildItem -Path $scripts -Recurse -Filter *.eval |
		Where-Object { $_.DirectoryName -ne $scripts } |
		ForEach-Object { Write-Host ("  {0}" -f $_.FullName.Substring($scripts.Length + 1)) }
	Write-Host ''
	exit 0
}

if (-not (Test-Path $exe)) {
	Write-Host "eval: no exe at $exe - build first" -ForegroundColor Red
	exit 2
}

# ---------------------------------------------------------------------------
# SELF-TEST: the runner is handed a script with a line that is not a command
# and must exit 1. Without this, "every suite passed" could mean the runner had
# stopped noticing anything at all.
# ---------------------------------------------------------------------------
if ($SelfTest) {
	$bad = Join-Path $scripts 'selftest-bad.eval'
	Write-Host ''
	Write-Host '=== eval self-test: a bad script must FAIL ==='
	$p = Start-Process -FilePath $exe -ArgumentList '-eval', $bad -PassThru -Wait
	Write-Host ("  bad script exited {0} (want 1)" -f $p.ExitCode)

	# ...and a script that cannot be READ is a different failure from one that
	# ran badly, so it gets its own code. A harness that conflated them would
	# report a typo'd path as a measurement.
	$missing = Join-Path $scripts 'no-such-file.eval'
	$q = Start-Process -FilePath $exe -ArgumentList '-eval', $missing -PassThru -Wait
	Write-Host ("  missing script exited {0} (want 2)" -f $q.ExitCode)

	$ok = ($p.ExitCode -eq 1) -and ($q.ExitCode -eq 2)
	Write-Host ''
	Write-Host ("eval RESULT={0} self_test=1" -f $(if ($ok) { 'PASS' } else { 'FAIL' }))
	if ($ok) { Write-Host 'the runner correctly reported both failures' }
	else { Write-Host 'A RUNNER THAT CANNOT FAIL MEANS NOTHING' -ForegroundColor Red }
	exit $(if ($ok) { 0 } else { 1 })
}

$run = if ($Only.Count -gt 0) { $suites | Where-Object { $Only -contains $_.name } } else { $suites }
if (-not $run) { Write-Host "eval: nothing matched -Only"; exit 2 }

$failed = 0
$results = @()
foreach ($s in $run) {
	$path = Join-Path $scripts $s.script
	if (-not $Table) {
		Write-Host ''
		Write-Host ('=' * 78)
		Write-Host ("{0} - {1}" -f $s.name, $s.what)
		Write-Host ('=' * 78)
	}
	$t0 = Get-Date
	$p = Start-Process -FilePath $exe -ArgumentList '-eval', $path -PassThru -Wait
	$secs = [int]((Get-Date) - $t0).TotalSeconds
	$verdict = if ($p.ExitCode -eq 0) { 'PASS' } else { 'FAIL' }
	if ($p.ExitCode -ne 0) { $failed++ }

	# THE MEASUREMENT. Read back from dungeon.log rather than captured from the
	# process: the game writes there, and it is the same file a human opens
	# after a run. One source, so the harness cannot show something the log does
	# not (docs/eval-harness.md - `logecho`).
	if ($s.measure) {
		if ($Table) { Write-Host ''; Write-Host ("--- {0} ---" -f $s.name) }
		# CASE-SENSITIVE on purpose: `TALLY` is the result and `tally reset` is
		# the command that begins a rung. Without this the table carries a line
		# of bookkeeping for every measurement it prints.
		Select-String -Path $log -CaseSensitive -Pattern ("^\[info \] console: ({0})" -f $s.measure) |
			ForEach-Object { Write-Host ('  ' + ($_.Line -replace '^\[info \] console: ', '')) }
	}
	$results += [pscustomobject]@{ Name = $s.name; Verdict = $verdict; Seconds = $secs }
}

Write-Host ''
Write-Host ('=' * 78)
foreach ($r in $results) {
	Write-Host ("  {0,-8} {1,-6} {2,4}s" -f $r.Name, $r.Verdict, $r.Seconds)
}
Write-Host ''
Write-Host ("eval RESULT={0} suites={1} failures={2} self_test=0" -f `
	$(if ($failed -eq 0) { 'PASS' } else { 'FAIL' }), $results.Count, $failed)
if ($failed -eq 0) {
	Write-Host 'every suite ran; the NUMBERS above are the result, not this line'
}
exit $(if ($failed -eq 0) { 0 } else { 1 })
