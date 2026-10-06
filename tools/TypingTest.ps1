# ============================================================================
# tools\TypingTest.ps1 - no typed character is ever lost or reordered.
#
# Every harness here drives the dev console by posting WM_CHAR, one character
# at a time. Twice on 2026-09-30 one character went missing - `sheet status`
# arrived as `shee status` right after `sheet 1` opened the character sheet,
# `hudpanel layout standard` as `hudpanel layut standard` after a HUD rebuild -
# and the run failed for a reason that had nothing to do with what it measured.
#
# Four ways the input layer could do that, each a phase here:
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
#   TOGGLE  - losing focus also wiped the frame's key PRESS edges, so the
#             console's own key, pressed in the same message pump as a focus
#             change, vanished: the console stayed shut and every later command
#             went nowhere (a harness error in two runs on 2026-10-05, while
#             another session's games took the foreground). This phase reopens
#             the console with the toggle and WM_KILLFOCUS posted back to back,
#             then types a line only an open console can echo.
#   UNICODE - a typed character was cut to the low byte of its UTF-16 unit
#             (code-review C383): u-umlaut became a byte the font drew as '?',
#             Cyrillic became control bytes, and c-caron (U+010D) and
#             C-circumflex (U+0108) became Enter and Backspace. This phase types
#             `echo` lines holding u-umlaut, Cyrillic, both of those, a surrogate
#             pair (U+10348, two WM_CHARs), and Backspaces that must take a
#             whole Cyrillic letter and a whole surrogate pair.
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
# -SelfTest checks the checker: `inputpoke` makes the game throw away the lines
# typed during its next second - the very loss this test exists to catch - and it
# is typed in EVERY phase, so EVERY phase must fail, and for no other reason
# (SpellTest's rule; no save touched): a FAIL from FOCUS alone would say nothing
# of whether ORDER, TOGGLE or UNICODE still count. The poke drops TEXT, not a key edge, so
# TOGGLE fails under it through its echo line; the edge loss itself was watched
# fail on the build before the fix (0 of 3 blurred toggles opened the console).
# Each phase records its count where the verdict reads it (Compare-Phase), so a
# phase the verdict ignores is one the self-test sees pass. The poke drops WHOLE
# lines: it once left fragments, which the console's type-ahead completed into
# commands of their own (`save inputpoke` wrote a save into the shared
# DungeonSaves).
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
Assert-ExeCurrent $exe
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

# UNICODE phase: typed at the plain pace, then Enter. $keys may hold a Backspace
# as [char]8 - sent as the KEY (OnKey's kTypedBack), never as a WM_CHAR, the way
# a keyboard does it. $want is what the echo must say.
function Send-UnicodeLine([string]$keys, [string]$want) {
	$run = ''
	foreach ($c in $keys.ToCharArray()) {
		if ([int]$c -eq 8) {
			if ($run) { Send-Text $run; $run = '' }
			Send-Key 0x08
		} else {
			$run += $c
		}
	}
	if ($run) { Send-Text $run }
	Send-Key $VK_RETURN
	$script:typed += $want
}

# Every console echo in the log from line $from on, prefix stripped. Read as
# UTF-8, which the log is: PS 5.1 would read it as ANSI.
function Get-Echoes([int]$from) {
	$lines = @(Get-Content -Encoding UTF8 $log)
	$out = @()
	for ($i = $from; $i -lt $lines.Count; $i++) {
		if ($lines[$i] -match 'console: > (.*)$') { $out += $Matches[1] }
	}
	return ,$out
}

# A line for the report: anything past ASCII as <U+XXXX>, since this console's
# code page would draw it as '?' and hide exactly the difference being shown.
function Format-Line([string]$s) {
	$sb = New-Object Text.StringBuilder
	foreach ($c in $s.ToCharArray()) {
		if ([int]$c -ge 0x20 -and [int]$c -lt 0x7F) { [void]$sb.Append($c) }
		else { [void]$sb.AppendFormat('<U+{0:X4}>', [int]$c) }
	}
	return $sb.ToString()
}

# The phases, and what each one found: Compare-Phase records here, and the
# verdict and the self-test both read only this - a phase cannot be measured and
# then left out of either.
$phases = @('FOCUS', 'ORDER', 'TOGGLE', 'UNICODE')
$script:phaseWrong = [ordered]@{}

# Compares what was typed with what the console echoed (case and all), and
# records the mismatches as that phase's count.
function Compare-Phase([string]$name, [int]$from) {
	Start-Sleep -Seconds 2
	$echoes = Get-Echoes $from
	$bad = 0
	$n = [Math]::Max($script:typed.Count, $echoes.Count)
	for ($i = 0; $i -lt $n; $i++) {
		$want = if ($i -lt $script:typed.Count) { $script:typed[$i] } else { '<nothing>' }
		$got = if ($i -lt $echoes.Count) { $echoes[$i] } else { '<nothing>' }
		if ($want -cne $got) {
			if ($bad -lt 6) { Write-Host "  line $($i + 1): typed '$(Format-Line $want)', the console got '$(Format-Line $got)'" -ForegroundColor Red }
			$bad++
		}
	}
	Write-Host "  $name - $($script:typed.Count) lines typed, $($echoes.Count) echoed, $bad wrong"
	$script:phaseWrong[$name] = $bad
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
		Write-Host 'SELF-TEST: the game drops the lines typed after each inputpoke' -ForegroundColor Yellow
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
	Compare-Phase 'FOCUS' $from

	Write-Host "ORDER: $Rounds rounds of the heavy commands back to back"
	$from = @(Get-Content $log).Count
	$script:typed = @()
	for ($r = 1; $r -le $Rounds; $r++) {
		foreach ($l in $heavy) {
			if ($SelfTest) { Send-RushedLine 'inputpoke' }
			Send-RushedLine $l
		}
	}
	Compare-Phase 'ORDER' $from

	Write-Host "TOGGLE: $Rounds rounds, the console reopened by a toggle sharing its pump with a focus loss"
	$from = @(Get-Content $log).Count
	$script:typed = @()
	for ($r = 1; $r -le $Rounds; $r++) {
		Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 400   # shut, plainly
		# Down, focus lost, up - back to back, so one pump holds all three.
		Send-Message $WM_KEYDOWN $VK_CONSOLE 1
		Send-Message $WM_KILLFOCUS 0 0
		Send-Message $WM_KEYUP $VK_CONSOLE 0xC0000001
		Start-Sleep -Milliseconds 400
		# Only an OPEN console echoes it; a lost toggle sends it nowhere.
		if ($SelfTest) { Send-Text 'inputpoke'; Send-Key 0x0D; $script:typed += 'inputpoke' }
		Send-Text "echo toggle $r"; Send-Key $VK_RETURN
		$script:typed += "echo toggle $r"
	}
	Compare-Phase 'TOGGLE' $from

	# Built from code points: this file is ASCII. [char]8 is a Backspace key.
	$uu = [string][char]0x00FC                                           # u-umlaut
	$zhuk = (@(0x0416, 0x0443, 0x043A) | ForEach-Object { [char]$_ }) -join ''  # "zhuk", Cyrillic
	$zhe = [string][char]0x0416
	$ccaron = [string][char]0x010D  # its low byte is 0x0D, Enter
	$ccirc = [string][char]0x0108   # its low byte is 0x08, Backspace
	$hwair = [char]::ConvertFromUtf32(0x10348)  # past U+FFFF: a surrogate pair
	$bs = [string][char]8
	$unicode = @(
		@("echo gr${uu}n ${zhuk}", "echo gr${uu}n ${zhuk}"),
		@("echo ${ccaron}ech ${ccirc}a ${ccaron}", "echo ${ccaron}ech ${ccirc}a ${ccaron}"),
		@("echo ${hwair} hwair", "echo ${hwair} hwair"),
		@("echo ab${zhe}${bs}c", 'echo abc'),
		@("echo ${zhuk}${hwair}${bs}${bs}ok", "echo $($zhuk.Substring(0, 2))ok")
	)
	Write-Host "UNICODE: $Rounds rounds of u-umlaut, Cyrillic, U+010D, U+0108, a surrogate pair and Backspace"
	$from = @(Get-Content $log).Count
	$script:typed = @()
	for ($r = 1; $r -le $Rounds; $r++) {
		foreach ($pair in $unicode) {
			if ($SelfTest) { Send-Text 'inputpoke'; Send-Key 0x0D; $script:typed += 'inputpoke' }
			Send-UnicodeLine $pair[0] $pair[1]
		}
	}
	Compare-Phase 'UNICODE' $from

	# The run must not have touched a save: no Continue, no save written. Read
	# from THIS game's log, so another session writing to the shared
	# DungeonSaves meanwhile cannot fail it.
	$saveLines = @(Select-String -Path $log -Pattern 'Loaded game from |Saved game to ' -EA SilentlyContinue)
	foreach ($s in $saveLines) { Write-Host "  the run touched a save: $($s.Line)" -ForegroundColor Red }

	# Per phase, from the record: a phase never judged is a failure, not a pass.
	$unjudged = @($phases | Where-Object { -not $script:phaseWrong.Contains($_) })
	$clean = @($phases | Where-Object { $script:phaseWrong.Contains($_) -and $script:phaseWrong[$_] -eq 0 })
	$counts = (@($phases | ForEach-Object {
		$n = if ($script:phaseWrong.Contains($_)) { $script:phaseWrong[$_] } else { 'never' }
		"$($_.ToLower())_wrong=$n"
	}) + "saves_touched=$($saveLines.Count)") -join ' '
	foreach ($p in $unjudged) { Write-Host "  $p was never judged" -ForegroundColor Red }

	$result = if ($unjudged.Count -eq 0 -and $clean.Count -eq $phases.Count -and
				  $saveLines.Count -eq 0) { 'PASS' } else { 'FAIL' }
	if ($SelfTest) {
		# SpellTest's rule: the poke reached every phase, so every phase must have
		# failed - and nothing else may have (a touched save is the run going
		# wrong, not the poke being caught).
		foreach ($p in $clean) { Write-Host "  self-test: $p passed with its text dropped" -ForegroundColor Red }
		if ($unjudged.Count -eq 0 -and $clean.Count -eq 0 -and $saveLines.Count -eq 0) {
			Write-Host "TYPINGTEST SELFTEST PASS $counts - every phase caught the dropped characters" -ForegroundColor Green
			$code = 0
		} else {
			Write-Host "TYPINGTEST SELFTEST FAIL $counts - text was dropped in every phase and not every phase failed for it alone" -ForegroundColor Red
			$code = 1
		}
	} else {
		$color = if ($result -eq 'PASS') { 'Green' } else { 'Red' }
		Write-Host "TYPINGTEST $result $counts" -ForegroundColor $color
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
