# ============================================================================
# tools\TypingTest.ps1 - no typed character is ever lost or reordered.
#
# Every harness here drives the dev console by posting WM_CHAR, one character
# at a time. Twice on 2026-09-30 one character went missing - `sheet status`
# arrived as `shee status` right after `sheet 1` opened the character sheet,
# `hudpanel layout standard` as `hudpanel layut standard` after a HUD rebuild -
# and the run failed for a reason that had nothing to do with what it measured.
#
# Two ways the input layer could do that, each a phase here:
#
#   FOCUS   - losing focus wiped the typed text. WM_KILLFOCUS clears the input
#             state (a key held when focus leaves would otherwise stay down
#             forever), and the typed characters went with it, so a character
#             already queued in the same frame as the focus change vanished.
#             Any window taking the foreground does it - another harness
#             launching its own game, a notification. This phase posts
#             WM_KILLFOCUS mid-word on every line at the other harnesses' pace.
#   ORDER   - Enter and Backspace were read as key EDGES after the frame's
#             typed text, so a frame holding `...t<Enter>s` ran `...ts` and the
#             next line lost its first letter. A heavy frame (opening the
#             sheet, rebuilding the HUD) is exactly what batches them. This
#             phase types the heavy commands back to back, each starting the
#             instant the previous Enter is posted.
#
# Every line must come back in the log's echo (`console: > <line>`, logecho on)
# EXACTLY as typed, in order. Exit code 0 = PASS.
#
#   .\tools\TypingTest.ps1                 # debug build, 4 rounds of each phase
#   .\tools\TypingTest.ps1 -Rounds 10
#   .\tools\TypingTest.ps1 -SelfTest       # must come back FAIL
#
# -SelfTest checks the checker: `inputpoke` makes the game throw away the text
# typed during its next frames - the very loss this test exists to catch - so a
# run that still PASSes has stopped looking.
#
# The run ends on the Standard layout with the sheet shut whatever happens, and
# repairs settings.ini if it was left on hud_layout=1, so a failure here never
# leaks into whatever runs next.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
param(
	[ValidateSet('debug', 'release')][string]$Config = 'debug',
	[int]$Rounds = 4,
	[int]$LoadTimeoutSec = 120,
	[switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"

. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not $env:DN_HARNESS_MUTED) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }

$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'

if (-not (Test-Path $exe)) { throw "no build at $exe - run build.cmd $Config first" }
if (Get-Process Dungeon -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
	throw 'Dungeon.exe is already running - close it (this test drives its own instance)'
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class TypingTestWin {
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@

$WM_KEYDOWN = 0x100; $WM_KEYUP = 0x101; $WM_CHAR = 0x102; $WM_KILLFOCUS = 0x0008

function Post([uint32]$msg, [int64]$w, [int64]$l) {
	[TypingTestWin]::PostMessage($hwnd, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null
}

# The other harnesses' Send-Key: down, a beat, up, a pause.
function Send-Key([int]$vk) {
	Post $WM_KEYDOWN $vk 1
	Start-Sleep -Milliseconds 60
	Post $WM_KEYUP $vk 0xC0000001
	Start-Sleep -Milliseconds 250
}

function Send-Text([string]$text, [int]$ms = 40, [int]$blurAt = -1) {
	$i = 0
	foreach ($c in $text.ToCharArray()) {
		Post $WM_CHAR ([int]$c) 1
		# Straight after the character, with no pause, so both land in one frame.
		if ($i -eq $blurAt) { Post $WM_KILLFOCUS 0 0 }
		Start-Sleep -Milliseconds $ms
		$i++
	}
}

# FOCUS phase: the other harnesses' pace, focus lost after the third character.
function Send-BlurredLine([string]$text) {
	Send-Text $text 40 2
	Send-Key 0x0D
	$script:typed += $text
}

# ORDER phase: typed fast, Enter posted down+up at once and NO pause after it,
# so the next line's first characters share the frame with this Enter.
function Send-RushedLine([string]$text) {
	Send-Text $text 15
	Post $WM_KEYDOWN 0x0D 1
	Post $WM_KEYUP 0x0D 0xC0000001
	$script:typed += $text
}

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

# Every console echo in the log from line $from on, prefix stripped.
function Get-Echoes([int]$from) {
	$lines = @(Get-Content $log)
	$out = @()
	for ($i = $from; $i -lt $lines.Count; $i++) {
		if ($lines[$i] -match 'console: > (.*)$') { $out += $Matches[1] }
	}
	return ,$out
}

# Compares what was typed with what the console echoed; returns the mismatches.
function Compare-Phase([string]$name, [int]$from) {
	Start-Sleep -Seconds 2
	$echoes = Get-Echoes $from
	$bad = 0
	$n = [Math]::Max($script:typed.Count, $echoes.Count)
	for ($i = 0; $i -lt $n; $i++) {
		$want = if ($i -lt $script:typed.Count) { $script:typed[$i] } else { '<nothing>' }
		$got = if ($i -lt $echoes.Count) { $echoes[$i] } else { '<nothing>' }
		if ($want -ne $got) {
			if ($bad -lt 6) { Write-Host "  line $($i + 1): typed '$want', the console got '$got'" -ForegroundColor Red }
			$bad++
		}
	}
	Write-Host "  $name - $($script:typed.Count) lines typed, $($echoes.Count) echoed, $bad wrong"
	return $bad
}

$heavy = @('sheet 1', 'sheet status', 'sheet off', 'hudpanel layout minimal', 'hudpanel list',
	'hudpanel layout standard', 'hudpanel list')

Remove-Item $log -ErrorAction SilentlyContinue
Write-Host "launching $exe"
$proc = Start-Process -FilePath $exe -WorkingDirectory $bin -ArgumentList '-project', 'dungeon-demo' -PassThru
$hwnd = [IntPtr]::Zero
$code = 1
try {
	Wait-ForLog '--- load: ' $LoadTimeoutSec 'the boot load' | Out-Null
	$proc.Refresh()
	$hwnd = $proc.MainWindowHandle
	if ($hwnd -eq [IntPtr]::Zero) { throw 'the game has no main window' }

	Write-Host 'starting a new game'
	Send-Key 0x0D
	Wait-ForLog '^\[info \] (Level ready: |New game started|Loaded game from )' $LoadTimeoutSec 'the dungeon load' | Out-Null
	Start-Sleep -Seconds 2

	# Wait until the console answers (see AllocTest.ps1: a save can stage a
	# second load, and commands are refused while it runs).
	Send-Key 0xC0
	Start-Sleep -Milliseconds 500
	$answered = $false
	for ($try = 1; $try -le 10 -and -not $answered; $try++) {
		Send-Text 'logecho on'; Send-Key 0x0D
		Start-Sleep -Seconds 2
		$answered = [bool](Select-String -Path $log -Pattern 'console: > logecho on' -EA SilentlyContinue)
	}
	if (-not $answered) { throw 'the console never accepted a command' }

	# Warm the sheet and both layouts once, so no measured line is the very
	# first build of anything.
	foreach ($l in 'sheet 1', 'sheet off', 'hudpanel layout minimal', 'hudpanel layout standard') {
		Send-Text $l; Send-Key 0x0D
		Start-Sleep -Milliseconds 600
	}

	if ($SelfTest) {
		Write-Host 'SELF-TEST: the game drops the text typed during each line' -ForegroundColor Yellow
	}

	Write-Host "FOCUS: $Rounds rounds, focus lost mid-word on every line"
	$from = @(Get-Content $log).Count
	$script:typed = @()
	for ($r = 1; $r -le $Rounds; $r++) {
		foreach ($l in $heavy) {
			# The poke covers the NEXT line (it is typed, and echoed, itself).
			if ($SelfTest) { Send-Text 'inputpoke'; Send-Key 0x0D; $script:typed += 'inputpoke' }
			Send-BlurredLine $l
		}
	}
	$badFocus = Compare-Phase 'FOCUS' $from

	Write-Host "ORDER: $Rounds rounds of the heavy commands back to back"
	$from = @(Get-Content $log).Count
	$script:typed = @()
	for ($r = 1; $r -le $Rounds; $r++) {
		foreach ($l in $heavy) {
			if ($SelfTest) { Send-RushedLine 'inputpoke' }
			Send-RushedLine $l
		}
	}
	$badOrder = Compare-Phase 'ORDER' $from

	$result = if ($badFocus -eq 0 -and $badOrder -eq 0) { 'PASS' } else { 'FAIL' }
	if ($SelfTest) {
		if ($result -eq 'FAIL') {
			Write-Host 'TYPINGTEST SELFTEST PASS - the harness caught the dropped characters' -ForegroundColor Green
			$code = 0
		} else {
			Write-Host 'TYPINGTEST SELFTEST FAIL - text was dropped on purpose and the run still passed' -ForegroundColor Red
			$code = 1
		}
	} else {
		$color = if ($result -eq 'PASS') { 'Green' } else { 'Red' }
		Write-Host "TYPINGTEST $result focus_wrong=$badFocus order_wrong=$badOrder" -ForegroundColor $color
		$code = if ($result -eq 'PASS') { 0 } else { 1 }
	}
} finally {
	if (-not $proc.HasExited) {
		if ($hwnd -ne [IntPtr]::Zero) {
			# Put the layout back however the run went: typed at the plain pace.
			Start-Sleep -Milliseconds 800
			Send-Text 'sheet off'; Send-Key 0x0D
			Send-Text 'hudpanel layout standard'; Send-Key 0x0D
			Start-Sleep -Milliseconds 800
			Send-Text 'quit'; Send-Key 0x0D
		}
		if (-not $proc.WaitForExit(5000)) { $proc.Kill() }
	}
	$ini = Join-Path $bin 'settings.ini'
	if ((Test-Path $ini) -and (Select-String -Path $ini -Pattern '^hud_layout=1' -Quiet)) {
		Write-Host 'settings.ini was left on the Minimal layout - putting it back' -ForegroundColor Yellow
		$text = [IO.File]::ReadAllText($ini) -replace '(?m)^hud_layout=1', 'hud_layout=0'
		[IO.File]::WriteAllText($ini, $text, (New-Object Text.UTF8Encoding $false))
	}
}
exit $code
