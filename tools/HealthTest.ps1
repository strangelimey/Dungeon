# ============================================================================
# tools\HealthTest.ps1 - the diagnostics regression run.
#
# docs/diagnostics.md says the game should never again die without saying why.
# This is the run that checks it, and it checks the way Michael actually finds
# out: by breaking the real game on purpose and then reading dungeon.log, which
# is the surface a crash is meant to be found on. Nothing here inspects the
# engine's internals - if the answer is not in the log, it does not count.
#
#   .\tools\HealthTest.ps1                 # every case, debug build
#   .\tools\HealthTest.ps1 -Only fault     # one case
#   .\tools\HealthTest.ps1 -SelfTest       # checks the CHECKER (see below)
#
# Exit code 0 = PASS. One machine-readable verdict line, like alloctest.
#
# -SELFTEST INVERTS THE VERDICT, the same trick AllocTest.ps1 and Bc7Test.ps1
# use. It runs every case WITHOUT injecting the failure, so every expectation
# should go unmet and the run must come back FAIL. A checker that cannot be seen
# to fail is not evidence: without this, a regex that matched anything - or a
# log-parse that never ran - would report a clean sweep forever.
#
# Every step waits on a LOG LINE rather than sleeping, so a slow cold-cache load
# stretches the wait instead of failing the run.
#
# ASCII ONLY, deliberately: PowerShell 5.1 reads a BOM-less .ps1 as ANSI, so a
# stray em-dash in a comment is a parse error, not a cosmetic issue.
# ============================================================================
[CmdletBinding()]
param(
	[ValidateSet('debug', 'release')][string]$Config = 'debug',
	[int]$LoadTimeoutSec = 240,
	[string]$Only = '',
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
public class HealthTestWin {
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@

# ---------------------------------------------------------------------------
# THE CASES. `inject` is the console line that breaks something; `expect` is
# what must appear in dungeon.log afterwards; `survives` says whether the
# process is supposed to still be running when it is over.
#
# NOT COVERED, and said out loud rather than quietly skipped: the Killed kind
# (a hard force-terminate) has no console command - it is the THREADS panel's
# kill button - so it cannot be driven from here. Everything else is.
# ---------------------------------------------------------------------------
$cases = @(
	@{
		name = 'throw'
		desc = 'a main-thread throw is caught, recorded, and the game plays on'
		inject = @('crashpoke throw')
		settle = 3
		survives = $true
		expect = @(
			"exception on 'main': crashpoke: a deliberate main-thread throw",
			'Game_DevCommands\.cpp:\d+'   # the THROW site, not the catch site
		)
		dump = $false
	},
	@{
		name = 'worker'
		desc = 'a worker that throws every tick is recorded per tick and keeps running'
		inject = @('crashpoke worker')
		settle = 6
		survives = $true
		expect = @(
			"exception on 'demo\.thrower' \(worker \d+, tick 0\)",
			"exception on 'demo\.thrower' \(worker \d+, tick [1-9]\d*\)",  # more than one
			'Game_DevCommands\.cpp:\d+'
		)
		dump = $false
	},
	@{
		name = 'stall'
		desc = 'a wedged worker is recorded as a stall, once, without a reboot'
		inject = @('threadwedge')
		settle = 6
		survives = $true
		expect = @("stall on 'demo\.wedged'.*past its \d+ ms watchdog")
		dump = $false
	},
	@{
		name = 'probe'
		desc = 'a live stalled worker can be asked what it is stuck on'
		inject = @('threadwedge', 'health probe demo.wedged')
		settle = 4
		survives = $true
		expect = @(
			"probe 'demo\.wedged' #\d+ \[stalled\]",
			'DelayExecution',              # the OS frame IS the diagnosis here
			'Game_DevCommands\.cpp:\d+'    # and the line it is stuck on
		)
		dump = $false
	},
	@{
		name = 'restart'
		desc = 'an over-budget worker stalls and the supervisor reboots it'
		# 1500 ms a tick against a 200 ms watchdog: past the supervisor's 5x line.
		inject = @('threadspawn 1500')
		settle = 8
		survives = $true
		expect = @(
			"stall on 'demo\.worker'",
			"restart on 'demo\.worker'.*rebooted \(restart #\d+\)"
		)
		dump = $false
	},
	@{
		name = 'fault'
		desc = 'an access violation - which no catch can see - reports and dumps'
		inject = @('crashpoke fault')
		settle = 5
		survives = $false
		expect = @(
			"fault on 'main': access violation writing 0x0",
			'CRASH: access violation',
			'faulting stack:',
			'Game_DevCommands\.cpp:\d+'    # walked from the CONTEXT record
		)
		dump = $true
	},
	@{
		name = 'assert'
		desc = 'an assertion reports and dumps BEFORE it aborts'
		inject = @('crashpoke assert')
		settle = 5
		survives = $false
		expect = @(
			"FATAL on 'main': Assertion failed",
			'minidump was written'
		)
		dump = $true
	}
)

if ($Only) {
	$cases = @($cases | Where-Object { $_.name -eq $Only })
	if ($cases.Count -eq 0) { throw "no case named '$Only'" }
}

# ---------------------------------------------------------------------------
function Wait-ForLog([string]$pattern, [int]$timeoutSec, [string]$what) {
	$deadline = (Get-Date).AddSeconds($timeoutSec)
	while ((Get-Date) -lt $deadline) {
		if ($proc.HasExited) {
			throw "the game exited early (code $($proc.ExitCode)) while waiting for $what"
		}
		if (Test-Path $log) {
			$hit = Select-String -Path $log -Pattern $pattern -ErrorAction SilentlyContinue |
				Select-Object -Last 1
			if ($hit) { return $hit.Line }
		}
		Start-Sleep -Milliseconds 400
	}
	throw "timed out after ${timeoutSec}s waiting for $what"
}
function Send-Key([int]$vk) {
	[HealthTestWin]::PostMessage($hwnd, 0x100, [IntPtr]$vk, [IntPtr]1) | Out-Null
	Start-Sleep -Milliseconds 60
	[HealthTestWin]::PostMessage($hwnd, 0x101, [IntPtr]$vk, [IntPtr][int64]0xC0000001) | Out-Null
	Start-Sleep -Milliseconds 250
}
function Send-Text([string]$text) {
	foreach ($c in $text.ToCharArray()) {
		# WM_CHAR: the console reads typed characters, not virtual keys.
		[HealthTestWin]::PostMessage($hwnd, 0x102, [IntPtr][int]$c, [IntPtr]1) | Out-Null
		Start-Sleep -Milliseconds 30
	}
}

# Runs one case in its own process (half of them kill the game) and returns
# $true if every expectation was met.
function Invoke-Case($case) {
	Write-Host ''
	Write-Host "[$($case.name)] $($case.desc)"

	Remove-Item $log -ErrorAction SilentlyContinue
	Get-ChildItem $bin -Filter *.dmp -ErrorAction SilentlyContinue | Remove-Item -Force

	$script:proc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
	$script:hwnd = [IntPtr]::Zero
	try {
		Wait-ForLog '--- load: ' $LoadTimeoutSec 'the boot load' | Out-Null
		# The boot-load line can beat the window handle becoming visible to the
		# process object, so this WAITS rather than asking once - a one-shot read
		# failed a case that passes perfectly well by hand.
		$deadline = (Get-Date).AddSeconds(30)
		while ((Get-Date) -lt $deadline) {
			$proc.Refresh()
			$script:hwnd = $proc.MainWindowHandle
			if ($hwnd -ne [IntPtr]::Zero) { break }
			Start-Sleep -Milliseconds 300
		}
		if ($hwnd -eq [IntPtr]::Zero) { throw 'the game never showed a main window' }

		# With no save present the first landing-page entry is Start New Game.
		# RETRIED, because a single PostMessage keystroke is not reliable enough
		# to hang a seven-minute run on: one case failed twice on a dropped Enter
		# at startup, having nothing to do with what it was testing. Pressing
		# Enter again on an already-loading game is harmless.
		$loaded = $false
		for ($try = 1; $try -le 3 -and -not $loaded; $try++) {
			Send-Key 0x0D
			$deadline = (Get-Date).AddSeconds(60)
			while ((Get-Date) -lt $deadline) {
				if ($proc.HasExited) { throw "the game exited during the dungeon load" }
				if ((Test-Path $log) -and
					(Select-String -Path $log -Pattern 'Game loaded: ' -ErrorAction SilentlyContinue)) {
					$loaded = $true
					break
				}
				Start-Sleep -Milliseconds 400
			}
			if (-not $loaded) { Write-Host "  (retrying Start New Game, attempt $($try + 1))" }
		}
		if (-not $loaded) { throw 'the dungeon never loaded' }
		Start-Sleep -Seconds 2

		Send-Key 0xC0                    # ` opens the console
		Start-Sleep -Milliseconds 600
		if ($SelfTest) {
			# The injection is SKIPPED on purpose. Everything below still runs,
			# so an expectation that is met anyway is an expectation that was
			# never really testing the injection.
			Write-Host '  self-test: skipping the injection'
		} else {
			foreach ($cmd in $case.inject) {
				# A command does NOT close the console, so it stays open for the
				# next one - toggling here would send the next line to the game
				# as movement keys.
				Send-Text $cmd
				Send-Key 0x0D
				Start-Sleep -Seconds 2
			}
		}
		Start-Sleep -Seconds $case.settle
	} finally {
		# Captured BEFORE we shut it down: whether the game was still running of
		# its own accord is the answer a `survives` case turns on, and quitting
		# it ourselves would erase the distinction.
		$script:diedEarly = $proc.HasExited
		if (-not $proc.HasExited) {
			if ($hwnd -ne [IntPtr]::Zero) { Send-Text 'quit'; Send-Key 0x0D }
			if (-not $proc.WaitForExit(6000)) { $proc.Kill() }
		}
	}

	# --- the verdict, read from the log and nowhere else --------------------
	$lines = if (Test-Path $log) { Get-Content $log } else { @() }
    $text = $lines -join "`n"
	$ok = $true

	# `survives` is checked BEFORE the log: a case whose whole point is that the
	# game kept playing has failed if the process died, however good its log is.
	# Only meaningful when the failure was actually injected.
	if (-not $SelfTest) {
		$stillRan = -not $script:diedEarly
		if ($case.survives -and -not $stillRan) {
			Write-Host '  [FAIL] the game died - it was supposed to survive this' -ForegroundColor Red
			$ok = $false
		}
	}

	foreach ($pattern in $case.expect) {
		if ($text -match $pattern) {
			Write-Host "  [ok  ] $pattern"
		} else {
			Write-Host "  [FAIL] not in the log: $pattern" -ForegroundColor Red
			$ok = $false
		}
	}
	if ($case.dump) {
		$dumps = @(Get-ChildItem $bin -Filter *.dmp -ErrorAction SilentlyContinue)
		if ($dumps.Count -gt 0) {
			$mb = [math]::Round($dumps[0].Length / 1MB, 1)
			Write-Host "  [ok  ] minidump written ($($dumps[0].Name), $mb MB)"
		} else {
			Write-Host '  [FAIL] no minidump was written' -ForegroundColor Red
			$ok = $false
		}
	}
	return $ok
}

# ---------------------------------------------------------------------------
$failures = 0
foreach ($case in $cases) {
	$script:diedEarly = $false
	try {
		if (-not (Invoke-Case $case)) { $failures++ }
	} catch {
		# An early exit is itself a result for a `survives` case, not a harness
		# error: report it as a failed case and carry on to the next one.
		Write-Host "  [FAIL] $($_.Exception.Message)" -ForegroundColor Red
		$failures++
		if (Get-Process Dungeon -ErrorAction SilentlyContinue) {
			Get-Process Dungeon | Stop-Process -Force
		}
	}
	# Let the previous process release the log file before the next one truncates
	# it - cases run back to back and the loser of that race looks like a load
	# that never happened.
	Start-Sleep -Seconds 2
}

Write-Host ''
# A self-test PASSES when the run FAILS: the point is that the checker can tell
# the difference between a failure that happened and one that did not.
$verdict = if ($failures -eq 0) { 'PASS' } else { 'FAIL' }
$want = if ($SelfTest) { 'FAIL' } else { 'PASS' }
Write-Host "healthtest RESULT=$verdict cases=$($cases.Count) failures=$failures self_test=$([int]$SelfTest.IsPresent)"
if ($SelfTest) {
	if ($verdict -eq 'FAIL') {
		Write-Host 'SELF-TEST PASSED - the harness reports absence as failure' -ForegroundColor Green
	} else {
		Write-Host 'SELF-TEST FAILED - the harness passed with nothing injected' -ForegroundColor Red
	}
} elseif ($verdict -eq 'PASS') {
	Write-Host 'PASS - every failure was caught, recorded and explained' -ForegroundColor Green
} else {
	Write-Host "FAIL - $failures case(s) went unreported" -ForegroundColor Red
}
Write-Host 'NOTE: the Killed kind is not covered here - a hard kill is a THREADS panel button, not a command.'
exit ([int]($verdict -ne $want))
