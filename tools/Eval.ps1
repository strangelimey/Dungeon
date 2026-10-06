# ============================================================================
# tools\Eval.ps1 - run the eval suites and surface what they measured.
#
#   .\tools\Eval.ps1                    # every suite
#   .\tools\Eval.ps1 -Only ladder       # one of them
#   .\tools\Eval.ps1 -List
#   .\tools\Eval.ps1 -SelfTest          # the runner must FAIL on purpose
#   .\tools\Eval.ps1 -Table             # print ONLY the measurements
#   .\tools\Eval.ps1 -Headless          # no window, no drawing
#   .\tools\Eval.ps1 -OutFile before.txt # ...and SAVE it, to diff against later
#
# DIFFING TWO RUNS is the whole point of the numbers, and until -OutFile existed
# there was no supported way to keep one: every line here goes to the host, so
# `Eval.ps1 > before.txt` wrote an EMPTY FILE (docs/eval-audit.md F2). The shape
# a knob change is measured in:
#
#   .\tools\Eval.ps1 -Headless -Table -OutFile before.txt
#   ...edit balance.cat...
#   .\tools\Eval.ps1 -Headless -Table -OutFile after.txt
#   Compare-Object (gc before.txt) (gc after.txt)
#
# -Headless is NOT primarily a speed switch, and saying so up front saves
# somebody measuring it hopefully: ten suites go 42s -> 37s, because the time is
# the asset load plus the `step` loops, and a `step` runs many sim ticks inside
# ONE frame. There are simply not many frames to save. What it buys is a run that
# does not steal focus, survives being run over RDP or from a scheduled task, and
# can be run several at a time without contending for the GPU. The numbers are
# identical either way - that equivalence is checked under -SelfTest.
#
# Exit 0 = every suite's script ran to completion, every line did what it said,
# and every suite measured something. That is FOUR conditions, and it used to be
# one (docs/eval-audit.md). A line now fails the run if it named no command
# (a typo), if it named one that then REFUSED (a spawn onto rock, a tp into a
# wall, a `step` past its per-call ceiling), if the script ended out of play (a
# party wipe returns to the title screen and every dev command keeps answering
# from there), or if a suite's measure regex matched nothing at all.
#
# The first run with those checks failed three of the ten suites and all three
# were real: two had been measuring 55.6 minutes and calling it an hour, and one
# had been printing its closing figures off a dead party.
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
	[switch]$Headless,
	[string]$OutFile = '',
	[ValidateSet('debug', 'release')][string]$Config = 'debug'
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"

# Muted for the whole run, restored however it ends (tools\HarnessAudio.ps1).
. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not (Test-HarnessMuted $bin)) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }

$scripts = Join-Path $root 'tools\EvalScripts'
$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'

# ONE RUN PER WORKTREE: this worktree's game writes one dungeon.log, truncated
# on open, and a second run would interleave its verdict source with this one's
# (code-review C430). Another worktree's game has its own log and is fine.
# And never a stale exe: a measurement of yesterday's binary reads as one of
# today's change (C426).
. (Join-Path $PSScriptRoot 'HarnessGame.ps1')
Assert-ExeCurrent $exe
Assert-NotRunning $exe

# EVERY REPORT LINE GOES THROUGH HERE so the run can be both coloured on screen
# and saved to a file. Write-Host alone cannot be redirected (that is F2) and
# Write-Output alone loses the colour that makes a FAIL findable in 300 lines of
# numbers, so this does both and -OutFile writes the buffer at the end.
$transcript = New-Object System.Collections.Generic.List[string]
function Say {
	param([string]$Text = '', [string]$Colour = '')
	$transcript.Add($Text) | Out-Null
	if ($Colour) { Write-Host $Text -ForegroundColor $Colour } else { Write-Host $Text }
}
function SaveTranscript {
	if (-not $OutFile) { return }
	# WriteAllLines, not Set-Content: PS 5.1's -Encoding utf8 prepends a BOM, and
	# a BOM in the first line makes the first line of every diff spurious.
	$path = if ([IO.Path]::IsPathRooted($OutFile)) { $OutFile }
			else { Join-Path (Get-Location).Path $OutFile }
	[IO.File]::WriteAllLines($path, [string[]]$transcript)
	Write-Host ("saved to {0} ({1} lines)" -f $OutFile, $transcript.Count) -ForegroundColor DarkGray
}

# READ THE LOG AS UTF-8. The game writes it as UTF-8 and PS 5.1's Get-Content
# defaults to the ANSI code page, so every em-dash arrived as three characters
# and the report could not be pasted anywhere (F10). This fixes the DATA; how a
# console then draws it is the console's code page and not this script's
# business - see the utf8-console-codepage note.
function ReadLog {
	if (-not (Test-Path $log)) { return @() }
	@(Get-Content $log -Encoding UTF8)
}

# One game run for the self-test's comparisons, with the log DELETED first: a
# launch that died before opening its log would otherwise leave the previous
# run's log in place, and a comparison would read one run against itself and
# pass. Finished = it wrote its 'eval BATCH RESULT=' line.
function Invoke-EvalRun([string[]]$argList) {
	Remove-Item $log -ErrorAction SilentlyContinue
	$p = Start-Process -FilePath $exe -ArgumentList $argList -PassThru -Wait
	$done = [bool](ReadLog | Where-Object { $_ -match 'eval BATCH RESULT=' })
	if (-not $done) { Write-Host ("  the game did not finish '{0}' (exit {1})" -f ($argList -join ' '), $p.ExitCode) -ForegroundColor Red }
	return $done
}

# ---------------------------------------------------------------------------
# THE SUITES. `measure` is the regex whose matching log lines ARE the result -
# what a reader compares across runs. A suite with no measure line is a smoke
# test: it proves the machinery, and has nothing to say about balance.
#
# NEVER PUT `^` INSIDE A MEASURE. The filter is `^\[info \] console: (<measure>)`,
# so a caret in the alternation asserts start-of-STRING in the middle of the
# pattern and can never match. `expedition` carried `|^state ` from the day it
# was written and has never printed one `state` line; the NOMEASURE check cannot
# catch it either, because the suite's other alternatives still match. The line
# is already anchored for you - just write the text.
#
# Only top-level scripts appear here. rungs\ and presets\ are FRAGMENTS pulled
# in by `include`/`sweep`; running one on its own would start from whatever the
# world happened to be in.
# ---------------------------------------------------------------------------
$suites = @(
	# These two used to carry `measure = $null` and print a header over blank
	# space. That was indistinguishable from a suite whose regex had stopped
	# matching, and since the blank shape appeared on EVERY run a reader was
	# trained to skim past exactly the shape that means the measurement is gone
	# (docs/eval-audit.md F5/F25). Both had plenty to show; nobody had said so.
	@{
		name = 'smoke'; script = 'smoke.eval'
		what = 'the runner drives the game unattended, start to finish'
		measure = '--- |state |  \[[0-9]\] |  \w+ @ '
	},
	@{
		name = 'arena'; script = 'arena.eval'
		what = 'arenas carve, monsters spawn where asked, a fight resolves'
		# `mapinfo`'s walkable count is the ONLY readout that can see an arena
		# that failed to carve, which is exactly the blindness F15 walked
		# through: a script places monsters inside bounds the `arena` line
		# reported, two of the cells are rock, the spawns are refused and the
		# rung measures a third of what its header claims.
		measure = '--- |arena \w|\d+x\d+ map|  \w+ @ |no monsters'
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
		measure = '===|TALLY |rested [0-9.]+s|  \[0\] Brand|state '
	},
	@{
		name = 'parties'; script = 'smallparty.eval'
		what = 'parties of one, three and two: who the formation lets a monster reach'
		measure = '===|  \[[0-9]\]|TALLY |roster [0-9]|rested [0-9.]+s|state '
	},
	@{
		name = 'rootmotion'; script = 'rootmotion.eval'
		what = 'a walking, then dying, kit skeleton: how far its body strays from its square'
		measure = '--- |  skel_warrior @ '
	},
	@{
		name = 'spawnrise'; script = 'spawnrise.eval'
		what = 'a freshly spawned skeleton holds its square until it is up'
		# The cell must read 14,9 on every line that also says `rising`.
		measure = '--- |  skel_warrior @ '
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
	# -OutFile is for the MEASUREMENT report, which is the thing anybody diffs
	# across a knob change. The self-test is a pass/fail artefact and its output
	# is not comparable run to run, so it does not build a transcript - say so
	# rather than write nothing and let the caller wonder.
	if ($OutFile) {
		Write-Host '-OutFile is ignored with -SelfTest (it saves the measurement report, not this)' -ForegroundColor Yellow
	}
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
	$resetRan = Invoke-EvalRun @('-eval', (Join-Path $scripts 'resettest.eval'))
	$rt = ReadLog
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
	$resetOk = $resetRan -and ($diff -eq '')
	Write-Host ("  {0} baseline lines compared - {1}" -f $blocks[0].Count,
		$(if ($resetOk) { 'identical' } else { $diff }))

	# --- and BATCHING changes nothing ----------------------------------------
	# A suite must measure the same thing whether it ran alone or after another.
	# Without this the speedup could be quietly buying wrong numbers - which is
	# the failure mode that matters, because it looks exactly like a fast run.
	Write-Host ''
	Write-Host '=== a batched suite must match a solo one ==='
	$probe = Join-Path $scripts 'supplies.eval'
	$grab = { ReadLog | Where-Object { $_ -cmatch '^\[info \] console:   \[0\] Brand' } }
	$soloRan = Invoke-EvalRun @('-eval', $probe)
	$solo = & $grab
	$batchRan = Invoke-EvalRun @('-eval', (Join-Path $scripts 'resources.eval'), $probe)
	$batched = @(& $grab | Select-Object -Last $solo.Count)
	$batchOk = $soloRan -and $batchRan -and ($solo.Count -gt 0) -and ($solo.Count -eq $batched.Count) -and
			   -not @(0..($solo.Count - 1) | Where-Object { $solo[$_] -cne $batched[$_] }).Count
	Write-Host ("  {0} lines compared - {1}" -f $solo.Count,
		$(if ($batchOk) { 'identical batched and solo' } else { 'DIFFERENT' }))

	# --- and HEADLESS changes nothing ----------------------------------------
	# Same argument as the batching check above, for the same reason: a mode that
	# quietly measured something else would look exactly like a mode that worked.
	# Headless skips the whole render half of the frame, and the risk is that
	# something the simulation depends on was living in there - the staged
	# loader's frame counter already was, and only turned up because the load hung.
	Write-Host ''
	Write-Host '=== a headless run must match a windowed one ==='
	# EVERY console line is compared, so the one line that is wall-clock BY DESIGN
	# has its number masked: `reset` times itself ("loaded in 14 ms"), and two
	# runs round to different milliseconds often enough that this check failed on
	# a coin toss, blaming headless for a timer. Masked, not dropped, so a reset
	# line that goes missing on one side still counts as a difference.
	$grabAll = { ReadLog | Where-Object { $_ -cmatch '^\[info \] console: ' } |
		ForEach-Object { $_ -creplace '^(\[info \] console: reset: \w+ in )\d+( ms)', '${1}#${2}' } }
	$winRan = Invoke-EvalRun @('-eval', $probe)
	$windowed = @(& $grabAll)
	$hidRan = Invoke-EvalRun @('-headless', '-eval', $probe)
	$hidden = @(& $grabAll)
	$headBad = @(0..([Math]::Max($windowed.Count, $hidden.Count) - 1) |
		Where-Object { $windowed[$_] -cne $hidden[$_] })
	$headOk = $winRan -and $hidRan -and ($windowed.Count -gt 0) -and ($headBad.Count -eq 0)
	Write-Host ("  {0} lines compared - {1}" -f $windowed.Count,
		$(if ($headOk) { 'identical headless and windowed' }
		  elseif ($windowed.Count -eq 0) { 'DIFFERENT: the windowed run printed nothing' }
		  else { "DIFFERENT: $($headBad.Count) line(s), first '$($windowed[$headBad[0]])' vs '$($hidden[$headBad[0]])'" }))

	# --- and the NUMBERS still move ------------------------------------------
	# EVERY CHECK ABOVE IS PLUMBING. They prove the runner runs, that recycling
	# and batching and headless change nothing - and not one of them asks whether
	# the TALLY still describes the fight. A counting site could be deleted
	# tomorrow and this harness would report ten green suites forever
	# (docs/eval-audit.md F1, which is the finding the whole audit turned on).
	#
	# respond.eval runs three pairs of arms, each identical but for one knob the
	# combat model says MUST move one of the three headline numbers. Thresholds
	# are set from MEASURED separations and left deliberately loose: this asks
	# whether the instrument is ALIVE, not whether the balance is right. A number
	# that stops responding fails; a number that responds differently after a
	# balance change does not.
	Write-Host ''
	Write-Host '=== a knob that must move a number, moves it ==='
	$respondRan = Invoke-EvalRun @('-eval', (Join-Path $scripts 'respond.eval'))
	$arm = $null
	$samples = @{}
	foreach ($line in ReadLog) {
		if ($line -cmatch '^\[info \] console: ARM (\S+)$') { $arm = $Matches[1]; $samples[$arm] = @(); continue }
		# hitrate is `n/a` when nothing swung - a rate over no trials is undefined,
		# not zero. Such a sample contributes its damage and its downs but must
		# not drag a hit-rate average toward 0, so Rate is $null and the average
		# below skips it.
		if ($arm -and $line -cmatch ('^\[info \] console: TALLY dealt=([0-9.]+) taken=([0-9.]+) ' +
									 'swings=(\d+) hits=(\d+) misses=(\d+) hitrate=([0-9.]+|n/a) ' +
									 'crits=(\d+) fumbles=(\d+) slain=(\d+) downed=(\d+)')) {
			$samples[$arm] += [pscustomobject]@{
				Dealt = [double]$Matches[1]; Taken = [double]$Matches[2]
				Swings = [int]$Matches[3]
				Rate = $(if ($Matches[6] -eq 'n/a') { $null } else { [double]$Matches[6] })
				Downed = [int]$Matches[10]
			}
		}
	}
	# A MISSING OR EMPTY ARM MUST FAIL, not divide by zero and pass. This is the
	# guard that keeps the whole check non-vacuous: if the tally went dead every
	# aggregate below would be 0, and a ratio of 0/0 must not read as "unchanged".
	$armNames = @('dex-low', 'dex-high', 'skill-low', 'skill-high', 'threat-low', 'threat-high')
	$respondOk = $respondRan
	$missing = @($armNames | Where-Object { -not $samples.ContainsKey($_) -or $samples[$_].Count -lt 3 })
	if ($missing.Count) {
		Write-Host ("  arms missing or too small: {0}" -f ($missing -join ', ')) -ForegroundColor Red
		$respondOk = $false
	}
	if ($respondOk) {
		$agg = @{}
		foreach ($a in $armNames) {
			$g = $samples[$a]
			$sw = ($g | Measure-Object Swings -Sum).Sum
			# Samples with no swings carry Rate = $null and are EXCLUDED from the
			# average rather than counted as zero, which is the same distinction
			# the `n/a` exists to make.
			$rated = @($g | Where-Object { $null -ne $_.Rate })
			$agg[$a] = [pscustomobject]@{
				N = $g.Count
				Rate = $(if ($rated.Count) { ($rated | Measure-Object Rate -Average).Average } else { 0 })
				Swings = $sw
				PerSwing = $(if ($sw -gt 0) { ($g | Measure-Object Dealt -Sum).Sum / $sw } else { 0 })
				Taken = ($g | Measure-Object Taken -Average).Average
				Downed = ($g | Measure-Object Downed -Sum).Sum
			}
		}
		# Each row: what must be true, why that threshold, and what was measured
		# when it was written. Ratios are 1.3x against separations of 2.0x and
		# 1.6x; the `taken` pair is a difference rather than a ratio because its
		# low arm sits near zero and a ratio there is meaningless.
		$tests = @(
			@{ what = 'hitrate responds to DEX'
			   got  = $agg['dex-high'].Rate; ref = $agg['dex-low'].Rate
			   ok   = ($agg['dex-low'].Rate -gt 0) -and ($agg['dex-high'].Rate -ge $agg['dex-low'].Rate * 1.3)
			   note = 'want high >= low x1.3 (measured 0.336 -> 0.668)' }
			@{ what = 'dealt-per-swing responds to weapon skill'
			   got  = $agg['skill-high'].PerSwing; ref = $agg['skill-low'].PerSwing
			   ok   = ($agg['skill-low'].PerSwing -gt 0) -and ($agg['skill-high'].PerSwing -ge $agg['skill-low'].PerSwing * 1.3)
			   note = 'want high >= low x1.3 (measured 11.02 -> 17.84)' }
			@{ what = 'taken responds to monster strength'
			   got  = $agg['threat-high'].Taken; ref = $agg['threat-low'].Taken
			   ok   = ($agg['threat-high'].Taken -gt 50) -and (($agg['threat-high'].Taken - $agg['threat-low'].Taken) -gt 60)
			   note = 'want high > 50 and high - low > 60 (measured 19.2 -> 185.1)' }
			@{ what = 'downed is counted at all'
			   got  = $agg['threat-high'].Downed; ref = $agg['threat-low'].Downed
			   ok   = ($agg['threat-high'].Downed -gt 0)
			   note = 'want high > 0 (measured 2 -> 11)' }
		)
		foreach ($t in $tests) {
			Write-Host ("  {0,-42} {1,8:N2} vs {2,8:N2}  {3}" -f $t.what, $t.got, $t.ref,
				$(if ($t.ok) { 'ok' } else { 'FAIL - ' + $t.note })) `
				-ForegroundColor $(if ($t.ok) { 'Gray' } else { 'Red' })
			if (-not $t.ok) { $respondOk = $false }
		}
	}

	# --- one run per worktree; a killed run does not count -------------------
	# Two runs of THIS script, as a person would start them (code-review C430).
	# The first runs a real suite; while its game is up a second must be REFUSED
	# (exit 3) - it would share that game's dungeon.log. Then the first's game is
	# killed, by PID, and the first must say the game did not finish and exit
	# non-zero rather than read its partial log as a result. Both find this bin
	# muted already and must say so (C433: every harness logs its mute state).
	Write-Host ''
	Write-Host '=== a second run is refused; a killed run does not count ==='
	$outA = Join-Path $env:TEMP "eval-selftest-first-$PID.txt"
	$outB = Join-Path $env:TEMP "eval-selftest-second-$PID.txt"
	$child = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
		'-Config', $Config, '-Only', 'supplies', '-Headless')
	$first = Start-Process powershell -ArgumentList $child -PassThru -WindowStyle Hidden -RedirectStandardOutput $outA
	# Touch the handle NOW: a Process from Start-Process without -Wait that never
	# had it read reports ExitCode as $null after it ends - and $null -ne 0, so
	# "exits non-zero" would pass on no exit code at all.
	$null = $first.Handle
	$game = $null
	$deadline = (Get-Date).AddSeconds(60)
	while (-not $game -and -not $first.HasExited -and (Get-Date) -lt $deadline) {
		Start-Sleep -Milliseconds 300
		$game = Get-Process Dungeon -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Select-Object -First 1
	}
	$second = Start-Process powershell -ArgumentList $child -PassThru -Wait -WindowStyle Hidden -RedirectStandardOutput $outB
	if ($game) { Start-Sleep -Seconds 2; $game.Kill() }
	if (-not $first.WaitForExit(120000)) { $first.Kill() }
	$saidA = if (Test-Path $outA) { Get-Content $outA -Raw } else { '' }
	$saidB = if (Test-Path $outB) { Get-Content $outB -Raw } else { '' }
	Remove-Item $outA, $outB -ErrorAction SilentlyContinue
	$guards = @(
		@{ what = "the first run's game started";        ok = [bool]$game },
		@{ what = 'a second run beside it is refused';  ok = ($second.ExitCode -eq 3) -and ($saidB -match 'refused: ') },
		@{ what = 'the killed run says it did not finish'; ok = ($saidA -match 'THE GAME DID NOT FINISH') },
		@{ what = '...and exits non-zero';              ok = ($first.ExitCode -is [int]) -and ($first.ExitCode -ne 0) },
		@{ what = 'both said the bin was already muted'; ok = ($saidA -match 'audio: already muted') -and ($saidB -match 'audio: already muted') }
	)
	$guardOk = $true
	foreach ($g in $guards) {
		Write-Host ("  {0,-42} {1}" -f $g.what, $(if ($g.ok) { 'ok' } else { 'FAIL' })) -ForegroundColor $(if ($g.ok) { 'Gray' } else { 'Red' })
		if (-not $g.ok) { $guardOk = $false }
	}
	Write-Host ("  (second run exited {0}, first {1})" -f $second.ExitCode, $first.ExitCode)

	$ok = ($p.ExitCode -eq 1) -and ($q.ExitCode -eq 2) -and $resetOk -and $batchOk -and $headOk -and $respondOk -and $guardOk
	Write-Host ''
	Write-Host ("eval RESULT={0} self_test=1" -f $(if ($ok) { 'PASS' } else { 'FAIL' }))
	if ($ok) { Write-Host 'the runner reports both failures, recycling and headless change nothing, the numbers still move, and a second or killed run does not count' }
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
$args = @()
if ($Headless) { $args += '-headless' }
$args += @('-eval') + $paths
$t0 = Get-Date
# The log deleted first, so a game that dies before writing one leaves nothing
# for the "did it finish" check below to mistake for this run's.
Remove-Item $log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -ArgumentList $args -PassThru -Wait
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
$logLines = ReadLog
# A RUN COUNTS ONLY IF IT FINISHED. Every batch the runner completes - or
# abandons on a timeout - ends in 'eval BATCH RESULT='; a game that crashed or
# was killed writes neither, and its partial log must not be read as a result
# (code-review C430: the exit code used to go unread).
$batchLine = @($logLines | Where-Object { $_ -match 'eval BATCH RESULT=' }) | Select-Object -Last 1
$runDied = -not $batchLine
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
		Say ''
		Say ('=' * 78)
		Say ("{0} - {1}" -f $s.name, $s.what)
		Say ('=' * 78)
	}
	# A suite whose section is MISSING never ran - the batch was abandoned by a
	# timeout, say. That has to read as a failure and not as a quiet blank.
	$verdict = if ($verdicts.ContainsKey($s.script)) { $verdicts[$s.script] } else { 'NOTRUN' }
	$note = ''

	$shown = 0
	$produced = 0
	if ($section.ContainsKey($s.script)) {
		# Everything the suite SAID, minus the runner's echo of each command it
		# was given - those are the script, not its answers.
		$produced = @($section[$s.script] |
			Where-Object { $_ -cmatch '^\[info \] console: ' -and $_ -cnotmatch '^\[info \] console: > ' }).Count
	}
	if ($s.measure -and $section.ContainsKey($s.script)) {
		if ($Table) { Say ''; Say ("--- {0} ---" -f $s.name) }
		# CASE-SENSITIVE on purpose: `TALLY` is the result and `tally reset` is
		# the command that begins a rung. Without this the table carries a line
		# of bookkeeping for every measurement it prints.
		$hits = @($section[$s.script] |
			Where-Object { $_ -cmatch ("^\[info \] console: ({0})" -f $s.measure) })
		$shown = $hits.Count
		$hits | ForEach-Object { Say ('  ' + ($_ -replace '^\[info \] console: ', '')) }
	}

	# A MEASURE THAT MATCHED NOTHING IS A BROKEN SUITE, NOT A QUIET ONE, and
	# until now the two were indistinguishable: both print a header and blank
	# space, which is exactly what `smoke` and `arena` legitimately do on every
	# run - so a reader is trained to skim past the one shape that means the
	# measurement has been silently lost (docs/eval-audit.md F25). Editing an
	# `echo` a regex keys on is all it takes.
	if ($s.measure -and $shown -eq 0) {
		Say '  MEASURED NOTHING - the suite ran but its measure regex matched no line' 'Red'
		Say ("  regex: {0}" -f $s.measure) 'Red'
		$note = 'measured nothing'
		if ($verdict -eq 'PASS') { $verdict = 'NOMEASURE' }
	}

	# HOW MUCH WAS SUPPRESSED. The report shows what a per-suite regex matched and
	# silently drops the rest, which across the ten suites is about 70% of what the
	# run produced - and that 70% is where every integrity signal in this audit was
	# hiding (docs/eval-audit.md F4). One line per suite is what tells a reader
	# there is a log worth opening, and roughly where the interesting part is.
	if ($produced -gt 0 -and -not $Table) {
		Say ("  [{0} of {1} lines shown; the rest is in dungeon.log]" -f $shown, $produced) 'DarkGray'
	}

	# WARNINGS AND ERRORS THE GAME WROTE MID-SUITE. The measure filter only ever
	# looks at `[info ] console:` lines, so every `[warn ]` and `[ERROR]` in the
	# log was invisible to the report BY CONSTRUCTION - a failed model load or a
	# steady-state allocation violation showed up as ten green PASS lines
	# (docs/eval-audit.md F24). Stack frames are indented under their message, so
	# requiring a non-space right after the tag keeps the message and drops the
	# forty lines of symbols beneath it.
	if ($section.ContainsKey($s.script)) {
		$noise = @($section[$s.script] |
			Where-Object { $_ -cmatch '^\[(warn |ERROR)\] [^\s]' } |
			ForEach-Object { $_ -replace '^\[(warn |ERROR)\] ', '' } |
			Select-Object -Unique)
		if ($noise.Count -gt 0) {
			Say ("  {0} warning/error line(s) in the log:" -f $noise.Count) 'Yellow'
			$noise | Select-Object -First 6 | ForEach-Object { Say ("    ! {0}" -f $_) 'Yellow' }
			if ($noise.Count -gt 6) { Say ("    ... and {0} more (see dungeon.log)" -f ($noise.Count - 6)) 'Yellow' }
			if ($note) { $note += '; ' }
			$note += ("{0} warn/error" -f $noise.Count)
		}
	}

	if ($verdict -ne 'PASS') { $failed++ }
	$results += [pscustomobject]@{ Name = $s.name; Verdict = $verdict; Note = $note }
}

Say ''
Say ('=' * 78)
foreach ($r in $results) {
	Say ("  {0,-12} {1,-10} {2}" -f $r.Name, $r.Verdict, $r.Note) `
		$(if ($r.Verdict -eq 'PASS') { '' } else { 'Red' })
}
Say ''
if ($runDied) {
	Say ("THE GAME DID NOT FINISH (exit code {0}): no 'eval BATCH RESULT=' line - nothing above counts" -f $p.ExitCode) 'Red'
	$failed++
} elseif ($p.ExitCode -ne 0 -and $failed -eq 0) {
	Say ("the game exited {0} although every suite passed - the run does not count" -f $p.ExitCode) 'Red'
	$failed++
}
# ONE total rather than a column of per-suite times: they all ran in one process
# now, so a per-suite wall clock would be a number the harness cannot honestly
# produce. The load is paid once and shows up in whichever suite went first.
#
# THE SECONDS ARE DELIBERATELY NOT IN THE SAVED TRANSCRIPT'S COMPARISON VALUE:
# they change run to run on the same build, so a diff of two -OutFile reports
# would always show this line. It stays because a human wants it; a reader
# diffing two runs should expect exactly this one line to differ.
Say ("eval RESULT={0} suites={1} failures={2} seconds={3} self_test=0" -f `
	$(if ($failed -eq 0) { 'PASS' } else { 'FAIL' }), $results.Count, $failed, $totalSecs)
if ($failed -eq 0) {
	Say 'every suite ran; the NUMBERS above are the result, not this line'
}
SaveTranscript
exit $(if ($failed -eq 0) { 0 } else { 1 })
