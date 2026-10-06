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
# -SELFTEST runs every case WITHOUT injecting its failure and NAMES what it
# expects: every case fails, and fails on EACH of its own expectations (no
# pattern met, no minidump) - SpellTest's rule, code-review C419. A checker that
# cannot be seen to fail is not evidence: without this, a regex that matched
# anything would report a clean sweep forever. And a case that fails for the
# WRONG reason fails the self-test: a harness error (the game died at boot, the
# console never answered), a game that died with nothing injected, or a run
# whose log lacks the CONTROL line - the echo of the `logecho off` typed just
# before the injection, which proves the verdict read this run's log and the run
# reached its injection point. Before, any failure passed: a game that crashed
# at boot "passed" the self-test seven times over.
#
# Every step waits on a LOG LINE rather than sleeping, so a slow cold-cache load
# stretches the wait instead of failing the run.
#
# Two cases look at what a caught throw leaves BEHIND rather than at its record
# (code-review C208, the UI walk's clip - Game/ClipPoke.h): `uiclip` throws from
# inside a scroll area's walk and then clicks a button outside it, which must
# still land; `uinest` checks a scroll area nested in a tab page leaves its
# siblings their clicks and their drawing.
#
# The cases that END the process (`fault`, `overflow`, `assert`) each want ONE
# report line, written after the dump and naming what became of it (code-review
# C385), and a dump on disk. `overflow` is a deliberate stack overflow on the main
# thread: the fault whose report needs a stack of its own (C388).
#
# Four cases break the GPU (code-review C195). `devremoved` removes the D3D12
# device on purpose - a TDR's shape without hanging a GPU on a shared machine -
# and needs the failed call's HRESULT by name, the removal reason and DRED's
# record in the log BEFORE the FATAL line, after `dredpoke` has put a made-up
# record through the same readout (an asked-for removal leaves DRED no command
# list in flight). `devassert`, `devfault` and `devterminate` remove it and die
# at once, before any call fails, so the removal can only reach the log through
# the crash handler's fatal note - one case per place a report runs it.
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

# Muted for the whole run, restored however it ends (tools\HarnessAudio.ps1).
. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not (Test-HarnessMuted $bin)) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }
# Launch, input and log waits: the one shared copy (tools\HarnessGame.ps1).
. (Join-Path $PSScriptRoot 'HarnessGame.ps1')

$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'

if (-not (Test-Path $exe)) { throw "no build at $exe - run build.cmd $Config first" }
Assert-ExeCurrent $exe
Assert-NotRunning $exe

# A left click at client point (x, y). The pointer moves there first, as a hand
# would, so the press lands where it is aimed.
function Send-Click([int]$x, [int]$y) {
	$at = [int64](($y -shl 16) -bor ($x -band 0xFFFF))
	Send-Message 0x200 0 $at; Start-Sleep -Milliseconds 150   # WM_MOUSEMOVE
	Send-Message 0x201 1 $at; Start-Sleep -Milliseconds 60    # WM_LBUTTONDOWN
	Send-Message 0x202 0 $at; Start-Sleep -Milliseconds 250   # WM_LBUTTONUP
}

# ---------------------------------------------------------------------------
# A pattern for a log line and, within the forty lines after it, a line naming
# `frame`. A recorded stack is logged one frame a line right under its event's
# own line, so this is how "the stall line carries a stack that names X" reads
# in the log. Anchored to its line on purpose: since the stall and the kill
# records carry WALKED stacks (code-review C387), the same OS frames turn up
# under more than one event, and a bare frame pattern would be met by
# whichever logged first.
function After([string]$line, [string]$frame) {
	return "$line[^\n]*(?:\n[^\n]*){0,40}?\n[^\n]*$frame"
}

# A pattern met only when `text` is in the log exactly ONCE. A crash is reported
# in one line, written after its dump (code-review C385): the handlers used to
# log it first, before the dump, and then again beside the dump's status.
function Once([string]$text) {
	return "(?s)\A(?!.*?(?:$text).*?(?:$text)).*?(?:$text)"
}

# A pattern for log lines met IN ORDER, each within the eighty lines after the
# one before it - for a report whose ORDER is the claim. The GPU cases need it
# (code-review C195): a removal is logged BEFORE ReportFatal's FATAL line when
# a failed HRESULT reports it, and AFTER a crash's own lines when the fatal note
# does, and `After` alone would pass a line printed on the wrong side of the
# FATAL one. Eighty lines a hop, not forty: a hop may cross a recorded stack (32
# frames) or a fault's walked one (up to 62, cut at wWinMain).
function InOrder([string[]]$lines) {
	$p = $lines[0]
	for ($i = 1; $i -lt $lines.Count; $i++) { $p += "[^\n]*(?:\n[^\n]*){0,80}?\n[^\n]*$($lines[$i])" }
	return $p
}

# ---------------------------------------------------------------------------
# THE CASES. `inject` is the console line that breaks something; `expect` is
# what must appear in dungeon.log afterwards; `survives` says whether the
# process is supposed to still be running when it is over. An optional `after`
# runs once the injections are in - in a self-test too, where it must find
# nothing to act on.
#
# Every event kind is covered. The Killed kind is driven by `threadkill`, the
# THREADS panel's kill button as a command - which is all it lacked before.
# ---------------------------------------------------------------------------

# A removed device's report, as Graphics/D3DUtil.cpp logs it. The page-fault
# line names its two real answers: a bare 'DRED page fault: ' would also take
# the 'unavailable' line a runtime prints when page faults were never switched
# on, and so pass with DRED half off.
$removedLine = 'gpu device removed: reason 0x887A00[0-9A-F]{2} \(DXGI_ERROR_'
$crumbsLine = 'DRED auto-breadcrumbs: (\d+ command list\(s\)|more than \d+ command lists) in flight'
$pageFaultLine = 'DRED page fault: (none - |GPU virtual address 0x)'

$cases = @(
	@{
		name = 'throw'
		desc = 'a main-thread throw is caught, recorded, and the game plays on'
		inject = @('crashpoke throw')
		settle = 3
		survives = $true
		expect = @(
			"exception on 'main': crashpoke: a deliberate main-thread throw",
			'Game_DevDiagnostics\.cpp:\d+'   # the THROW site, not the catch site
		)
		dump = $false
	},
	@{
		name = 'uiclip'
		desc = 'a throw inside a scroll area''s walk leaves no clip behind: a later click outside it lands'
		inject = @('crashpoke uiclip')
		# The click goes in with the console SHUT (an open console owns the
		# input), at the point the poke logged before it threw. A self-test has
		# no such line and clicks nothing. The console is opened again at the end
		# for the harness's own `quit`.
		after = {
			$at = Select-String -Path $log -Pattern 'crashpoke uiclip: the button .* is at (\d+),(\d+)' -EA SilentlyContinue |
				Select-Object -Last 1
			if (-not $at) { Write-Host '  no button point in the log - nothing to click'; return }
			$x = [int]$at.Matches[0].Groups[1].Value
			$y = [int]$at.Matches[0].Groups[2].Value
			Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 600
			Write-Host "  clicking the button outside the scroll area at $x,$y"
			Send-Click $x $y
			Start-Sleep -Seconds 1
			Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 600
		}
		settle = 2
		survives = $true
		# The message is the premise: the throw names the clip in force and the
		# button outside it ONLY when both hold (Game/ClipPoke.cpp), else it says
		# the case tests nothing. The stack line is just the throw site - the
		# poke's whole call chain is in that file, so it cannot show the clip.
		expect = @(
			"exception on 'main': crashpoke: a deliberate throw inside a scroll area's walk, under its clip \[\d+,\d+ \d+x\d+\], the button \[\d+,\d+ \d+x\d+\] outside it",
			'ClipPoke\.cpp:\d+',
			'crashpoke uiclip: a click outside the scroll area landed'
		)
		dump = $false
	},
	@{
		name = 'uinest'
		desc = 'a scroll area nested in a tab page leaves its siblings their clicks and drawing'
		inject = @('clippoke')
		settle = 2
		survives = $true
		expect = @('clippoke: PASS')
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
			'Game_DevDiagnostics\.cpp:\d+'
		)
		dump = $false
	},
	@{
		name = 'stall'
		desc = 'a wedged worker is recorded as a stall, once, with where it is stuck'
		inject = @('threadwedge')
		settle = 6
		survives = $true
		# The stall line, and under it the stack the supervisor WALKED when it saw
		# the stall: the OS wait, the line the job is stuck on, and a std:: frame
		# - which IsPlumbingFrame drops, so it shows only if the walked stack was
		# logged WHOLE, as the probe prints one.
		expect = @(
			"stall on 'demo\.wedged'.*past its \d+ ms watchdog",
			(After "stall on 'demo\.wedged'" 'DelayExecution'),
			(After "stall on 'demo\.wedged'" 'Game_DevDiagnostics\.cpp:\d+'),
			(After "stall on 'demo\.wedged'" 'std::this_thread::sleep_for')
		)
		dump = $false
	},
	@{
		name = 'probe'
		desc = 'a live stalled worker can be asked what it is stuck on'
		inject = @('threadwedge', 'health probe demo.wedged')
		settle = 4
		survives = $true
		# Under the PROBE's own line: the stall record logs the same frames.
		expect = @(
			"probe 'demo\.wedged' #\d+ \[stalled\]",
			(After "probe 'demo\.wedged' #\d+ \[stalled\]" 'DelayExecution'),
			(After "probe 'demo\.wedged' #\d+ \[stalled\]" 'Game_DevDiagnostics\.cpp:\d+')
		)
		dump = $false
	},
	@{
		name = 'kill'
		desc = 'a wedged worker is force-terminated, and the kill says where it was stuck'
		inject = @('threadwedge', 'threadkill demo.wedged')
		settle = 4
		survives = $true
		expect = @(
			"killed on 'demo\.wedged'.*force-terminated",
			(After "killed on 'demo\.wedged'" 'DelayExecution'),
			(After "killed on 'demo\.wedged'" 'Game_DevDiagnostics\.cpp:\d+'),
			(After "killed on 'demo\.wedged'" 'std::this_thread::sleep_for'),
			"thread 'demo\.wedged' would not stop"
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
		# ONE line, and written AFTER the dump: it says what became of the dump,
		# which nothing before the dump could know (code-review C385).
		expect = @(
			"CRASH: fault on 'main': access violation writing 0x0[^\n]*minidump \d of \d written",
			(Once 'access violation writing 0x0'),
			(After 'faulting stack:' 'Game_DevDiagnostics\.cpp:\d+')   # walked from the CONTEXT record
		)
		dump = $true
	},
	@{
		name = 'overflow'
		desc = 'a stack overflow - the fault with no stack left to report on - reports and dumps'
		inject = @('crashpoke overflow')
		settle = 6
		survives = $false
		# Without room kept back on the thread's stack and a reporter thread to
		# write the dump, the report faulted again inside itself and the process
		# ended with neither (code-review C388).
		expect = @(
			"CRASH: fault on 'main': stack overflow[^\n]*minidump \d of \d written",
			(Once 'stack overflow \(code'),
			(After 'faulting stack:' 'OverflowPoke')
		)
		dump = $true
	},
	@{
		name = 'assert'
		desc = 'an assertion reports and dumps BEFORE it aborts'
		inject = @('crashpoke assert')
		settle = 5
		survives = $false
		# One line again: the assertion and the dump's status together.
		expect = @(
			"FATAL on 'main': Assertion failed[^\n]*minidump \d of \d written",
			(Once 'crashpoke: a deliberate assertion failure')
		)
		dump = $true
	},
	@{
		name = 'devremoved'
		desc = 'a removed GPU device - a TDR, asked for - reports the HRESULT, the removal reason and DRED''s record'
		# ID3D12Device5::RemoveDevice: the next checked D3D12 call fails, and the
		# report must say why (code-review C195). Before, it named only the call:
		# "Assertion failed: SUCCEEDED(hr_)" and the expression, the same line an
		# out-of-memory left. An asked-for removal leaves DRED no command list in
		# flight, so `dredpoke` first puts a made-up record through the same
		# readout - the breadcrumb lines only a real TDR would otherwise reach. Its
		# expectations sit under its own line, the removal's after the failed call.
		inject = @('dredpoke', 'crashpoke devremoved')
		settle = 5
		survives = $false
		# The queued 'frame' list (0 completed) must be ONE line with no op window
		# and no stop marker: the next line is the immediate list's. The removal's
		# lines must fall BETWEEN the failed call and the FATAL line - FailHr's own
		# report; the fatal note would print the same lines after FATAL (the three
		# cases below check that path).
		expect = @(
			(After 'dredpoke: a made-up DRED record' 'DRED auto-breadcrumbs: 3 command list\(s\) in flight'),
			(After 'dredpoke: a made-up DRED record' "list 'frame' on queue 'direct queue': 14 ops, 7 completed - stopped at #7 DrawIndexedInstanced"),
			(After 'dredpoke: a made-up DRED record' '#3 DrawInstanced\n'),
			(After 'dredpoke: a made-up DRED record' '#6 SetMarker "scene"'),
			(After 'dredpoke: a made-up DRED record' '#7 DrawIndexedInstanced   <- the GPU stopped here'),
			(After 'dredpoke: a made-up DRED record' '#11 ResourceBarrier \(not completed\)'),
			(After 'dredpoke: a made-up DRED record' "list 'frame' on queue 'direct queue': 14 ops, none completed - not started, or stopped at its first op \(#0 ResourceBarrier\)\n[^\n]*list 'immediate'"),
			(After 'dredpoke: a made-up DRED record' "list 'immediate' on queue 'direct queue': 3 ops, all completed"),
			(After 'dredpoke: a made-up DRED record' 'DRED page fault: GPU virtual address 0x0000000012340000'),
			(After 'dredpoke: a made-up DRED record' "live resource 'scene color'"),
			(After 'dredpoke: a made-up DRED record' "recently freed resource 'shadow cube 3'"),
			'D3D12 call failed at \S+\.cpp:\d+: .+ returned 0x887A000[5-7] \(DXGI_ERROR_DEVICE_(REMOVED|HUNG|RESET)\)',
			(InOrder @('D3D12 call failed at', $removedLine, "FATAL on 'main': D3D12 call failed")),
			(InOrder @('D3D12 call failed at', $crumbsLine, "FATAL on 'main': D3D12 call failed")),
			(InOrder @('D3D12 call failed at', $pageFaultLine, "FATAL on 'main': D3D12 call failed")),
			"FATAL on 'main': D3D12 call failed: 0x887A000[5-7] DXGI_ERROR_DEVICE_"
		)
		dump = $true
	},
	# The removal with NO failed HRESULT to report it: the same command dies at
	# once, before any D3D12 call can fail, so the reason and DRED's record can
	# only come from the FATAL NOTE (crash::SetFatalNote) - one case for each of
	# its three call sites, each anchored to that site's last own line so the
	# note is seen to run LAST.
	@{
		name = 'devassert'
		desc = 'an assert straight after a removal reports the removal too, through the fatal note'
		inject = @('crashpoke devremoved assert')
		settle = 5
		survives = $false
		# ReportFatal: the FATAL line (written after the dump, naming it - C385) and
		# its stack, then the note.
		expect = @(
			"FATAL on 'main': Assertion failed[^\n]*minidump \d of \d written",
			(InOrder @("FATAL on 'main': Assertion failed[^\n]*minidump \d of \d written", $removedLine)),
			(InOrder @("FATAL on 'main': Assertion failed", $removedLine, $crumbsLine)),
			(InOrder @("FATAL on 'main': Assertion failed", $removedLine, $pageFaultLine))
		)
		dump = $true
	},
	@{
		name = 'devfault'
		desc = 'a fault straight after a removal reports the removal too, after its own stack'
		inject = @('crashpoke devremoved fault')
		settle = 5
		survives = $false
		# The fault filter: the CRASH line (after the dump), the walked stack, the note.
		expect = @(
			"CRASH: fault on 'main': access violation writing 0x0[^\n]*minidump \d of \d written",
			(InOrder @("CRASH: fault on 'main': access violation", 'faulting stack:', $removedLine)),
			(InOrder @("CRASH: fault on 'main': access violation", $removedLine, $crumbsLine)),
			(InOrder @("CRASH: fault on 'main': access violation", $removedLine, $pageFaultLine))
		)
		dump = $true
	},
	@{
		name = 'devterminate'
		desc = 'a std::terminate straight after a removal reports the removal too'
		inject = @('crashpoke devremoved terminate')
		settle = 5
		survives = $false
		# The terminate handler: the TERMINATE line (after the dump), the note.
		expect = @(
			"TERMINATE: FATAL on 'main': std::terminate called with no exception in flight[^\n]*minidump \d of \d written",
			(InOrder @("TERMINATE: FATAL on 'main': std::terminate called", $removedLine)),
			(InOrder @("TERMINATE: FATAL on 'main': std::terminate called", $removedLine, $crumbsLine)),
			(InOrder @("TERMINATE: FATAL on 'main': std::terminate called", $removedLine, $pageFaultLine))
		)
		dump = $true
	}
)

if ($Only) {
	$cases = @($cases | Where-Object { $_.name -eq $Only })
	if ($cases.Count -eq 0) { throw "no case named '$Only'" }
}

# ---------------------------------------------------------------------------
# The CONTROL line: the echo of the `logecho off` typed just before the
# injection (Wait-ConsoleReady waits for exactly this). It must be in the log
# whether or not anything was injected - it is what says the verdict below read
# THIS run's log, and that the run got as far as its injection.
$control = '(?m)console: > logecho off\s*$'   # (?m): matched per line of the joined log too

# Runs one case in its own process (half of them kill the game) and returns
# what its log showed. Throws on a HARNESS error - a run that never reached its
# injection - which is never a verdict about the game.
function Invoke-Case($case) {
	Get-ChildItem $bin -Filter *.dmp -ErrorAction SilentlyContinue | Remove-Item -Force

	$script:hwnd = [IntPtr]::Zero
	try {
		Start-HarnessGame $exe $bin $log $LoadTimeoutSec
		# A new game by COMMAND, never Enter on the landing page (Continue
		# whenever a save exists - all seven cases once failed on a WorldTest
		# leftover's load). And waited out to the LEVEL, not to 'Game loaded:',
		# which lands before the level's own load has begun while every console
		# command is still refused (C429): on a cold cache that refused the
		# injections, and the case failed for a harness reason.
		Start-NewGame $LoadTimeoutSec | Out-Null

		# Console OPEN for the injections, and logecho OFF: the verdict reads only
		# what the game logs on its own, and a mirrored console line must never
		# count as evidence. Its own echo proves the console takes commands.
		Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 600
		if (-not (Wait-ConsoleReady 'logecho off' $control)) {
			throw 'the console never accepted a command after the new game'
		}

		# A command never closes the console, so the case's lines go straight in
		# - toggling it here would CLOSE it.
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
		if ($case.after) { & $case.after }
		Start-Sleep -Seconds $case.settle
	} finally {
		# Captured BEFORE we shut it down: whether the game was still running of
		# its own accord is the answer a `survives` case turns on, and quitting
		# it ourselves would erase the distinction.
		$script:diedEarly = $proc -and $proc.HasExited
		Stop-HarnessGame 6000
	}

	# --- what the log says, and nowhere else ------------------------------------
	$text = if (Test-Path $log) { (Get-Content $log) -join "`n" } else { '' }
	return [pscustomobject]@{
		control = $text -match $control
		died = $script:diedEarly
		met = @($case.expect | Where-Object { $text -match $_ })
		unmet = @($case.expect | Where-Object { $text -notmatch $_ })
		dump = @(Get-ChildItem $bin -Filter *.dmp -ErrorAction SilentlyContinue) | Select-Object -First 1
	}
}

# A real run: every expectation met, the game alive if it should be, the dump
# written if it should be. Returns whether the case passed.
function Judge-Case($case, $r) {
	$ok = $true
	if (-not $r.control) {
		Write-Host '  [FAIL] no control line - the verdict is not reading this run, or it never reached its injection' -ForegroundColor Red
		$ok = $false
	}
	# `survives` is checked BEFORE the log: a case whose whole point is that the
	# game kept playing has failed if the process died, however good its log is.
	if ($case.survives -and $r.died) {
		Write-Host '  [FAIL] the game died - it was supposed to survive this' -ForegroundColor Red
		$ok = $false
	}
	foreach ($p in $r.met) { Write-Host "  [ok  ] $p" }
	foreach ($p in $r.unmet) {
		Write-Host "  [FAIL] not in the log: $p" -ForegroundColor Red
		$ok = $false
	}
	if ($case.dump) {
		# A dump of the stacks and the memory they point at runs to tens of MB. A
		# file of a few bytes is one whose writing failed half-way - what a stack
		# overflow left when its report ran out of stack (code-review C388) - and
		# is no dump.
		if ($r.dump -and $r.dump.Length -ge 1MB) {
			Write-Host "  [ok  ] minidump written ($($r.dump.Name), $([math]::Round($r.dump.Length / 1MB, 1)) MB)"
		} elseif ($r.dump) {
			Write-Host "  [FAIL] the minidump is $($r.dump.Length) bytes ($($r.dump.Name)) - its writing failed" -ForegroundColor Red
			$ok = $false
		} else {
			Write-Host '  [FAIL] no minidump was written' -ForegroundColor Red
			$ok = $false
		}
	}
	return $ok
}

# A self-test run (nothing injected): the case must fail on EVERY one of its
# expectations, and for no other reason. Returns whether it failed exactly so.
function Judge-SelfTestCase($case, $r) {
	$asExpected = $true
	if (-not $r.control) {
		Write-Host '  [FAIL] no control line - a broken run, not a caught fault' -ForegroundColor Red
		$asExpected = $false
	}
	if ($r.died) {
		Write-Host '  [FAIL] the game died with nothing injected' -ForegroundColor Red
		$asExpected = $false
	}
	foreach ($p in $r.unmet) { Write-Host "  [ok  ] unmet with nothing injected: $p" }
	foreach ($p in $r.met) {
		Write-Host "  [FAIL] MET with nothing injected - it cannot see the fault: $p" -ForegroundColor Red
		$asExpected = $false
	}
	if ($case.dump) {
		if ($r.dump) {
			Write-Host "  [FAIL] a minidump with nothing injected ($($r.dump.Name))" -ForegroundColor Red
			$asExpected = $false
		} else {
			Write-Host '  [ok  ] no minidump with nothing injected'
		}
	}
	return $asExpected
}

# ---------------------------------------------------------------------------
if ($SelfTest) {
	Write-Host "self-test: every case's injection is skipped, so each of these must fail on every expectation: $(($cases | ForEach-Object { $_.name }) -join ', ')"
}
$failures = 0        # cases a real run would fail
$notAsNamed = 0      # -SelfTest: cases that did not fail exactly as named
$harnessErrors = 0   # runs that never reached a verdict
foreach ($case in $cases) {
	$script:diedEarly = $false
	# Cleared per case so the catch below can never act on the PREVIOUS case's
	# process object when this one failed before Start-Process returned.
	$script:proc = $null
	Write-Host ''
	Write-Host "[$($case.name)] $($case.desc)"
	try {
		$r = Invoke-Case $case
		if ($SelfTest) {
			if (-not (Judge-SelfTestCase $case $r)) { $notAsNamed++ }
			# The plain verdict, for the RESULT line: met everything, so a real
			# run would have passed it.
			if (-not ($r.control -and $r.unmet.Count -eq 0 -and
					  (-not $case.dump -or ($r.dump -and $r.dump.Length -ge 1MB)) -and
					  -not ($case.survives -and $r.died))) { $failures++ }
		} elseif (-not (Judge-Case $case $r)) {
			$failures++
		}
	} catch {
		# A HARNESS ERROR: the run never got to its injection (or its verdict).
		# A failure either way - and under -SelfTest it is not the failure that
		# was asked for, so it fails the self-test instead of passing it.
		Write-Host "  [FAIL] harness error: $($_.Exception.Message)" -ForegroundColor Red
		$failures++
		$notAsNamed++
		$harnessErrors++
		# Only the process THIS script started. This used to be `Get-Process
		# Dungeon | Stop-Process -Force`, which also killed every other game on
		# the machine - another worktree's, another session's, Michael's own.
		if ($script:proc -and -not $script:proc.HasExited) {
			$script:proc.Kill()
			$script:proc.WaitForExit(5000) | Out-Null
		}
	}
	# Let the previous process release the log file before the next one truncates
	# it - cases run back to back and the loser of that race looks like a load
	# that never happened.
	Start-Sleep -Seconds 2
}

Write-Host ''
# RESULT is the plain verdict on the cases in both modes (FAIL under -SelfTest,
# when the checker works); under -SelfTest the exit code says whether they
# failed EXACTLY as named.
$verdict = if ($failures -eq 0) { 'PASS' } else { 'FAIL' }
Write-Host "healthtest RESULT=$verdict cases=$($cases.Count) failures=$failures harness_errors=$harnessErrors self_test=$([int]$SelfTest.IsPresent)"
if ($SelfTest) {
	if ($notAsNamed -eq 0) {
		Write-Host "SELF-TEST PASSED - all $($cases.Count) case(s) failed on every expectation, and for no other reason" -ForegroundColor Green
	} else {
		Write-Host "SELF-TEST FAILED - $notAsNamed case(s) did not fail exactly as named (a met expectation, a death, a harness error)" -ForegroundColor Red
	}
	exit ([int]($notAsNamed -ne 0))
}
if ($failures -eq 0) {
	Write-Host 'PASS - every failure was caught, recorded and explained' -ForegroundColor Green
} else {
	Write-Host "FAIL - $failures case(s) went unreported ($harnessErrors harness error(s))" -ForegroundColor Red
}
exit ([int]($failures -ne 0))
