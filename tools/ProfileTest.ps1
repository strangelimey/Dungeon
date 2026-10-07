# ============================================================================
# tools\ProfileTest.ps1 - the frame budget still adds up, and still reacts.
#
# The profile panel's job is to say WHERE A FRAME WENT and what is holding it
# back. Both claims can rot silently:
#
#   * Someone adds a blocking call - a fence, a sleep, a lock - and does not
#     instrument it. The panel keeps reporting, and the unaccounted time lands
#     in `cpu` by elimination, so an idle engine reads as CPU-bound. Nothing
#     crashes; the readout just quietly starts lying, in the one direction that
#     sends you optimising the wrong half of the engine.
#
#   * The verdict stops REACTING. A `bound by display` that is hard-wired looks
#     identical to a correct one on a display-bound machine, and this whole
#     instrument was nearly shipped without ever having been seen to say
#     anything else.
#
# So this takes a SNAPSHOT, MAKES A CHANGE, TAKES ANOTHER, and asserts on the
# difference - the same loop a person uses the feature for.
#
#   .\tools\ProfileTest.ps1
#   .\tools\ProfileTest.ps1 -SelfTest    # exit 0 = the checker works (below)
#
# -SELFTEST is the same real run with ONE named fault: the verdict also asks
# for a snapshot that is never taken, and the self-test passes only if exactly
# that coverage check fails and every other check passes (SpellTest's rule,
# code-review C419) - so a run that recorded nothing, or died half way, fails
# the self-test instead of passing it, and so does a harness error (a throw).
# WHAT IT DOES NOT PROVE, said here because the old header implied more: no
# fault is injected into the PARTITION or REACTION checks, so neither has ever
# been watched to fail on demand. They have each failed for real during
# development, which is weaker evidence. And two checks SKIP rather than run
# when the machine cannot exercise them - no GPU timings (WARP), and nothing for
# the cap to hold back (the compositor already paces at its target); each skip
# is named in the output and counted on the verdict line.
#
# NEEDS A PROFILING BUILD (debug-profile / release-profile). Without DN_PROFILE
# every zone compiles to nothing and there is no budget to check - which is why
# the config is validated up front rather than producing an empty pass.
#
# Refuses to run beside ANY Dungeon.exe, not only this worktree's: a second game
# on the GPU would be part of what it measures.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[ValidateSet('debug-profile', 'release-profile')][string]$Config = 'release-profile',
	[int]$LoadTimeoutSec = 240,
	# Seconds per snapshot. Four is enough to average out a hitch at any frame
	# rate this engine reaches; the whole run is four of them plus load.
	[double]$SnapSeconds = 4.0,
	# Checks the CHECKER: also asks for a snapshot that is never taken, and
	# exactly that coverage check must fail (see the header).
	[switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"

# Muted for the whole run, restored however it ends (tools\HarnessAudio.ps1).
. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not (Test-HarnessMuted $bin)) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }
# Launch, input and log waits: the one shared copy (tools\HarnessGame.ps1).
. (Join-Path $PSScriptRoot 'HarnessGame.ps1')

$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'

if (-not (Test-Path $exe)) { throw "no build at $exe - run build.cmd $Config first" }
Assert-ExeCurrent $exe
# ANY Dungeon.exe, not only this build's (Assert-NotRunning): a second game on
# the GPU would be part of what this measures.
if (Get-Process Dungeon -ErrorAction SilentlyContinue) {
	throw 'Dungeon.exe is already running - close it (this test drives its own instance)'
}

# The console STAYS OPEN for the whole run (InGameTest's rule): a second toggle
# would close it and send the next command to the game as movement keys.
function Run-Cmd([string]$c) { Send-Text $c; Send-Key 0x0D; Start-Sleep -Milliseconds 900 }
function Take-Snap([string]$name) {
	Run-Cmd "profile snap $name $SnapSeconds"
	Start-Sleep -Seconds ($SnapSeconds + 2)
}

# --- what the run records ---------------------------------------------------
# Four snapshots across two changes. Each change is chosen to move a DIFFERENT
# term of the budget, so a panel that has stopped reacting cannot pass by
# accident on one of them.
$snapNames = @('lowcap', 'ultracap', 'uncapped', 'capped')

$proc = $null
$hwnd = [IntPtr]::Zero
try {
	Start-HarnessGame $exe $bin $log $LoadTimeoutSec
	# Through the console's `newgame`, never the landing page (Continue on the
	# newest shared save), waited out to the level (tools\HarnessGame.ps1).
	Start-NewGame $LoadTimeoutSec | Out-Null
	Start-Sleep -Seconds 2

	Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 700     # console open, and it stays open
	# logecho OFF again, so the log below is the one the verdict was written
	# against and no echo lands inside a snapshot. Retried until its own echo
	# lands (echoed before it turns echoing off).
	if (-not (Wait-ConsoleReady 'logecho off' 'console: > logecho off\s*$')) {
		throw 'the console never accepted a command after the new game'
	}
	# Face down a corridor rather than into a wall a metre away: a wall is the
	# cheapest scene in the game and would leave the quality change with almost
	# nothing to move.
	Run-Cmd 'face n'
	Run-Cmd 'framecap on'
	Run-Cmd 'quality 0'
	Start-Sleep -Seconds 8                            # the quality swap rebuilds
	Take-Snap 'lowcap'

	Run-Cmd 'quality 3'
	Start-Sleep -Seconds 18                           # ultra reloads every texture at 4k
	Take-Snap 'ultracap'

	Run-Cmd 'quality 0'
	Start-Sleep -Seconds 8
	Run-Cmd 'framecap off'
	Start-Sleep -Seconds 2
	Take-Snap 'uncapped'

	Run-Cmd 'framecap on'
	Start-Sleep -Seconds 2
	Take-Snap 'capped'
} finally {
	Stop-HarnessGame 8000
}

# --- the verdict, read from the log -----------------------------------------
$lines = if (Test-Path $log) { Get-Content $log } else { @() }
$failures = 0
$failedChecks = @()   # each failure's CHECK name, for the -SelfTest rule
$skips = @()
function Fail([string]$check, [string]$m) {
	Write-Host "  [FAIL] $m" -ForegroundColor Red
	$script:failures++
	$script:failedChecks += $check
}
function Ok([string]$m) { Write-Host "  [ok  ] $m" }
function Skip([string]$m) { Write-Host "  [skip] $m" -ForegroundColor Yellow; $script:skips += $m }
# The one check -SelfTest expects to fail, and no other.
$neverTaken = 'snapshot_never_taken'
$selfTestExpected = @("coverage $neverTaken")

# profilesnap NAME frame=.. cpu=.. wait=.. present=.. cap=.. gpu=.. rows=N samples=N secs=N
$snaps = @{}
foreach ($l in ($lines | Select-String 'profilesnap ')) {
	if ($l.Line -match 'profilesnap (\S+) frame=([\d.]+) cpu=([\d.]+) wait=([\d.]+) present=([\d.]+) cap=([\d.]+) gpu=([\d.]+) rows=(\d+) samples=(\d+)') {
		$snaps[$Matches[1]] = [pscustomobject]@{
			frame = [double]$Matches[2]; cpu = [double]$Matches[3]
			wait = [double]$Matches[4]; present = [double]$Matches[5]
			cap = [double]$Matches[6]; gpu = [double]$Matches[7]
			rows = [int]$Matches[8]; samples = [int]$Matches[9]
		}
	}
}

Write-Host ''
# COVERAGE FIRST. A snapshot that never reached the log was never taken, and a
# run that quietly recorded nothing must not read as clean. -SelfTest asks for
# one more, which nothing takes.
if ($SelfTest) { Write-Host "self-test: expecting exactly this check to fail: $($selfTestExpected -join ', ')" }
$want = if ($SelfTest) { @($snapNames) + $neverTaken } else { $snapNames }
foreach ($n in $want) {
	if ($snaps.ContainsKey($n)) {
		$s = $snaps[$n]
		Ok ("recorded {0,-9} frame {1,6:N3}  cpu {2,5:N3}  wait {3,5:N3}  present {4,5:N3}  cap {5,5:N3}  gpu {6,5:N3}  ({7} frames)" -f `
			$n, $s.frame, $s.cpu, $s.wait, $s.present, $s.cap, $s.gpu, $s.samples)
	} else {
		Fail "coverage $n" "snapshot '$n' never reached the log - it was not recorded"
	}
}

# The rest needs every REAL snapshot, and runs under -SelfTest too: its fault
# is the extra snapshot alone, and everything else must still pass.
if (@($snapNames | Where-Object { -not $snaps.ContainsKey($_) }).Count -eq 0) {
	# --- 1. THE PARTITION. cpu is defined as the frame minus every block, so
	# these must sum to the frame by construction. They stop summing the moment
	# a new blocking call is added without a zone - the failure this whole check
	# exists for, and the one that silently reports an idle engine as CPU-bound.
	foreach ($n in $snapNames) {
		$s = $snaps[$n]
		$sum = $s.cpu + $s.wait + $s.present + $s.cap
		$drift = [math]::Abs($s.frame - $sum)
		$tol = [math]::Max(0.05, $s.frame * 0.02)
		if ($drift -le $tol) {
			Ok ("{0,-9} budget accounts for the frame (drift {1:N4} ms)" -f $n, $drift)
		} else {
			Fail "partition $n" ("{0}: cpu+wait+present+cap = {1:N3} but frame = {2:N3} (drift {3:N3} ms > {4:N3}) - an unaccounted block" -f `
				$n, $sum, $s.frame, $drift, $tol)
		}
	}

	# --- 2. THE PANEL REACTS TO GPU LOAD. low -> ultra is 1k vs 4k textures, a
	# bigger light budget and a higher shadow tier; if the GPU timings do not
	# move for that, the GPU half of the readout has stopped reporting.
	$g0 = $snaps['lowcap'].gpu; $g1 = $snaps['ultracap'].gpu
	if ($g0 -le 0.0005) {
		Skip 'no GPU timings on this adapter (WARP?) - the GPU half is unchecked'
	} elseif ($g1 -gt $g0 * 1.20) {
		Ok ("ultra moved GPU work {0:N3} -> {1:N3} ms (+{2:N0}%)" -f $g0, $g1, (($g1 / $g0 - 1) * 100))
	} else {
		Fail 'gpu reacts' ("ultra barely moved GPU work: {0:N3} -> {1:N3} ms - the GPU timings look stuck" -f $g0, $g1)
	}

	# --- 3. THE FRAME CAP HOLDS. Parsed from the game's own report rather than
	# assumed, so this is not pinned to the monitor of whoever wrote it.
	# hz can be fractional (82.5 at 165 Hz / interval 2, 59.94 - code-review C200).
	$capLine = $lines | Select-String 'framecap enabled=1 hz=(\d+(\.\d+)?)' | Select-Object -Last 1
	$capHz = if ($capLine -and $capLine.Line -match 'hz=(\d+(\.\d+)?)') {
		[double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
	} else { 0 }
	$capped = $snaps['capped']; $uncapped = $snaps['uncapped']
	if ($capHz -le 0) {
		Skip 'the game reported no cap target - cap accuracy unchecked'
	} else {
		$targetMs = 1000.0 / $capHz
		$err = [math]::Abs($capped.frame - $targetMs) / $targetMs
		if ($err -le 0.12) {
			Ok ("cap holds {0:N3} ms against a {1:N3} ms target ({2} Hz, {3:N1}% off)" -f `
				$capped.frame, $targetMs, $capHz, ($err * 100))
		} else {
			Fail 'cap holds' ("cap missed: frame {0:N3} ms against a {1:N3} ms target ({2:N1}% off)" -f `
				$capped.frame, $targetMs, ($err * 100))
		}

		# --- 4. AND THE CAP IS THE THING DOING IT. Conditional on purpose: on a
		# single-monitor desktop the compositor already paces at the cap target,
		# so there is nothing for the cap to hold back and its absence is not a
		# fault. Named as a skip rather than passed silently.
		if ($uncapped.frame -ge $targetMs * 0.9) {
			Skip ("nothing to cap here: uncapped already ran at {0:N3} ms, at or past the {1:N3} ms target" -f `
				$uncapped.frame, $targetMs)
		} elseif ($capped.cap -gt $capped.frame * 0.25 -and $capped.frame -gt $uncapped.frame * 1.1) {
			Ok ("cap did the holding: frame {0:N3} -> {1:N3} ms with {2:N3} ms in wait.cap" -f `
				$uncapped.frame, $capped.frame, $capped.cap)
		} else {
			Fail 'cap does it' ("frame changed {0:N3} -> {1:N3} ms but wait.cap only accounts for {2:N3} ms" -f `
				$uncapped.frame, $capped.frame, $capped.cap)
		}
	}
}

Write-Host ''
$verdict = if ($failures -eq 0) { 'PASS' } else { 'FAIL' }
Write-Host ("profiletest RESULT={0} failures={1} skipped={2} self_test={3}" -f `
	$verdict, $failures, $skips.Count, [int]$SelfTest.IsPresent)
# What a green run here does NOT prove, said out loud rather than left to be
# assumed (the header says it too): no fault is injected into the partition or
# reaction checks, so neither has been watched to fail on demand. They have each
# failed for real during development, which is weaker evidence than a harness
# that can produce the failure on request.
Write-Host 'NOT self-tested: the partition and reaction assertions (the self-test injects only a missing snapshot)'
if ($SelfTest) {
	# SpellTest's rule: exactly the injected fault fails, everything else passes.
	$unexpected = @($failedChecks | Where-Object { $selfTestExpected -notcontains $_ })
	$uncaught = @($selfTestExpected | Where-Object { $failedChecks -notcontains $_ })
	foreach ($u in $uncaught) { Write-Host "  self-test: '$u' was injected but PASSED" -ForegroundColor Red }
	foreach ($u in $unexpected) { Write-Host "  self-test: '$u' failed but was not injected" -ForegroundColor Red }
	$asExpected = $unexpected.Count -eq 0 -and $uncaught.Count -eq 0
	if ($asExpected) {
		Write-Host 'SELF-TEST PASSED - exactly the snapshot that was never taken failed, and the real run passed' -ForegroundColor Green
	} else {
		Write-Host 'SELF-TEST FAILED - the checks did not fail exactly where the fault was injected' -ForegroundColor Red
	}
	exit ([int](-not $asExpected))
}
if ($verdict -eq 'PASS') {
	Write-Host 'PASS' -ForegroundColor Green
} else {
	Write-Host "FAIL - $failures problem(s)" -ForegroundColor Red
}
exit ([int]($verdict -ne 'PASS'))
