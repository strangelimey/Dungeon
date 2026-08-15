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
	},
	@{
		name = 'supplies'; script = 'supplies.eval'
		what = 'food and water: what they cost, and what an empty meter does'
		measure = '===|  \[[0-9]\]|consumes|gains nothing'
	},
	@{
		name = 'rest'; script = 'rest.eval'
		what = 'the rest state: what it costs, and the three ways it ends'
		measure = '===|  \[0\]|rest (on|off) \(world'
	},
	@{
		name = 'expedition'; script = 'expedition.eval'
		what = 'fight, retreat, rest, repeat - how many fights a load of supplies buys'
		measure = '===|TALLY |rested [0-9.]+s|  \[0\] Brand|^state '
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

	# --- `reset` really equals a new game ------------------------------------
	# The recycling the whole batch form rests on. resettest.eval takes a
	# baseline after a real load, wrecks the world every way the harness can,
	# resets, and takes it again; the two blocks must match line for line.
	#
	# It is HERE rather than in the suite list because it is not a measurement -
	# nothing about it is a number to compare across knob changes. It is a check,
	# and a check belongs with the other checks.
	Write-Host ''
	Write-Host '=== reset must equal a new game ==='
	Start-Process -FilePath $exe -ArgumentList '-eval', (Join-Path $scripts 'resettest.eval') -Wait
	$rt = @(Get-Content $log)
	$blocks = @(@(), @())
	$which = -1
	foreach ($line in $rt) {
		if ($line -match 'console: === BASELINE A') { $which = 0; continue }
		if ($line -match 'console: === BASELINE B') { $which = 1; continue }
		if ($line -match 'console: === wrecking')   { $which = -1; continue }
		# Only the readouts, never the echoed commands that produced them.
		if ($which -ge 0 -and $line -match '^\[info \] console: (?!> )(.+)$') {
			$blocks[$which] += $Matches[1]
		}
	}
	$diff = if ($blocks[0].Count -eq 0) { 'no baseline captured' }
			elseif ($blocks[0].Count -ne $blocks[1].Count) { "line counts differ ($($blocks[0].Count) vs $($blocks[1].Count))" }
			else {
				$bad = @(0..($blocks[0].Count - 1) | Where-Object { $blocks[0][$_] -cne $blocks[1][$_] })
				if ($bad.Count) { "$($bad.Count) line(s) differ, first: '$($blocks[0][$bad[0]])' vs '$($blocks[1][$bad[0]])'" } else { '' }
			}
	$resetOk = ($diff -eq '')
	Write-Host ("  {0} baseline lines compared - {1}" -f $blocks[0].Count,
		$(if ($resetOk) { 'identical' } else { $diff }))

	# --- and BATCHING changes nothing ----------------------------------------
	# A suite must measure the same thing whether it ran alone or after another.
	# Without this the speedup could be quietly buying wrong numbers - which is
	# the failure mode that matters, because it looks exactly like a fast run.
	Write-Host ''
	Write-Host '=== a batched suite must match a solo one ==='
	$probe = Join-Path $scripts 'supplies.eval'
	$grab = { @(Get-Content $log) | Where-Object { $_ -cmatch '^\[info \] console:   \[0\] Brand' } }
	Start-Process -FilePath $exe -ArgumentList '-eval', $probe -Wait
	$solo = & $grab
	Start-Process -FilePath $exe -ArgumentList '-eval', (Join-Path $scripts 'resources.eval'), $probe -Wait
	$batched = @(& $grab | Select-Object -Last $solo.Count)
	$batchOk = ($solo.Count -gt 0) -and ($solo.Count -eq $batched.Count) -and
			   -not @(0..($solo.Count - 1) | Where-Object { $solo[$_] -cne $batched[$_] }).Count
	Write-Host ("  {0} lines compared - {1}" -f $solo.Count,
		$(if ($batchOk) { 'identical batched and solo' } else { 'DIFFERENT' }))

	$ok = ($p.ExitCode -eq 1) -and ($q.ExitCode -eq 2) -and $resetOk -and $batchOk
	Write-Host ''
	Write-Host ("eval RESULT={0} self_test=1" -f $(if ($ok) { 'PASS' } else { 'FAIL' }))
	if ($ok) { Write-Host 'the runner reports both failures, and recycling changes nothing' }
	else { Write-Host 'A RUNNER THAT CANNOT FAIL MEANS NOTHING' -ForegroundColor Red }
	exit $(if ($ok) { 0 } else { 1 })
}

$run = if ($Only.Count -gt 0) { $suites | Where-Object { $Only -contains $_.name } } else { $suites }
if (-not $run) { Write-Host "eval: nothing matched -Only"; exit 2 }

# ONE PROCESS FOR EVERY SUITE. A dungeon load is ~12 seconds and a `reset` is
# ~340 ms, so ten suites in one process pay one load instead of ten - measured
# 155s -> 37s. Each script opens with `reset`, which loads for the first one in
# the batch and recycles for the rest (docs/eval-harness.md).
$paths = @($run | ForEach-Object { Join-Path $scripts $_.script })
$t0 = Get-Date
$p = Start-Process -FilePath $exe -ArgumentList (@('-eval') + $paths) -PassThru -Wait
$totalSecs = [int]((Get-Date) - $t0).TotalSeconds

# THE MEASUREMENT. Read back from dungeon.log rather than captured from the
# process: the game writes there, and it is the same file a human opens after a
# run. One source, so the harness cannot show something the log does not
# (docs/eval-harness.md - `logecho`).
#
# With one process the log holds every suite's output end to end, so it is split
# on the runner's own markers - `eval: 'x' queued` opens a section and
# `eval RESULT=... script=x` closes it. Splitting on the RUNNER's lines rather
# than on the suites' own echoes means a suite cannot break the split by
# printing something that looks like a header.
$logLines = @(Get-Content $log)
$section = @{}
$verdicts = @{}
$current = $null
foreach ($line in $logLines) {
	if ($line -match "^\[info \] eval: '([^']+)' queued") { $current = $Matches[1]; $section[$current] = @() ; continue }
	if ($line -match '^\[(info |ERROR)\] eval RESULT=(\w+) script=(\S+)') { $verdicts[$Matches[3]] = $Matches[2]; $current = $null; continue }
	if ($current) { $section[$current] += $line }
}

$failed = 0
$results = @()
foreach ($s in $run) {
	if (-not $Table) {
		Write-Host ''
		Write-Host ('=' * 78)
		Write-Host ("{0} - {1}" -f $s.name, $s.what)
		Write-Host ('=' * 78)
	}
	# A suite whose section is MISSING never ran - the batch was abandoned by a
	# timeout, say. That has to read as a failure and not as a quiet blank.
	$verdict = if ($verdicts.ContainsKey($s.script)) { $verdicts[$s.script] } else { 'NOTRUN' }
	if ($verdict -ne 'PASS') { $failed++ }

	if ($s.measure -and $section.ContainsKey($s.script)) {
		if ($Table) { Write-Host ''; Write-Host ("--- {0} ---" -f $s.name) }
		# CASE-SENSITIVE on purpose: `TALLY` is the result and `tally reset` is
		# the command that begins a rung. Without this the table carries a line
		# of bookkeeping for every measurement it prints.
		$section[$s.script] |
			Where-Object { $_ -cmatch ("^\[info \] console: ({0})" -f $s.measure) } |
			ForEach-Object { Write-Host ('  ' + ($_ -replace '^\[info \] console: ', '')) }
	}
	$results += [pscustomobject]@{ Name = $s.name; Verdict = $verdict }
}

Write-Host ''
Write-Host ('=' * 78)
foreach ($r in $results) {
	Write-Host ("  {0,-12} {1}" -f $r.Name, $r.Verdict)
}
Write-Host ''
# ONE total rather than a column of per-suite times: they all ran in one process
# now, so a per-suite wall clock would be a number the harness cannot honestly
# produce. The load is paid once and shows up in whichever suite went first.
Write-Host ("eval RESULT={0} suites={1} failures={2} seconds={3} self_test=0" -f `
	$(if ($failed -eq 0) { 'PASS' } else { 'FAIL' }), $results.Count, $failed, $totalSecs)
if ($failed -eq 0) {
	Write-Host 'every suite ran; the NUMBERS above are the result, not this line'
}
exit $(if ($failed -eq 0) { 0 } else { 1 })
