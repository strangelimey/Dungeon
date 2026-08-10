# ============================================================================
# tools\InGameTest.ps1 - the audits that need a running game.
#
# Two checks that were built but never automated, sharing one launch because
# the expensive part is loading the dungeon, not running them:
#
#   levelcheck  every level file is present and every model a catalog type
#               names is installed. The baked pool is GITIGNORED, so a fresh
#               clone or a stale worktree provision has entries whose assets are
#               absent - and a missing model is a LoadModelOrDie that takes the
#               process down at level load, possibly on a level nobody has
#               visited in weeks.
#
#   uioverlap   CLAUDE.md says RUN IT AFTER TOUCHING ANY SCREEN, and the one
#               manual sweep found four defects nobody had reported. The command
#               was written to log its findings with a label precisely so a
#               scripted sweep would be collectable, and then nothing ever swept.
#
# COVERAGE IS SELF-VERIFYING. Each screen's audit writes its LABEL to the log,
# so a step that silently failed to open its screen shows up as a missing label
# rather than as a clean pass. Screens are listed below; anything not in that
# list is named in the output rather than left to be assumed.
#
#   .\tools\InGameTest.ps1
#   .\tools\InGameTest.ps1 -SelfTest     # expects FAIL (see below)
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[ValidateSet('debug', 'release')][string]$Config = 'debug',
	[int]$LoadTimeoutSec = 240,
	# Checks the CHECKER: runs the sweep but asks for a label that is never
	# emitted, so the coverage assertion must FAIL. Proves the log-parse is
	# really looking rather than passing on an empty result.
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
public class InGameWin {
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
	[InGameWin]::PostMessage($hwnd, 0x100, [IntPtr]$vk, [IntPtr]1) | Out-Null
	Start-Sleep -Milliseconds 60
	[InGameWin]::PostMessage($hwnd, 0x101, [IntPtr]$vk, [IntPtr][int64]0xC0000001) | Out-Null
	Start-Sleep -Milliseconds 250
}
function Send-Text([string]$t) {
	foreach ($c in $t.ToCharArray()) {
		[InGameWin]::PostMessage($hwnd, 0x102, [IntPtr][int]$c, [IntPtr]1) | Out-Null
		Start-Sleep -Milliseconds 30
	}
}
# The console STAYS OPEN between commands - a second toggle would close it and
# send the next line to the game as movement keys.
function Open-Console { Send-Key 0xC0; Start-Sleep -Milliseconds 600 }
function Run-Cmd([string]$c) { Send-Text $c; Send-Key 0x0D; Start-Sleep -Seconds 2 }

# The screens this sweeps, and how each is reached. Keyboard only: scripted
# mouse clicks proved unreliable against a layout whose rows move.
#
# `viaConsole` is the distinction that matters. An open console CAPTURES
# keystrokes - the first version of this sent Esc and M straight into the
# console's input line and swept nothing but the HUD three times over, which is
# precisely the silent-coverage failure the label check below exists to catch
# (and did). A screen opened by a KEY therefore needs the console closed first
# and reopened to type into; a screen opened by a COMMAND needs it open.
$screens = @(
	@{ label = 'sweep_hud';    viaConsole = $true;  open = { };                 close = { } },
	@{ label = 'sweep_paused'; viaConsole = $false; open = { Send-Key 0x1B };    close = { Send-Key 0x1B } },
	@{ label = 'sweep_map';    viaConsole = $false; open = { Send-Key 0x4D };    close = { Send-Key 0x4D } },
	@{ label = 'sweep_editor'; viaConsole = $true;  open = { Run-Cmd 'editor' }; close = { Run-Cmd 'editor off' } }
)
# NOT swept, and named rather than left to be assumed: the settings page and the
# character sheet. Settings is reached by menu navigation whose entry order
# shifts with whether a save exists, and the sheet opens only by clicking a
# portrait. Both want a click, and a scripted click against a moving layout is
# how a sweep starts silently auditing the wrong screen.
$notSwept = 'settings page, character sheet (both need a mouse click)'

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

	# Landing page: with no save present the first entry is Start New Game.
	# Retried, because one dropped PostMessage keystroke should not fail a run.
	$loaded = $false
	for ($try = 1; $try -le 3 -and -not $loaded; $try++) {
		Send-Key 0x0D
		$d2 = (Get-Date).AddSeconds(60)
		while ((Get-Date) -lt $d2) {
			if ($proc.HasExited) { throw 'the game exited during the dungeon load' }
			if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'Game loaded: ' -EA SilentlyContinue)) {
				$loaded = $true; break
			}
			Start-Sleep -Milliseconds 400
		}
	}
	if (-not $loaded) { throw 'the dungeon never loaded' }
	Start-Sleep -Seconds 2

	Open-Console
	Run-Cmd 'levelcheck'

	# Invariant across the loop: the console is OPEN on entry and on exit.
	foreach ($s in $screens) {
		$label = if ($SelfTest) { 'sweep_never_emitted' } else { $s.label }
		if ($s.viaConsole) {
			& $s.open
			Run-Cmd "uioverlap $label"
			& $s.close
		} else {
			Send-Key 0xC0; Start-Sleep -Milliseconds 400   # close: let the key reach the game
			& $s.open
			Start-Sleep -Milliseconds 600
			Open-Console
			# The console sits OUTSIDE the widget tree by design, so the screen
			# behind it is still what gets audited.
			Run-Cmd "uioverlap $label"
			Send-Key 0xC0; Start-Sleep -Milliseconds 400
			& $s.close
			Start-Sleep -Milliseconds 600
			Open-Console
		}
	}
} finally {
	if (-not $proc.HasExited) {
		if ($hwnd -ne [IntPtr]::Zero) { Send-Text 'quit'; Send-Key 0x0D }
		if (-not $proc.WaitForExit(8000)) { $proc.Kill() }
	}
}

# --- the verdict, read from the log -----------------------------------------
$lines = if (Test-Path $log) { Get-Content $log } else { @() }
$text = $lines -join "`n"
$failures = 0

Write-Host ''
$lc = $lines | Select-String 'levelcheck RESULT=' | Select-Object -Last 1
if ($lc) {
	Write-Host "  $($lc.Line.Substring($lc.Line.IndexOf('levelcheck')))"
	if ($lc.Line -notmatch 'RESULT=PASS') { $failures++ }
} else {
	Write-Host '  [FAIL] levelcheck never reported' -ForegroundColor Red
	$failures++
}

# Coverage first: a screen whose label never reached the log was never audited,
# and a sweep that quietly skipped half the screens must not read as clean.
foreach ($s in $screens) {
	if ($text -match [regex]::Escape($s.label)) {
		Write-Host "  [ok  ] swept $($s.label)"
	} else {
		Write-Host "  [FAIL] $($s.label) never reached the log - screen not audited" -ForegroundColor Red
		$failures++
	}
}

# Then the findings themselves.
$dirty = $lines | Select-String 'uioverlap:' | Where-Object { $_.Line -notmatch 'clean' }
if ($dirty) {
	Write-Host "  [FAIL] uioverlap found overlaps:" -ForegroundColor Red
	$dirty | ForEach-Object { Write-Host "     $($_.Line)" }
	$failures++
} else {
	Write-Host '  [ok  ] every swept screen kept its widgets to their own areas'
}

Write-Host ''
$verdict = if ($failures -eq 0) { 'PASS' } else { 'FAIL' }
$want = if ($SelfTest) { 'FAIL' } else { 'PASS' }
Write-Host "ingametest RESULT=$verdict failures=$failures self_test=$([int]$SelfTest.IsPresent)"
if ($SelfTest) {
	if ($verdict -eq 'FAIL') {
		Write-Host 'SELF-TEST PASSED - the coverage check reports a missing sweep' -ForegroundColor Green
	} else {
		Write-Host 'SELF-TEST FAILED - it passed with no screen actually audited' -ForegroundColor Red
	}
} elseif ($verdict -eq 'PASS') {
	Write-Host 'PASS' -ForegroundColor Green
} else {
	Write-Host "FAIL - $failures problem(s)" -ForegroundColor Red
}
Write-Host "NOT swept: $notSwept"
exit ([int]($verdict -ne $want))
