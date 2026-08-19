# ============================================================================
# tools\AllocTest.ps1 - the steady-state allocation regression test.
#
# ARCHITECTURE.md says a steady-state frame allocates nothing on the heap. This
# is the run that checks it: launch the game, start a new game, let the world
# settle, then have the in-game guard measure a window of genuinely steady
# frames (Core/AllocTrack + the `alloctest` dev command). Exit code 0 = PASS.
#
#   .\tools\AllocTest.ps1                    # debug build, 10-second window
#   .\tools\AllocTest.ps1 -Seconds 30
#   .\tools\AllocTest.ps1 -Wounded           # the REGENERATING steady state
#   .\tools\AllocTest.ps1 -Config release    # needs -DDN_TRACK_ALLOCS=ON
#
# The party stands still on purpose. Player-driven EVENTS (a bump message, a
# level line) legitimately allocate - allocation proportional to events is not
# what the rule forbids - so the assertion is about frames where nothing
# happened, which is where zero is unambiguously the right answer. Anything
# that does allocate is named with a full call stack in dungeon.log.
#
# WHY -Wounded EXISTS, and it is the same trap this project keeps meeting: a
# FRESH party is at full health, and regeneration only runs BELOW maximum - so
# the default run walks straight past the whole resource tick and reports a
# confident PASS for code it never executed (docs/health-and-healing.md). The
# regenerating party is a real steady state (it is what walking away from a
# fight looks like) and it is where the per-frame skill-XP award lives, so it
# needs its own run. Absent and correct report identically; give the check
# something to be wrong about.
#
# It wounds with a SHORT bleed and lets it expire before measuring, so the DoT's
# own event lines fall outside the window, and it sets the resource practices
# high first - not to make the numbers big, but because a level-up mid-window is
# an event that would allocate legitimately and muddy the verdict. At level 20 a
# practice needs 41 more XP, which ten seconds of regeneration cannot reach.
#
# Every step is driven by what the log actually says rather than by sleeps, so
# a slow cold-cache load stretches the wait instead of failing the run.
#
# ASCII ONLY, deliberately: PowerShell 5.1 reads a BOM-less .ps1 as ANSI, so a
# stray em-dash in a comment is a parse error, not a cosmetic issue.
# ============================================================================
[CmdletBinding()]
param(
	[ValidateSet('debug', 'release')][string]$Config = 'debug',
	[int]$Seconds = 10,
	[int]$LoadTimeoutSec = 240,
	# Measures a WOUNDED party instead of a fresh one. See the note above: a
	# full-health party never runs the regeneration path at all.
	[switch]$Wounded,
	# Checks the CHECKER: makes the game allocate every frame on purpose
	# (`allocpoke`) and passes only if the run comes back FAIL.
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
public class AllocTestWin {
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@

# Waits for a pattern to appear in the log, returning the matching line.
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
		Start-Sleep -Milliseconds 500
	}
	throw "timed out after ${timeoutSec}s waiting for $what"
}

function Send-Key([int]$vk) {
	[AllocTestWin]::PostMessage($hwnd, 0x100, [IntPtr]$vk, [IntPtr]1) | Out-Null
	Start-Sleep -Milliseconds 60
	[AllocTestWin]::PostMessage($hwnd, 0x101, [IntPtr]$vk, [IntPtr][int64]0xC0000001) | Out-Null
	Start-Sleep -Milliseconds 250
}

function Send-Text([string]$text) {
	foreach ($c in $text.ToCharArray()) {
		# WM_CHAR: the console reads typed characters, not virtual keys.
		[AllocTestWin]::PostMessage($hwnd, 0x102, [IntPtr][int]$c, [IntPtr]1) | Out-Null
		Start-Sleep -Milliseconds 40
	}
}

Remove-Item $log -ErrorAction SilentlyContinue
Write-Host "launching $exe"
$proc = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
$hwnd = [IntPtr]::Zero
$code = 1
try {
	# The boot queue's table means the menu is up and the window exists.
	Wait-ForLog '--- load: ' $LoadTimeoutSec 'the boot load' | Out-Null
	$proc.Refresh()
	$hwnd = $proc.MainWindowHandle
	if ($hwnd -eq [IntPtr]::Zero) { throw 'the game has no main window' }

	# Landing page: with no save present the first entry is Start New Game.
	Write-Host 'starting a new game'
	Send-Key 0x0D
	Wait-ForLog 'Game loaded: ' $LoadTimeoutSec 'the dungeon load' | Out-Null

	if ($Wounded) {
		Write-Host 'wounding the party so the regeneration path actually runs'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		# Echo to the log, because a PASS here is only worth anything if the
		# wounding actually happened - and a swallowed keystroke (0xC0 toggles,
		# so one stray press eats every command after it) would leave a run that
		# looks exactly like a clean one. The `party` line below is the evidence.
		Send-Text 'logecho on'; Send-Key 0x0D
		foreach ($m in 0, 1, 2, 3) {
			# Level the practices first (a level-up inside the window is an
			# event, and events are allowed to allocate - see the header).
			foreach ($s in 'constitution', 'conditioning', 'attunement') {
				Send-Text "setskill $m $s 20"; Send-Key 0x0D
			}
			Send-Text "effect bleed $m 6 2"; Send-Key 0x0D
		}
		# Let the bleed run out, so only the recovery is inside the window.
		Start-Sleep -Seconds 4
		Send-Text 'party'; Send-Key 0x0D
		Start-Sleep -Milliseconds 300
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
		# Refuse to report on a party that is not actually hurt. Without this the
		# switch could silently degrade into the plain run it exists to replace.
		$hurt = Select-String -Path $log -Pattern 'hp \d+\.\d+/\d+\.\d+' |
			Where-Object { $_.Line -match 'hp (\d+\.\d+)/(\d+\.\d+)' -and
						   [double]$Matches[1] -lt [double]$Matches[2] }
		if (-not $hurt) { throw 'the party is at full health - the wounding did not land' }
		Write-Host "  wounded: $((($hurt | Select-Object -Last 4).Line -replace '^.*console: ', '') -join '; ')"
	}

	if ($SelfTest) {
		Write-Host 'self-test: arming allocpoke, expecting the run to FAIL'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text "allocpoke $($Seconds * 4 + 60)"
		Send-Key 0x0D
		Start-Sleep -Milliseconds 500
	}

	Write-Host "measuring ${Seconds}s of steady frames"
	Send-Key 0xC0 # `~` opens the console
	Start-Sleep -Milliseconds 500
	Send-Text "alloctest $Seconds"
	Send-Key 0x0D

	# The command closes the console itself, then spends its budget on armed
	# frames only; its own deadline guarantees a line either way.
	$line = Wait-ForLog 'alloctest RESULT=' ($Seconds * 4 + 60) 'the alloctest result'
	$result = if ($line -match 'RESULT=(\w+)') { $Matches[1] } else { 'UNKNOWN' }
	Write-Host ''
	Write-Host $line.Substring($line.IndexOf('alloctest'))

	# A self-test INVERTS the verdict: the guard is working only if the run it
	# was asked to break comes back FAIL.
	$want = if ($SelfTest) { 'FAIL' } else { 'PASS' }
	switch ($result) {
		'PASS' {
			if ($SelfTest) {
				Write-Host 'SELF-TEST FAILED - allocpoke allocated and the guard missed it' -ForegroundColor Red
			} else {
				Write-Host 'PASS - no steady-state frame allocated' -ForegroundColor Green
			}
		}
		'FAIL' {
			if ($SelfTest) {
				Write-Host 'SELF-TEST PASSED - the guard caught the deliberate allocation' -ForegroundColor Green
			} else {
				Write-Host 'FAIL - call sites follow (also in dungeon.log)' -ForegroundColor Red
				Select-String -Path $log -Pattern '^\[warn' | ForEach-Object { Write-Host "  $($_.Line)" }
			}
		}
		default {
			Write-Host "$result - the game never reached a steady frame" -ForegroundColor Yellow
		}
	}
	$code = if ($result -eq $want) { 0 } else { 1 }
} finally {
	if (-not $proc.HasExited) {
		# Quit through the console so shutdown runs (it logs whole-run heap totals).
		if ($hwnd -ne [IntPtr]::Zero) {
			Send-Key 0xC0; Start-Sleep -Milliseconds 400
			Send-Text 'quit'; Send-Key 0x0D
		}
		if (-not $proc.WaitForExit(5000)) { $proc.Kill() }
	}
}
exit $code
