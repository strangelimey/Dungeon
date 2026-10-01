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
#   .\tools\AllocTest.ps1 -Impact            # bolts landing, expiring, a blast
#   .\tools\AllocTest.ps1 -Pause             # Esc to the pause menu and back
#   .\tools\AllocTest.ps1 -Sheet             # the sheet: hover, tabs, item dialog
#   .\tools\AllocTest.ps1 -Panels            # drag and resize the floating HUD
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
# bursting). Anything that allocates is named with a full call stack in
# dungeon.log, once per unique stack.
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
# drag or a window that never opened reports exactly like a clean run.
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
	# Pauses (Esc) and resumes inside the window. See the note above.
	[switch]$Pause,
	# Works the character sheet inside the window. See the note above.
	[switch]$Sheet,
	# Drags and resizes the floating HUD panels inside the window. See above.
	[switch]$Panels,
	# Checks the CHECKER: makes the game allocate every frame on purpose
	# (`allocpoke`) and passes only if the run comes back FAIL.
	[switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"

# Muted for the whole run, restored however it ends (tools\HarnessAudio.ps1).
. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not $env:DN_HARNESS_MUTED) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }

$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'

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
# held (wparam MK_LBUTTON), release - what a player's hand sends.
function Send-Drag([int]$x0, [int]$y0, [int]$x1, [int]$y1, [int]$steps = 10) {
	$at = { param($x, $y) [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
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
	Start-Sleep -Milliseconds 300
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

	# Landing page: with no save present the first entry is Start New Game.
	Write-Host 'starting a new game'
	Send-Key 0x0D
	# NOT 'Game loaded:' - since the world loads on demand that line comes from
	# a load TASK, before the starting level's own load has begun, and every
	# console command typed then is refused as "still loading". A level load
	# ends with 'Level ready:'; a new game that lands without one (the world
	# map, or a level already in memory) says 'New game started'.
	$ready = Wait-ForLog '^\[info \] (Level ready: |New game started)' $LoadTimeoutSec 'the dungeon load'
	Write-Host "  $($ready -replace '^\[info \] ', '')"
	Start-Sleep -Milliseconds 500

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
		Send-Text "cast $CastMember 0 fire"; Send-Key 0x0D
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
		# The console refuses commands while the level loads; a NEW "Level
		# ready" line is the moment it will take them again. NEW, counted from
		# before the goto: when the landing page Continues an eval save, the
		# game has ALREADY printed one for eval_arena, the wait matched it at
		# once, and `tp` and `face` were typed into the reload and refused - the
		# party then fired the whole barrage the wrong way.
		$readyPattern = '^\[info \] Level ready: eval_arena'
		$readyBefore = @(Select-String -Path $log -Pattern $readyPattern).Count
		Send-Text 'goto eval_arena'; Send-Key 0x0D
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
		Send-Text "autocast 0 flame $ImpactEvery"; Send-Key 0x0D
		Send-Text 'autocast 1 flame'; Send-Key 0x0D
		Send-Text 'autocast 2 fireburst'; Send-Key 0x0D
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

	if ($Panels) {
		Write-Host 'resetting the HUD layout and warming the panel drags up'
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		Send-Text 'hudpanel lock off'; Send-Key 0x0D
		Send-Key 0xC0
		Start-Sleep -Milliseconds 600
		# Where the grabs are, from the window's own size (the default layout is
		# in fractions of it): the Movement dock's title, and the bottom-right
		# corner grip of the Hands dock (the default 1600x900 spots, as shares).
		$rc = New-Object AllocTestWin+RECT
		[AllocTestWin]::GetClientRect($hwnd, [ref]$rc) | Out-Null
		$script:moveX = [int]($rc.Right * 0.8775); $script:moveY = [int]($rc.Bottom * 0.1733)
		# Below the inventory window's default rect (0.23..0.77 down) and above the
		# log footer: a grab landing ON the window would act on its slots instead.
		$script:awayX = [int]($rc.Right * 0.30); $script:awayY = [int]($rc.Bottom * 0.80)
		$script:gripX = [int]($rc.Right * 0.9835); $script:gripY = [int]($rc.Bottom * 0.5575)
		$script:pullX = [int]($rc.Right * 0.96); $script:pullY = [int]($rc.Bottom * 0.53)
		# WARM-UP: one drag and one pull outside the window - the pull's new scale
		# bakes a font size, a first time for the process. Then back to default.
		Send-Drag $script:moveX $script:moveY $script:awayX $script:awayY
		Send-Drag $script:awayX $script:awayY $script:moveX $script:moveY
		Send-Drag $script:gripX $script:gripY $script:pullX $script:pullY
		Start-Sleep -Milliseconds 400
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'hudpanel reset'; Send-Key 0x0D
		# The party inventory window stays open through the window, so its draw
		# (every slot of every pack, every frame) is measured too. Its default
		# spot is clear of both grabs above.
		Send-Text 'inventory'; Send-Key 0x0D
		Send-Key 0xC0
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

	# -Panels: drag the Movement dock away by its title and home again, then
	# pull the Hands dock's corner grip, while the window runs. The first wait
	# clears the console close plus the guard's warm-up, so the drags land in
	# ARMED frames; each release saves settings.ini.
	if ($Panels) {
		for ($cycle = 1; $cycle -le 3; $cycle++) {
			Start-Sleep -Seconds 3
			if (Select-String -Path $log -Pattern 'alloctest RESULT=' -Quiet) { break }
			Send-Drag $script:moveX $script:moveY $script:awayX $script:awayY
			Send-Drag $script:awayX $script:awayY $script:moveX $script:moveY
			if ($cycle -eq 1) { Send-Drag $script:gripX $script:gripY $script:pullX $script:pullY }
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
	if ($Melee -or $Impact) {
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
		if ($missing.Count -gt 0 -and $result -eq 'PASS') {
			Write-Host "$($missing -join ', ') inside the window - the impact path was not measured" -ForegroundColor Yellow
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

	# And for -Panels: the drags must have LANDED - the move dock saved off its
	# default spot and the hands dock off scale 1 - or nothing was measured.
	if ($Panels) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		Send-Text 'hudpanel list'; Send-Key 0x0D
		Start-Sleep -Milliseconds 500
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
		if ((-not $moved -or -not $scaled -or -not $invShown) -and $result -eq 'PASS') {
			Write-Host 'a drag did not land, or the inventory was not open, inside the window - the panel path was not measured' -ForegroundColor Yellow
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
