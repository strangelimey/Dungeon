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
#   .\tools\AllocTest.ps1 -Melee             # a monster swinging at the party
#   .\tools\AllocTest.ps1 -Cast              # a bolt in flight + an open spellbook
#   .\tools\AllocTest.ps1 -Impact            # bolts landing, expiring, a blast, a crate alight
#   .\tools\AllocTest.ps1 -Hand              # the hand spells: light, douse, flare, fill, pebble
#   .\tools\AllocTest.ps1 -Pause             # Esc to the pause menu and back
#   .\tools\AllocTest.ps1 -Sheet             # the sheet: hover, tabs, item dialog
#   .\tools\AllocTest.ps1 -Panels            # drag and resize the floating HUD
#   .\tools\AllocTest.ps1 -Minimal [-Sheet]  # any mode, under the party-card layout
#   .\tools\AllocTest.ps1 -Items             # pack -> cursor -> floor -> cursor -> pack
#   .\tools\AllocTest.ps1 -Packs             # swap a 4-slot and an 8-slot bag
#   .\tools\AllocTest.ps1 -Throw             # lift a rock, throw it at a wall, again
#   .\tools\AllocTest.ps1 -Walk              # key turns: the party AND the pad's stones
#   .\tools\AllocTest.ps1 -Config release    # needs -DDN_TRACK_ALLOCS=ON
#
# THE RULE HAS NO EXCEPTIONS: an allocation in a settled frame is a bug, and
# that includes frames where something HAPPENED. A bump message, a level line, a
# monster's swing - since docs/message-allocation.md printing any of them
# allocates nothing, so the guard carries no notion of an event and no list of
# things it forgives. (It used to: "allocation proportional to events is not
# what the rule forbids" was written here as policy, and it was a
# rationalisation of a defect - loc::Tr copying text the table already owned.)
#
# The party stands still in the default run because that is the BASELINE - the
# least a steady state can be - not because events are excused. A still party
# simply keeps the event paths OUT of the window, and a path outside the window
# passes whether it allocates or not. That is what the modes below are for:
# each one puts an event path INSIDE the window (-Wounded the regeneration tick,
# -Melee a monster's swing and its narration, -Cast a bolt in flight and an open
# spellbook, -Impact bolts launching, striking a FRESH monster, expiring and
# bursting, and a crate the blasts leave alight burning down). Anything that
# allocates is named with a full call stack in dungeon.log, once per unique
# stack.
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
# It wounds with a SHORT bleed and lets it expire before measuring, so the
# window holds the regeneration tick and nothing else, and it sets the resource
# practices high first - not to make the numbers big, but so a level-up (which
# re-derives the maxima) cannot land mid-window and make the verdict depend on
# how near a practice happened to be to its next level. That keeps the run
# repeatable; it is NOT an excuse. A level-up that allocated would be a bug like
# any other - this run is just not the one that looks for it. At level 20 a
# practice needs 41 more XP, which ten seconds of regeneration cannot reach.
#
# -Melee IS THE SAME TRAP AGAIN. No monster reaches a party standing at the
# start inside the window, so MonsterAttack - its name lookup and its narration
# - went unmeasured, and every swing allocated three times (a concatenated
# "monster." key, loc::Tr's copy, the string local) while every run passed.
# This spawns a weakened monster beside the party, waits for its first blow to
# LAND (one-time warm-up - a sound's first voice of its format, a member's first
# entry in a skill table - stays outside the window), then measures with it
# still swinging, and refuses a PASS unless the tally shows the party was
# actually hit INSIDE the window. Swings are events, and since the message path
# stopped allocating (docs/message-allocation.md) events get no exemption.
#
# -Cast, AND AGAIN (2026-09-28). No run ever cast a spell or opened a book, so a
# bolt copied its payload - four std::string effect ids, which the debug CRT
# allocates for - on every frame of its flight, and an open spellbook rebuilt a
# vector of rune slots twice a frame, while every run passed. This freezes the
# world (timescale 0, so the bolt can neither land nor fizzle), has a caster
# learn fire and cast it, and opens that member's book (`book`), so the window
# holds a projectile in flight AND a book being redrawn. The LAUNCH itself is
# an event in the console's own frame, which the guard never arms; it was
# checked by hand from the hand use menu and the book's Cast button.
#
# -Impact, BECAUSE -Cast CANNOT LAND ANYTHING. -Cast freezes the world, so its
# bolt never arrives: the strike (fx::Deal, the burn a flame leaves on the
# monster, the hit lines, threat), the expiry of a bolt that flies past, the
# impact sparks and an area blast all stayed outside every window. This runs
# the world. It goes to eval_arena (an open room, so no wall of the showcase
# level decides what gets measured), stands the party three squares from a
# FROZEN, toughened monster (`freeze on`: it stands in the line of fire and
# never walks up to swing, which is -Melee's job), and hands the casting to the
# harness (`autocast`): two flame casters in OPPOSITE lanes - one bolt strikes,
# the other flies past the monster and expires at the end of its reach - and a
# Fire Burst, which detonates on contact or where it stops. In an open room
# the blast's force is spent before it reaches back three squares. (A bolt's
# `range` is in METRES, so five squares out, the first try, was out of reach
# of everything and measured nothing but expiries.) A launch now happens in a world
# frame, so the window holds launches AND landings. It waits until each of the
# three has happened once (first times are warm-up), then measures, and
# refuses a PASS unless the tally shows a bolt hit, an expiry and a blast
# INSIDE the window.
#
# AND IT MEASURES A FRESH MONSTER. Warm-up may only absorb a first time for the
# PROCESS; a first time for a MONSTER is paid again by every monster in play.
# Warming up and measuring on one target passed while every monster's first
# burn allocated twice (its effects list growing from empty, its flame plume
# made on ignition). So the rotation is held (`autocast hold`), the party
# steps aside to a new, untouched target, and alloctest's first ARMED frame
# releases the barrage and restarts the tally; the verdict frame logs that
# tally, which is what the refusal reads.
#
# -Pause IS THE OTHER HALF OF THE RULE: WHICH FRAMES IT COVERS. The guard judges
# a frame on the state at its top, so the frame Esc is pressed in starts as
# Playing and ends as Paused - and it rebuilds the pause menu (a widget tree, and
# ListSaves parsing every save for the Load entry) inside a frame armed as
# steady. That logged ~5000 allocations on every Esc (2026-09-28). A frame that
# LEAVES the guarded states is a transition, and Game::Update now disarms it; this
# run presses Esc during the window, resumes, and repeats, so a regression of
# that rule - or a resume path that allocates in the frames after it - lands
# inside the window. It refuses a PASS unless the verdict line counts at least
# one such transition (`transitions=`), since a swallowed Esc would otherwise
# report exactly like a clean run.
#
# -Sheet IS THE CHARACTER SHEET'S TURN (docs/ui-updates-plan.md). The sheet is a
# guarded state, and since ui-updates it does things every frame the pointer
# moves: the status bar names whatever is under it, on every tab. A right-click
# opens the item details dialog - IN an armed frame, which is why that dialog is
# built once rather than per open - and it spins a 3D model every frame it is
# up; a middle-click opens the use menu. None of it ran in any window before.
# This opens the sheet, warms the dialog up once through `itemdetails` (a first
# open bakes its fonts, which is a first time for the process, not a steady
# cost), then during the window hovers two slots, pages through all five tabs,
# right-clicks an item, lets the model turn, Escs, middle-clicks a rune and Escs
# again - three times. It refuses a PASS unless `itemdetails status` counts
# opens made during the window, since a missed click reports exactly like a
# clean run.
#
# -Panels IS THE FLOATING HUD'S TURN (docs/ui-panels-plan.md P3a). Every HUD
# panel moves and resizes under the mouse now, inside armed frames: a drag
# re-places the panel every frame it is held, a corner grip rescales it (a new
# font size the first time, which is a first time, not a steady cost - so the
# warm-up below drags once before the window), and the release SAVES
# settings.ini, which excuses itself (GameSettings::Save). This resets the
# layout, warms up, then during the window drags the Movement dock by its title
# and back and pulls the Hands dock's corner grip - three times - with the
# party inventory WINDOW (P3b) open the whole while. It refuses a PASS unless
# `hudpanel list` afterwards shows all three (the move dock saved off its
# default, the hands dock off scale 1, the inventory shown), since a missed
# drag or a window that never opened reports exactly like a clean run. Every
# drag holds Ctrl (a panel only arranges under it). After the window it also
# checks the arranging rules: a Ctrl+click on a panel's reset button puts every
# panel home, a drag WITHOUT Ctrl moves nothing, and a drag ending 4 px short of
# another panel's edge lands on it - any of them failing is a FAIL.
# THE TRAY (ui-updates Phase 8) rides the same window: the Movement dock is
# minimized by its Ctrl button and restored by its tray button, and the counts
# `hudpanel list` prints must show the trip landed.
# -Items IS MOVING AN ITEM, which no run did (found by accident 2026-09-30, when
# a -Panels click on the ui-panels branch landed on an inventory slot and a later
# one on the floor). Two defects, both logged with call stacks: every pick, put
# and swap COPIED the cursor's id into a fresh std::string (the debug CRT
# allocates for any string it constructs, short or not), and the first time a
# kind of item reached the floor its kind was BUILT - a rune's first drop loaded
# its PBR set, 246 allocations and 2 MB in one guarded frame. The cursor now
# swaps strings with the slot (Game/Inventory.h HeldItem) and every item kind is
# built at load (DungeonWorld::PreloadItemKinds).
# This goes to eval_arena (open floor ahead, like -Impact), freezes it, puts two
# runes in the pack of a member with room, and opens the party inventory window from the
# console (`inventory`). Each cycle: pick the rune out of its slot, click the
# floor (drop), click it again (lift), and put it back - the window is
# non-modal, so it stays open throughout. The floor point is computed from the
# camera (70 degree lens, eye 1.55 m up) to land in the FAR quarter of the
# square ahead, so the lift - which samples the ray at the item's own height,
# nearer the party - still lands in the same quarter. ONE rune runs a warm-up
# cycle first, which also checks the coordinates (`inventory status`); the
# window then moves the OTHER rune, so its FIRST drop is inside the window - a
# first time for a KIND is paid again by every kind a player drops, so it is
# not warm-up (the -Impact lesson). It refuses a PASS unless the window's tally
# counts at least two drops and two lifts, which also puts the put-back between
# them inside the window.
#
# -Packs IS -Items' KNOWN-LEFT CASE: equipping a bag with more slots than the
# one it replaces. A pack's slots were a std::vector, so a bigger bag grew it,
# and even growth inside its capacity constructed a std::string per new slot
# (the debug CRT allocates for each). Pack slots are now a fixed-capacity list
# whose strings exist from the start (Game/Inventory.h PackSlots). This puts an
# ammo pouch (8 slots) in an empty pack-row square and a herb pouch (4) on the
# cursor, both through the sheet's own clicks, warms up one swap pair, then
# clicks that square twice per cycle inside the window: herb in (8 -> 4), ammo
# back (4 -> 8). It refuses a PASS unless `sheet status` counts two equips made
# during the window.
#
# -All IS THE PARTY WINDOW (more-ui-updates Phase 5): the sheet's "All" opens a
# card per member on the sheet's tab, each card the sheet's own code. Its four
# cards are built and warmed with the HUD, so opening it - a click in a guarded
# frame - must add nothing. This opens the sheet, reads where "All", the tab
# stones and member 0's portrait are (`sheet status`, `inventory stone`,
# `hudpanel list`), runs one cycle as a warm-up, then cycles inside the window:
# All, every tab with a hover over each card, Esc, the portrait (the sheet
# again). It refuses a PASS unless `inventory status` counts two opens inside.
#
# -Glass IS THE TRANSPARENT QUEUE (transparency Phase 1). A see-through draw is
# not issued but QUEUED, then sorted and drawn after the opaque scene - and no
# other mode ever has glass on screen, so none of that would be measured. This
# places a glass kind (-GlassCategory / -GlassKind, by default the empty flask
# item) in eval_arena one square ahead of the party and measures with it in
# view. It refuses a PASS unless `glass` counts
# a frame that drew glass for (nearly) every armed frame of the window.
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
	# Measures a party IN MELEE: a monster beside it swinging through the whole
	# window. See the note above - a party nobody attacks never runs the swing.
	[switch]$Melee,
	[string]$MeleeMonster = 'skeleton',
	# Scales the spawned monster's hp AND damage (the `spawn` 5th argument), so
	# it keeps swinging for the whole window without wiping the party.
	[double]$MeleeStrength = 0.3,
	# Measures a bolt IN FLIGHT and an OPEN SPELLBOOK. See the note above.
	[switch]$Cast,
	# The caster: Maren, a rear-rank caster, by default.
	[int]$CastMember = 2,
	# Measures bolts LANDING: impacts, expiries and an area blast, with the
	# world running. See the note above.
	[switch]$Impact,
	# MEDIUM on purpose: it stands in one quarter of its square, so exactly one
	# of the two flame lanes strikes it and the other flies past. A Large body
	# (the plain skeleton) fills the square, both lanes hit, and nothing ever
	# expires - the first run that tried it saw 100 strikes and 0 expiries.
	[string]$ImpactMonster = 'skel_swarm',
	# The target's hp scale (`spawn`'s 5th argument): it must outlive the
	# warm-up AND the window under a bolt every fraction of a second.
	[double]$ImpactStrength = 400,
	# Seconds between casts, round-robin over the rotation below.
	[double]$ImpactEvery = 0.4,
	# Casts the four hand spells at a wall torch inside the window (spell-updates
	# Phase 8). See the note at the setup.
	[switch]$Hand,
	# Pauses (Esc) and resumes inside the window. See the note above.
	[switch]$Pause,
	# Works the character sheet inside the window. See the note above.
	[switch]$Sheet,
	# Opens the PARTY WINDOW from the sheet's "All" and works every tab of it
	# inside the window (more-ui-updates Phase 5). See the note above.
	[switch]$All,
	# Drags and resizes the floating HUD panels inside the window. See above.
	[switch]$Panels,
	# Runs whichever mode under the Minimal HUD layout (one card per member,
	# docs/ui-panels-plan.md P4), and puts Standard back afterwards.
	[switch]$Minimal,
	# Turns the party by KEY inside the window - a full circle each way - so the
	# movement pad presses its cut stones (more-ui-updates: a key move presses
	# the matching stone, via Party::ActCount). Refuses a PASS unless the
	# verdict's moves= counts them.
	[switch]$Walk,
	# Moves an item pack -> floor -> pack inside the window. See the note above.
	[switch]$Items,
	# The warm-up item and the measured one: two different kinds, the second
	# never dropped before the window opens.
	[string]$WarmItem = 'rune_air',
	[string]$MeasureItem = 'rune_water',
	# Swaps a small and a big bag in the pack row inside the window. See above.
	[switch]$Packs,
	# Lifts a rock off the floor and throws it at a wall, round and round,
	# inside the window (ui-updates Phase 10). See the note at the setup.
	[switch]$Throw,
	# Stands the party facing a GLASS decoration for the whole window, so the
	# transparent queue (transparency Phase 1) queues, sorts and flushes in armed
	# frames. See the note above.
	[switch]$Glass,
	[string]$GlassCategory = 'items',
	[string]$GlassKind = 'flask_empty',
	# Starts with a CREATED party instead of the default four: a `newparty` spec
	# (party creation, docs/party-creation-plan.md phase 2), e.g.
	# 'premade=0 | premade=1 | premade=2' for three. Any mode runs under it; the
	# member loops below walk only the members it builds.
	[string]$Party = '',
	# Checks the CHECKER: makes the game allocate every frame on purpose
	# (`allocpoke`) and passes only if the run comes back FAIL.
	[switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
# -Items spends about four armed seconds a round trip and needs two whole ones
# inside the window, so its default window is longer.
if (($Items -or $Throw -or $All) -and -not $PSBoundParameters.ContainsKey('Seconds')) { $Seconds = 20 }
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"

# Muted for the whole run, restored however it ends (tools\HarnessAudio.ps1).
. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not $env:DN_HARNESS_MUTED) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }

$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'

# How many members the run plays: the default four, or one per `|`-separated
# member of -Party.
$memberCount = if ($Party) { @($Party -split '\|').Count } else { 4 }
if ($memberCount -lt 1 -or $memberCount -gt 4) { throw "-Party names $memberCount members; a party has 1 to 4" }

if (-not (Test-Path $exe)) { throw "no build at $exe - run build.cmd $Config first" }
# THIS build's exe only: another worktree's game is a different process with its
# own log, and everything below addresses the instance this script launched.
if (Get-Process Dungeon -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
	throw 'Dungeon.exe is already running - close it (this test drives its own instance)'
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class AllocTestWin {
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
	[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
	public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

# A mouse message at client pixel (x, y): WM_MOUSEMOVE first, so the game's
# pointer is where the button lands, then the down/up pair (none for a hover).
function Send-Mouse([int]$x, [int]$y, [uint32]$down = 0, [uint32]$up = 0, [int]$wparam = 0) {
	$l = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
	[AllocTestWin]::PostMessage($hwnd, 0x200, [IntPtr]0, $l) | Out-Null
	Start-Sleep -Milliseconds 150
	if ($down -ne 0) {
		[AllocTestWin]::PostMessage($hwnd, $down, [IntPtr]$wparam, $l) | Out-Null
		Start-Sleep -Milliseconds 60
		[AllocTestWin]::PostMessage($hwnd, $up, [IntPtr]0, $l) | Out-Null
		Start-Sleep -Milliseconds 250
	}
}

# `itemdetails status`'s open count (needs logecho on and the console open).
# A left-button drag in client pixels: press, a run of moves with the button
# held (wparam MK_LBUTTON), release - what a player's hand sends. With Ctrl
# held through it by default, since a panel only arranges under Ctrl; -NoCtrl
# is the plain drag that must NOT move one.
function Send-Drag([int]$x0, [int]$y0, [int]$x1, [int]$y1, [int]$steps = 10, [switch]$NoCtrl) {
	$at = { param($x, $y) [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
	if (-not $NoCtrl) {
		[AllocTestWin]::PostMessage($hwnd, 0x100, [IntPtr]0x11, [IntPtr]1) | Out-Null
		Start-Sleep -Milliseconds 60
	}
	[AllocTestWin]::PostMessage($hwnd, 0x200, [IntPtr]0, (& $at $x0 $y0)) | Out-Null
	Start-Sleep -Milliseconds 150
	[AllocTestWin]::PostMessage($hwnd, 0x201, [IntPtr]1, (& $at $x0 $y0)) | Out-Null
	Start-Sleep -Milliseconds 80
	for ($i = 1; $i -le $steps; $i++) {
		$x = [int]($x0 + ($x1 - $x0) * $i / $steps); $y = [int]($y0 + ($y1 - $y0) * $i / $steps)
		[AllocTestWin]::PostMessage($hwnd, 0x200, [IntPtr]1, (& $at $x $y)) | Out-Null
		Start-Sleep -Milliseconds 40
	}
	Start-Sleep -Milliseconds 80
	[AllocTestWin]::PostMessage($hwnd, 0x202, [IntPtr]0, (& $at $x1 $y1)) | Out-Null
	Start-Sleep -Milliseconds 100
	if (-not $NoCtrl) {
		[AllocTestWin]::PostMessage($hwnd, 0x101, [IntPtr]0x11, [IntPtr][int64]0xC0000001) | Out-Null
	}
	Start-Sleep -Milliseconds 200
}

# A Ctrl+click at client pixel (x, y): what presses an arranging panel's reset.
function Send-CtrlClick([int]$x, [int]$y) {
	[AllocTestWin]::PostMessage($hwnd, 0x100, [IntPtr]0x11, [IntPtr]1) | Out-Null
	Start-Sleep -Milliseconds 60
	Send-Mouse $x $y 0x201 0x202 1
	[AllocTestWin]::PostMessage($hwnd, 0x101, [IntPtr]0x11, [IntPtr][int64]0xC0000001) | Out-Null
	Start-Sleep -Milliseconds 200
}

# One trip through the closed-panels tray (-Panels): Ctrl+click the Movement
# dock's minimize button, then a plain click on its tray button. The waits cover
# a button's push (it fires ~0.12 s after the release, ui::Button).
function Invoke-TrayTrip {
	Send-CtrlClick $script:hideX $script:hideY
	Start-Sleep -Milliseconds 400
	Send-Mouse $script:trayX $script:trayY 0x201 0x202 1
	Start-Sleep -Milliseconds 500
}

# `hudpanel list`'s row for one panel (console open, logecho on around it).
function Get-PanelRow([string]$id) {
	$before = @(Select-String -Path $log -Pattern "console:   $id ").Count
	Send-Key 0xC0
	Start-Sleep -Milliseconds 500
	Send-Text 'logecho on'; Send-Key 0x0D
	Send-Text 'hudpanel list'; Send-Key 0x0D
	Start-Sleep -Milliseconds 500
	Send-Text 'logecho off'; Send-Key 0x0D
	Send-Key 0xC0
	Start-Sleep -Milliseconds 400
	$rows = @(Select-String -Path $log -Pattern "console:   $id ")
	if ($rows.Count -le $before) { throw "the console never listed the $id panel" }
	return $rows[-1].Line
}

# The last line matching $pattern after sending $command (console open, logecho
# on), minus the log prefix.
function Get-ConsoleAnswer([string]$command, [string]$pattern) {
	$before = @(Select-String -Path $log -Pattern $pattern -SimpleMatch).Count
	Send-Text $command; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern $pattern -SimpleMatch)
		if ($lines.Count -gt $before) { return $lines[-1].Line -replace '^.*console: ', '' }
		Start-Sleep -Milliseconds 200
	}
	throw "the console never answered ``$command``"
}

# One -All cycle, starting with the sheet up and the console shut: its "All"
# button opens the party window (on the sheet's tab), every tab stone is
# clicked with a hover over each card after it, Esc closes the window, and a
# click on member 0's portrait brings the sheet back. Ends as it began.
function Invoke-AllCycle {
	Send-Click $script:allX $script:allY
	Start-Sleep -Milliseconds 500
	foreach ($i in 1, 2, 3, 4, 0) {
		Send-Click $script:stones[$i].X $script:stones[$i].Y
		Start-Sleep -Milliseconds 250
		foreach ($p in $script:cardPoints) { Send-Mouse $p.X $p.Y }
	}
	Send-Key 0x1B
	Start-Sleep -Milliseconds 300
	Send-Click $script:portraitX $script:portraitY
	Start-Sleep -Milliseconds 500
}

function Get-DetailOpens {
	$before = @(Select-String -Path $log -Pattern 'item details: .* opens=').Count
	Send-Text 'itemdetails status'; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern 'item details: .* opens=(\d+)')
		if ($lines.Count -gt $before) { return [int]$lines[-1].Matches[0].Groups[1].Value }
		Start-Sleep -Milliseconds 200
	}
	throw 'the console never answered `itemdetails status`'
}

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

# Asks the console for the encounter tally and returns one numeric field of the
# NEW line it prints (needs logecho on). Counting the lines first is what stops
# it reading the previous answer back.
function Get-TallyField([string]$field) {
	$before = @(Select-String -Path $log -Pattern 'TALLY ').Count
	Send-Text 'tally'; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(10)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern 'TALLY ')
		if ($lines.Count -gt $before) {
			$script:lastTally = $lines[-1].Line -replace '^.*TALLY ', 'TALLY '
			if ($lines[-1].Line -match "\b$field=([0-9.]+)") { return [double]$Matches[1] }
			throw "tally printed no '$field': $($lines[-1].Line)"
		}
		Start-Sleep -Milliseconds 200
	}
	throw 'the console never answered `tally` - is logecho on?'
}

# `glass`'s frames= field: main-scene frames that drew any glass since launch
# (needs logecho on and the console open). Counts lines first, like the tally.
function Get-GlassFrames {
	$pattern = 'console: glass queued='
	$before = @(Select-String -Path $log -Pattern $pattern -SimpleMatch).Count
	Send-Text 'glass'; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(10)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern $pattern -SimpleMatch)
		if ($lines.Count -gt $before) {
			if ($lines[-1].Line -match '\bframes=(\d+)') { return [int]$Matches[1] }
			throw "glass printed no frames=: $($lines[-1].Line)"
		}
		Start-Sleep -Milliseconds 200
	}
	throw 'the console never answered `glass` - is logecho on?'
}

# Throws unless the party stands on x,z facing north (asks `pos`; needs logecho
# on). -Impact's whole geometry hangs on it: a `tp` or `face` swallowed by a
# busy console leaves the party firing somewhere else, and the barrage then
# measures bolts expiring into a far wall.
function Assert-PartyAt([int]$x, [int]$z) {
	$want = "console: $x,$z facing north"
	$before = @(Select-String -Path $log -Pattern $want -SimpleMatch).Count
	Send-Text 'pos'; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		if (@(Select-String -Path $log -Pattern $want -SimpleMatch).Count -gt $before) { return }
		Start-Sleep -Milliseconds 200
	}
	$got = Select-String -Path $log -Pattern 'console: \d+,\d+ facing ' | Select-Object -Last 1
	throw "the party is not at $x,$z facing north (pos: $(if ($got) { $got.Line } else { 'no answer' }))"
}

# Goes to eval_arena, freezes its monsters and heals the party (needs logecho on
# and the console open). -Impact and -Items both want its open floor; -Hand
# names another level, for its wall torch.
function Enter-FrozenArena([string]$Stem = 'eval_arena') {
	# The console refuses commands while the level loads; a NEW "Level
	# ready" line is the moment it will take them again. NEW, counted from
	# before the goto: when the landing page Continues an eval save, the
	# game has ALREADY printed one for eval_arena, the wait matched it at
	# once, and `tp` and `face` were typed into the reload and refused - the
	# party then fired the whole barrage the wrong way.
	$readyPattern = "^\[info \] Level ready: $Stem"
	$readyBefore = @(Select-String -Path $log -Pattern $readyPattern).Count
	Send-Text "goto $Stem"; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds($LoadTimeoutSec)
	while (@(Select-String -Path $log -Pattern $readyPattern).Count -le $readyBefore) {
		if ($proc.HasExited) { throw "the game exited during the arena load (code $($proc.ExitCode))" }
		if ((Get-Date) -gt $deadline) { throw 'timed out waiting for the arena load' }
		Start-Sleep -Milliseconds 500
	}
	# FREEZE FIRST, THEN HEAL. A new game arrives on eval_arena's start
	# square among the arena's own monsters, which got two seconds to act
	# while the script typed: a fresh run found Sera down at 0 hp, and every
	# one of her 54 casts refused. (A Continue into an eval save happened
	# to arrive somewhere quieter, which is why it passed.)
	Send-Text 'freeze on'; Send-Key 0x0D
	Send-Text 'heal'; Send-Key 0x0D
	Start-Sleep -Milliseconds 800
}

# `inventory status`, minus the log prefix (needs logecho on and the console
# open): "inventory: open|closed held=<id|none> | 0: <slot> <slot> ... | 1: ...".
function Get-InventoryStatus {
	$pattern = 'console: inventory: '
	$before = @(Select-String -Path $log -Pattern $pattern -SimpleMatch).Count
	Send-Text 'inventory status'; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern $pattern -SimpleMatch)
		if ($lines.Count -gt $before) { return $lines[-1].Line -replace '^.*console: ', '' }
		Start-Sleep -Milliseconds 200
	}
	throw 'the console never answered `inventory status`'
}

# Member $m's pack slots ('-' = free), from an `inventory status` line.
function Get-PackSlots([string]$status, [int]$m) {
	if ($status -notmatch "\| ${m}:((?: [^|\s]+)*)") { throw "no member $m in: $status" }
	return @($Matches[1].Trim() -split ' ')
}

# Where item $id sits in member $m's pack.
function Get-PackSlot([string]$status, [int]$m, [string]$id) {
	$i = [array]::IndexOf((Get-PackSlots $status $m), $id)
	if ($i -lt 0) { throw "$id is not in member $m's pack: $status" }
	return $i
}

# The centre of member $m's pack slot $i in the party window, as the game
# reports it (`inventory slot`; needs logecho on, the console open and the
# window open on its Inventory tab). It used to be worked out here from the
# old window's fractions; the party window (more-ui-updates Phase 5) lays
# itself out in em, which a harness cannot see.
function Get-InventorySlotPoint([int]$m, [int]$i) {
	$pattern = "console: inventory slot $m ${i}: "
	$before = @(Select-String -Path $log -Pattern $pattern -SimpleMatch).Count
	Send-Text "inventory slot $m $i"; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern $pattern -SimpleMatch)
		if ($lines.Count -gt $before) {
			if ($lines[-1].Line -notmatch ': (\d+),(\d+) \d+x\d+$') { throw "unreadable: $($lines[-1].Line)" }
			return [pscustomobject]@{ X = [int]$Matches[1]; Y = [int]$Matches[2] }
		}
		Start-Sleep -Milliseconds 200
	}
	throw "the console never answered ``inventory slot $m $i`` (is the window open?)"
}

# A left click at client pixel (x, y).
function Send-Click([int]$x, [int]$y) { Send-Mouse $x $y 0x201 0x202 1 }

# One round trip for the item in member 0's pack slot $slot, starting with the
# party inventory window OPEN and the console shut: out of the slot onto the
# cursor; a floor click, the drop; a second, the lift; then the item put back.
# The window is a NON-MODAL floating window (ui-panels P3b), so it stays open
# throughout and the floor below it takes the clicks directly. Ends as it began.
function Invoke-ItemRoundTrip($slot) {
	Send-Click $slot.X $slot.Y
	Send-Click $script:floorX $script:floorY
	Send-Click $script:floorX $script:floorY
	Send-Click $slot.X $slot.Y
}

# `sheet status`'s pack-row line (needs logecho on, the console open and the
# sheet up): the row (one entry per pack-row square, '-' = none), the selected
# square, its slot count and the equips counted so far.
function Get-SheetPacks {
	$pattern = 'console: sheet packs: '
	$before = @(Select-String -Path $log -Pattern $pattern -SimpleMatch).Count
	Send-Text 'sheet status'; Send-Key 0x0D
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern $pattern -SimpleMatch)
		if ($lines.Count -gt $before) {
			$line = $lines[-1].Line -replace '^.*console: sheet packs: ', ''
			if ($line -notmatch '^(.*) selected=(\d+) slots=(\d+) equips=(\d+)$') {
				throw "unreadable sheet packs line: $line"
			}
			return [pscustomobject]@{
				Row = @($Matches[1].Trim() -split ' ')
				Selected = [int]$Matches[2]; Slots = [int]$Matches[3]
				Equips = [int]$Matches[4]; Line = $line
			}
		}
		Start-Sleep -Milliseconds 200
	}
	throw 'the console never answered `sheet status`'
}

# Centres on the sheet's Inventory tab, as window fractions measured from the
# -Sheet run's calibration (backpack slots 3 and 4 at 0.6675 / 0.72 of the
# width, 0.5033 of the height): a slot step of 0.0525 of the width, and the
# pack row 0.161 of the sheet BODY above the grid (CharacterSheetLayout.h:
# kPackY - kPackRowY), the body being 0.62 of the window's height (kBodyH).
function Get-SheetGridPoint([int]$i) {
	return [pscustomobject]@{
		X = [int]($script:clientW * (0.51 + ($i % 6) * 0.0525))
		Y = [int]($script:clientH * (0.5033 + [math]::Floor($i / 6) * 0.147 * 0.62))
	}
}
function Get-SheetPackRowPoint([int]$i) {
	return [pscustomobject]@{
		X = [int]($script:clientW * (0.51 + $i * 0.0525))
		Y = [int]($script:clientH * (0.5033 - 0.161 * 0.62))
	}
}

# One numeric field of the tally line Get-TallyField last read.
function Get-LastTallyField([string]$field) {
	if ($script:lastTally -match "\b$field=([0-9.]+)") { return [double]$Matches[1] }
	throw "tally printed no '$field': $script:lastTally"
}

# The three things -Impact must see happen, from one fresh `tally`.
function Get-ImpactCounts {
	Get-TallyField 'bolthits' | Out-Null
	return [pscustomobject]@{
		Hits = Get-LastTallyField 'bolthits'
		Expired = Get-LastTallyField 'expired'
		Blasts = Get-LastTallyField 'blasts'
	}
}

Remove-Item $log -ErrorAction SilentlyContinue
Write-Host "launching $exe"
$proc = Start-Process -FilePath $exe -WorkingDirectory $bin -ArgumentList '-project', 'dungeon-demo' -PassThru
$hwnd = [IntPtr]::Zero
$code = 1
try {
	# The boot queue's table means the menu is up and the window exists.
	Wait-ForLog '--- load: ' $LoadTimeoutSec 'the boot load' | Out-Null
	$proc.Refresh()
	$hwnd = $proc.MainWindowHandle
	if ($hwnd -eq [IntPtr]::Zero) { throw 'the game has no main window' }

	# START A NEW GAME THROUGH THE CONSOLE, not the landing page. Enter there is
	# the FIRST entry, which is Continue whenever a loadable save exists - and
	# the eval suites and other sessions leave saves behind in the one shared
	# Documents\DungeonSaves. So the run measured whichever save was newest, and
	# one whose level the world already held printed neither line waited for
	# below, timing the run out (portraits branch, 2026-10-02). `newgame` calls
	# the menu entry's own callback (Game_DevEval.cpp), so this is the same new
	# game whatever the menu holds. logecho first, so the retry can see a
	# command land.
	Write-Host 'starting a new game'
	Send-Key 0xC0
	Start-Sleep -Milliseconds 500
	$started = $false
	for ($try = 1; $try -le 10 -and -not $started; $try++) {
		Send-Text 'logecho on'; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		$started = [bool](Select-String -Path $log -Pattern 'console: > logecho on' -EA SilentlyContinue)
	}
	if (-not $started) { throw 'the console never accepted a command on the title screen' }
	if ($Party) {
		Write-Host "  with a created party of $memberCount"
		Send-Text "newparty $Party"; Send-Key 0x0D
	} else {
		Send-Text 'newgame'; Send-Key 0x0D
	}
	Start-Sleep -Milliseconds 300
	Send-Key 0xC0
	# NOT 'Game loaded:' - since the world loads on demand that line comes from
	# a load TASK, before the starting level's own load has begun, and every
	# console command typed then is refused as "still loading". A level load
	# ends with 'Level ready:'; a new game that lands without one (the world
	# map, or a level already in memory) says 'New game started'. And Enter
	# CONTINUES whenever a save exists (the eval suites leave them behind); a
	# save of a level already in memory loads inline and says only 'Loaded game
	# from', which this once waited past for its whole timeout.
	$ready = Wait-ForLog '^\[info \] (Level ready: |New game started|Loaded game from )' $LoadTimeoutSec 'the dungeon load'
	Write-Host "  $($ready -replace '^\[info \] ', '')"
	Start-Sleep -Milliseconds 500
	# A REFUSED `newparty` still ends in a game - the default four's - so the run
	# would measure the wrong party and PASS. The command's own line is the proof.
	if ($Party -and -not (Select-String -Path $log -Pattern "console: new game with a party of $memberCount\b" -EA SilentlyContinue)) {
		throw "newparty did not build the party of $memberCount (see dungeon.log)"
	}

	# AND WAIT UNTIL THE CONSOLE ANSWERS before relying on it. The first level
	# being ready still does not mean commands are live: Enter on the landing
	# page is Continue whenever a loadable save exists (the eval suites leave
	# them behind), a save naming another level stages a SECOND load after the
	# first 'Level ready:', and the console refuses commands while it runs.
	# tools\InGameTest.ps1 learned the same thing; this is the same answer: open
	# the console once, retry a harmless command until the log echoes it, then
	# shut it so everything below starts from a closed console as before.
	# (`$answered`, not `$ready`: -Melee reads the party's cell off $ready.)
	Start-Sleep -Seconds 2
	Send-Key 0xC0
	Start-Sleep -Milliseconds 500
	$answered = $false
	for ($try = 1; $try -le 10 -and -not $answered; $try++) {
		Send-Text 'logecho on'; Send-Key 0x0D
		Start-Sleep -Seconds 2
		$answered = [bool](Select-String -Path $log -Pattern 'console: > logecho on' -EA SilentlyContinue)
	}
	if (-not $answered) { throw 'the console never accepted a command' }
	Send-Text 'logecho off'; Send-Key 0x0D
	Start-Sleep -Milliseconds 300
	Send-Key 0xC0 # closed again: each path below opens it for itself
	Start-Sleep -Milliseconds 400

	# -Minimal: the whole run under the Minimal HUD layout (the party cards).
	# FIRST, before any mode sets its scene up: the switch REBUILDS the HUD, which
	# would close a spellbook -Cast had opened. A first time, out here before the
	# window; the verdict below refuses a PASS unless the cards were actually up.
	if ($Minimal) {
		Write-Host 'switching the HUD to the Minimal layout'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'hudpanel layout minimal'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 600
	}

	if ($Wounded) {
		Write-Host 'wounding the party so the regeneration path actually runs'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		# Echo to the log, because a PASS here is only worth anything if the
		# wounding actually happened - and a swallowed keystroke (0xC0 toggles,
		# so one stray press eats every command after it) would leave a run that
		# looks exactly like a clean one. The `party` line below is the evidence.
		Send-Text 'logecho on'; Send-Key 0x0D
		foreach ($m in 0..($memberCount - 1)) {
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

	if ($Melee) {
		if ($ready -notmatch 'Level ready: \S+ at (\d+),(\d+)') {
			throw 'the new game did not open in a level - there is no party cell to fight beside'
		}
		$px = [int]$Matches[1]; $pz = [int]$Matches[2]
		Write-Host "putting a $MeleeMonster (x$MeleeStrength) beside the party at $px,$pz"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		# The first orthogonal neighbour `spawn` accepts (it refuses a wall or a
		# taken cell, and says so) - no cell of any one level is hardcoded. It is
		# spawned FACING the party (+z is south), as arena.eval does.
		$spawnedAt = $null
		foreach ($d in @(@(1, 0, 'w'), @(-1, 0, 'e'), @(0, 1, 'n'), @(0, -1, 's'))) {
			$x = $px + $d[0]; $z = $pz + $d[1]
			Send-Text "spawn $MeleeMonster $x $z $($d[2]) $MeleeStrength"; Send-Key 0x0D
			Start-Sleep -Milliseconds 400
			if (Select-String -Path $log -Pattern "spawned $MeleeMonster at $x,$z" -Quiet) {
				$spawnedAt = "$x,$z"; break
			}
		}
		if (-not $spawnedAt) { throw "no cell beside $px,$pz would take a $MeleeMonster" }
		Write-Host "  spawned at $spawnedAt; waiting for its first blow to land (warm-up)"
		Send-Text 'tally reset'; Send-Key 0x0D
		$deadline = (Get-Date).AddSeconds(60)
		while ((Get-TallyField 'taken') -le 0) {
			if ((Get-Date) -gt $deadline) {
				Send-Text 'monsters'; Send-Key 0x0D # what it was doing, into the log
				throw 'the monster never landed a blow (its state is in dungeon.log)'
			}
			Start-Sleep -Seconds 1
		}
		# A few more swings, so each outcome's first time (a miss line, a second
		# member struck) is also warm-up rather than window.
		Start-Sleep -Seconds 4
		Send-Text 'tally reset'; Send-Key 0x0D
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	if ($Cast) {
		Write-Host "freezing the world, casting a bolt, and opening member $CastMember's book"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text 'timescale 0'; Send-Key 0x0D
		Send-Text "learn $CastMember fire"; Send-Key 0x0D
		Send-Text "learn $CastMember project"; Send-Key 0x0D
		# Enough fire skill that a two-rune cast cannot fumble (Magic.cpp: 35% a rune
		# past the first, less 10% a level) - the run measures a bolt, not luck.
		Send-Text "setskill $CastMember fire 5"; Send-Key 0x0D
		# Kenaz Tiwaz: Kenaz alone is a hand spell now, with nothing in flight.
		Send-Text "cast $CastMember 0 fire project"; Send-Key 0x0D
		Send-Text "book $CastMember"; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
		# Refuse to measure unless both actually happened: a swallowed key or a
		# fizzled cast would otherwise leave a run that looks exactly like a
		# clean one. (A book refusal is printed as a Refuse, not 'book open'.)
		if (-not (Select-String -Path $log -Pattern 'console: cast away' -Quiet)) {
			throw 'the cast did not go off - no bolt is in flight to measure'
		}
		if (-not (Select-String -Path $log -Pattern 'console: book open: ' -Quiet)) {
			throw 'the spellbook did not open'
		}
	}

	if ($Impact) {
		Write-Host 'going to eval_arena for an open field of fire'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena
		# The room is open from 1,1 to 26,22 (28x24 with a solid border). The
		# monster stands THREE squares north of the party: a bolt's `range` is
		# in METRES (flame's 8 m is 3.2 squares), so at five every bolt went
		# out in open air short of it. In an open room a Fire Burst's force is
		# spent a square or two out, so three is also past its reach back.
		$tx = 14; $tz = 14; $px = 14; $pz = 17
		Send-Text "tp $px $pz"; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Assert-PartyAt $px $pz
		Send-Text "spawn $ImpactMonster $tx $tz s $ImpactStrength"; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		if (-not (Select-String -Path $log -Pattern "spawned $ImpactMonster at $tx,$tz" -Quiet)) {
			throw "the arena would not take a $ImpactMonster at $tx,$tz"
		}
		# Members 0 and 1 cast down OPPOSITE lanes (front-left, front-right),
		# so whichever lane the monster's slot is not in flies past and
		# expires; member 2 throws the blast.
		# Single-target bolts in the two lanes (Puff of Flame is a hand spell now).
		Send-Text "autocast 0 waterbolt $ImpactEvery"; Send-Key 0x0D
		Send-Text 'autocast 1 waterbolt'; Send-Key 0x0D
		Send-Text 'autocast 2 firebolt_burst'; Send-Key 0x0D
		Send-Text 'tally reset'; Send-Key 0x0D
		Write-Host "  casting at a $ImpactMonster (x$ImpactStrength) from $px,$pz; waiting for a hit, an expiry and a blast (warm-up)"
		$deadline = (Get-Date).AddSeconds(60)
		while ($true) {
			$c = Get-ImpactCounts
			if ($c.Hits -gt 0 -and $c.Expired -gt 0 -and $c.Blasts -gt 0) { break }
			if ((Get-Date) -gt $deadline) {
				# Into the log: where everything stands, and what each caster's
				# attempts came to (a rotation entry that only fails says so).
				Send-Text 'monsters'; Send-Key 0x0D
				Send-Text 'autocast'; Send-Key 0x0D
				Send-Text 'party'; Send-Key 0x0D
				Start-Sleep -Milliseconds 500
				throw "the warm-up never saw all three (last: $script:lastTally)"
			}
			Start-Sleep -Seconds 1
		}
		# EVERY CASTER MUST HAVE CAST. The three counts above can all arrive
		# with one entry of the rotation refused throughout (a downed member),
		# and then the lane pattern the window depends on is not the one this
		# header describes. Failures alone prove nothing - a fumble is one.
		$castPattern = 'console:   member \d+ casts \S+: \d+ cast, \d+ failed'
		$castBefore = @(Select-String -Path $log -Pattern $castPattern).Count
		Send-Text 'autocast'; Send-Key 0x0D
		Start-Sleep -Milliseconds 800
		$castRows = @(Select-String -Path $log -Pattern $castPattern) | Select-Object -Skip $castBefore
		if ($castRows.Count -ne 3) { throw "``autocast`` listed $($castRows.Count) entries, not 3" }
		foreach ($r in $castRows) {
			if ($r.Line -match ': 0 cast,') {
				Send-Text 'party'; Send-Key 0x0D
				throw "a caster never cast: $($r.Line -replace '^.*console:\s+', '') (party state is in dungeon.log)"
			}
		}
		# A few more rounds, so each outcome's first time in the PROCESS (a
		# miss line, a sound's first voice, the first detonation) is warm-up
		# rather than window.
		Start-Sleep -Seconds 4
		# THEN A FRESH TARGET. What the warm-up absorbs must be a first time
		# for the process, never a first time for a MONSTER - every monster
		# in play is new once, so its first burn (an effects list growing from
		# empty) or first threat entry is a steady cost of casting, not
		# warm-up. Warming up and measuring on one target hid exactly that. So
		# the party steps four squares west, out of the line of the worn-in
		# target, and a new one is spawned three squares ahead of it, never
		# touched by anything before the window opens.
		#
		# AND NOTHING FIRES AT IT UNTIL THE WINDOW OPENS, so the rotation is
		# HELD FIRST. The guard skips a 120-frame warm-up after the console
		# shuts, and at a cast every 0.4 s the fresh target's first hits landed
		# in that gap - or, with the hold typed after the move, in the half
		# second the script spent typing it. Both passed a run that should have
		# failed. A held rotation is released by alloctest's first ARMED frame,
		# which also restarts the tally, so the count read afterwards is the
		# window's. The pause lets bolts already in flight land on the old one.
		Send-Text 'autocast hold'; Send-Key 0x0D
		Start-Sleep -Seconds 1
		$px -= 4; $tx -= 4
		Send-Text "tp $px $pz"; Send-Key 0x0D
		Assert-PartyAt $px $pz
		Send-Text "spawn $ImpactMonster $tx $tz s $ImpactStrength"; Send-Key 0x0D
		Start-Sleep -Milliseconds 300
		if (-not (Select-String -Path $log -Pattern "spawned $ImpactMonster at $tx,$tz" -Quiet)) {
			throw "the arena would not take a fresh $ImpactMonster at $tx,$tz"
		}
		# Refuse unless it really is untouched: `monsters` lists a live effect
		# in brackets after the hp, and a burn caught early is exactly the
		# thing this step exists to keep out of the warm-up.
		Start-Sleep -Milliseconds 500
		$before = @(Select-String -Path $log -Pattern "console:   $ImpactMonster @ $tx,$tz ").Count
		Send-Text 'monsters'; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		$row = @(Select-String -Path $log -Pattern "console:   $ImpactMonster @ $tx,$tz ")
		if ($row.Count -le $before) { throw "``monsters`` did not list the fresh $ImpactMonster" }
		# (Past the "[info ]" the log line opens with, which is a bracket too.)
		$listed = $row[-1].Line -replace '^.*console: ', ''
		if ($listed -match '\[') { throw "the fresh target was touched before the window: $listed" }
		Write-Host "  fresh $ImpactMonster at $tx,$tz, party at $px,$pz, untouched"
		# AND A FRESH CRATE beside it, in the Fire Burst's ring: a blast leaves a
		# piece of dungeon alight, and its burn ticks every frame until it breaks
		# (TickBreakables). Fresh for the same reason as the target - a crate's
		# first burn and its break are a steady cost of fighting beside one. It is
		# off the bolts' column, so it never stands between a bolt and its mark.
		$cx = $tx + 1
		Send-Text "editor place decorations crate $cx $tz"; Send-Key 0x0D
		Send-Text 'mappage close'; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		if (-not (Select-String -Path $log -Pattern "editor place: crate at $cx,$tz" -Quiet)) {
			throw "the arena would not take a crate at $cx,$tz"
		}
		Send-Text "breakables $cx $tz"; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		$crate = @(Select-String -Path $log -Pattern "console:   decoration crate @ $cx,$tz ")
		if ($crate.Count -eq 0) { throw "the crate at $cx,$tz is not breakable" }
		Write-Host "  fresh $($crate[-1].Line -replace '^.*console:\s+', '')"
		# AND A LIT BRAZIER THAT BREAKS INSIDE THE WINDOW: a wrecked fixture puts
		# its fire out and thins the haze it fed (DouseFixture) - the haze texture
		# is rewritten in place, which used to be a whole new texture built
		# mid-frame. Braziers shrug off fire, so it gets a POISON (earth) that eats
		# its 30 hp in about five seconds: applied as the very last thing before
		# the console shuts, so the break lands a few seconds into the window.
		$bx = $tx - 3
		Send-Text "editor place fixtures brazier $bx $tz"; Send-Key 0x0D
		Send-Text 'mappage close'; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		if (-not (Select-String -Path $log -Pattern "editor place: brazier at $bx,$tz" -Quiet)) {
			throw "the arena would not take a brazier at $bx,$tz"
		}
		Send-Text "breakables $bx $tz poison 6 30"; Send-Key 0x0D
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	# -Hand: THE HAND SPELLS (spell-updates Phase 8). A tier-1 spell changes the
	# WORLD AHEAD and the ITEMS IN HAND, not a monster: Kenaz lights a held torch
	# (an item renamed in its slot) and the wall torch ahead (a fire's state, the
	# turbidity grid refreshed in place on the GPU), Laguz fills a held waterskin
	# a step at a time and then douses that torch (a smoke effect on the fire),
	# Ansuz flares it, and Berkano conjures a pebble into an empty hand and then
	# at the feet (a floor drop). None of that is a bolt, so -Cast and -Impact
	# never reach it. crypt1's torch at 4,3 is the target, the monsters frozen;
	# `autocast` casts each from a world frame, so the window holds the casts.
	#
	# The ITEM paths happen once each before a slot is used up (a full skin
	# stops filling, a lit torch stays lit), so the warm-up runs the whole
	# rotation, then the rotation is HELD and FRESH items go back in hand - an
	# unlit torch, an empty skin, an empty hand - and the window's first armed
	# frame releases it. So the first light, both fills and a hand landing fall
	# inside the window, and every cast after them is a fire change or a drop.
	if ($Hand) {
		Write-Host "going to crypt1's wall torch for the hand spells"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena 'crypt1'
		Send-Text 'tp 4 3'; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Assert-PartyAt 4 3
		$handKit = {
			Send-Text 'equip torch 2 1'; Send-Key 0x0D
			Send-Text 'equip waterskin_empty 3 1'; Send-Key 0x0D
			Send-Text 'equip none 0 0'; Send-Key 0x0D
		}
		& $handKit
		Send-Text 'learn 0 earth'; Send-Key 0x0D
		Send-Text 'learn 1 air'; Send-Key 0x0D
		Send-Text 'learn 2 fire'; Send-Key 0x0D
		Send-Text 'learn 3 water'; Send-Key 0x0D
		Send-Text 'autocast 2 flame 0.3'; Send-Key 0x0D
		Send-Text 'autocast 3 splash'; Send-Key 0x0D
		Send-Text 'autocast 1 gust'; Send-Key 0x0D
		Send-Text 'autocast 0 rock'; Send-Key 0x0D
		Write-Host '  warming the rotation up'
		Start-Sleep -Seconds 6
		$castPattern = 'console:   member \d+ casts \S+: \d+ cast, \d+ failed'
		$castBefore = @(Select-String -Path $log -Pattern $castPattern).Count
		Send-Text 'autocast hold'; Send-Key 0x0D
		Send-Text 'autocast'; Send-Key 0x0D
		Start-Sleep -Milliseconds 800
		$script:handRows = @(Select-String -Path $log -Pattern $castPattern) | Select-Object -Skip $castBefore
		if ($script:handRows.Count -ne 4) { throw "``autocast`` listed $($script:handRows.Count) entries, not 4" }
		foreach ($r in $script:handRows) {
			if ($r.Line -match ': 0 cast,') { throw "a hand spell never cast in the warm-up: $($r.Line -replace '^.*console:\s+', '')" }
		}
		Start-Sleep -Milliseconds 500
		& $handKit
		Start-Sleep -Milliseconds 300
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	if ($Sheet) {
		Write-Host 'opening the sheet with a rune and a blade in the pack'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		# The sheet is a floating window now (ui-panels P3b): the clicks below aim
		# at its DEFAULT spot and size, so put it back there first.
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		# A new party's pack holds three pieces of armour (slots 0-2), so these
		# land in slots 3 and 4 - the cells the clicks below aim at.
		Send-Text 'give rune_fire 0'; Send-Key 0x0D
		Send-Text 'give flamebrand 0'; Send-Key 0x0D
		# Skills part-way to their next level, so the Skills tab the cycle passes
		# through draws its framed progress bars (ui-bars-updates) - a new party
		# has trained nothing, and an empty tab measured nothing there.
		Send-Text 'setskill 0 blade 1.5'; Send-Key 0x0D
		Send-Text 'setskill 0 fire 2.3'; Send-Key 0x0D
		Send-Text 'setskill 0 conditioning 0.6'; Send-Key 0x0D
		Send-Text 'sheet 0'; Send-Key 0x0D
		# WARM-UP: one open of the dialog, and a moment for it to draw, bakes its
		# fonts and glyphs - a first time for the process, outside the window.
		Send-Text 'itemdetails flamebrand 1.4'; Send-Key 0x0D
		Start-Sleep -Seconds 1
		Send-Text 'itemdetails off'; Send-Key 0x0D
		$script:opensBefore = Get-DetailOpens
		if ($script:opensBefore -le 0) { throw 'the warm-up never opened the item details dialog' }
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
		if (-not (Select-String -Path $log -Pattern 'console: sheet open: ' -Quiet)) {
			throw 'the sheet did not open'
		}
		# Where the two cells are, from the window's own size (the sheet lays out
		# in fractions of it): backpack slots 3 and 4 of the default layout.
		$rc = New-Object AllocTestWin+RECT
		[AllocTestWin]::GetClientRect($hwnd, [ref]$rc) | Out-Null
		$script:runeX = [int]($rc.Right * 0.6675); $script:bladeX = [int]($rc.Right * 0.72)
		$script:slotY = [int]($rc.Bottom * 0.5033)
	}

	if ($All) {
		Write-Host 'opening the sheet, and the party window from its All button'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		# Every click below is read off the game, at the default layout.
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		Send-Text 'give rune_fire 0'; Send-Key 0x0D # an item for the Inventory cards to draw
		Send-Text 'sheet 0'; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		$line = Get-ConsoleAnswer 'sheet status' 'console: sheet all: '
		if ($line -notmatch 'sheet all: (\d+),(\d+)') { throw "unreadable: $line" }
		$script:allX = [int]$Matches[1]; $script:allY = [int]$Matches[2]
		# Member 0's portrait: the left end of the party bar, as tall as the bar.
		$before = @(Select-String -Path $log -Pattern 'console:   party ').Count
		Send-Text 'hudpanel list'; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		$rows = @(Select-String -Path $log -Pattern 'console:   party ')
		if ($rows.Count -le $before -or $rows[-1].Line -notmatch 'px (-?\d+),(-?\d+) (\d+)x(\d+)') {
			throw 'no party bar rect from `hudpanel list`'
		}
		$script:portraitX = [int]$Matches[1] + [int]([int]$Matches[4] * 0.45)
		$script:portraitY = [int]$Matches[2] + [int]([int]$Matches[4] * 0.5)
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		# The window, open, says where its stones and its cards are.
		Send-Click $script:allX $script:allY
		Start-Sleep -Milliseconds 800
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		$script:stones = @()
		for ($i = 0; $i -lt 5; $i++) {
			$line = Get-ConsoleAnswer "inventory stone $i" "console: inventory stone ${i}: "
			if ($line -notmatch ': (\d+),(\d+)$') { throw "unreadable: $line" }
			$script:stones += [pscustomobject]@{ X = [int]$Matches[1]; Y = [int]$Matches[2] }
		}
		$script:cardPoints = @()
		for ($m = 0; $m -lt $memberCount; $m++) { $script:cardPoints += Get-InventorySlotPoint $m 0 }
		Send-Text 'inventory off'; Send-Key 0x0D
		Send-Text 'sheet 0'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		# WARM-UP: one whole cycle, which also checks that every click landed -
		# the window opened, closed, and the portrait brought the sheet back.
		Invoke-AllCycle
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		$status = Get-InventoryStatus
		$sheetLine = Get-ConsoleAnswer 'sheet status' 'console: sheet: '
		if ($status -notmatch 'opens=(\d+)' -or [int]$Matches[1] -lt 2 -or
			$status -notmatch '^inventory: closed' -or $sheetLine -notmatch '^sheet: open') {
			throw "the warm-up cycle went wrong ($status; $sheetLine) - All $($script:allX),$($script:allY), " +
				"portrait $($script:portraitX),$($script:portraitY)"
		}
		$status -match 'opens=(\d+)' | Out-Null
		$script:allOpensBefore = [int]$Matches[1]
		Write-Host "  warm-up cycle ok (All at $($script:allX),$($script:allY))"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	if ($Panels) {
		Write-Host 'resetting the HUD layout and warming the panel drags up'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		Send-Text 'hudpanel lock off'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 600
		# Where the grabs are, read off the docks' OWN rects (`hudpanel list`), so
		# a change to the default layout - the party bar grew taller once and the
		# Movement dock slid down under a fixed grab point - cannot make the drags
		# miss: a point in the Movement dock's title row, and just inside the
		# Hands dock's bottom-right corner (the resize wedge).
		$rc = New-Object AllocTestWin+RECT
		[AllocTestWin]::GetClientRect($hwnd, [ref]$rc) | Out-Null
		$moveRect = Get-PanelRow 'move'
		$handsRect = Get-PanelRow 'hands'
		if ($moveRect -notmatch 'px (-?\d+),(-?\d+) (\d+)x(\d+)') { throw "no move dock rect: $moveRect" }
		$script:moveX = [int]$Matches[1] + [int]([int]$Matches[3] * 0.25)
		$script:moveY = [int]$Matches[2] + 12
		if ($handsRect -notmatch 'px (-?\d+),(-?\d+) (\d+)x(\d+)') { throw "no hands dock rect: $handsRect" }
		$script:gripX = [int]$Matches[1] + [int]$Matches[3] - 4
		$script:gripY = [int]$Matches[2] + [int]$Matches[4] - 4
		$script:pullX = $script:gripX - [int]($rc.Right * 0.0235)
		$script:pullY = $script:gripY - [int]($rc.Bottom * 0.0275)
		# Below the inventory window's default rect (0.23..0.77 down) and above the
		# log footer: a grab landing ON the window would act on its slots instead.
		$script:awayX = [int]($rc.Right * 0.30); $script:awayY = [int]($rc.Bottom * 0.80)
		# THE TRAY (ui-updates Phase 8): the Movement dock's MINIMIZE is the
		# top-right Ctrl button, with RESET one button to its left; the tray's
		# button for it then heads the column, right edges level, in the strip
		# the dock's default spot starts under (GameUI::DockColumnTop: padding 0.3 em,
		# a 1.4 em button - HudTray.h - and a 0.5 em gap, so the button's centre
		# is 1 em in from the dock's right and 1.5 em above its top). The button
		# side and so the em come from `hudpanel list`.
		$gripRow = @(Select-String -Path $log -Pattern 'console: hud layout .*grip (\d+)px')[-1].Line
		if ($gripRow -notmatch 'grip (\d+)px') { throw "no grip size: $gripRow" }
		$script:grip = [int]$Matches[1]
		$em = $script:grip / 1.2
		if ($moveRect -notmatch 'px (-?\d+),(-?\d+) (\d+)x(\d+)') { throw "no move dock rect: $moveRect" }
		$mLeft = [int]$Matches[1]; $mTop = [int]$Matches[2]; $mRight = $mLeft + [int]$Matches[3]
		$script:hideX = $mRight - 6; $script:hideY = $mTop + 6
		$script:resetX = $mRight - $script:grip - 7; $script:resetY = $mTop + 6
		$script:trayX = [int]($mRight - 1.0 * $em); $script:trayY = [int]($mTop - 1.5 * $em)
		# WARM-UP: one drag and one pull outside the window - the pull's new scale
		# bakes a font size, a first time for the process - and one trip through
		# the tray. Then back to default.
		Send-Drag $script:moveX $script:moveY $script:awayX $script:awayY
		Send-Drag $script:awayX $script:awayY $script:moveX $script:moveY
		Send-Drag $script:gripX $script:gripY $script:pullX $script:pullY
		Invoke-TrayTrip
		Start-Sleep -Milliseconds 400
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		# The party window stays open through the window, so its draw (every
		# card, every frame) is measured too. Parked small at the top-left, clear
		# of both grabs above and of the away point (its default size covers it).
		Send-Text 'hudpanel inventory 0.05 0.02 0.6'; Send-Key 0x0D
		Send-Text 'inventory'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
	}

	if ($Items) {
		Write-Host "going to eval_arena: $WarmItem warms up, $MeasureItem is measured"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena
		# Facing north across open floor at 14,17 (the room is open from 1,1 to
		# 26,22), so the square ahead takes a drop. WALKED onto, not teleported:
		# `tp` reveals nothing, and a drop only lands on a square the party has
		# SEEN - an unseen one falls back to its feet, out of view, and the first
		# run of this lifted nothing for exactly that reason. A step reveals the
		# eight squares round where it lands.
		Send-Text 'tp 14 18'; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		Send-Key 0x57 # W: one step forward
		Start-Sleep -Seconds 1
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		Assert-PartyAt 14 17
		# Whoever has two free pack slots carries them: a Continued eval save can
		# leave any member's pack full.
		$status = Get-InventoryStatus
		$member = -1
		for ($m = 0; $m -lt $memberCount -and $member -lt 0; $m++) {
			if (@(Get-PackSlots $status $m | Where-Object { $_ -eq '-' }).Count -ge 2) { $member = $m }
		}
		if ($member -lt 0) { throw "no member has two free pack slots: $status" }
		Send-Text "give $WarmItem $member"; Send-Key 0x0D
		Send-Text "give $MeasureItem $member"; Send-Key 0x0D
		Start-Sleep -Milliseconds 300
		$status = Get-InventoryStatus
		$warmSlot = Get-PackSlot $status $member $WarmItem
		$measureSlot = Get-PackSlot $status $member $MeasureItem
		Write-Host "  member $member carries $WarmItem in slot $warmSlot, $MeasureItem in slot $measureSlot"
		$rc = New-Object AllocTestWin+RECT
		[AllocTestWin]::GetClientRect($hwnd, [ref]$rc) | Out-Null
		$script:clientW = [double]$rc.Right; $script:clientH = [double]$rc.Bottom
		# The window opens (on its Inventory tab) and lays itself out before its
		# slots can be asked where they are. PARKED small at the top-left: at its
		# default size it covers the floor point below.
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		Send-Text 'hudpanel inventory 0.05 0.02 0.6'; Send-Key 0x0D
		Send-Text 'inventory'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 600
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		$script:warmPoint = Get-InventorySlotPoint $member $warmSlot
		$script:measurePoint = Get-InventorySlotPoint $member $measureSlot
		# THE FLOOR POINT, from the camera: a 70 degree vertical lens with the eye
		# 1.55 m up (kEyeHeight), level. A pixel ndc units below the centre sees
		# the floor at 1.55 / (ndc * tan 35) metres. 3.3 m is the FAR quarter of
		# the square ahead (2.5 to 3.75), far enough that the lift - which
		# samples the ray at the item's own mid-height, a rune's 0.23 m, so about
		# 2.8 m out - is still in it. Left of centre puts both in the west half.
		$ndc = (1.55 / 3.3) / [math]::Tan(35 * [math]::PI / 180)
		$script:floorX = [int]($script:clientW * 0.40)
		$script:floorY = [int]($script:clientH * (0.5 + $ndc / 2))
		Send-Text 'tally reset'; Send-Key 0x0D
		Send-Text 'inventory'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Seconds 1
		# WARM-UP, and the check that every click lands where it is aimed: a
		# first drop's sound voice and the like are first times for the PROCESS.
		Invoke-ItemRoundTrip $script:warmPoint
		Start-Sleep -Milliseconds 500
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		$drops = Get-TallyField 'drops'
		$lifts = Get-LastTallyField 'lifts'
		$status = Get-InventoryStatus
		if ($drops -ne 1 -or $lifts -ne 1 -or $status -notmatch 'held=none' -or
			(Get-PackSlot $status $member $WarmItem) -ne $warmSlot) {
			throw "the warm-up round trip went wrong (drops=$drops lifts=$lifts; $status) - " +
				"slot $($script:warmPoint.X),$($script:warmPoint.Y), floor $($script:floorX),$($script:floorY)"
		}
		Write-Host "  warm-up round trip ok (floor point $($script:floorX),$($script:floorY))"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	# -Throw (ui-updates Phase 10): THROWING, by clicks - a lift off the floor,
	# a click above the floor's horizon that throws it, the flight, the wall it
	# hits and the landing. The party stands one square back from eval_arena's
	# north wall (14,2 facing north), so every throw hits the wall and comes
	# down in the square ahead - in its FIRST free quarter, slot 0, which facing
	# north is the far-left one: exactly -Items' floor point. So the loop needs
	# no feedback: lift there, throw high, wait out throw_interval, again.
	if ($Throw) {
		Write-Host 'going to eval_arena''s north wall with a rock'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena
		Send-Text 'tp 14 3'; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		Send-Key 0x57 # W: one step forward, which reveals the squares round it
		Start-Sleep -Seconds 1
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		Assert-PartyAt 14 2
		$rc = New-Object AllocTestWin+RECT
		[AllocTestWin]::GetClientRect($hwnd, [ref]$rc) | Out-Null
		$script:clientW = [double]$rc.Right; $script:clientH = [double]$rc.Bottom
		# The floor point as -Items works it out (see there), and a point well
		# above the horizon: the ray never meets the floor, so it is a throw.
		$ndc = (1.55 / 3.3) / [math]::Tan(35 * [math]::PI / 180)
		$script:floorX = [int]($script:clientW * 0.40)
		$script:floorY = [int]($script:clientH * (0.5 + $ndc / 2))
		$script:skyX = [int]($script:clientW * 0.50)
		$script:skyY = [int]($script:clientH * 0.30)
		# The first rock comes from nowhere: the leader throws one, and it lands
		# where every later one will.
		Send-Text 'tally reset'; Send-Key 0x0D
		Send-Text 'throw rock'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 1500
		# WARM-UP, and the check that the loop's clicks land: one whole cycle by
		# hand. A first throw's sound voice and the like are first times for the
		# PROCESS, not steady costs.
		Send-Click $script:floorX $script:floorY
		Start-Sleep -Milliseconds 300
		Send-Click $script:skyX $script:skyY
		Start-Sleep -Milliseconds 1500
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		$throws = Get-TallyField 'throws'
		$lifts = Get-LastTallyField 'lifts'
		$landed = (Get-LastTallyField 'throwlandings') + (Get-LastTallyField 'throwstrikes')
		if ($throws -ne 2 -or $lifts -ne 1 -or $landed -ne 2) {
			throw "the warm-up throw went wrong (throws=$throws lifts=$lifts landed=$landed) - " +
				"floor $($script:floorX),$($script:floorY), sky $($script:skyX),$($script:skyY)"
		}
		Write-Host "  warm-up throw ok (floor point $($script:floorX),$($script:floorY))"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	# -Glass: eval_arena, the party one square back from the north wall, and the
	# glass kind on the square between them, in full view.
	if ($Glass) {
		Write-Host "standing in front of a $GlassKind in eval_arena"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Enter-FrozenArena
		Send-Text 'tp 14 3'; Send-Key 0x0D
		Send-Text 'face n'; Send-Key 0x0D
		Start-Sleep -Milliseconds 400
		Assert-PartyAt 14 3
		Send-Text "editor place $GlassCategory $GlassKind 14 2"; Send-Key 0x0D
		Start-Sleep -Milliseconds 600
		$placed = Select-String -Path $log -Pattern "console: editor place: $GlassKind at 14,2" -SimpleMatch -Quiet
		if (-not $placed) { throw "the $GlassKind was not placed at 14,2 (see the message log)" }
		Send-Text 'editor off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console
		Start-Sleep -Milliseconds 400
		Send-Key 0x4D # M closes the map overlay the editor left open
		Start-Sleep -Seconds 1
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		$script:glassBefore = Get-GlassFrames
		Start-Sleep -Seconds 1
		$warm = (Get-GlassFrames) - $script:glassBefore
		if ($warm -le 0) { throw "no frame drew glass with the $GlassKind in view - is it marked transparent?" }
		$script:glassBefore = Get-GlassFrames
		Write-Host "  glass in view ($warm frames drew it in the warm-up second)"
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
	}

	if ($Packs) {
		Write-Host 'putting an ammo pouch in the pack row and a herb pouch on the cursor'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$rc = New-Object AllocTestWin+RECT
		[AllocTestWin]::GetClientRect($hwnd, [ref]$rc) | Out-Null
		$script:clientW = [double]$rc.Right; $script:clientH = [double]$rc.Bottom
		# Whoever has two free slots in the pack on show takes both bags (a
		# Continued eval save can leave any member's pack full).
		$status = Get-InventoryStatus
		$member = -1
		for ($m = 0; $m -lt $memberCount -and $member -lt 0; $m++) {
			if (@(Get-PackSlots $status $m | Where-Object { $_ -eq '-' }).Count -ge 2) { $member = $m }
		}
		if ($member -lt 0) { throw "no member has two free pack slots: $status" }
		Send-Text "give ammo_pouch $member"; Send-Key 0x0D
		Send-Text "give herb_pouch $member"; Send-Key 0x0D
		Start-Sleep -Milliseconds 300
		$status = Get-InventoryStatus
		$ammoAt = Get-SheetGridPoint (Get-PackSlot $status $member 'ammo_pouch')
		$herbAt = Get-SheetGridPoint (Get-PackSlot $status $member 'herb_pouch')
		Send-Text "sheet $member"; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		$p = Get-SheetPacks
		$shown = $p.Selected
		$free = [array]::IndexOf($p.Row, '-')
		if ($free -lt 0) { throw "member $member has no empty pack-row square: $($p.Line)" }
		$script:bagAt = Get-SheetPackRowPoint $free
		$shownAt = Get-SheetPackRowPoint $shown
		Write-Host "  member $member, bags into pack-row square $free (the pack on show is $shown)"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 600
		Send-Click $ammoAt.X $ammoAt.Y                 # the ammo pouch onto the cursor
		Send-Click $script:bagAt.X $script:bagAt.Y     # into the empty square (0 -> 8)
		Send-Click $shownAt.X $shownAt.Y                 # back to the pack holding the herb pouch
		Send-Click $herbAt.X $herbAt.Y                 # the herb pouch onto the cursor
		# WARM-UP: one swap pair, first times for the process.
		Send-Click $script:bagAt.X $script:bagAt.Y
		Send-Click $script:bagAt.X $script:bagAt.Y
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		$p = Get-SheetPacks
		$status = Get-InventoryStatus
		if ($p.Equips -ne 3 -or $p.Row[$free] -ne 'ammo_pouch' -or $p.Slots -ne 8 -or
			$status -notmatch 'held=herb_pouch ') {
			throw "the pack setup went wrong ($($p.Line); $status) - grid $($ammoAt.X),$($ammoAt.Y), " +
				"row $($script:bagAt.X),$($script:bagAt.Y)"
		}
		$script:equipsBefore = $p.Equips
		Write-Host '  setup and a warm-up swap pair ok'
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0 # close the console again; alloctest reopens it below
		Start-Sleep -Milliseconds 400
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

	# -Pause: Esc into the pause menu and Esc back out, a few times, while the
	# window runs. Each wait clears the console close / resume plus the guard's
	# 120-frame warm-up, so the Esc lands in an ARMED frame; paused frames and
	# the warm-up after a resume are not armed and cost the window nothing.
	# -Walk: turn by key, a full circle right and one left, twice, while the
	# window runs. TURNS, not steps, so the party ends where it began and nothing
	# it might walk into (a wall bump, a stair) muddies what is measured: a turn
	# is an Act like any move, and it presses a pad stone the same way.
	if ($Walk) {
		for ($cycle = 1; $cycle -le 2; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			for ($t = 0; $t -lt 4; $t++) { Send-Key 0x45; Start-Sleep -Milliseconds 350 } # E
			for ($t = 0; $t -lt 4; $t++) { Send-Key 0x51; Start-Sleep -Milliseconds 350 } # Q
		}
	}

	if ($Pause) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Send-Key 0x1B # pause
			Start-Sleep -Seconds 1
			Send-Key 0x1B # and resume
		}
	}

	# -Sheet: work the sheet while the window runs. The first wait clears the
	# console close plus the guard's 120-frame warm-up, so the clicks land in
	# ARMED frames. Esc closes whichever popup is up (the dialog, the menu) -
	# never the sheet, which only closes on an Esc with nothing open.
	if ($Sheet) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Send-Mouse $script:runeX $script:slotY  # hover: the status bar names it
			Send-Mouse $script:bladeX $script:slotY
			for ($t = 0; $t -lt 5; $t++) { Send-Key 0x09 } # all five tabs, round to Inventory
			Send-Mouse $script:bladeX $script:slotY 0x204 0x205 2 # right: details
			Start-Sleep -Seconds 2                                 # the model turns
			Send-Key 0x1B
			Send-Mouse $script:runeX $script:slotY 0x207 0x208 0x10 # middle: use menu
			Start-Sleep -Milliseconds 500
			Send-Key 0x1B
		}
	}

	# -All: All, every tab, Esc, the portrait - a cycle every few seconds while
	# the window runs. The first wait clears the console close plus the guard's
	# warm-up, so the first click on All is armed.
	if ($All) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Invoke-AllCycle
		}
	}

	# -Panels: drag the Movement dock away by its title and home again, then
	# pull the Hands dock's corner grip, while the window runs. The first wait
	# clears the console close plus the guard's warm-up, so the drags land in
	# ARMED frames; each release saves settings.ini.
	if ($Panels) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			# First, while the dock is still on its default spot: into the tray
			# and back out.
			if ($cycle -eq 1) { Invoke-TrayTrip }
			Send-Drag $script:moveX $script:moveY $script:awayX $script:awayY
			Send-Drag $script:awayX $script:awayY $script:moveX $script:moveY
			if ($cycle -eq 1) { Send-Drag $script:gripX $script:gripY $script:pullX $script:pullY }
		}
	}

	# -Packs: two clicks on the bag square a cycle - the herb pouch in (8 -> 4)
	# and the ammo pouch back (4 -> 8), the growth this mode exists for.
	if ($Packs) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Send-Click $script:bagAt.X $script:bagAt.Y
			Send-Click $script:bagAt.X $script:bagAt.Y
		}
	}

	# -Items: round trips with the measured item while the window runs. The
	# window is still open from the warm-up; the first wait clears the console
	# close plus the guard's 120-frame warm-up, so the first pick is armed.
	if ($Items) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Invoke-ItemRoundTrip $script:measurePoint
		}
	}

	# -Throw: lift the rock from the square ahead and throw it at the wall, a
	# round every ~2 s (throw_interval is 1 s, and the flight and landing take
	# well under one). The first wait clears the console close plus the guard's
	# warm-up, so the first lift is armed.
	if ($Throw) {
		Start-Sleep -Seconds 3
		for ($cycle = 1; $cycle -le 6; $cycle++) {
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Send-Click $script:floorX $script:floorY
			Start-Sleep -Milliseconds 300
			Send-Click $script:skyX $script:skyY
			Start-Sleep -Milliseconds 1700
		}
	}

	# The command closes the console itself, then spends its budget on armed
	# frames only; its own deadline guarantees a line either way.
	$line = Wait-ForLog 'alloctest RESULT=' ($Seconds * 4 + 60) 'the alloctest result'
	$result = if ($line -match 'RESULT=(\w+)') { $Matches[1] } else { 'UNKNOWN' }
	Write-Host ''
	Write-Host $line.Substring($line.IndexOf('alloctest'))

	# WHAT HAPPENED INSIDE THE WINDOW, from the game itself: the verdict frame
	# logs the harness tally, which the window's first ARMED frame restarted.
	# (Asking `tally` afterwards used to count the console's frames, the
	# guard's warm-up and whatever landed while the question was being typed.)
	if ($Melee -or $Impact -or $Items -or $Throw) {
		$script:lastTally = (Wait-ForLog 'alloctest window TALLY ' 10 'the window tally') -replace '^.*TALLY ', 'TALLY '
		Write-Host "  in the window: $script:lastTally"
	}

	# A melee PASS counts only if the swing path actually ran inside the window.
	# Without this, a monster that wandered off, or a party knocked out before
	# the window opened, would report exactly like a clean fight.
	if ($Melee) {
		$taken = Get-LastTallyField 'taken'
		if ($taken -le 0 -and $result -eq 'PASS') {
			Write-Host 'the monster landed no blow inside the window - the swing path was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# The same refusal for -Impact, over all three things it exists to see: a
	# monster that died in the warm-up, or a rotation that stopped, would
	# otherwise report exactly like a clean barrage.
	if ($Impact) {
		$missing = @()
		if ((Get-LastTallyField 'bolthits') -le 0) { $missing += 'no bolt hit' }
		if ((Get-LastTallyField 'expired') -le 0) { $missing += 'no bolt expired' }
		if ((Get-LastTallyField 'blasts') -le 0) { $missing += 'no blast went off' }
		if ((Get-LastTallyField 'sceneryticks') -le 0) { $missing += 'no burning crate ticked' }
		if ((Get-LastTallyField 'doused') -le 0) { $missing += 'no fixture broke and went out' }
		# What the crate came to, for the reader: `broken` means its burn finished
		# it inside the window, so the break was measured as well as the ticks.
		# The console is shut again afterwards - the quit below reopens it.
		Send-Key 0xC0; Start-Sleep -Milliseconds 400
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text "breakables $cx $tz"; Send-Key 0x0D
		Send-Text "breakables $bx $tz"; Send-Key 0x0D
		Send-Text 'logecho off'; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		Send-Key 0xC0; Start-Sleep -Milliseconds 300
		foreach ($what in @("decoration crate @ $cx,$tz ", "fixture brazier @ $bx,$tz ")) {
			$row = @(Select-String -Path $log -Pattern "console:   $what")
			if ($row.Count -gt 0) {
				Write-Host "  after the window: $($row[-1].Line -replace '^.*console:\s+', '')"
			}
		}
		if ($missing.Count -gt 0 -and $result -eq 'PASS') {
			Write-Host "$($missing -join ', ') inside the window - the impact path was not measured" -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Hand: every entry of the rotation cast again (several times - the
	# counts run on past the window until the hold, so one is not enough to say
	# it was inside), and the fresh items were USED: the torch lit, the skin
	# filled, the empty hand holding a pebble.
	if ($Hand) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text 'autocast hold'; Send-Key 0x0D
		$castPattern = 'console:   member \d+ casts \S+: \d+ cast, \d+ failed'
		$castBefore = @(Select-String -Path $log -Pattern $castPattern).Count
		$torchBefore = @(Select-String -Path $log -Pattern 'console:   \[\d\] \w+ hand \d: ').Count
		Send-Text 'autocast'; Send-Key 0x0D
		Send-Text 'torch'; Send-Key 0x0D
		Start-Sleep -Milliseconds 800
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		$after = @(Select-String -Path $log -Pattern $castPattern) | Select-Object -Skip $castBefore
		$hands = (@(Select-String -Path $log -Pattern 'console:   \[\d\] \w+ hand \d: ') |
			Select-Object -Skip $torchBefore | ForEach-Object { $_.Line -replace '^.*console:\s+', '' }) -join '; '
		$short = @()
		for ($i = 0; $i -lt $after.Count -and $i -lt $script:handRows.Count; $i++) {
			$was = if ($script:handRows[$i].Line -match ': (\d+) cast,') { [int]$Matches[1] } else { 0 }
			$now = if ($after[$i].Line -match ': (\d+) cast,') { [int]$Matches[1] } else { 0 }
			Write-Host "  $($after[$i].Line -replace '^.*console:\s+', '') ($($now - $was) since the warm-up)"
			if ($now - $was -lt 3) { $short += ($after[$i].Line -replace '^.*casts (\S+):.*$', '$1') }
		}
		Write-Host "  hands: $hands"
		$used = @()
		if ($hands -notmatch '\[2\] Maren hand 1: torch_lit ') { $used += 'the torch was not lit' }
		if ($hands -notmatch '\[3\] Tilo hand 1: waterskin ') { $used += 'the skin was not filled' }
		if ($hands -notmatch '\[0\] Brand hand 0: pebble ') { $used += 'no pebble landed in hand' }
		if (($short.Count -gt 0 -or $used.Count -gt 0 -or $after.Count -ne 4) -and $result -eq 'PASS') {
			if ($short.Count -gt 0) { Write-Host "too few casts after the warm-up: $($short -join ', ')" -ForegroundColor Yellow }
			if ($used.Count -gt 0) { Write-Host "$($used -join ', ') - the item paths were not measured" -ForegroundColor Yellow }
			$result = 'UNMEASURED'
		}
	}

	# And for -Walk: no move counted means no key landed in an armed frame.
	if ($Walk) {
		$moves = if ($line -match '\bmoves=(\d+)') { [int]$Matches[1] } else { 0 }
		Write-Host "  key moves inside the window: $moves"
		if ($moves -lt 4 -and $result -eq 'PASS') {
			Write-Host 'fewer than four key moves landed - the pad presses were not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Pause: no transition counted means no Esc landed in an armed
	# frame, and the run measured nothing it exists to measure.
	if ($Pause) {
		$transitions = if ($line -match '\btransitions=(\d+)') { [int]$Matches[1] } else { 0 }
		Write-Host "  transitions inside the window: $transitions"
		if ($transitions -le 0 -and $result -eq 'PASS') {
			Write-Host 'no Esc landed in an armed frame - the pause transition was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Packs: two equips made during the window (one each way), or the
	# clicks missed and the growth was not measured.
	if ($Packs) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$equips = (Get-SheetPacks).Equips - $script:equipsBefore
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		Write-Host "  bags equipped by a click during the window: $equips"
		if ($equips -lt 2 -and $result -eq 'PASS') {
			Write-Host 'fewer than two equips - the pack growth was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Items: two drops and two lifts inside the window, which puts the
	# first round trip's put-back between them inside it too. Fewer means a
	# click missed or the window closed early, and the moves were not measured.
	if ($Items) {
		# The party window was parked for the run; put the layout back.
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		Send-Key 0xC0
		$drops = Get-LastTallyField 'drops'
		$lifts = Get-LastTallyField 'lifts'
		Write-Host "  floor drops / lifts inside the window: $drops / $lifts"
		if (($drops -lt 2 -or $lifts -lt 2) -and $result -eq 'PASS') {
			Write-Host 'fewer than two round trips inside the window - the item moves were not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Throw: two lifts, two throws and two flights that came down,
	# inside the window - or a click missed and the throw was not measured.
	if ($Throw) {
		$throws = Get-LastTallyField 'throws'
		$lifts = Get-LastTallyField 'lifts'
		$landed = (Get-LastTallyField 'throwlandings') + (Get-LastTallyField 'throwstrikes')
		Write-Host "  lifts / throws / came down inside the window: $lifts / $throws / $landed"
		if (($lifts -lt 2 -or $throws -lt 2 -or $landed -lt 2) -and $result -eq 'PASS') {
			Write-Host 'fewer than two whole throws inside the window - throwing was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Glass: the glass must have been drawn through the window - a frame
	# count that did not move means it left the view and nothing was measured.
	if ($Glass) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$glassFrames = (Get-GlassFrames) - $script:glassBefore
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		$windowFrames = if ($line -match '\bframes=(\d+)') { [int]$Matches[1] } else { 0 }
		Write-Host "  frames that drew glass: $glassFrames (window: $windowFrames armed frames)"
		if ($glassFrames -lt $windowFrames -and $result -eq 'PASS') {
			Write-Host 'glass was not in view for the whole window - the transparent queue was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Sheet: no new open of the dialog means the right-click missed (or
	# landed outside the window), and the open path was not measured.
	if ($Sheet) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$opens = (Get-DetailOpens) - $script:opensBefore
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		Write-Host "  item details opened by a right-click: $opens"
		if ($opens -le 0 -and $result -eq 'PASS') {
			Write-Host 'no right-click opened the dialog - the open path was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -All: the window must have opened inside the window, twice, or a
	# click missed and the open path was not measured.
	if ($All) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$status = Get-InventoryStatus
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		$opens = if ($status -match 'opens=(\d+)') { [int]$Matches[1] - $script:allOpensBefore } else { 0 }
		Write-Host "  party window opens inside the window: $opens"
		if ($opens -lt 2 -and $result -eq 'PASS') {
			Write-Host 'fewer than two opens of the party window - it was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

	# And for -Panels: the drags must have LANDED - the move dock saved off its
	# default spot and the hands dock off scale 1 - or nothing was measured.
	if ($Panels) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text 'hudpanel list'; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		# Closed for the checks below: the reset button puts it home too, and at
		# its default size the party window covers the Movement dock's drags.
		Send-Text 'inventory off'; Send-Key 0x0D
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		$moveRow = @(Select-String -Path $log -Pattern 'console:   move ')[-1].Line
		$handsRow = @(Select-String -Path $log -Pattern 'console:   hands ')[-1].Line
		Write-Host "  $($moveRow -replace '^.*console:   ', '')"
		Write-Host "  $($handsRow -replace '^.*console:   ', '')"
		$invRow = @(Select-String -Path $log -Pattern 'console:   inventory ')[-1].Line
		Write-Host "  $($invRow -replace '^.*console:   ', '')"
		$moved = $moveRow -notmatch 'saved default'
		$scaled = $handsRow -notmatch 'scale 1\.00'
		$invShown = $invRow -match 'inventory shown'
		# The tray: the counts are since launch and the warm-up made one trip, so
		# the window's trip shows as a second of each - and the dock is back.
		$hudRow = @(Select-String -Path $log -Pattern 'console: hud layout ')[-1].Line
		$trips = if ($hudRow -match 'minimizes (\d+), restores (\d+)') { [Math]::Min([int]$Matches[1], [int]$Matches[2]) } else { 0 }
		Write-Host "  tray trips (warm-up included): $trips; move dock $(if ($moveRow -match 'minimized') { 'still minimized' } else { 'restored' })"
		$tripped = $trips -ge 2 -and $moveRow -notmatch 'minimized'
		if ((-not $moved -or -not $scaled -or -not $invShown -or -not $tripped) -and $result -eq 'PASS') {
			Write-Host 'a drag or the tray trip did not land, or the inventory was not open, inside the window - the panel path was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}

		# The arranging rules, after the window: a Ctrl+click on the moved dock's
		# RESET button (beside its minimize in the top-right corner, read off the
		# dock's own rect) puts every panel home, and a drag WITHOUT Ctrl then
		# moves nothing.
		if ($moveRow -match 'px (-?\d+),(-?\d+) (\d+)x(\d+)') {
			$rx = [int]$Matches[1] + [int]$Matches[3] - $script:grip - 7; $ry = [int]$Matches[2] + 6
			Send-Mouse $rx $ry
			Send-CtrlClick $rx $ry
			$resetMove = Get-PanelRow 'move'
			$resetHands = Get-PanelRow 'hands'
			Send-Drag $script:moveX $script:moveY $script:awayX $script:awayY -NoCtrl
			$plainMove = Get-PanelRow 'move'
			Write-Host "  after reset: $($resetMove -replace '^.*console:   ', '')"
			Write-Host "  after a plain drag: $($plainMove -replace '^.*console:   ', '')"
			$wasReset = $resetMove -match 'saved default' -and $resetHands -match 'saved default.*scale 1\.00'
			$stayed = $plainMove -match 'saved default'
			# SNAPPING: drag the move dock left until its right edge is 4 px short
			# of the hands dock's left edge (they share a column, so the hands
			# dock sits right below it); it must land butted up against it.
			$snapped = $false; $snapNote = 'rects unreadable'
			if ($plainMove -match 'px (-?\d+),(-?\d+) (\d+)x(\d+)') {
				$mLeft = [int]$Matches[1]; $mW = [int]$Matches[3]
				if ($resetHands -match 'px (-?\d+),') {
					$hLeft = [int]$Matches[1]
					$dx = ($hLeft - 4) - ($mLeft + $mW)
					Send-Drag $script:moveX $script:moveY ($script:moveX + $dx) $script:moveY
					$snapMove = Get-PanelRow 'move'
					Write-Host "  after a snapping drag: $($snapMove -replace '^.*console:   ', '')"
					$want = $hLeft - $mW
					$snapNote = "wanted the left edge at $want"
					if ($snapMove -match 'px (-?\d+),') { $snapped = [Math]::Abs([int]$Matches[1] - $want) -le 1 }
					Send-Key 0xC0
					Start-Sleep -Milliseconds 500
					Send-Text 'hudpanel reset'; Send-Key 0x0D
					Send-Key 0xC0
				}
			}
			if (-not $wasReset -or -not $stayed -or -not $snapped) {
				Write-Host $(if (-not $wasReset) { 'the reset button did not put the panels home' }
							 elseif (-not $stayed) { 'a drag without Ctrl moved a panel' }
							 else { "a drag 4 px from an edge did not snap to it ($snapNote)" }) -ForegroundColor Red
				if ($result -eq 'PASS') { $result = 'FAIL' }
			}
		} else {
			Write-Host 'could not read the move dock''s rect - the reset button was not checked' -ForegroundColor Yellow
			if ($result -eq 'PASS') { $result = 'UNMEASURED' }
		}
	}

	# And for -Minimal: the cards must have been up - else the run measured the
	# Standard HUD and says nothing about the Minimal one. Then Standard goes
	# back, so the next harness on this build starts where it expects.
	if ($Minimal) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text 'hudpanel list'; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
		Send-Text 'hudpanel layout standard'; Send-Key 0x0D
		Send-Text 'logecho off'; Send-Key 0x0D
		Send-Key 0xC0
		$cardsRow = @(Select-String -Path $log -Pattern 'console:   cards ')[-1].Line
		Write-Host "  $($cardsRow -replace '^.*console:   ', '')"
		if ($cardsRow -notmatch 'cards +shown' -and $result -eq 'PASS') {
			Write-Host 'the party cards were not up - the Minimal layout was not measured' -ForegroundColor Yellow
			$result = 'UNMEASURED'
		}
	}

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
		'UNMEASURED' { } # already explained above
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
