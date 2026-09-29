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
# -Melee a monster's swing and its narration). Anything that allocates is named
# with a full call stack in dungeon.log, once per unique stack.
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

	# A melee PASS counts only if the swing path actually ran inside the window.
	# Without this, a monster that wandered off, or a party knocked out before
	# the window opened, would report exactly like a clean fight.
	if ($Melee) {
		Send-Key 0xC0
		Start-Sleep -Milliseconds 500
		Send-Text 'logecho on'; Send-Key 0x0D
		$taken = Get-TallyField 'taken'
		Write-Host "  in the window: $script:lastTally"
		Send-Key 0xC0
		Start-Sleep -Milliseconds 400
		if ($taken -le 0 -and $result -eq 'PASS') {
			Write-Host 'the monster landed no blow inside the window - the swing path was not measured' -ForegroundColor Yellow
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
