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
# EXACTLY as typed, in order. And the run must neither load nor write a save:
# it starts through the console's `newgame`, never Enter on the title screen,
# which is Continue on whatever save is newest in the shared DungeonSaves.
# Exit code 0 = PASS.
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

# Muted for the whole run, restored however it ends (tools\HarnessAudio.ps1).
. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not (Test-HarnessMuted $bin)) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }
# Launch, input and log waits: the one shared copy (tools\HarnessGame.ps1).
. (Join-Path $PSScriptRoot 'HarnessGame.ps1')
$HarnessCharMs = 40

$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'

if (-not (Test-Path $exe)) { throw "no build at $exe - run build.cmd $Config first" }
Assert-NotRunning $exe

# FOCUS phase: the other harnesses' pace, focus lost after the third character.
function Send-BlurredLine([string]$text) {
	Send-Text $text 40 -BlurAt 2
	Send-Key $VK_RETURN
	$script:typed += $text
}

# ORDER phase: typed fast, Enter posted down+up at once and NO pause after it,
# so the next line's first characters share the frame with this Enter.
function Send-RushedLine([string]$text) {
	Send-Text $text 15
	Send-Message $WM_KEYDOWN $VK_RETURN 1
	Send-Message $WM_KEYUP $VK_RETURN 0xC0000001
	$script:typed += $text
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

$proc = $null
$hwnd = [IntPtr]::Zero
$code = 1
try {
	Start-HarnessGame $exe $bin $log $LoadTimeoutSec
	# Through the console's `newgame`: Enter on the title screen is Continue on
	# the newest shared save whenever one exists. Leaves logecho on.
	Start-NewGame $LoadTimeoutSec | Out-Null
	Send-Key $VK_CONSOLE
	Start-Sleep -Milliseconds 500

	# Warm the sheet and both layouts once, so no measured line is the very
	# first build of anything.
	foreach ($l in 'sheet 1', 'sheet off', 'hudpanel layout minimal', 'hudpanel layout standard') {
		Send-Text $l; Send-Key $VK_RETURN
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

	# The run must not have touched a save: no Continue, no save written. Read
	# from THIS game's log, so another session writing to the shared
	# DungeonSaves meanwhile cannot fail it.
	$saveLines = @(Select-String -Path $log -Pattern 'Loaded game from |Saved game to ' -EA SilentlyContinue)
	foreach ($s in $saveLines) { Write-Host "  the run touched a save: $($s.Line)" -ForegroundColor Red }

	$result = if ($badFocus -eq 0 -and $badOrder -eq 0 -and $saveLines.Count -eq 0) { 'PASS' } else { 'FAIL' }
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
		Write-Host "TYPINGTEST $result focus_wrong=$badFocus order_wrong=$badOrder saves_touched=$($saveLines.Count)" -ForegroundColor $color
		$code = if ($result -eq 'PASS') { 0 } else { 1 }
	}
} finally {
	if ($proc -and -not $proc.HasExited -and $hwnd -ne [IntPtr]::Zero) {
		# Put the layout back however the run went: typed at the plain pace.
		Start-Sleep -Milliseconds 800
		Send-Text 'sheet off'; Send-Key $VK_RETURN
		Send-Text 'hudpanel layout standard'; Send-Key $VK_RETURN
		Start-Sleep -Milliseconds 800
	}
	Stop-HarnessGame
	$ini = Join-Path $bin 'settings.ini'
	if ((Test-Path $ini) -and (Select-String -Path $ini -Pattern '^hud_layout=1' -Quiet)) {
		Write-Host 'settings.ini was left on the Minimal layout - putting it back' -ForegroundColor Yellow
		$text = [IO.File]::ReadAllText($ini) -replace '(?m)^hud_layout=1', 'hud_layout=0'
		[IO.File]::WriteAllText($ini, $text, (New-Object Text.UTF8Encoding $false))
	}
}
exit $code
