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
#   backdrop    (code-review C365) what Render drew behind the pause menu and
#               the sheet opened from the world map: the world map, with no 3D
#               pass - not the parked dungeon. A drawing fact, so only a run
#               that renders can check it; it is those two screens' status.
#
# COVERAGE IS SELF-VERIFYING, and a label alone does not verify it: the game
# logs `uioverlap [<label>] --- state <s>` the moment the command runs, whether
# or not the screen opened (code-review C427). So each screen is judged on four
# things, all read from the log:
#
#   label    exactly one `uioverlap [<label>] ---` header, matched EXACTLY - as
#            a bare substring, sweep_worlds was satisfied by sweep_worldsettings
#   status   its open step's own STATUS line (a command's report of what is now
#            on screen: `generate dialog: regenerate tab 2 ...`, `sheet: open
#            member 0 ...`) logged between this screen's `sweep-open` marker and
#            its header - a refused open step leaves the screen beneath, and that
#            audit is clean for the wrong reason. Every screen MUST name one: a
#            row with no `status` is refused before the game launches
#   state    the app state the header names (menu, playing, paused, sheet,
#            worldmap) - a party wiped mid-sweep audits the title from then on
#   audit    the audit's own summary after the header (clean, or its findings)
#
# and the run ends by requiring `state` to be playing or worldmap. Logecho stays
# ON for the whole sweep, since the status lines are console output; the verdict
# reads only the lines the game logs ITSELF (`[info ] uioverlap...`), never the
# mirrored `console: uioverlap: auditing...`.
#
#   .\tools\InGameTest.ps1
#   .\tools\InGameTest.ps1 -SelfTest     # see below; exit 0 = the checker works
#
# -SELFTEST is a real sweep with the real labels and TWO named faults injected,
# and it passes only if exactly those two checks fail and everything else passes
# (SpellTest's rule, code-review C419): sweep_gencomplexity's open step asks for
# a tab the dialog does not have, which is REFUSED, so its status line never
# lands; and sweep_worlds is opened but never audited, so its label is missing
# while its longer sibling sweep_worldsettings is present.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[ValidateSet('debug', 'release')][string]$Config = 'debug',
	[int]$LoadTimeoutSec = 240,
	# Checks the CHECKER: two faults injected into a real sweep (above), and
	# exactly those two checks must fail.
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
function Open-Console { Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 600 }
function Run-Cmd([string]$c) { Send-Text $c; Send-Key $VK_RETURN; Start-Sleep -Seconds 2 }
# A marker or a probe: it only prints, so there is nothing to wait out.
function Run-Quick([string]$c) { Send-Text $c; Send-Key $VK_RETURN; Start-Sleep -Milliseconds 400 }
# A new game with a CREATED party (party creation, docs/party-creation-plan.md
# phase 2), waited out: `newparty` restarts the game, and a command typed while
# its level loads is refused - so wait for the level, then for the console.
function Run-Party([string]$spec) {
	$ready = '^\[info \] (Level ready: |New game started)'
	$before = Get-LogMatchCount $ready
	Run-Cmd "newparty $spec"
	Wait-ForNewLog $ready $LoadTimeoutSec "the new game of '$spec'" $before | Out-Null
	if (-not (Wait-ConsoleReady)) { throw "the console never answered after newparty $spec" }
}

# Status-line patterns: Answer = a console answer (mirrored by logecho, so never
# the `console: > ...` echo of the typed line), Logged = a line the game logs
# itself. An empty one would match any line, so it is refused.
function Answer([string]$s) {
	if (-not $s.Trim()) { throw 'an empty status pattern matches any console line' }
	"^\[info \] console: $s"
}
function Logged([string]$s) {
	if (-not $s.Trim()) { throw 'an empty status pattern matches any line' }
	"^\[info \] $s"
}

# The id sweep_longtitle gives a dungeon: 32 characters, the type editor's own
# rename limit (TypeEditorDialog's name field maxLength).
$longTitleId = 'sweep_long_dungeon_identifier_32'

# The game window's CLIENT area, read and resized from here as dragging its edge
# would resize it (WM_SIZE: the swapchain and the fonts follow). For the hand
# menu sweep's sub-900p window. ASYNC, so a game that stopped pumping messages
# cannot hang the run; the pause lets the fonts settle (0.25 s) before the next
# command. Its own type, not HarnessWin, which is shared with other harnesses.
if (-not ([System.Management.Automation.PSTypeName]'IgtWin').Type) {
	Add-Type @'
using System;
using System.Runtime.InteropServices;
public class IgtWin {
	public struct RECT { public int Left, Top, Right, Bottom; }
	[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
'@
}
function Get-ClientSize {
	$r = New-Object IgtWin+RECT
	[IgtWin]::GetClientRect($hwnd, [ref]$r) | Out-Null
	return @($r.Right, $r.Bottom)
}
function Set-ClientSize([int]$w, [int]$h) {
	$outer = New-Object IgtWin+RECT; $client = New-Object IgtWin+RECT
	[IgtWin]::GetWindowRect($hwnd, [ref]$outer) | Out-Null
	[IgtWin]::GetClientRect($hwnd, [ref]$client) | Out-Null
	$cx = $w + ($outer.Right - $outer.Left) - $client.Right
	$cy = $h + ($outer.Bottom - $outer.Top) - $client.Bottom
	# SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS.
	[IgtWin]::SetWindowPos($hwnd, [IntPtr]::Zero, 0, 0, $cx, $cy, 0x4016) | Out-Null
	Start-Sleep -Milliseconds 800
}

# The screens this sweeps, and how each is reached. Keyboard only: scripted
# mouse clicks proved unreliable against a layout whose rows move.
#
# `viaConsole` is the distinction that matters. An open console CAPTURES
# keystrokes - the first version of this sent Esc and M straight into the
# console's input line and swept nothing but the HUD three times over, which is
# precisely the silent-coverage failure the checks below exist to catch (and
# did). A screen opened by a KEY therefore needs the console closed first and
# reopened to type into; a screen opened by a COMMAND needs it open.
#
# `probe` runs after the open step to print a status (`state`, `sheet status`,
# ...) where the open command's own answer is a claim rather than a report;
# `status` is what must be logged; `state` what the header must name.
# `selfTestOpen` / `selfTestNoAudit` are the -SelfTest faults.
$screens = @(
	@{ label = 'sweep_hud'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'hudpanel layout standard' }; close = { }
	   probe = @('hudpanel list'); status = @((Answer 'hud layout standard, ')) },
	@{ label = 'sweep_paused'; state = 'paused'; viaConsole = $false
	   open = { Send-Key $VK_ESCAPE }; close = { Send-Key $VK_ESCAPE }
	   probe = @('state'); status = @((Answer 'state paused\s*$')) },
	@{ label = 'sweep_map'; state = 'playing'; viaConsole = $false
	   open = { Send-Key 0x4D }; close = { Send-Key 0x4D }
	   probe = @('mappage'); status = @((Answer 'map page: \w+ \(toggle \w+, map open\)')) },
	@{ label = 'sweep_editor'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor' }; close = { Run-Cmd 'editor off' }
	   status = @((Answer 'map: editor mode\s*$')) },
	# The character SHEET, which was on the not-swept list purely because it
	# opened only by clicking a portrait. It carries more hand-laid-out content
	# than any other screen, so being the one nobody audited was exactly
	# backwards; the `sheet` dev command opens it through the same entry point
	# the click uses, and it is swept like everything else now.
	@{ label = 'sweep_sheet'; state = 'sheet'; viaConsole = $true
	   open = { Run-Cmd 'sheet 0' }; close = { Run-Cmd 'sheet off' }
	   probe = @('sheet status'); status = @((Answer 'sheet: open member 0 ')) },
	# The portrait picker over the sheet (docs/portraits-plan.md), opened the way
	# the sheet's "Change portrait" button opens it.
	@{ label = 'sweep_portraits'; state = 'sheet'; viaConsole = $true
	   open = { Run-Cmd 'sheet 0'; Run-Cmd 'portrait picker 0' }
	   close = { Run-Cmd 'portrait picker off'; Run-Cmd 'sheet off' }
	   status = @((Logged 'portrait picker: open for \w+\s*$'), (Answer 'picker open ')) },
	# The floating HUD's other shapes (docs/ui-panels-plan.md P3b/P4): the party
	# inventory WINDOW, and the MINIMAL layout - the party bar and the hands
	# folded into one card per member, with the Magic dock (a member knows a
	# symbol first, or it is not shown) moved to the left column.
	@{ label = 'sweep_inventory'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'inventory' }; close = { Run-Cmd 'inventory off' }
	   probe = @('inventory status'); status = @((Answer 'inventory: open tab ')) },
	@{ label = 'sweep_minimal'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'learn 0 fire'; Run-Cmd 'hudpanel layout minimal' }
	   close = { Run-Cmd 'hudpanel layout standard' }
	   probe = @('hudpanel list'); status = @((Answer 'hud layout minimal, ')) },
	# A HUD hand box's USE MENU in a 720p window (code-review C382): the bare
	# hand's Combat / Magic group rows are what a sub-18 px rem cut, and the Magic
	# submenu is open too. `handmenu` opens it as a right-click on the box does,
	# crediting two spells to the hand's quick list for the Magic group. The HUD
	# lays a menu out only while the console is SHUT and the map is too (`editor
	# off` above left it open in Player mode, and its first run audited a menu
	# never sized), so both shut first. Its status is `handmenu status` seeing it
	# LAID OUT: two groups or more, a submenu with rows, a box with an area, in a
	# window under 900 high - unsized, the box is 0x0 and the audit skips it
	# (clean for the wrong reason), and at 900p the old raw-pixel geometry cut a
	# group row by about a pixel, under the audit's slack.
	@{ label = 'sweep_handmenu'; state = 'playing'; viaConsole = $true
	   open = {
		   Run-Cmd 'mappage close'
		   $script:clientBefore = Get-ClientSize
		   Set-ClientSize 1280 720
		   Run-Cmd 'equip none 0 0'
		   Run-Cmd 'handmenu 0 0 firebolt earthbolt_burst'
		   Run-Cmd 'handmenu group 1'
		   Send-Key 0xC0; Start-Sleep -Milliseconds 800
		   Open-Console }
	   close = {
		   Run-Cmd 'handmenu off'
		   Set-ClientSize $script:clientBefore[0] $script:clientBefore[1] }
	   probe = @('handmenu status')
	   status = @((Answer ('handmenu: open - \d+ rows, ([2-9]|\d{2,}) groups, submenu [1-9]\d* rows; ' +
				   'box \[-?\d+,-?\d+ [1-9]\d*x[1-9]\d*\]; HUD rem [\d.]+ px, window \d+x([1-8]\d\d|\d{1,2})\b'))) },
	# The level generator's dialog in BOTH modes (docs/level-building.md P1):
	# CREATE (the toolbar's [+]) and REGENERATE, each on its first tab (the
	# dialog keeps the tab it was last left on). Opened only - nothing is
	# generated, so the sweep writes no level.
	@{ label = 'sweep_gencreate'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'generate dialog new'; Run-Cmd 'generate dialog tab 0' }
	   close = { Run-Cmd 'generate dialog off'; Run-Cmd 'editor off' }
	   status = @((Answer 'generate dialog: create tab 0 ')) },
	@{ label = 'sweep_genregen'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'generate dialog'; Run-Cmd 'generate dialog tab 0' }
	   close = { Run-Cmd 'generate dialog off'; Run-Cmd 'editor off' }
	   status = @((Answer 'generate dialog: regenerate tab 0 ')) },
	# ...and its OTHER tabs: a sweep only sees the tab that is showing, and the
	# checkbox and ramp slider live on the last one.
	@{ label = 'sweep_gencomplexity'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'generate dialog'; Run-Cmd 'generate dialog tab 1' }
	   close = { Run-Cmd 'generate dialog off'; Run-Cmd 'editor off' }
	   status = @((Answer 'generate dialog: regenerate tab 1 '))
	   # The -SelfTest fault: a tab the form does not have is REFUSED, and the
	   # dialog stays on the tab it showed - the screen beneath this sweep.
	   selfTestOpen = { Run-Cmd 'editor'; Run-Cmd 'generate dialog'; Run-Cmd 'generate dialog tab 99' } },
	@{ label = 'sweep_genpopulation'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'generate dialog'; Run-Cmd 'generate dialog tab 2' }
	   close = { Run-Cmd 'generate dialog off'; Run-Cmd 'editor off' }
	   status = @((Answer 'generate dialog: regenerate tab 2 ')) },
	# The Style tab (tool-refinement Phase 7), with a style picked.
	@{ label = 'sweep_genstyle'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'generate dialog'; Run-Cmd 'generate dialog style small_crypt'; Run-Cmd 'generate dialog tab 3' }
	   close = { Run-Cmd 'generate dialog off'; Run-Cmd 'editor off' }
	   status = @((Answer 'generate dialog: regenerate tab 3 style=small_crypt ')) },
	# The STAIR inspector, both layouts: crypt1's stair down at 1,1 (destination
	# and Go to) and its exit at 7,8 (one "leads out to" line).
	# `editor inspect` is what a right-click on the square does. It goes to crypt1
	# FIRST, rather than trusting where the game started: the run used to press
	# Enter on the landing page, which was Continue whenever a save existed and
	# loaded whatever level it named (an eval save puts it on eval_arena), and the
	# first version of this swept two empty squares of the arena - clean, and
	# vacuous. Its status line names the inspector, the square and the level.
	# FROZEN, because crypt1 has a monster and the world simulates under the
	# editor: unfrozen, it killed the party mid-sweep, and every screen after
	# this one was reached from the title screen instead.
	@{ label = 'sweep_stair'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'freeze on'; Run-Cmd 'goto crypt1'; Run-Cmd 'editor inspect 1 1' }
	   close = { Run-Cmd 'editor inspect off'; Run-Cmd 'editor off' }
	   status = @((Logged 'editor inspect: stair \(1, 1 on crypt1\)')) },
	@{ label = 'sweep_stairexit'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor inspect 7 8' }
	   close = { Run-Cmd 'editor inspect off'; Run-Cmd 'editor off' }
	   status = @((Logged 'editor inspect: stair \(7, 8 on crypt1\)')) },
	# The WORLD screen's two dialogs: each one opens only on the world map (the
	# one state that routes input to it), so the sweep goes there and comes back.
	@{ label = 'sweep_worldsettings'; state = 'worldmap'; viaConsole = $true
	   open = { Run-Cmd 'worldmap on'; Run-Cmd 'worldsettings' }
	   close = { Run-Cmd 'worldsettings off'; Run-Cmd 'worldmap off' }
	   status = @((Answer 'world settings open\s*$')) },
	@{ label = 'sweep_worlds'; state = 'worldmap'; viaConsole = $true
	   open = { Run-Cmd 'worldmap on'; Run-Cmd 'worlds dialog' }
	   close = { Run-Cmd 'worlds dialog off'; Run-Cmd 'worldmap off' }
	   status = @((Answer 'worlds dialog open: '))
	   # The -SelfTest fault: opened, never audited - and the label that IS
	   # audited just before it, sweep_worldsettings, contains this one.
	   selfTestNoAudit = $true },
	# The PAUSE MENU and the SHEET over the world map (code-review C365): each
	# must draw the world map behind it, not the parked dungeon that stays
	# resident under it. `backdrop` reports what Render drew, and is each row's
	# status. Esc is a KEY, so the console shuts for it and reopens to type
	# into. The sheet sweep reads a level's sheet first - the control, a
	# readout that can say "scene drawn" - and demands both lines.
	@{ label = 'sweep_worldpause'; state = 'paused'; viaConsole = $true
	   open = { Run-Cmd 'worldmap on'; Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 400
				Send-Key $VK_ESCAPE; Start-Sleep -Milliseconds 800; Open-Console }
	   close = { Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 400; Send-Key $VK_ESCAPE
				 Start-Sleep -Milliseconds 800; Open-Console; Run-Cmd 'worldmap off' }
	   probe = @('backdrop'); status = @((Answer 'backdrop: paused over worldmap - scene skipped')) },
	@{ label = 'sweep_worldsheet'; state = 'sheet'; viaConsole = $true
	   open = { Run-Cmd 'sheet 0'; Run-Cmd 'backdrop'; Run-Cmd 'sheet off'
				Run-Cmd 'worldmap on'; Run-Cmd 'sheet 0' }
	   close = { Run-Cmd 'sheet off'; Run-Cmd 'worldmap off' }
	   probe = @('backdrop')
	   status = @((Answer 'backdrop: sheet over playing - scene drawn'),
				  (Answer 'backdrop: sheet over worldmap - scene skipped')) },
	# The delete CONFIRMATION, which needs a world that may be deleted - so it
	# makes its own and deletes it on the way out, rather than depending on
	# whatever worlds happen to be on this machine.
	@{ label = 'sweep_worlddelete'; state = 'worldmap'; viaConsole = $true
	   open = { Run-Cmd 'worldmap on'; Run-Cmd 'worlds new wt_sweep'; Run-Cmd 'worlds dialog delete wt_sweep' }
	   close = { Run-Cmd 'worlds dialog off'; Run-Cmd 'worlds delete wt_sweep wt_sweep'; Run-Cmd 'worldmap off' }
	   status = @((Answer "worlds dialog open: .* deleting 'wt_sweep'")) },
	# A DUNGEON's delete confirmation (W10), inside the type editor. The demo's
	# own dungeons are both refused (the opening, the party), so it makes an
	# empty one of its own and deletes it on the way out, through the same rule.
	@{ label = 'sweep_dungeondelete'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'newtype dungeons'; Run-Cmd 'dungeons dialog dungeon1'; Run-Cmd 'dungeons dialog delete' }
	   close = { Run-Cmd 'dungeons dialog off'; Run-Cmd 'dungeons delete dungeon1 dungeon1'; Run-Cmd 'editor off' }
	   status = @((Answer "dungeons dialog: open 'dungeon1' confirming")) },
	# A LONG ID IN THE TYPE EDITOR'S TITLE (code-review C455): the title is the
	# id's rename affordance (game::EditableTitle), and at full title size a
	# 32-character id runs past its slot - it must SHRINK to fit, not escape and
	# not cut (an id's tail is what names it). An empty dungeon of its own,
	# renamed long and deleted on the way out, as above. Its status is the
	# dialog's own answer naming the long id - a sweep of the bare editor would
	# be clean.
	@{ label = 'sweep_longtitle'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'newtype dungeons'; Run-Cmd "dungeons rename dungeon1 $longTitleId"
				Run-Cmd "dungeons dialog $longTitleId" }
	   close = { Run-Cmd 'dungeons dialog off'; Run-Cmd "dungeons delete $longTitleId $longTitleId"; Run-Cmd 'editor off' }
	   status = @((Answer ([regex]::Escape("dungeons dialog: open '$longTitleId'")))) },
	# The NEW WORLD dialog (editor-updates P4), from the level editor's toolbar:
	# with "Copy one level" picked, so its level dropdown row is the live one...
	@{ label = 'sweep_newworld'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'worlds newdialog source level crypt1' }
	   close = { Run-Cmd 'worlds newdialog off'; Run-Cmd 'editor off' }
	   status = @((Answer 'new world dialog open: source level '), (Answer "  copy level 'crypt1'\s*$")) },
	# ...with the WIZARD picked (P5), when its four rows join a taller card...
	@{ label = 'sweep_newworldwizard'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'worlds newdialog source wizard' }
	   close = { Run-Cmd 'worlds newdialog off'; Run-Cmd 'editor off' }
	   status = @((Answer 'new world dialog open: source wizard ')) },
	# ...with BLANK picked, when the Style row (Phase 7) joins the plain card...
	@{ label = 'sweep_newworldblank'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'worlds newdialog source blank'; Run-Cmd 'worlds newdialog style dirt_tunnels' }
	   close = { Run-Cmd 'worlds newdialog off'; Run-Cmd 'editor off' }
	   status = @((Answer "new world dialog open: source blank made '' style dirt_tunnels ")) },
	# ...and after a Create, when "Switch now" joins the footer. It makes a
	# blank world of its own and deletes it on the way out.
	@{ label = 'sweep_newworldmade'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'editor'; Run-Cmd 'worlds newdialog create wt_nwsweep' }
	   close = { Run-Cmd 'worlds newdialog off'; Run-Cmd 'worlds delete wt_nwsweep wt_nwsweep'; Run-Cmd 'editor off' }
	   status = @((Answer "new world dialog open: source blank made 'wt_nwsweep' ")) },
	# SHORT PARTIES (party creation phase 2), LAST because each one restarts the
	# game: three members (the bar keeps four slots, the hands go 2+1, the party
	# window's fourth card goes inert) and then one. A refused `newparty` leaves
	# the default four up, and their sweep is the HUD swept again - so each one's
	# status is the party it built, and the game it landed in.
	@{ label = 'sweep_party3'; state = 'playing'; viaConsole = $true
	   open = { Run-Party 'premade=0 | premade=1 | premade=2' }; close = { }
	   probe = @('state'); status = @((Answer 'new game with a party of 3\s*$'), (Answer 'state playing\s*$')) },
	@{ label = 'sweep_party3sheet'; state = 'sheet'; viaConsole = $true
	   open = { Run-Cmd 'sheet 2' }; close = { Run-Cmd 'sheet off' }
	   probe = @('sheet status'); status = @((Answer 'sheet: open member 2 ')) },
	@{ label = 'sweep_party3inventory'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'inventory' }; close = { Run-Cmd 'inventory off' }
	   probe = @('inventory status'); status = @((Answer 'inventory: open tab ')) },
	@{ label = 'sweep_party3minimal'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'learn 0 fire'; Run-Cmd 'hudpanel layout minimal' }
	   close = { Run-Cmd 'hudpanel layout standard' }
	   probe = @('hudpanel list'); status = @((Answer 'hud layout minimal, ')) },
	@{ label = 'sweep_party1'; state = 'playing'; viaConsole = $true
	   open = { Run-Party 'premade=0' }; close = { }
	   probe = @('state'); status = @((Answer 'new game with a party of 1\s*$'), (Answer 'state playing\s*$')) },
	@{ label = 'sweep_party1minimal'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'learn 0 fire'; Run-Cmd 'hudpanel layout minimal' }
	   close = { Run-Cmd 'hudpanel layout standard' }
	   probe = @('hudpanel list'); status = @((Answer 'hud layout minimal, ')) }
)
# The screens swept on the TITLE, before the game starts - the party creation
# page (phase 3), opened by its dev twin, which drives the page's own code.
# Each opens on top of the one before; the run backs out after the last.
$titleScreens = @(
	@{ label = 'sweep_partycreation'; state = 'menu'; open = { Run-Cmd 'partypage open' }
	   status = @((Logged "party creation: the page opens for '")) },
	@{ label = 'sweep_partydefault'; state = 'menu'; open = { Run-Cmd 'partypage default' }
	   probe = @('partypage status'); status = @((Answer 'member 3\*? name=.* premade=3 ')) },
	@{ label = 'sweep_partypicker'; state = 'menu'; open = { Run-Cmd 'partypage picker' }
	   status = @((Logged 'portrait picker: open for .* \(party creation\)')) }
)
# The checks -SelfTest expects to fail, and no others (see the header).
$selfTestExpected = @('sweep_gencomplexity: status', 'sweep_worlds: label')

# Every screen must say how it is known to have OPENED. One with no `status`
# would be judged on its label, state and audit alone - "the command ran", the
# coverage C427 removed - and the real run and the -SelfTest would both still
# pass. So a row without one is refused here, before anything launches.
foreach ($s in @($titleScreens) + @($screens)) {
	$pats = @($s.status | Where-Object { $_ })
	if (-not $s.label -or -not $s.state -or -not $s.open -or $pats.Count -eq 0) {
		throw "screen '$($s.label)' needs a label, a state, an open step and at least one status pattern - without a status line its audit cannot show the screen opened"
	}
}

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

# One screen: a marker, the open step, its probes, the audit. The console is
# OPEN on entry and on exit.
function Sweep-Screen($s) {
	$open = if ($SelfTest -and $s.selfTestOpen) { $s.selfTestOpen } else { $s.open }
	Run-Quick "echo sweep-open $($s.label)"
	if ($s.viaConsole -ne $false) {
		& $open
	} else {
		Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 400   # close: let the key reach the game
		& $open
		Start-Sleep -Milliseconds 600
		Open-Console
	}
	foreach ($p in @($s.probe)) { if ($p) { Run-Quick $p } }
	# The console sits OUTSIDE the widget tree by design, so the screen behind
	# it is still what gets audited.
	if (-not ($SelfTest -and $s.selfTestNoAudit)) { Run-Cmd "uioverlap $($s.label)" }
	if ($s.viaConsole -ne $false) {
		if ($s.close) { & $s.close }
	} else {
		Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 400
		& $s.close
		Start-Sleep -Milliseconds 600
		Open-Console
	}
}

$proc = $null
$hwnd = [IntPtr]::Zero
try {
	Start-HarnessGame $exe $bin $log $LoadTimeoutSec
	if ($SelfTest) { Write-Host "self-test: expecting exactly these to fail: $($selfTestExpected -join ', ')" }

	# THE PARTY CREATION PAGE (party creation phase 3), on the title screen
	# where it lives: a new member, the default four, and the face picker over
	# the page. The console must answer first (retried until a NEW echo lands),
	# which also turns logecho on for the whole run.
	Open-Console
	if (-not (Wait-ConsoleReady)) { throw 'the console never accepted a command on the title screen' }
	foreach ($s in $titleScreens) { Sweep-Screen $s }
	Run-Cmd 'partypage back'
	Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 400   # closed: Start-NewGame opens it

	# Through the console's `newgame`, never the landing page (Continue on the
	# newest shared save), waited out to the level (tools\HarnessGame.ps1). It
	# leaves logecho on and the console closed.
	Start-NewGame $LoadTimeoutSec | Out-Null
	Start-Sleep -Seconds 1

	Open-Console
	if (-not (Wait-ConsoleReady)) { throw 'the console never accepted a command after the new game' }
	# Retried until it answers, counting only a NEW report.
	Wait-ConsoleReady 'levelcheck' 'levelcheck RESULT=' 10 4000 | Out-Null

	foreach ($s in $screens) { Sweep-Screen $s }

	# Where the sweep ENDED: still in the game, or every screen after the one
	# that lost it was audited from somewhere else.
	Run-Quick 'echo sweep-end'
	Run-Quick 'state'
	Start-Sleep -Seconds 1
} finally {
	Stop-HarnessGame 8000
}

# --- the verdict, read from the log -----------------------------------------
$lines = @(if (Test-Path $log) { Get-Content $log })
$failed = New-Object System.Collections.Generic.List[string]   # "<label>: <check>"
$global = 0

function Lines-Between([int]$from, [int]$to) {   # exclusive both ends
	if ($to - $from -le 1) { return @() }
	return @($lines[($from + 1)..($to - 1)])
}
function Fail-Check([string]$label, [string]$check, [string]$why) {
	$script:failed.Add("${label}: $check")
	Write-Host "  [FAIL] ${label}: $why" -ForegroundColor Red
}

# The log's own structure: the headers in order, the harness's open markers,
# and where the sweep ended.
$headers = @()
$markers = @{}
$endMarker = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
	$l = $lines[$i]
	if ($l -match '^\[info \] uioverlap \[([^\]]+)\] --- state (\S+)\s*$') {
		$headers += [pscustomobject]@{ at = $i; label = $Matches[1]; state = $Matches[2] }
	} elseif ($l -match '^\[info \] console: sweep-open (\S+)\s*$') {
		if (-not $markers.ContainsKey($Matches[1])) { $markers[$Matches[1]] = @() }
		$markers[$Matches[1]] += $i
	} elseif ($l -match '^\[info \] console: sweep-end\s*$') {
		$endMarker = $i
	}
}

Write-Host ''
$lc = $lines | Select-String 'levelcheck RESULT=' | Select-Object -Last 1
if ($lc) {
	Write-Host "  $($lc.Line.Substring($lc.Line.IndexOf('levelcheck')))"
	if ($lc.Line -notmatch 'RESULT=PASS') { $global++ }
} else {
	Write-Host '  [FAIL] levelcheck never reported' -ForegroundColor Red
	$global++
}

foreach ($s in @($titleScreens) + @($screens)) {
	# LABEL: exactly one header naming exactly this label.
	$mine = @(for ($k = 0; $k -lt $headers.Count; $k++) { if ($headers[$k].label -ceq $s.label) { $k } })
	if ($mine.Count -ne 1) {
		Fail-Check $s.label 'label' $(if ($mine.Count -eq 0) { 'never reached the log - screen not audited' }
									  else { "audited $($mine.Count) times" })
		continue
	}
	$k = $mine[0]
	$h = $headers[$k]
	$prev = if ($k -gt 0) { $headers[$k - 1].at } else { -1 }
	$next = if ($k + 1 -lt $headers.Count) { $headers[$k + 1].at }
			elseif ($endMarker -gt $h.at) { $endMarker } else { $lines.Count }
	$ok = $true

	# STATE: where the audit happened.
	if ($h.state -ne $s.state) {
		Fail-Check $s.label 'state' "audited in state '$($h.state)', not '$($s.state)'"
		$ok = $false
	}

	# STATUS: the open step's own report, between this screen's marker and its
	# header - never a line some earlier screen left.
	$start = $prev
	if ($markers.ContainsKey($s.label)) {
		$m = @($markers[$s.label] | Where-Object { $_ -gt $prev -and $_ -lt $h.at }) | Select-Object -Last 1
		if ($null -ne $m) { $start = $m }
	}
	$before = Lines-Between $start $h.at
	$missing = @(foreach ($p in $s.status) { if (-not ($before -match $p)) { $p } })
	if ($missing.Count -gt 0) {
		Fail-Check $s.label 'status' "its open step never logged $($missing -join ' / ') - the audit saw whatever was underneath"
		$ok = $false
	}

	# AUDIT: the summary the audit writes when it is done, and its findings.
	$after = Lines-Between $h.at $next
	$sum = @($after -match '^\[info \] uioverlap: (clean|\d+ findings)') | Select-Object -First 1
	if (-not $sum) {
		Fail-Check $s.label 'audit' 'the audit never reported after its header'
		$ok = $false
	} elseif ($sum -notmatch 'uioverlap: clean') {
		Fail-Check $s.label 'findings' "found overlaps:"
		$after -match '^\[info \]   \S' | ForEach-Object { Write-Host "     $_" }
		$ok = $false
	}
	if ($ok) { Write-Host "  [ok  ] swept $($s.label) ($($h.state), status logged, clean)" }
}

# Where the sweep ended.
$endState = @(Lines-Between $endMarker $lines.Count) -match '^\[info \] console: state (\S+)\s*$' |
	Select-Object -Last 1
if ($endMarker -lt 0 -or -not $endState) {
	Write-Host '  [FAIL] the sweep never reported where it ended (no `state` after it)' -ForegroundColor Red
	$global++
} elseif ($endState -notmatch 'console: state (playing|worldmap)\s*$') {
	Write-Host "  [FAIL] the sweep ended out of the game: $($endState -replace '^\[info \] console: ', '')" -ForegroundColor Red
	$global++
} else {
	Write-Host "  [ok  ] the sweep ended in the game ($($endState -replace '^\[info \] console: ', ''))"
}

Write-Host ''
$problems = $failed.Count + $global
$verdict = if ($problems -eq 0) { 'PASS' } else { 'FAIL' }
Write-Host "ingametest RESULT=$verdict screens=$(@($titleScreens).Count + @($screens).Count) failures=$problems self_test=$([int]$SelfTest.IsPresent)"
if ($SelfTest) {
	# SpellTest's rule: exactly the injected faults fail, and everything else
	# passes - a sweep that fails everything proves nothing about the checks.
	$unexpected = @($failed | Where-Object { $selfTestExpected -notcontains $_ })
	$uncaught = @($selfTestExpected | Where-Object { $failed -notcontains $_ })
	foreach ($u in $uncaught) { Write-Host "  self-test: '$u' was injected but PASSED" -ForegroundColor Red }
	foreach ($u in $unexpected) { Write-Host "  self-test: '$u' failed but was not injected" -ForegroundColor Red }
	if ($global -gt 0) { Write-Host '  self-test: a run-wide check failed - the run itself is broken' -ForegroundColor Red }
	$asExpected = $unexpected.Count -eq 0 -and $uncaught.Count -eq 0 -and $global -eq 0
	if ($asExpected) {
		Write-Host 'SELF-TEST PASSED - exactly the injected faults failed: a refused open step and a missing label' -ForegroundColor Green
	} else {
		Write-Host 'SELF-TEST FAILED - the checks did not fail exactly where the faults were injected' -ForegroundColor Red
	}
	Write-Host "NOT swept: $notSwept"
	exit ([int](-not $asExpected))
}
if ($verdict -eq 'PASS') {
	Write-Host 'PASS' -ForegroundColor Green
} else {
	Write-Host "FAIL - $problems problem(s)" -ForegroundColor Red
}
Write-Host "NOT swept: $notSwept"
exit ([int]($verdict -ne 'PASS'))
