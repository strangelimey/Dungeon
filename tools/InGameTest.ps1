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

# The console STAYS OPEN between commands - a second toggle would close it and
# send the next line to the game as movement keys.
function Open-Console { Send-Key 0xC0; Start-Sleep -Milliseconds 600 }
function Run-Cmd([string]$c) { Send-Text $c; Send-Key 0x0D; Start-Sleep -Seconds 2 }
# A new game with a CREATED party (party creation, docs/party-creation-plan.md
# phase 2), waited out: `newparty` restarts the game, and a command typed while
# its level loads is refused. logecho first, so the command's own line ("new
# game with a party of N") reaches the log for the verdict below.
function Run-Party([string]$spec) {
	Run-Cmd 'logecho on'
	$before = @(Select-String -Path $log -Pattern 'New game started' -EA SilentlyContinue).Count
	Run-Cmd "newparty $spec"
	$deadline = (Get-Date).AddSeconds($LoadTimeoutSec)
	while ((Get-Date) -lt $deadline -and
		   @(Select-String -Path $log -Pattern 'New game started' -EA SilentlyContinue).Count -le $before) {
		if ($proc.HasExited) { throw 'the game exited during newparty' }
		Start-Sleep -Milliseconds 400
	}
	Start-Sleep -Seconds 2
	# Off again: an echoed `uioverlap: auditing ...` line reads as a finding
	# to the verdict below, which takes any uioverlap line that is not "clean".
	Run-Cmd 'logecho off'
}

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
	@{ label = 'sweep_editor'; viaConsole = $true;  open = { Run-Cmd 'editor' }; close = { Run-Cmd 'editor off' } },
	# The character SHEET, which was on the not-swept list purely because it
	# opened only by clicking a portrait. It carries more hand-laid-out content
	# than any other screen, so being the one nobody audited was exactly
	# backwards; the `sheet` dev command opens it through the same entry point
	# the click uses, and it is swept like everything else now.
	@{ label = 'sweep_sheet';  viaConsole = $true;  open = { Run-Cmd 'sheet 0' }; close = { Run-Cmd 'sheet off' } },
	# The portrait picker over the sheet (docs/portraits-plan.md), opened the way
	# the sheet's "Change portrait" button opens it.
	@{ label = 'sweep_portraits'; viaConsole = $true
	   open = { Run-Cmd 'sheet 0'; Run-Cmd 'portrait picker 0' }
	   close = { Run-Cmd 'portrait picker off'; Run-Cmd 'sheet off' } },
	# The floating HUD's other shapes (docs/ui-panels-plan.md P3b/P4): the party
	# inventory WINDOW, and the MINIMAL layout - the party bar and the hands
	# folded into one card per member, with the Magic dock (a member knows a
	# symbol first, or it is not shown) moved to the left column.
	@{ label = 'sweep_inventory'; viaConsole = $true; open = { Run-Cmd 'inventory' }; close = { Run-Cmd 'inventory off' } },
	@{ label = 'sweep_minimal'; viaConsole = $true
	   open = { Run-Cmd 'learn 0 fire'; Run-Cmd 'hudpanel layout minimal' }
	   close = { Run-Cmd 'hudpanel layout standard' } },
	# The level generator's dialog in BOTH modes (docs/level-building.md P1):
	# CREATE (the toolbar's [+]) and REGENERATE. Opened only - nothing is
	# generated, so the sweep writes no level.
	@{ label = 'sweep_gencreate'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'generate dialog new' }
	   close = { Run-Cmd 'generate dialog off'; Run-Cmd 'editor off' } },
	@{ label = 'sweep_genregen'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'generate dialog' }
	   close = { Run-Cmd 'generate dialog off'; Run-Cmd 'editor off' } },
	# ...and its OTHER tabs: a sweep only sees the tab that is showing, and the
	# checkbox and ramp slider live on the last one.
	@{ label = 'sweep_gencomplexity'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'generate dialog'; Run-Cmd 'generate dialog tab 1' }
	   close = { Run-Cmd 'generate dialog off'; Run-Cmd 'editor off' } },
	@{ label = 'sweep_genpopulation'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'generate dialog'; Run-Cmd 'generate dialog tab 2' }
	   close = { Run-Cmd 'generate dialog off'; Run-Cmd 'editor off' } },
	# The Style tab (tool-refinement Phase 7), with a style picked.
	@{ label = 'sweep_genstyle'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'generate dialog'; Run-Cmd 'generate dialog style small_crypt'; Run-Cmd 'generate dialog tab 3' }
	   close = { Run-Cmd 'generate dialog off'; Run-Cmd 'editor off' } },
	# The STAIR inspector, both layouts: crypt1's stair down at 1,1 (destination
	# and Go to) and its exit at 7,8 (one "leads out to" line).
	# `editor inspect` is what a right-click on the square does. It goes to crypt1
	# FIRST, rather than trusting where the game started: the run used to press
	# Enter on the landing page, which was Continue whenever a save existed and
	# loaded whatever level it named (an eval save puts it on eval_arena), and the
	# first version of this swept two empty squares of the arena - clean, and
	# vacuous. The verdict below demands the dialog really opened, both times.
	# FROZEN, because crypt1 has a monster and the world simulates under the
	# editor: unfrozen, it killed the party mid-sweep, and every screen after
	# this one was reached from the title screen instead.
	@{ label = 'sweep_stair'; viaConsole = $true
	   open = { Run-Cmd 'freeze on'; Run-Cmd 'goto crypt1'; Run-Cmd 'editor inspect 1 1' }
	   close = { Run-Cmd 'editor inspect off'; Run-Cmd 'editor off' } },
	@{ label = 'sweep_stairexit'; viaConsole = $true
	   open = { Run-Cmd 'editor inspect 7 8' }
	   close = { Run-Cmd 'editor inspect off'; Run-Cmd 'editor off' } },
	# The WORLD screen's two dialogs, LAST because reaching them leaves the
	# dungeon: each one opens only on the world map (the one state that routes
	# input to it), so the sweep goes there and comes back.
	@{ label = 'sweep_worldsettings'; viaConsole = $true
	   open = { Run-Cmd 'worldmap on'; Run-Cmd 'worldsettings' }
	   close = { Run-Cmd 'worldsettings off'; Run-Cmd 'worldmap off' } },
	@{ label = 'sweep_worlds'; viaConsole = $true
	   open = { Run-Cmd 'worldmap on'; Run-Cmd 'worlds dialog' }
	   close = { Run-Cmd 'worlds dialog off'; Run-Cmd 'worldmap off' } },
	# The delete CONFIRMATION, which needs a world that may be deleted - so it
	# makes its own and deletes it on the way out, rather than depending on
	# whatever worlds happen to be on this machine.
	@{ label = 'sweep_worlddelete'; viaConsole = $true
	   open = { Run-Cmd 'worldmap on'; Run-Cmd 'worlds new wt_sweep'; Run-Cmd 'worlds dialog delete wt_sweep' }
	   close = { Run-Cmd 'worlds dialog off'; Run-Cmd 'worlds delete wt_sweep wt_sweep'; Run-Cmd 'worldmap off' } },
	# A DUNGEON's delete confirmation (W10), inside the type editor. The demo's
	# own dungeons are both refused (the opening, the party), so it makes an
	# empty one of its own and deletes it on the way out, through the same rule.
	@{ label = 'sweep_dungeondelete'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'newtype dungeons'; Run-Cmd 'dungeons dialog dungeon1'; Run-Cmd 'dungeons dialog delete' }
	   close = { Run-Cmd 'dungeons dialog off'; Run-Cmd 'dungeons delete dungeon1 dungeon1'; Run-Cmd 'editor off' } },
	# The NEW WORLD dialog (editor-updates P4), from the level editor's toolbar:
	# with "Copy one level" picked, so its level dropdown row is the live one...
	@{ label = 'sweep_newworld'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'worlds newdialog source level crypt1' }
	   close = { Run-Cmd 'worlds newdialog off'; Run-Cmd 'editor off' } },
	# ...and after a Create, when "Switch now" joins the footer. It makes a
	# blank world of its own and deletes it on the way out.
	# ...with the WIZARD picked (P5), when its four rows join a taller card...
	@{ label = 'sweep_newworldwizard'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'worlds newdialog source wizard' }
	   close = { Run-Cmd 'worlds newdialog off'; Run-Cmd 'editor off' } },
	# ...with BLANK picked, when the Style row (Phase 7) joins the plain card.
	@{ label = 'sweep_newworldblank'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'worlds newdialog source blank'; Run-Cmd 'worlds newdialog style dirt_tunnels' }
	   close = { Run-Cmd 'worlds newdialog off'; Run-Cmd 'editor off' } },
	@{ label = 'sweep_newworldmade'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'worlds newdialog create wt_nwsweep' }
	   close = { Run-Cmd 'worlds newdialog off'; Run-Cmd 'worlds delete wt_nwsweep wt_nwsweep'; Run-Cmd 'editor off' } },
	# SHORT PARTIES (party creation phase 2), LAST because each one restarts the
	# game: three members (the bar keeps four slots, the hands go 2+1, the party
	# window's fourth card goes inert) and then one. The verdict demands both
	# parties were really built.
	@{ label = 'sweep_party3'; viaConsole = $true
	   open = { Run-Party 'premade=0 | premade=1 | premade=2' }; close = { } },
	@{ label = 'sweep_party3sheet'; viaConsole = $true
	   open = { Run-Cmd 'sheet 2' }; close = { Run-Cmd 'sheet off' } },
	@{ label = 'sweep_party3inventory'; viaConsole = $true
	   open = { Run-Cmd 'inventory' }; close = { Run-Cmd 'inventory off' } },
	@{ label = 'sweep_party3minimal'; viaConsole = $true
	   open = { Run-Cmd 'learn 0 fire'; Run-Cmd 'hudpanel layout minimal' }
	   close = { Run-Cmd 'hudpanel layout standard' } },
	@{ label = 'sweep_party1'; viaConsole = $true
	   open = { Run-Party 'premade=0' }; close = { } },
	@{ label = 'sweep_party1minimal'; viaConsole = $true
	   open = { Run-Cmd 'learn 0 fire'; Run-Cmd 'hudpanel layout minimal' }
	   close = { Run-Cmd 'hudpanel layout standard' } }
)
# The screens swept on the TITLE, before the game starts - the party creation
# page (phase 3), opened by its dev twin, which drives the page's own code.
# Each opens on top of the one before; the run backs out after the last.
$titleScreens = @(
	@{ label = 'sweep_partycreation'; open = { Run-Cmd 'partypage open' } },
	@{ label = 'sweep_partydefault';  open = { Run-Cmd 'partypage default' } },
	@{ label = 'sweep_partypicker';   open = { Run-Cmd 'partypage picker' } }
)
# NOT swept, and named rather than left to be assumed. The settings page is
# reached by menu navigation whose entry order shifts with whether a save
# exists, and a scripted click against a moving layout is how a sweep starts
# silently auditing the wrong screen.
#
# And a LIMIT worth stating even for the screen that IS swept now: uioverlap
# audits the WIDGET TREE, while the sheet's stat bars and skill rows are drawn
# directly. A clean sweep there says its tree is sound, not that every painted
# bar fits inside the panel.
$notSwept = 'settings page (needs a mouse click); the sheet''s hand-drawn bars (uioverlap sees widgets, not direct draws)'

$proc = $null
$hwnd = [IntPtr]::Zero
try {
	Start-HarnessGame $exe $bin $log $LoadTimeoutSec

	# THE PARTY CREATION PAGE (party creation phase 3), on the title screen
	# where it lives: a new member, the default four, and the face picker over
	# the page. The console must answer first (retried until a NEW echo lands);
	# then echo off for the audits (see below) - the verdict reads the page's
	# own log line, not an echo.
	Open-Console
	if (-not (Wait-ConsoleReady)) { throw 'the console never accepted a command on the title screen' }
	Run-Cmd 'logecho off'
	foreach ($s in $titleScreens) {
		$label = if ($SelfTest) { 'sweep_never_emitted' } else { $s.label }
		& $s.open
		Run-Cmd "uioverlap $label"
	}
	Run-Cmd 'partypage back'
	Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 400   # closed: Start-NewGame opens it

	# Through the console's `newgame`, never the landing page (Continue on the
	# newest shared save), waited out to the level (tools\HarnessGame.ps1).
	Start-NewGame $LoadTimeoutSec | Out-Null
	Start-Sleep -Seconds 1

	Open-Console
	# logecho OFF again before the sweep: echoed console output would put every
	# `uioverlap: auditing...` line into the log, and the verdict reads any
	# uioverlap line that is not "clean" as a finding. Retried until its own
	# echo lands (the line is echoed before the command turns echoing off).
	if (-not (Wait-ConsoleReady 'logecho off' 'console: > logecho off\s*$')) {
		throw 'the console never accepted a command after the new game'
	}
	# Retried until it answers, counting only a NEW report.
	Wait-ConsoleReady 'levelcheck' 'levelcheck RESULT=' 10 4000 | Out-Null

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
	Stop-HarnessGame 8000
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
foreach ($s in @($titleScreens) + @($screens)) {
	if ($text -match [regex]::Escape($s.label)) {
		Write-Host "  [ok  ] swept $($s.label)"
	} else {
		Write-Host "  [FAIL] $($s.label) never reached the log - screen not audited" -ForegroundColor Red
		$failures++
	}
}

# A dialog screen counts only if the dialog OPENED: a sweep of the empty editor
# behind it is clean for the wrong reason.
$stairs = @($lines | Select-String 'editor inspect: stair')
if ($stairs.Count -ge 2) {
	Write-Host '  [ok  ] the stair inspector opened for both sweeps'
} else {
	Write-Host "  [FAIL] the stair inspector opened $($stairs.Count) of 2 times - its sweep audited an empty editor" -ForegroundColor Red
	$lines | Select-String 'editor inspect: ' | ForEach-Object { Write-Host "     $($_.Line)" }
	$failures++
}
# Likewise the portrait picker: without it the sweep audited the sheet beneath.
# Twice: over the sheet, and over the party creation page.
$pickers = @($lines | Select-String 'portrait picker: open for ').Count
if ($pickers -ge 2) {
	Write-Host '  [ok  ] the portrait picker opened for both its sweeps'
} else {
	Write-Host "  [FAIL] the portrait picker opened $pickers of 2 times - a sweep audited the page beneath" -ForegroundColor Red
	$failures++
}
# And the party creation page, or its sweeps audited the title screen.
if ($lines | Select-String 'party creation: the page opens for ') {
	Write-Host '  [ok  ] the party creation page opened for its sweeps'
} else {
	Write-Host '  [FAIL] the party creation page never opened - its sweeps audited the title' -ForegroundColor Red
	$failures++
}

# And the short-party screens only if `newparty` built them: a refused one
# leaves the default four up, and their sweep is the HUD swept again.
foreach ($n in 3, 1) {
	if ($lines | Select-String "console: new game with a party of $n\b") {
		Write-Host "  [ok  ] a party of $n was built for its sweeps"
	} else {
		Write-Host "  [FAIL] no party of $n was built - its sweeps audited the default four" -ForegroundColor Red
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
