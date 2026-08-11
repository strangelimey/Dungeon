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
#   .\tools\ProfileTest.ps1 -SelfTest    # expects FAIL (see below)
#
# NEEDS A PROFILING BUILD (debug-profile / release-profile). Without DN_PROFILE
# every zone compiles to nothing and there is no budget to check - which is why
# the config is validated up front rather than producing an empty pass.
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
	# Checks the CHECKER: parses for a snapshot that is never taken, so the
	# coverage assertion must FAIL. Proves the log-parse is really reading
	# rather than passing on an empty result.
	[switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"
$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'

if (-not (Test-Path $exe)) { throw "no build at $exe - run build.cmd $Config first" }
if (Get-Process Dungeon -ErrorAction SilentlyContinue) {
	throw 'Dungeon.exe is already running - close it (this test drives its own instance)'
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class ProfWin {
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@

function Wait-ForLog([string]$pattern, [int]$timeoutSec, [string]$what) {
	$deadline = (Get-Date).AddSeconds($timeoutSec)
	while ((Get-Date) -lt $deadline) {
		if ($proc.HasExited) { throw "the game exited early (code $($proc.ExitCode)) waiting for $what" }
		if (Test-Path $log) {
			$hit = Select-String -Path $log -Pattern $pattern -ErrorAction SilentlyContinue | Select-Object -Last 1
			if ($hit) { return $hit.Line }
		}
		Start-Sleep -Milliseconds 400
	}
	throw "timed out after ${timeoutSec}s waiting for $what"
}
function Send-Key([int]$vk) {
	[ProfWin]::PostMessage($hwnd, 0x100, [IntPtr]$vk, [IntPtr]1) | Out-Null
	Start-Sleep -Milliseconds 60
	[ProfWin]::PostMessage($hwnd, 0x101, [IntPtr]$vk, [IntPtr][int64]0xC0000001) | Out-Null
	Start-Sleep -Milliseconds 250
}
function Send-Text([string]$t) {
	foreach ($c in $t.ToCharArray()) {
		[ProfWin]::PostMessage($hwnd, 0x102, [IntPtr][int]$c, [IntPtr]1) | Out-Null
		Start-Sleep -Milliseconds 30
	}
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

Remove-Item $log -ErrorAction SilentlyContinue
Write-Host "launching $exe"
$proc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
$hwnd = [IntPtr]::Zero
try {
	Wait-ForLog '--- load: ' $LoadTimeoutSec 'the boot load' | Out-Null
	$deadline = (Get-Date).AddSeconds(30)
	while ((Get-Date) -lt $deadline) {
		$proc.Refresh(); $hwnd = $proc.MainWindowHandle
		if ($hwnd -ne [IntPtr]::Zero) { break }
		Start-Sleep -Milliseconds 300
	}
	if ($hwnd -eq [IntPtr]::Zero) { throw 'the game never showed a main window' }

	# Landing page. Retried: one dropped PostMessage keystroke should not fail a run.
	$loaded = $false
	for ($try = 1; $try -le 3 -and -not $loaded; $try++) {
		Send-Key 0x0D
		$d2 = (Get-Date).AddSeconds(90)
		while ((Get-Date) -lt $d2) {
			if ($proc.HasExited) { throw 'the game exited during the dungeon load' }
			if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'Game loaded: ' -EA SilentlyContinue)) {
				$loaded = $true; break
			}
			Start-Sleep -Milliseconds 400
		}
	}
	if (-not $loaded) { throw 'the dungeon never loaded' }
	Start-Sleep -Seconds 3

	Send-Key 0xC0; Start-Sleep -Milliseconds 700     # console open, and it stays open
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
	if (-not $proc.HasExited) {
		if ($hwnd -ne [IntPtr]::Zero) { Send-Text 'quit'; Send-Key 0x0D }
		if (-not $proc.WaitForExit(8000)) { $proc.Kill() }
	}
}

# --- the verdict, read from the log -----------------------------------------
$lines = if (Test-Path $log) { Get-Content $log } else { @() }
$failures = 0
$skips = @()
function Fail([string]$m) { Write-Host "  [FAIL] $m" -ForegroundColor Red; $script:failures++ }
function Ok([string]$m) { Write-Host "  [ok  ] $m" }
function Skip([string]$m) { Write-Host "  [skip] $m" -ForegroundColor Yellow; $script:skips += $m }

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
# run that quietly recorded nothing must not read as clean.
$want = if ($SelfTest) { @('snapshot_never_taken') } else { $snapNames }
foreach ($n in $want) {
	if ($snaps.ContainsKey($n)) {
		$s = $snaps[$n]
		Ok ("recorded {0,-9} frame {1,6:N3}  cpu {2,5:N3}  wait {3,5:N3}  present {4,5:N3}  cap {5,5:N3}  gpu {6,5:N3}  ({7} frames)" -f `
			$n, $s.frame, $s.cpu, $s.wait, $s.present, $s.cap, $s.gpu, $s.samples)
	} else {
		Fail "snapshot '$n' never reached the log - it was not recorded"
	}
}

if ($failures -eq 0) {
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
			Fail ("{0}: cpu+wait+present+cap = {1:N3} but frame = {2:N3} (drift {3:N3} ms > {4:N3}) - an unaccounted block" -f `
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
		Fail ("ultra barely moved GPU work: {0:N3} -> {1:N3} ms - the GPU timings look stuck" -f $g0, $g1)
	}

	# --- 3. THE FRAME CAP HOLDS. Parsed from the game's own report rather than
	# assumed, so this is not pinned to the monitor of whoever wrote it.
	$capLine = $lines | Select-String 'framecap enabled=1 hz=(\d+)' | Select-Object -Last 1
	$capHz = if ($capLine -and $capLine.Line -match 'hz=(\d+)') { [int]$Matches[1] } else { 0 }
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
			Fail ("cap missed: frame {0:N3} ms against a {1:N3} ms target ({2:N1}% off)" -f `
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
			Fail ("frame changed {0:N3} -> {1:N3} ms but wait.cap only accounts for {2:N3} ms" -f `
				$uncapped.frame, $capped.frame, $capped.cap)
		}
	}
}

Write-Host ''
$verdict = if ($failures -eq 0) { 'PASS' } else { 'FAIL' }
$wantV = if ($SelfTest) { 'FAIL' } else { 'PASS' }
Write-Host ("profiletest RESULT={0} failures={1} skipped={2} self_test={3}" -f `
	$verdict, $failures, $skips.Count, [int]$SelfTest.IsPresent)
if ($SelfTest) {
	if ($verdict -eq 'FAIL') {
		Write-Host 'SELF-TEST PASSED - the coverage check reports a snapshot that was never taken' -ForegroundColor Green
	} else {
		Write-Host 'SELF-TEST FAILED - it passed with no snapshot actually recorded' -ForegroundColor Red
	}
} elseif ($verdict -eq 'PASS') {
	Write-Host 'PASS' -ForegroundColor Green
} else {
	Write-Host "FAIL - $failures problem(s)" -ForegroundColor Red
}
# What a green run here does NOT prove, said out loud rather than left to be
# assumed: -SelfTest only inverts the COVERAGE assertion, so the partition and
# reaction checks above have never been watched to fail on demand. They have
# each failed for real during development, which is weaker evidence than a
# harness that can produce the failure on request.
Write-Host 'NOT self-tested: the partition and reaction assertions (only coverage inverts)'
exit ([int]($verdict -ne $wantV))
