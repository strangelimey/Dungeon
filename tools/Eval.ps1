# ============================================================================
# tools\Eval.ps1 - run the eval suites and surface what they measured.
#
#   .\tools\Eval.ps1                    # every suite
#   .\tools\Eval.ps1 -Only ladder       # one of them
#   .\tools\Eval.ps1 -List
#   .\tools\Eval.ps1 -SelfTest          # the runner must FAIL on purpose
#   .\tools\Eval.ps1 -Table             # print ONLY the measurements
#   .\tools\Eval.ps1 -Headless          # no window, no drawing
#   .\tools\Eval.ps1 -Warp              # draw on WARP, the software rasterizer
#   .\tools\Eval.ps1 -OutFile before.txt # ...and SAVE it, to diff against later
#
# DIFFING TWO RUNS is the whole point of the numbers, and until -OutFile existed
# there was no supported way to keep one: every line here goes to the host, so
# `Eval.ps1 > before.txt` wrote an EMPTY FILE (docs/eval-audit.md F2). The shape
# a knob change is measured in:
#
#   .\tools\Eval.ps1 -Headless -Table -OutFile before.txt
#   ...edit balance.cat...
#   .\tools\Eval.ps1 -Headless -Table -OutFile after.txt
#   Compare-Object (gc before.txt) (gc after.txt)
#
# -Headless is NOT primarily a speed switch, and saying so up front saves
# somebody measuring it hopefully: ten suites go 42s -> 37s, because the time is
# the asset load plus the `step` loops, and a `step` runs many sim ticks inside
# ONE frame. There are simply not many frames to save. What it buys is a run that
# does not steal focus, survives being run over RDP or from a scheduled task, and
# can be run several at a time without contending for the GPU. The numbers are
# identical either way - that equivalence is checked under -SelfTest.
#
# Exit 0 = every suite's script ran to completion, every line did what it said,
# and every suite measured something. That is FOUR conditions, and it used to be
# one (docs/eval-audit.md). A line now fails the run if it named no command
# (a typo), if it named one that then REFUSED (a spawn onto rock, a tp into a
# wall, a `step` past its per-call ceiling - and since code-review C442 every
# declined command refuses rather than prints; a line written to PROBE a rule
# says `expect-refuse <line>` and fails if it is NOT refused), if the script
# ended out of play (a
# party wipe returns to the title screen and every dev command keeps answering
# from there), or if a suite's measure regex matched nothing at all.
#
# The first run with those checks failed three of the ten suites and all three
# were real: two had been measuring 55.6 minutes and calling it an hour, and one
# had been printing its closing figures off a dead party.
#
# A FIFTH: THE DEVICE (code-review batch 64). A debug build runs the D3D12 debug
# layer, which writes every error it sees into dungeon.log ("d3d12 error [id]"),
# and one there FAILS the run: a script whose frames read a freed mesh measured
# nothing worth keeping. -Warp is what gives that check teeth. On a real GPU the
# frames a `reset` or an `arena` freed had long finished, so the race the layer
# names never happened; WARP draws on the CPU, its frames are still in flight,
# and C193 (the full surface bake freeing chunk meshes with nothing drained) was
# an error 921 on WARP and silence on the GPU. -Warp also shrinks the window to
# 640x360 for the run (settings.ini, put back after): what it needs is frames,
# not pixels, and a full-size WARP frame turns a one-minute run into seven. A
# release build has no layer and says so.
#
# THIS IS A MEASUREMENT HARNESS, NOT A PASS/FAIL ONE, and the distinction is
# the whole reason it is not in CheckAll's tiers. A green verdict here means the
# scripts RAN - not that the numbers are good. The numbers are the artefact:
# TALLY lines and blast tables, printed so a knob change can be diffed against a
# previous run. Nothing here asserts a balance number is correct, because
# nobody has decided what correct is (see docs/eval-harness.md).
#
# What IS checked is that the runner still works: a suite that silently stopped
# measuring would otherwise read exactly like one whose numbers had not changed.
# -SelfTest is the guard - it hands the runner a script containing a line that
# is not a command and requires exit 1, and a batch where a declined setup line,
# a probe that is not refused and a probe refused only by the no-game gate must
# each fail on exactly its own counter while a refused probe passes. A second
# batch, with an unreadable script in the middle, must count that gap once, and
# its two scripts must show a step stopping at a party wipe (the tally's clock
# with it) and a console-thrown torch landing with the charge it had. A reset
# must equal a new game twice over: resettest.eval's two baselines (each with the
# `transients` readout and a non-empty `messages` one) match each other alone,
# and match again when the script
# runs batched after selftest-leavelevel.eval, which carves the harness level and
# leaves the party off it - the reset must come back to it and forget the carved
# stash, which that script must be seen holding (code-review C300). Its wrecking
# opens with three repros a LOAD and a STAIR must clear as a reset does - a gas,
# a smashed sconce and a burning brazier, a bleeding monster, a door, a sconce
# and props wrecked after the save - each with its control and a check that what
# reads whole is THERE (smashed again, or the saved wreck found) (code-review
# C292, C293) - and three that a load, a new game and a reset END what the game
# had running: a rest (lockstep handed back, no reason left), an undo step, a
# throw's wait and the kindle clock, each staged and seen before (C294, C295,
# C297); the batched run's first script must also see an ambush drop an undo
# step taken in the dungeon it replaced. The run takes a copy of the script
# whose save slot is named for this worktree (HarnessGame.ps1 Copy-EvalScript).
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[string[]]$Only = @(),
	[switch]$List,
	[switch]$SelfTest,
	[switch]$Table,
	[switch]$Headless,
	[switch]$Warp,
	[string]$OutFile = '',
	[ValidateSet('debug', 'release')][string]$Config = 'debug'
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"

# Muted for the whole run, restored however it ends (tools\HarnessAudio.ps1).
# Not for -List, which runs nothing (tools\CheckDocs.ps1 reads it).
. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not $List -and -not (Test-HarnessMuted $bin)) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }

$scripts = Join-Path $root 'tools\EvalScripts'
$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'

# ONE RUN PER WORKTREE: this worktree's game writes one dungeon.log, truncated
# on open, and a second run would interleave its verdict source with this one's
# (code-review C430). Another worktree's game has its own log and is fine.
# And never a stale exe: a measurement of yesterday's binary reads as one of
# today's change (C426).
. (Join-Path $PSScriptRoot 'HarnessGame.ps1')
if (-not $List) {
	Assert-ExeCurrent $exe
	Assert-NotRunning $exe
}

# EVERY REPORT LINE GOES THROUGH HERE so the run can be both coloured on screen
# and saved to a file. Write-Host alone cannot be redirected (that is F2) and
# Write-Output alone loses the colour that makes a FAIL findable in 300 lines of
# numbers, so this does both and -OutFile writes the buffer at the end.
$transcript = New-Object System.Collections.Generic.List[string]
function Say {
	param([string]$Text = '', [string]$Colour = '')
	$transcript.Add($Text) | Out-Null
	if ($Colour) { Write-Host $Text -ForegroundColor $Colour } else { Write-Host $Text }
}
function SaveTranscript {
	if (-not $OutFile) { return }
	# WriteAllLines, not Set-Content: PS 5.1's -Encoding utf8 prepends a BOM, and
	# a BOM in the first line makes the first line of every diff spurious.
	$path = if ([IO.Path]::IsPathRooted($OutFile)) { $OutFile }
			else { Join-Path (Get-Location).Path $OutFile }
	[IO.File]::WriteAllLines($path, [string[]]$transcript)
	Write-Host ("saved to {0} ({1} lines)" -f $OutFile, $transcript.Count) -ForegroundColor DarkGray
}

# READ THE LOG AS UTF-8. The game writes it as UTF-8 and PS 5.1's Get-Content
# defaults to the ANSI code page, so every em-dash arrived as three characters
# and the report could not be pasted anywhere (F10). This fixes the DATA; how a
# console then draws it is the console's code page and not this script's
# business - see the utf8-console-codepage note.
function ReadLog {
	if (-not (Test-Path $log)) { return @() }
	@(Get-Content $log -Encoding UTF8)
}

# resettest.eval's two baselines out of a log: every console readout between
# its `=== BASELINE A` / `=== BASELINE B` markers, never the echoed commands that
# produced them, and nothing from the wrecking in between. Lists, so a block of
# one line is still a list (PowerShell unrolls a bare array on the way out).
function Get-ResetBlocks([string[]]$lines) {
	$a = New-Object System.Collections.Generic.List[string]
	$b = New-Object System.Collections.Generic.List[string]
	$which = $null
	foreach ($line in $lines) {
		if ($line -match 'console: === BASELINE A') { $which = $a; continue }
		if ($line -match 'console: === BASELINE B') { $which = $b; continue }
		if ($line -match 'console: === wrecking')   { $which = $null; continue }
		if ($null -ne $which -and $line -match '^\[info \] console: (?!> )(.+)$') {
			$which.Add($Matches[1])
		}
	}
	return @{ A = $a; B = $b }
}

# resettest.eval's repros (code-review batches 77, 78): the `transients` readout
# that follows a `--- repro: <name> ---` echo, as numbers - level, blasts, effects
# (on monsters), the broken fixtures, decorations and doors, the pieces hurt but
# standing and the effects riding pieces; then the undo and redo depth, rest,
# lockstep and why rest last ended, and the throw cooldowns (their largest) and
# the kindle clock. $null when that repro printed no whole readout, which a
# check must read as a failure, never as zeroes.
function Get-ReproTransients([string[]]$lines, [string]$name) {
	$marker = '^\[info \] console: --- repro: ' + [regex]::Escape($name) + ' ---$'
	$at = -1
	for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -cmatch $marker) { $at = $i; break } }
	if ($at -lt 0) { return $null }
	return Read-Transients $lines ($at + 1)
}

# One `transients` readout, the first at or after line $from, read as above;
# $null when none follows before the next `--- ` echo (or the log ends).
function Read-Transients([string[]]$lines, [int]$from) {
	$r = @{}
	for ($i = $from; $i -lt $lines.Count; $i++) {
		$line = $lines[$i]
		if ($line -cmatch '^\[info \] console: --- ') { break } # the next repro: this one printed none
		if ($line -cmatch '^\[info \] console: transients on (\S+)$') { $r.level = $Matches[1] }
		elseif ($line -cmatch '^\[info \] console:   blasts=(\d+) monster_effects=(\d+) ') {
			$r.blasts = [int]$Matches[1]; $r.effects = [int]$Matches[2]
		} elseif ($line -cmatch '^\[info \] console:   broken fixtures=(\d+) decorations=(\d+) doors=(\d+)  hurt=(\d+) piece_effects=(\d+)$') {
			$r.fixtures = [int]$Matches[1]; $r.decorations = [int]$Matches[2]; $r.doors = [int]$Matches[3]
			$r.hurt = [int]$Matches[4]; $r.pieceEffects = [int]$Matches[5]
		} elseif ($line -cmatch '^\[info \] console:   undo=(\d+) redo=(\d+) resting=(on|off) lockstep=(on|off) rest_ended=(\S+)$') {
			$r.undo = [int]$Matches[1]; $r.redo = [int]$Matches[2]; $r.resting = $Matches[3]
			$r.lockstep = $Matches[4]; $r.restEnded = $Matches[5]
		} elseif ($line -cmatch '^\[info \] console:   throw=([\d.]+),([\d.]+),([\d.]+),([\d.]+) kindle=([\d.]+)$') {
			$r.throwWait= (@([double]$Matches[1], [double]$Matches[2], [double]$Matches[3], [double]$Matches[4]) |
				Measure-Object -Maximum).Maximum
			$r.kindle = $Matches[5]; break # the readout's last line
		}
	}
	foreach ($k in 'level', 'blasts', 'effects', 'fixtures', 'decorations', 'doors', 'hurt', 'pieceEffects',
				   'undo', 'redo', 'resting', 'lockstep', 'restEnded', 'throwWait', 'kindle') {
		if (-not $r.ContainsKey($k)) { return $null }
	}
	return $r
}

# The sixteen repro rows for one run's log: each repro's CONTROL (what it staged
# is really there - a repro that staged nothing must not pass), the line that
# must read clear, and the line that tells CLEAR from GONE. Nothing broken,
# hurt or burning is what an empty fixture table or a missing door reads as
# too, so the stair and repro 3 smash what must still be there and count it,
# and repro 2's load must bring back the wreck its save holds - which it can
# only lay on a fixture table the load seeded.
function Get-ReproChecks([string[]]$lines) {
	$say = { param($t) if ($null -eq $t) { 'no readout' } else {
		"on $($t.level): blasts=$($t.blasts) effects=$($t.effects) broken fixtures=$($t.fixtures) " +
		"decorations=$($t.decorations) doors=$($t.doors) hurt=$($t.hurt) piece_effects=$($t.pieceEffects)" } }
	# Every piece of dungeon whole, unhurt and carrying nothing.
	$whole = { param($t) [bool]($t -and $t.fixtures -eq 0 -and $t.decorations -eq 0 -and $t.doors -eq 0 -and
		$t.hurt -eq 0 -and $t.pieceEffects -eq 0) }
	$s0 = Get-ReproTransients $lines 'before the stair'
	$s1 = Get-ReproTransients $lines 'down the stair'
	$s2 = Get-ReproTransients $lines "crypt1's own, smashed"
	$l0 = Get-ReproTransients $lines 'before the load'
	$l1 = Get-ReproTransients $lines 'loaded over the gas'
	$d0 = Get-ReproTransients $lines 'smashed after the save'
	$d1 = Get-ReproTransients $lines 'loaded over the smash'
	$d2 = Get-ReproTransients $lines 'smashed again after the load'
	# Batch 78 (C294, C295, C297): what the GAME had running, staged the same way
	# before a load, a new game and a reset - lockstep off, then an undo step, the
	# kindle clock off its phase, a throw's wait and a rest - and ended by each.
	# "Fresh" for the kindle clock is what baseline A reads: where a new game
	# leaves it, not a number written here.
	$fresh = Get-BaselineTransients $lines
	$sayRun = { param($t) if ($null -eq $t) { 'no readout' } else {
		"on $($t.level): undo=$($t.undo) redo=$($t.redo) resting=$($t.resting) lockstep=$($t.lockstep) " +
		"rest_ended=$($t.restEnded) throw=$($t.throwWait) kindle=$($t.kindle)" +
		$(if ($fresh) { " (a new game's kindle=$($fresh.kindle))" } else { ' (no baseline A readout)' }) } }
	# Staged: resting, so lockstep forced on; an undo step; a throw's wait; the
	# kindle clock somewhere a fresh world's is not.
	$staged = { param($t) [bool]($t -and $fresh -and $t.resting -ceq 'on' -and $t.lockstep -ceq 'on' -and
		$t.undo -ge 1 -and $t.throwWait-gt 0 -and $t.kindle -cne $fresh.kindle) }
	# Ended: no rest, lockstep back OFF (what the rest found), no reason left from
	# the game before, no history, no wait, and the clock where a new game has it.
	$ended = { param($t) [bool]($t -and $fresh -and $t.resting -ceq 'off' -and $t.lockstep -ceq 'off' -and
		$t.restEnded -ceq 'none' -and $t.undo -eq 0 -and $t.redo -eq 0 -and $t.throwWait-eq 0 -and
		$t.kindle -ceq $fresh.kindle) }
	$g0 = Get-ReproTransients $lines 'resting before the load'
	$g1 = Get-ReproTransients $lines 'loaded mid-rest'
	$n0 = Get-ReproTransients $lines 'resting before the new game'
	$n1 = Get-ReproTransients $lines 'a new game mid-rest'
	$r0 = Get-ReproTransients $lines 'resting before the reset'
	$r1 = Get-ReproTransients $lines 'a reset mid-rest'
	$w0 = Get-ReproTransients $lines 'wrecked, before the reset'
	return @(
		@{ what = 'a gas hangs, a sconce smashed, a brazier burns'
		   ok = $s0 -and $s0.blasts -ge 1 -and $s0.fixtures -ge 1 -and $s0.hurt -ge 1 -and $s0.pieceEffects -ge 1
		   got = & $say $s0 },
		@{ what = '...down a stair: none of it came'
		   ok = (& $whole $s1) -and $s1.level -ceq 'crypt1' -and $s1.blasts -eq 0; got = & $say $s1 },
		@{ what = "...crypt1's own were there to smash"
		   ok = $s2 -and $s2.level -ceq 'crypt1' -and $s2.fixtures -ge 2; got = & $say $s2 },
		@{ what = 'a gas hangs, a monster bleeds'
		   ok = $l0 -and $l0.blasts -ge 1 -and $l0.effects -ge 1; got = & $say $l0 },
		@{ what = '...a load: no gas, no bleeding'
		   ok = $l1 -and $l1.blasts -eq 0 -and $l1.effects -eq 0; got = & $say $l1 },
		@{ what = '...and the saved wreck and burn are back'
		   ok = $l1 -and $l1.fixtures -ge 1 -and $l1.hurt -ge 1 -and $l1.pieceEffects -ge 1; got = & $say $l1 },
		@{ what = 'door, sconce, props wrecked after the save'
		   ok = $d0 -and $d0.doors -ge 1 -and $d0.fixtures -ge 1 -and $d0.decorations -ge 1 -and
				$d0.hurt -ge 1 -and $d0.pieceEffects -ge 1
		   got = & $say $d0 },
		@{ what = '...a load: all whole, none hurt or burning'
		   ok = & $whole $d1; got = & $say $d1 },
		@{ what = '...and all still there to smash'
		   ok = $d2 -and $d2.doors -ge 1 -and $d2.fixtures -ge 1 -and $d2.decorations -ge 2; got = & $say $d2 },
		@{ what = 'resting, an undo step, both clocks running'; ok = & $staged $g0; got = & $sayRun $g0 },
		@{ what = '...a load ends all of it, lockstep back'; ok = & $ended $g1; got = & $sayRun $g1 },
		@{ what = 'the same, staged again'; ok = & $staged $n0; got = & $sayRun $n0 },
		@{ what = '...a new game ends all of it'; ok = & $ended $n1; got = & $sayRun $n1 },
		@{ what = 'the same, staged once more'; ok = & $staged $r0; got = & $sayRun $r0 },
		@{ what = '...a reset ends all of it'; ok = & $ended $r1; got = & $sayRun $r1 },
		# The last reset's control: baseline B must then read as A, rest and all.
		@{ what = 'the wrecking ends resting, undo, a throw'
		   ok = $w0 -and $w0.resting -ceq 'on' -and $w0.undo -ge 1 -and $w0.throwWait-gt 0; got = & $sayRun $w0 }
	)
}

# Baseline A's `transients` readout (a new game's), $null when it printed none.
function Get-BaselineTransients([string[]]$lines) {
	$at = -1
	for ($i = 0; $i -lt $lines.Count; $i++) {
		if ($lines[$i] -cmatch 'console: === BASELINE A') { $at = $i; break }
	}
	if ($at -lt 0) { return $null }
	for ($i = $at + 1; $i -lt $lines.Count; $i++) {
		if ($lines[$i] -cmatch 'console: === ') { return $null } # past the block: it printed none
		if ($lines[$i] -cmatch '^\[info \] console: transients on \S+$') { return Read-Transients $lines $i }
	}
	return $null
}

# '' when two blocks match line for line, else what differs first.
function Compare-Blocks($want, $got) {
	if ($want.Count -eq 0) { return 'no baseline captured' }
	if ($want.Count -ne $got.Count) { return "line counts differ ($($want.Count) vs $($got.Count))" }
	$bad = @(0..($want.Count - 1) | Where-Object { $want[$_] -cne $got[$_] })
	if ($bad.Count) { return "$($bad.Count) line(s) differ, first: '$($want[$bad[0]])' vs '$($got[$bad[0]])'" }
	return ''
}

# One game run for the self-test's comparisons, with the log DELETED first: a
# launch that died before opening its log would otherwise leave the previous
# run's log in place, and a comparison would read one run against itself and
# pass. Finished = it wrote its 'eval BATCH RESULT=' line.
function Invoke-EvalRun([string[]]$argList) {
	Remove-Item $log -ErrorAction SilentlyContinue
	$p = Start-Process -FilePath $exe -ArgumentList $argList -PassThru -Wait
	$done = [bool](ReadLog | Where-Object { $_ -match 'eval BATCH RESULT=' })
	if (-not $done) { Write-Host ("  the game did not finish '{0}' (exit {1})" -f ($argList -join ' '), $p.ExitCode) -ForegroundColor Red }
	return $done
}

# Invoke-EvalRun with a DEADLINE, for a run that may hit a debug assert: an
# abort there parks a modal CRT dialog and the process looks alive forever, so
# past $timeoutSec it is killed - by PID, never by name (other worktrees' games
# are Dungeon.exe too) - and the run does not count.
function Invoke-EvalRunTimed([string[]]$argList, [int]$timeoutSec) {
	Remove-Item $log -ErrorAction SilentlyContinue
	$p = Start-Process -FilePath $exe -ArgumentList $argList -PassThru
	$null = $p.Handle # or ExitCode reads $null once it ends
	if (-not $p.WaitForExit($timeoutSec * 1000)) {
		Write-Host ("  the game was still running after {0} s - killed (pid {1})" -f $timeoutSec, $p.Id) -ForegroundColor Red
		Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
		$p.WaitForExit(10000) | Out-Null
		return $false
	}
	$done = [bool](ReadLog | Where-Object { $_ -match 'eval BATCH RESULT=' })
	if (-not $done) { Write-Host ("  the game did not finish '{0}' (exit {1})" -f ($argList -join ' '), $p.ExitCode) -ForegroundColor Red }
	return $done
}

# settings.ini with some keys REPLACED for the length of $body, then put back
# byte for byte however $body ends. (The mute's `volume=` line is part of what
# is put back; Invoke-Muted restores the real volume after the whole run.)
$ini = Join-Path $bin 'settings.ini'
function Invoke-WithIni([string[]]$set, [scriptblock]$body) {
	$before = if (Test-Path $ini) { [IO.File]::ReadAllText($ini) } else { $null }
	$keys = @($set | ForEach-Object { ($_ -split '=', 2)[0] + '=' })
	try {
		$keep = @((("$before") -split "\r?\n") | Where-Object {
			$line = $_
			$line -ne '' -and -not @($keys | Where-Object { $line.StartsWith($_) }).Count })
		[IO.File]::WriteAllText($ini, ((@($keep) + $set) -join "`n") + "`n")
		& $body
	} finally {
		if ($null -eq $before) { Remove-Item $ini -ErrorAction SilentlyContinue }
		else { [IO.File]::WriteAllText($ini, $before) }
	}
}
# What -Warp sets for a run: the 640x360 window it draws in (see the header).
$warpIni = @('reswidth=640', 'resheight=360', 'fullscreen=0')

# THE DEVICE, as the D3D12 debug layer saw it (the header's fifth condition).
# `Layer` = the layer was listening (a debug build says so at startup - silence
# from a layer that was never installed must not read as a clean run); `Errors`
# = every error or corruption line it wrote; `Warp` = the run drew on WARP.
function Get-DeviceReport([string[]]$lines) {
	[pscustomobject]@{
		Layer  = [bool]($lines | Where-Object { $_ -match 'D3D12 validation -> dungeon\.log' })
		Warp   = [bool]($lines | Where-Object { $_ -match 'Using WARP software rasterizer' })
		Errors = @($lines | Where-Object { $_ -cmatch '^\[ERROR\] d3d12 (error|CORRUPTION) ' })
	}
}

# ---------------------------------------------------------------------------
# THE SUITES. `measure` is the regex whose matching log lines ARE the result -
# what a reader compares across runs. A suite with no measure line is a smoke
# test: it proves the machinery, and has nothing to say about balance.
#
# NEVER PUT `^` INSIDE A MEASURE. The filter is `^\[info \] console: (<measure>)`,
# so a caret in the alternation asserts start-of-STRING in the middle of the
# pattern and can never match. `expedition` carried `|^state ` from the day it
# was written and has never printed one `state` line; the NOMEASURE check cannot
# catch it either, because the suite's other alternatives still match. The line
# is already anchored for you - just write the text.
#
# Only top-level scripts appear here. rungs\ and presets\ are FRAGMENTS pulled
# in by `include`/`sweep`; running one on its own would start from whatever the
# world happened to be in.
# ---------------------------------------------------------------------------
$suites = @(
	# These two used to carry `measure = $null` and print a header over blank
	# space. That was indistinguishable from a suite whose regex had stopped
	# matching, and since the blank shape appeared on EVERY run a reader was
	# trained to skim past exactly the shape that means the measurement is gone
	# (docs/eval-audit.md F5/F25). Both had plenty to show; nobody had said so.
	@{
		name = 'smoke'; script = 'smoke.eval'
		what = 'the runner drives the game unattended, start to finish'
		measure = '--- |state |  \[[0-9]\] |  \w+ @ '
	},
	@{
		name = 'arena'; script = 'arena.eval'
		what = 'arenas carve, monsters spawn where asked, a fight resolves'
		# `mapinfo`'s walkable count is the ONLY readout that can see an arena
		# that failed to carve, which is exactly the blindness F15 walked
		# through: a script places monsters inside bounds the `arena` line
		# reported, two of the cells are rock, the spawns are refused and the
		# rung measures a third of what its header claims.
		measure = '--- |arena \w|\d+x\d+ map|  \w+ @ |no monsters'
	},
	@{
		name = 'tiers'; script = 'tiers.eval'
		what = 'the same encounter at two preset tiers, same seed'
		measure = '===|  \[[0-9]\]'
	},
	@{
		name = 'ladder'; script = 'ladder.eval'
		what = 'the progression ladder: walk in, fight, tp away, repeat'
		measure = '=== RUNG|TALLY '
	},
	@{
		name = 'blast'; script = 'blast-geometry.eval'
		what = 'one detonation in four geometries'
		measure = '===|  skel_warrior|TALLY '
	},
	@{
		name = 'sweep'; script = 'sweep-novice.eval'
		what = 'one rung over twelve seeds - a distribution, not an anecdote'
		measure = 'TALLY '
	},
	@{
		name = 'resources'; script = 'resources.eval'
		what = 'the three pools: rates, the state gate, and what recovery trains'
		measure = '===|  \[[0-9]\]|  ref |    skill |    creep '
	},
	@{
		name = 'supplies'; script = 'supplies.eval'
		what = 'food and water: what they cost, and what an empty meter does'
		measure = '===|  \[[0-9]\]|consumes|gains nothing'
	},
	@{
		name = 'rest'; script = 'rest.eval'
		what = 'the rest state: what it costs, and the three ways it ends'
		measure = '===|  \[0\]|rest (on|off) \(world'
	},
	@{
		name = 'expedition'; script = 'expedition.eval'
		what = 'fight, retreat, rest, repeat - how many fights a load of supplies buys'
		measure = '===|TALLY |rested [0-9.]+s|  \[0\] Brand|state '
	},
	@{
		name = 'parties'; script = 'smallparty.eval'
		what = 'parties of one, three and two: who the formation lets a monster reach'
		measure = '===|  \[[0-9]\]|TALLY |roster [0-9]|rested [0-9.]+s|state '
	},
	@{
		name = 'rootmotion'; script = 'rootmotion.eval'
		what = 'a walking, then dying, kit skeleton: how far its body strays from its square'
		measure = '--- |  skel_warrior @ '
	},
	@{
		name = 'spawnrise'; script = 'spawnrise.eval'
		what = 'a freshly spawned skeleton holds its square until it is up'
		# The cell must read 14,9 on every line that also says `rising`.
		measure = '--- |  skel_warrior @ '
	}
)

if ($List) {
	Write-Host ''
	Write-Host 'eval suites:'
	foreach ($s in $suites) {
		Write-Host ("  {0,-10} {1,-20} {2}" -f $s.name, $s.script, $s.what)
	}
	Write-Host ''
	Write-Host 'fragments (pulled in by include/sweep, not run directly):'
	Get-ChildItem -Path $scripts -Recurse -Filter *.eval |
		Where-Object { $_.DirectoryName -ne $scripts } |
		ForEach-Object { Write-Host ("  {0}" -f $_.FullName.Substring($scripts.Length + 1)) }
	Write-Host ''
	exit 0
}

if (-not (Test-Path $exe)) {
	Write-Host "eval: no exe at $exe - build first" -ForegroundColor Red
	exit 2
}

# ---------------------------------------------------------------------------
# SELF-TEST: the runner is handed a script with a line that is not a command
# and must exit 1. Without this, "every suite passed" could mean the runner had
# stopped noticing anything at all.
# ---------------------------------------------------------------------------
if ($SelfTest) {
	# -OutFile is for the MEASUREMENT report, which is the thing anybody diffs
	# across a knob change. The self-test is a pass/fail artefact and its output
	# is not comparable run to run, so it does not build a transcript - say so
	# rather than write nothing and let the caller wonder.
	if ($OutFile) {
		Write-Host '-OutFile is ignored with -SelfTest (it saves the measurement report, not this)' -ForegroundColor Yellow
	}
	$bad = Join-Path $scripts 'selftest-bad.eval'
	Write-Host ''
	Write-Host '=== eval self-test: a bad script must FAIL ==='
	$p = Start-Process -FilePath $exe -ArgumentList '-eval', $bad -PassThru -Wait
	Write-Host ("  bad script exited {0} (want 1)" -f $p.ExitCode)

	# ...and a script that cannot be READ is a different failure from one that
	# ran badly, so it gets its own code. A harness that conflated them would
	# report a typo'd path as a measurement.
	$missing = Join-Path $scripts 'no-such-file.eval'
	$q = Start-Process -FilePath $exe -ArgumentList '-eval', $missing -PassThru -Wait
	Write-Host ("  missing script exited {0} (want 2)" -f $q.ExitCode)

	# --- a declined line is counted; a probe must be refused -----------------
	# code-review C442. A setup line the world declines must fail its script,
	# and `expect-refuse` must be exactly as strict the other way: a probe that
	# is refused passes, one that runs clean fails, and one refused only by the
	# console's gate (no game yet) fails too. One batch, and each script must
	# fail for ITS reason alone - the counters on its RESULT line are compared
	# whole, so a script failing for some other cause cannot stand in.
	Write-Host ''
	Write-Host '=== a declined line is counted; a probe must be refused ==='
	$declineRan = Invoke-EvalRun @('-eval',
		(Join-Path $scripts 'selftest-gate.eval'), (Join-Path $scripts 'selftest-refuse.eval'),
		(Join-Path $scripts 'selftest-unrefused.eval'), (Join-Path $scripts 'selftest-declined.eval'))
	$counts = @{}
	foreach ($line in ReadLog) {
		if ($line -match 'eval RESULT=(\w+) script=(\S+) lines=\d+ unknown=(\d+) refused=(\d+) unrefused=(\d+) endstate=(\w+)') {
			$counts[$Matches[2]] = '{0} unknown={1} refused={2} unrefused={3} endstate={4}' -f
				$Matches[1], $Matches[3], $Matches[4], $Matches[5], $Matches[6]
		}
	}
	$declineWant = @(
		@{ script = 'selftest-gate.eval';      want = 'FAIL unknown=0 refused=0 unrefused=1 endstate=playing'
		   what = 'a probe refused only by the gate fails' },
		@{ script = 'selftest-refuse.eval';    want = 'PASS unknown=0 refused=0 unrefused=0 endstate=playing'
		   what = 'a probe its rule refuses passes' },
		@{ script = 'selftest-unrefused.eval'; want = 'FAIL unknown=0 refused=0 unrefused=1 endstate=playing'
		   what = 'a probe that is not refused fails' },
		@{ script = 'selftest-declined.eval';  want = 'FAIL unknown=0 refused=2 unrefused=0 endstate=playing'
		   what = 'each declined setup line fails' }
	)
	$declineOk = $declineRan
	foreach ($d in $declineWant) {
		$got = if ($counts.ContainsKey($d.script)) { $counts[$d.script] } else { '(no verdict)' }
		$good = $got -eq $d.want
		Write-Host ("  {0,-42} {1}" -f $d.what, $(if ($good) { 'ok' } else { "FAIL - got '$got', want '$($d.want)'" })) `
			-ForegroundColor $(if ($good) { 'Gray' } else { 'Red' })
		if (-not $good) { $declineOk = $false }
	}

	# --- a wipe stops the clock; a gap in a batch counts once; a thrown torch -
	# code-review batch 11. ONE batch with an unreadable script in the MIDDLE
	# (C445): the runner must count it once and go straight on, so the batch
	# reads scripts=3 failed=1 and each real script writes exactly one verdict -
	# it used to write the verdict before the gap a second time and count it
	# again. The scripts either side carry the other checks: selftest-wipe.eval
	# steps across a party wipe, which must stop the step and say so with the
	# TALLY's clock stopped with it (C443), and must refuse a `rest until` off
	# the level (C444); selftest-throwcharge.eval throws a torch with 30 s left
	# from the console, which must land with no more than that (C447).
	Write-Host ''
	Write-Host '=== a wipe stops the clock; a gap in a batch counts once; a thrown torch keeps its charge ==='
	$gapRan = Invoke-EvalRun @('-eval', (Join-Path $scripts 'selftest-wipe.eval'),
		(Join-Path $scripts 'no-such-file.eval'), (Join-Path $scripts 'selftest-throwcharge.eval'))
	$gl = @(ReadLog)
	$stepIdx = -1
	for ($i = 0; $i -lt $gl.Count; $i++) {
		if ($gl[$i] -cmatch '^\[info \] console: stepped (\d+) ticks \(([0-9.]+)s\) - stopped: the party was wiped$') { $stepIdx = $i; break }
	}
	$wipedSecs = if ($stepIdx -ge 0) { [double]$Matches[2] } else { -1 }
	$tallySecs = -1
	if ($stepIdx -ge 0) {
		for ($i = $stepIdx + 1; $i -lt $gl.Count; $i++) {
			if ($gl[$i] -cmatch '^\[info \] console: TALLY .* secs=([0-9.]+) ') { $tallySecs = [double]$Matches[1]; break }
		}
	}
	# @() round each call: a scriptblock's output is unrolled, so a single match
	# would come back as a bare string and [0] would be its first character.
	$verdictsOf = { param($name) $gl | Where-Object { $_ -cmatch ('eval RESULT=\w+ script=' + [regex]::Escape($name) + ' ') } }
	$wipeVerdicts = @(& $verdictsOf 'selftest-wipe.eval')
	$torchVerdicts = @(& $verdictsOf 'selftest-throwcharge.eval')
	$cursorLine = @($gl | Where-Object { $_ -cmatch '^\[info \] console:   cursor: torch_lit charge 30\.0$' })
	$floorCharge = -1
	foreach ($line in $gl) {
		if ($line -cmatch '^\[info \] console:   floor torch_lit at 4,3 charge ([0-9.]+)$') { $floorCharge = [double]$Matches[1] }
	}
	$gapChecks = @(
		@{ what = 'a step across a wipe stops and says so'
		   ok = ($wipedSecs -gt 0) -and ($wipedSecs -lt 20)
		   got = $(if ($stepIdx -ge 0) { "stopped at $($wipedSecs)s" } else { 'no "stopped: the party was wiped" line' }) },
		@{ what = '...and the tally stops with it'
		   ok = ($tallySecs -ge 0) -and ([Math]::Abs($tallySecs - $wipedSecs) -le 0.051)
		   got = "tally secs=$tallySecs against $($wipedSecs)s stepped" },
		@{ what = 'rest until off the level is refused'
		   ok = [bool](@($gl | Where-Object { $_ -cmatch "eval: line \d+ refused, as expected: 'rest until 900'" }).Count)
		   got = 'no expected refusal of rest until' },
		@{ what = 'the wipe script: one verdict, PASS'
		   ok = ($wipeVerdicts.Count -eq 1) -and ($wipeVerdicts[0] -cmatch 'RESULT=PASS ')
		   got = "$($wipeVerdicts.Count) verdict(s): $($wipeVerdicts -join ' | ')" },
		@{ what = 'the unreadable script counts once'
		   ok = (@($gl | Where-Object { $_ -cmatch 'eval RESULT=FAIL script=\S*no-such-file\.eval - could not be read' }).Count -eq 1)
		   got = 'not exactly one "could not be read" verdict' },
		@{ what = 'the script after the gap: one verdict, PASS'
		   ok = ($torchVerdicts.Count -eq 1) -and ($torchVerdicts[0] -cmatch 'RESULT=PASS ')
		   got = "$($torchVerdicts.Count) verdict(s): $($torchVerdicts -join ' | ')" },
		@{ what = 'the batch counts scripts=3 failed=1'
		   ok = [bool](@($gl | Where-Object { $_ -cmatch 'eval BATCH RESULT=FAIL scripts=3 failed=1$' }).Count)
		   got = "$(@($gl | Where-Object { $_ -match 'eval BATCH RESULT=' }) -join ' | ')" },
		@{ what = 'a part-burnt torch went up on the cursor'
		   ok = [bool]$cursorLine.Count
		   got = 'no "cursor: torch_lit charge 30.0" line' },
		@{ what = '...and came down with what it had'
		   ok = ($floorCharge -gt 0) -and ($floorCharge -le 30)
		   got = $(if ($floorCharge -ge 0) { "it lies with $floorCharge s" } else { 'no floor torch at 4,3' }) }
	)
	$gapOk = $gapRan
	foreach ($c in $gapChecks) {
		Write-Host ("  {0,-42} {1}" -f $c.what, $(if ($c.ok) { 'ok' } else { "FAIL - $($c.got)" })) `
			-ForegroundColor $(if ($c.ok) { 'Gray' } else { 'Red' })
		if (-not $c.ok) { $gapOk = $false }
	}

	# --- `reset` really equals a new game ------------------------------------
	# The recycling the whole batch form rests on. resettest.eval takes a
	# baseline after a real load, wrecks the world every way the harness can,
	# resets, and takes it again; the two blocks must match line for line.
	#
	# It is HERE rather than in the suite list because it is not a measurement -
	# nothing about it is a number to compare across knob changes. It is a check,
	# and a check belongs with the other checks.
	Write-Host ''
	Write-Host '=== reset must equal a new game ==='
	# Its repros SAVE and LOAD, and the save folder is one folder every checkout
	# shares: another worktree's run of this same script would overwrite the save
	# between a `save` and its `load`. So the run takes a COPY of the script whose
	# slot is named for this checkout (HarnessGame.ps1), written into this build's
	# bin, and the save is deleted again after the second run below.
	$resetSave = Get-HarnessSaveName $root 'resettest'
	$resetScript = Copy-EvalScript (Join-Path $scripts 'resettest.eval') (Join-Path $bin 'eval-copies') @{ resettest = $resetSave }
	$resetRan = Invoke-EvalRun @('-eval', $resetScript)
	$soloLog = @(ReadLog)
	$solo = Get-ResetBlocks $soloLog
	$diff = Compare-Blocks $solo.A $solo.B
	# Both baselines carry the `transients` readout (code-review batch 12): what a
	# reset must clear that no other line shows. Demanded, so a resettest.eval
	# that lost it cannot go on comparing blocks blind to the leaks it is for.
	$hasTransients = [bool](@($solo.A | Where-Object { $_ -cmatch '^transients on \S+$' }).Count)
	if ($diff -eq '' -and -not $hasTransients) { $diff = 'no transients readout in the baseline' }
	# ...and the HUD log the party arrives to (code-review C364: the recycle and
	# a new game begin play through one tail). Demanded NON-EMPTY: a new game
	# opens with lines, and two empty logs would compare equal saying nothing.
	$hasMessages = [bool](@($solo.A | Where-Object { $_ -cmatch '^messages: [1-9]\d*$' }).Count)
	if ($diff -eq '' -and -not $hasMessages) { $diff = 'no non-empty messages readout in the baseline' }
	$resetOk = $resetRan -and ($diff -eq '')
	Write-Host ("  {0} baseline lines compared - {1}" -f $solo.A.Count,
		$(if ($resetOk) { 'identical' } else { $diff }))

	# --- ...and a LOAD or a STAIR leaves nothing behind either -------------------
	# code-review batch 77 (C292, C293). The blocks above compare a reset with a
	# new game, and the harness's reset cleared everything - but a real load and a
	# stair each cleared their own hand-copied list: a gas went on biting the next
	# floor or the loaded game, a burning monster kept burning, a door smashed
	# after the save came back from the load wrecked, and the fixture table handed
	# a smashed sconce's wreck to the next level's sconce on that square.
	# resettest.eval's wrecking opens with a repro of each (read its header).
	# Batch 78 (C294, C295, C297) adds three of what the GAME had running - a
	# rest, the undo history, a throw's wait and the kindle clock - which only the
	# harness's reset ended, and that by writing the rest flag past the lockstep
	# hand-back: a load, a new game and a reset must each end all of it.
	$reproOk = $resetRan
	# The script's own verdict too: the repros SMASH what must be there and
	# place what they wreck, and a smash or a placement that found nothing is a
	# refusal - which fails the script, and nothing else here would say so in the
	# solo run (the batched one below demands failed=0).
	$soloVerdicts = @($soloLog | Where-Object { $_ -cmatch 'eval RESULT=\w+ script=resettest\.eval ' })
	$reproRows = @(@{ what = 'resettest.eval: one verdict, PASS'
		ok = ($soloVerdicts.Count -eq 1) -and ($soloVerdicts[0] -cmatch 'RESULT=PASS ')
		got = "$($soloVerdicts.Count) verdict(s): $($soloVerdicts -join ' | ')" }) + @(Get-ReproChecks $soloLog)
	foreach ($c in $reproRows) {
		Write-Host ("  {0,-42} {1}" -f $c.what, $(if ($c.ok) { "ok - $($c.got)" } else { "FAIL - $($c.got)" })) `
			-ForegroundColor $(if ($c.ok) { 'Gray' } else { 'Red' })
		if (-not $c.ok) { $reproOk = $false }
	}

	# --- ...and a reset from ANOTHER LEVEL is the same reset ------------------
	# code-review C300. `reset` re-read whatever level it found, so a suite
	# batched after one that left the harness level ran somewhere else - and an
	# ambush leaves "~encounter", which has no file: the re-read hit the map
	# loader's assert and the run died. selftest-leavelevel.eval walks off the
	# level every way the harness can (a goto, the world map, an ambush) and
	# ends on the world map above the encounter; resettest.eval after it in ONE
	# batch must then print the solo run's blocks line for line - BOTH of them,
	# since its first reset is now the switch back and its second a recycle.
	# HEADLESS, because the defect this exists for is an ASSERT: a windowed debug
	# game parks on its CRT dialog and the self-test waits forever, where a
	# headless one records the FATAL, dumps and exits (crash::SetUnattended), and
	# the run reads as not finished. (Headless against windowed changes nothing;
	# the check below that says so runs every time.)
	Write-Host ''
	Write-Host '=== a reset from another level must match a solo run ==='
	$awayRan = Invoke-EvalRun @('-headless', '-eval', (Join-Path $scripts 'selftest-leavelevel.eval'),
		$resetScript)
	Remove-HarnessSaves @($resetSave)
	$al = @(ReadLog)
	# Its repros, as in the solo run: the same sixteen rows must hold after the
	# switch back from another level.
	$awayRepro = @(Get-ReproChecks $al)
	$awayReproBad = @($awayRepro | Where-Object { -not $_.ok })
	$al | Where-Object { $_ -cmatch 'FATAL' } | Select-Object -First 1 |
		ForEach-Object { Write-Host ("  {0}" -f $_) -ForegroundColor Red }
	$away = Get-ResetBlocks $al
	$awayA = Compare-Blocks $solo.A $away.A
	$awayB = Compare-Blocks $solo.B $away.B
	$switchLine = @($al | Where-Object { $_ -cmatch '^\[info \] console: reset: switched in \d+ ms \(from ~encounter to ' })
	# WHAT THE RESET HAD TO FORGET. The first script carves the harness level
	# before it leaves, and its last `transients` names every level still held
	# in memory; the harness level must be among them. A level load takes a
	# stash over the file, so a reset that kept the stashes would bring the
	# carved arena back into baseline A (and its `stashed` line would not read
	# none) - but only if there WAS a stash, which is what this demands.
	$ground = ''
	if ($switchLine.Count -and $switchLine[0] -cmatch ' to (\S+), by a level load\)$') { $ground = $Matches[1] }
	$endIdx = -1
	for ($i = 0; $i -lt $al.Count; $i++) {
		if ($al[$i] -cmatch '^\[info \] console: transients on ~encounter$') { $endIdx = $i }
	}
	$leftLine = ''
	if ($endIdx -ge 0) {
		for ($i = $endIdx + 1; $i -lt $al.Count; $i++) {
			if ($al[$i] -cmatch '^\[info \] console:   stashed maps=') { $leftLine = $al[$i]; break }
		}
	}
	$leftLevels = @()
	if ($leftLine -cmatch ' levels=(.+)$') { $leftLevels = @($Matches[1] -split ' ') }
	# THE AMBUSH DROPS THE UNDO HISTORY (code-review C297). The first script paints
	# a square of crypt1 and walks out, which keeps the step (crypt1 is parked);
	# the ambush then puts a generated level where crypt1 was, and an undo there
	# restored crypt1's map under "~encounter". The verdict brackets the ambush
	# ALONE: the readout just before it, on the world map (the SECOND "transients
	# on crypt1" - the parked level is still current), must count the step, and
	# the one just after it, inside the encounter before its own `leave` (the
	# FIRST "transients on ~encounter"), none. Taken across the walk out and the
	# encounter's leave as well, either of those starting to clear the history
	# would let an ambush that stopped read ok. The readout in crypt1 itself says
	# the paint took a step at all.
	$cryptIdx = @(for ($i = 0; $i -lt $al.Count; $i++) {
		if ($al[$i] -cmatch '^\[info \] console: transients on crypt1$') { $i }
	})
	$ambushIdx = -1
	for ($i = 0; $i -lt $al.Count; $i++) {
		if ($al[$i] -cmatch '^\[info \] console: transients on ~encounter$') { $ambushIdx = $i; break }
	}
	$cryptT = if ($cryptIdx.Count -ge 1) { Read-Transients $al $cryptIdx[0] } else { $null }
	$worldT = if ($cryptIdx.Count -ge 2) { Read-Transients $al $cryptIdx[1] } else { $null }
	# The ambush's readout must not be the script's last one (that is after the
	# encounter's leave); two readouts on ~encounter, or this is not the bracket.
	$encT = if ($ambushIdx -ge 0 -and $ambushIdx -lt $endIdx) { Read-Transients $al $ambushIdx } else { $null }
	$sayUndo = { param($t) if ($null -eq $t) { 'no readout' } else { "on $($t.level): undo=$($t.undo) redo=$($t.redo)" } }
	$awayChecks = @(
		@{ what = 'the first script ended off the level'
		   ok = [bool](@($al | Where-Object { $_ -cmatch '^\[info \] console: transients on ~encounter$' }).Count) -and
				[bool](@($al | Where-Object { $_ -cmatch 'eval RESULT=PASS script=selftest-leavelevel\.eval ' }).Count)
		   got = 'no PASS ending on ~encounter' },
		@{ what = '...holding the harness level stashed'
		   ok = ($ground -ne '') -and ($leftLevels -ccontains $ground)
		   got = "harness level '$ground', $(if ($leftLine) { "'$($leftLine -replace '^\[info \] console:\s+', '')'" } else { 'no stashed line' })" },
		@{ what = 'it took an undo step in crypt1'
		   ok = [bool]($cryptT -and $cryptT.undo -ge 1); got = & $sayUndo $cryptT },
		@{ what = '...the walk out kept it'
		   ok = [bool]($worldT -and $worldT.undo -ge 1); got = & $sayUndo $worldT },
		@{ what = '...and the ambush took the history'
		   ok = [bool]($encT -and $encT.level -ceq '~encounter' -and $encT.undo -eq 0 -and $encT.redo -eq 0)
		   got = & $sayUndo $encT },
		@{ what = 'the reset after it went back by a load'
		   ok = [bool]$switchLine.Count
		   got = 'no "reset: switched ... from ~encounter" line' },
		@{ what = 'its baseline A matches the solo run'
		   ok = ($awayA -eq '')
		   got = $awayA },
		@{ what = 'its baseline B matches the solo run'
		   ok = ($awayB -eq '')
		   got = $awayB },
		@{ what = 'its load and stair repros hold'
		   ok = ($awayReproBad.Count -eq 0)
		   got = "$($awayReproBad.Count) of $($awayRepro.Count) failed, first: $(if ($awayReproBad.Count) { "$($awayReproBad[0].what) ($($awayReproBad[0].got))" })" },
		@{ what = 'the batch counts scripts=2 failed=0'
		   ok = [bool](@($al | Where-Object { $_ -cmatch 'eval BATCH RESULT=PASS scripts=2 failed=0$' }).Count)
		   got = "$(@($al | Where-Object { $_ -match 'eval BATCH RESULT=' }) -join ' | ')" }
	)
	$awayOk = $awayRan
	foreach ($c in $awayChecks) {
		Write-Host ("  {0,-42} {1}" -f $c.what, $(if ($c.ok) { 'ok' } else { "FAIL - $($c.got)" })) `
			-ForegroundColor $(if ($c.ok) { 'Gray' } else { 'Red' })
		if (-not $c.ok) { $awayOk = $false }
	}

	# --- and BATCHING changes nothing ----------------------------------------
	# A suite must measure the same thing whether it ran alone or after another.
	# Without this the speedup could be quietly buying wrong numbers - which is
	# the failure mode that matters, because it looks exactly like a fast run.
	Write-Host ''
	Write-Host '=== a batched suite must match a solo one ==='
	$probe = Join-Path $scripts 'supplies.eval'
	$grab = { ReadLog | Where-Object { $_ -cmatch '^\[info \] console:   \[0\] Brand' } }
	$soloRan = Invoke-EvalRun @('-eval', $probe)
	$solo = & $grab
	$batchRan = Invoke-EvalRun @('-eval', (Join-Path $scripts 'resources.eval'), $probe)
	$batched = @(& $grab | Select-Object -Last $solo.Count)
	$batchOk = $soloRan -and $batchRan -and ($solo.Count -gt 0) -and ($solo.Count -eq $batched.Count) -and
			   -not @(0..($solo.Count - 1) | Where-Object { $solo[$_] -cne $batched[$_] }).Count
	Write-Host ("  {0} lines compared - {1}" -f $solo.Count,
		$(if ($batchOk) { 'identical batched and solo' } else { 'DIFFERENT' }))

	# --- and HEADLESS changes nothing ----------------------------------------
	# Same argument as the batching check above, for the same reason: a mode that
	# quietly measured something else would look exactly like a mode that worked.
	# Headless skips the whole render half of the frame, and the risk is that
	# something the simulation depends on was living in there - the staged
	# loader's frame counter already was, and only turned up because the load hung.
	Write-Host ''
	Write-Host '=== a headless run must match a windowed one ==='
	# EVERY console line is compared, so the one line that is wall-clock BY DESIGN
	# has its number masked: `reset` times itself ("loaded in 14 ms"), and two
	# runs round to different milliseconds often enough that this check failed on
	# a coin toss, blaming headless for a timer. Masked, not dropped, so a reset
	# line that goes missing on one side still counts as a difference.
	$grabAll = { ReadLog | Where-Object { $_ -cmatch '^\[info \] console: ' } |
		ForEach-Object { $_ -creplace '^(\[info \] console: reset: \w+ in )\d+( ms)', '${1}#${2}' } }
	$winRan = Invoke-EvalRun @('-eval', $probe)
	$windowed = @(& $grabAll)
	$hidRan = Invoke-EvalRun @('-headless', '-eval', $probe)
	$hidden = @(& $grabAll)
	$headBad = @(0..([Math]::Max($windowed.Count, $hidden.Count) - 1) |
		Where-Object { $windowed[$_] -cne $hidden[$_] })
	$headOk = $winRan -and $hidRan -and ($windowed.Count -gt 0) -and ($headBad.Count -eq 0)
	Write-Host ("  {0} lines compared - {1}" -f $windowed.Count,
		$(if ($headOk) { 'identical headless and windowed' }
		  elseif ($windowed.Count -eq 0) { 'DIFFERENT: the windowed run printed nothing' }
		  else { "DIFFERENT: $($headBad.Count) line(s), first '$($windowed[$headBad[0]])' vs '$($hidden[$headBad[0]])'" }))

	# --- and the NUMBERS still move ------------------------------------------
	# EVERY CHECK ABOVE IS PLUMBING. They prove the runner runs, that recycling
	# and batching and headless change nothing - and not one of them asks whether
	# the TALLY still describes the fight. A counting site could be deleted
	# tomorrow and this harness would report ten green suites forever
	# (docs/eval-audit.md F1, which is the finding the whole audit turned on).
	#
	# respond.eval runs three pairs of arms, each identical but for one knob the
	# combat model says MUST move one of the three headline numbers. Thresholds
	# are set from MEASURED separations and left deliberately loose: this asks
	# whether the instrument is ALIVE, not whether the balance is right. A number
	# that stops responding fails; a number that responds differently after a
	# balance change does not.
	Write-Host ''
	Write-Host '=== a knob that must move a number, moves it ==='
	$respondRan = Invoke-EvalRun @('-eval', (Join-Path $scripts 'respond.eval'))
	$arm = $null
	$samples = @{}
	foreach ($line in ReadLog) {
		if ($line -cmatch '^\[info \] console: ARM (\S+)$') { $arm = $Matches[1]; $samples[$arm] = @(); continue }
		# hitrate is `n/a` when nothing swung - a rate over no trials is undefined,
		# not zero. Such a sample contributes its damage and its downs but must
		# not drag a hit-rate average toward 0, so Rate is $null and the average
		# below skips it.
		if ($arm -and $line -cmatch ('^\[info \] console: TALLY dealt=([0-9.]+) taken=([0-9.]+) ' +
									 'swings=(\d+) hits=(\d+) misses=(\d+) hitrate=([0-9.]+|n/a) ' +
									 'crits=(\d+) fumbles=(\d+) slain=(\d+) downed=(\d+)')) {
			$samples[$arm] += [pscustomobject]@{
				Dealt = [double]$Matches[1]; Taken = [double]$Matches[2]
				Swings = [int]$Matches[3]
				Rate = $(if ($Matches[6] -eq 'n/a') { $null } else { [double]$Matches[6] })
				Downed = [int]$Matches[10]
			}
		}
	}
	# A MISSING OR EMPTY ARM MUST FAIL, not divide by zero and pass. This is the
	# guard that keeps the whole check non-vacuous: if the tally went dead every
	# aggregate below would be 0, and a ratio of 0/0 must not read as "unchanged".
	$armNames = @('dex-low', 'dex-high', 'skill-low', 'skill-high', 'threat-low', 'threat-high')
	$respondOk = $respondRan
	$missing = @($armNames | Where-Object { -not $samples.ContainsKey($_) -or $samples[$_].Count -lt 3 })
	if ($missing.Count) {
		Write-Host ("  arms missing or too small: {0}" -f ($missing -join ', ')) -ForegroundColor Red
		$respondOk = $false
	}
	if ($respondOk) {
		$agg = @{}
		foreach ($a in $armNames) {
			$g = $samples[$a]
			$sw = ($g | Measure-Object Swings -Sum).Sum
			# Samples with no swings carry Rate = $null and are EXCLUDED from the
			# average rather than counted as zero, which is the same distinction
			# the `n/a` exists to make.
			$rated = @($g | Where-Object { $null -ne $_.Rate })
			$agg[$a] = [pscustomobject]@{
				N = $g.Count
				Rate = $(if ($rated.Count) { ($rated | Measure-Object Rate -Average).Average } else { 0 })
				Swings = $sw
				PerSwing = $(if ($sw -gt 0) { ($g | Measure-Object Dealt -Sum).Sum / $sw } else { 0 })
				Taken = ($g | Measure-Object Taken -Average).Average
				Downed = ($g | Measure-Object Downed -Sum).Sum
			}
		}
		# Each row: what must be true, why that threshold, and what was measured
		# when it was written. Ratios are 1.3x against separations of 2.0x and
		# 1.6x; the `taken` pair is a difference rather than a ratio because its
		# low arm sits near zero and a ratio there is meaningless.
		$tests = @(
			@{ what = 'hitrate responds to DEX'
			   got  = $agg['dex-high'].Rate; ref = $agg['dex-low'].Rate
			   ok   = ($agg['dex-low'].Rate -gt 0) -and ($agg['dex-high'].Rate -ge $agg['dex-low'].Rate * 1.3)
			   note = 'want high >= low x1.3 (measured 0.336 -> 0.668)' }
			@{ what = 'dealt-per-swing responds to weapon skill'
			   got  = $agg['skill-high'].PerSwing; ref = $agg['skill-low'].PerSwing
			   ok   = ($agg['skill-low'].PerSwing -gt 0) -and ($agg['skill-high'].PerSwing -ge $agg['skill-low'].PerSwing * 1.3)
			   note = 'want high >= low x1.3 (measured 11.02 -> 17.84)' }
			@{ what = 'taken responds to monster strength'
			   got  = $agg['threat-high'].Taken; ref = $agg['threat-low'].Taken
			   ok   = ($agg['threat-high'].Taken -gt 50) -and (($agg['threat-high'].Taken - $agg['threat-low'].Taken) -gt 60)
			   note = 'want high > 50 and high - low > 60 (measured 19.2 -> 185.1)' }
			@{ what = 'downed is counted at all'
			   got  = $agg['threat-high'].Downed; ref = $agg['threat-low'].Downed
			   ok   = ($agg['threat-high'].Downed -gt 0)
			   note = 'want high > 0 (measured 2 -> 11)' }
		)
		foreach ($t in $tests) {
			Write-Host ("  {0,-42} {1,8:N2} vs {2,8:N2}  {3}" -f $t.what, $t.got, $t.ref,
				$(if ($t.ok) { 'ok' } else { 'FAIL - ' + $t.note })) `
				-ForegroundColor $(if ($t.ok) { 'Gray' } else { 'Red' })
			if (-not $t.ok) { $respondOk = $false }
		}
	}

	# --- one run per worktree; a killed run does not count -------------------
	# Two runs of THIS script, as a person would start them (code-review C430).
	# The first runs a real suite; while its game is up a second must be REFUSED
	# (exit 3) - it would share that game's dungeon.log. Then the first's game is
	# killed, by PID, and the first must say the game did not finish and exit
	# non-zero rather than read its partial log as a result. Both find this bin
	# muted already and must say so (C433: every harness logs its mute state).
	Write-Host ''
	Write-Host '=== a second run is refused; a killed run does not count ==='
	$outA = Join-Path $env:TEMP "eval-selftest-first-$PID.txt"
	$outB = Join-Path $env:TEMP "eval-selftest-second-$PID.txt"
	$child = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
		'-Config', $Config, '-Only', 'supplies', '-Headless')
	$first = Start-Process powershell -ArgumentList $child -PassThru -WindowStyle Hidden -RedirectStandardOutput $outA
	# Touch the handle NOW: a Process from Start-Process without -Wait that never
	# had it read reports ExitCode as $null after it ends - and $null -ne 0, so
	# "exits non-zero" would pass on no exit code at all.
	$null = $first.Handle
	$game = $null
	$deadline = (Get-Date).AddSeconds(60)
	while (-not $game -and -not $first.HasExited -and (Get-Date) -lt $deadline) {
		Start-Sleep -Milliseconds 300
		$game = Get-Process Dungeon -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Select-Object -First 1
	}
	$second = Start-Process powershell -ArgumentList $child -PassThru -Wait -WindowStyle Hidden -RedirectStandardOutput $outB
	if ($game) { Start-Sleep -Seconds 2; $game.Kill() }
	if (-not $first.WaitForExit(120000)) { $first.Kill() }
	$saidA = if (Test-Path $outA) { Get-Content $outA -Raw } else { '' }
	$saidB = if (Test-Path $outB) { Get-Content $outB -Raw } else { '' }
	Remove-Item $outA, $outB -ErrorAction SilentlyContinue
	$guards = @(
		@{ what = "the first run's game started";        ok = [bool]$game },
		@{ what = 'a second run beside it is refused';  ok = ($second.ExitCode -eq 3) -and ($saidB -match 'refused: ') },
		@{ what = 'the killed run says it did not finish'; ok = ($saidA -match 'THE GAME DID NOT FINISH') },
		@{ what = '...and exits non-zero';              ok = ($first.ExitCode -is [int]) -and ($first.ExitCode -ne 0) },
		@{ what = 'both said the bin was already muted'; ok = ($saidA -match 'audio: already muted') -and ($saidB -match 'audio: already muted') }
	)
	$guardOk = $true
	foreach ($g in $guards) {
		Write-Host ("  {0,-42} {1}" -f $g.what, $(if ($g.ok) { 'ok' } else { 'FAIL' })) -ForegroundColor $(if ($g.ok) { 'Gray' } else { 'Red' })
		if (-not $g.ok) { $guardOk = $false }
	}
	Write-Host ("  (second run exited {0}, first {1})" -f $second.ExitCode, $first.ExitCode)

	# --- a headless run never shows its window -------------------------------
	# The harnesses run the build Michael plays, and share its settings.ini. With
	# his saved display mode applied, every headless run showed its window, a
	# saved Borderless covered a monitor in black, and a saved Exclusive switched
	# the display (code-review C391). So under each saved mode a headless run is
	# WATCHED: its window, found by PID and class, must exist and never be
	# visible, and the run must finish. The ini is put back whatever happens.
	Write-Host ''
	Write-Host '=== a headless run never shows its window (Windowed, Borderless, Exclusive saved) ==='
	$ini = Join-Path $bin 'settings.ini'
	$iniBefore = if (Test-Path $ini) { [IO.File]::ReadAllText($ini) } else { $null }
	$hiddenOk = $true
	try {
		foreach ($mode in 0, 1, 2) {
			$keep = @((("$iniBefore") -split "\r?\n") | Where-Object { $_ -ne '' -and $_ -notmatch '^fullscreen=' })
			[IO.File]::WriteAllText($ini, ((@($keep) + "fullscreen=$mode") -join "`n") + "`n")
			Remove-Item $log -ErrorAction SilentlyContinue
			$g = Start-Process -FilePath $exe -ArgumentList '-headless', '-eval', (Join-Path $scripts 'smoke.eval') -PassThru
			$null = $g.Handle
			$found = $false
			$shown = $false
			while (-not $g.HasExited) {
				$h = [HarnessWin]::FindByPid([uint32]$g.Id, $HarnessWindowClass)
				if ($h -ne [IntPtr]::Zero) {
					$found = $true
					if ([HarnessWin]::IsWindowVisible($h)) { $shown = $true }
				}
				Start-Sleep -Milliseconds 50
			}
			$done = [bool](ReadLog | Where-Object { $_ -match 'eval BATCH RESULT=' })
			$fine = $found -and -not $shown -and $done
			if (-not $fine) { $hiddenOk = $false }
			Write-Host ("  {0,-10} window {1}, {2}, run {3}  {4}" -f @('Windowed', 'Borderless', 'Exclusive')[$mode],
				$(if ($found) { 'found' } else { 'NEVER FOUND' }), $(if ($shown) { 'SHOWN' } else { 'never shown' }),
				$(if ($done) { 'finished' } else { 'DID NOT FINISH' }), $(if ($fine) { 'ok' } else { 'FAIL' })) `
				-ForegroundColor $(if ($fine) { 'Gray' } else { 'Red' })
		}
	} finally {
		if ($null -eq $iniBefore) { Remove-Item $ini -ErrorAction SilentlyContinue }
		else { [IO.File]::WriteAllText($ini, $iniBefore) }
	}

	# --- what the load paths leave resident, and what they free in flight -----
	# code-review batch 64, one WARP run of lifetimes.eval (read its header):
	#   C193  `reset` and `arena` rebuild the surfaces with frames in flight, and
	#         the D3D12 debug layer must see no error. ON WARP, because on a GPU
	#         those frames had finished long before and the race never ran: with
	#         the drain cut out, this run logged error 921 on WARP and nothing on
	#         the GPU.
	#   C222  no file in the model cache pins CPU image bytes after the load, and
	#         the load did build multi-material models (or "0 pinned" is vacuous).
	#   C154  Low -> Ultra -> Low: each swap leaves every set at the new tier
	#         (stale=0); a prop set's albedo changes size with the tier and comes
	#         back to it; the SRV gauge, live and peak, ends where it stood right
	#         after the load. That baseline is read BEFORE any swap, and the run
	#         starts at Medium (quality=1: Low's 1k tier, checked) whatever the
	#         ini says: started at High or Ultra, the first `quality 0` would be
	#         a full swap of its own, so a baseline read after it would already
	#         carry whatever peak a load-before-free swap adds, and the row could
	#         not see one.
	#   C471  a planted set with no normal map (zz_selftest_nonormal, a copy of a
	#         UI icon, removed again) is named by `levelcheck`, and loads with a
	#         FLAT normal and exactly one warning - not the magenta checker.
	#         levelcheck's list must be EXACTLY the pool's albedos with no `_n`
	#         stem beside them, worked out here from the disk: naming the plant
	#         alone would pass a check that listed every albedo in the pool.
	Write-Host ''
	Write-Host '=== load-path lifetimes: a clean device on WARP, every set at its tier, no pinned images ==='
	$texDir = Join-Path $root 'assets\textures'
	$plant = Join-Path $texDir 'zz_selftest_nonormal_2k.png'
	$plantStem = 'zz_selftest_nonormal_2k'
	$lifeRan = $false
	$diskNoNormal = @()
	try {
		Copy-Item (Join-Path $root 'assets\ui\icon_close.png') $plant -ErrorAction Stop
		# THE DISK'S ANSWER, independent of AssetUtil: every map stem (.png and
		# .dds alike - either loads), and the resolution-tagged albedos among
		# them with no `<stem>_n` beside. Ordinal, as the C++ compares.
		$stemSet = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		foreach ($f in Get-ChildItem -LiteralPath $texDir -File) {
			if ($f.Extension -eq '.png' -or $f.Extension -eq '.dds') { [void]$stemSet.Add($f.BaseName) }
		}
		$diskNoNormal = @($stemSet | Where-Object { $_ -cmatch '_(1k|2k|4k)$' -and -not $stemSet.Contains($_ + '_n') })
		$lifeRan = Invoke-WithIni (@($warpIni) + 'quality=1') {
			Invoke-EvalRunTimed @('-warp', '-eval', (Join-Path $scripts 'lifetimes.eval')) 600
		}
	} catch {
		Write-Host ("  could not run it: {0}" -f $_.Exception.Message) -ForegroundColor Red
	} finally {
		Remove-Item $plant -ErrorAction SilentlyContinue
	}
	$life = ReadLog
	$con = @($life | Where-Object { $_ -cmatch '^\[info \] console: (?!> )' } |
		ForEach-Object { $_ -replace '^\[info \] console: ', '' })
	# The readout lines after an echoed marker: its head line, then its rows.
	function After([string]$marker) {
		$i = [Array]::IndexOf($con, $marker)
		if ($i -lt 0) { return @() }
		$out = @()
		for ($j = $i + 1; $j -lt $con.Count -and -not $con[$j].StartsWith('---'); ++$j) { $out += $con[$j] }
		return $out
	}
	function Head($rows) {
		$h = @($rows | Where-Object { $_ -match '^textures tier=' }) | Select-Object -First 1
		if (-not $h) { return $null }
		$o = @{}
		foreach ($kv in ([regex]::Matches($h, '(\w+)=(\S+)'))) { $o[$kv.Groups[1].Value] = $kv.Groups[2].Value }
		return $o
	}
	function Props($rows) {
		$o = @{}
		foreach ($r in $rows) { if ($r -match '^  prop (\S+) tier=\S+ albedo=(\S+)') { $o[$Matches[1]] = $Matches[2] } }
		return $o
	}
	$dev = Get-DeviceReport $life
	$debugCfg = $Config -eq 'debug'
	$cache = @($con | Where-Object { $_ -match '^modelcache ' })
	$cacheFirst = if ($cache.Count) { $cache[0] } else { '' }
	$lc = @($con | Where-Object { $_ -match '^levelcheck RESULT=' }) | Select-Object -First 1
	$lcCount = if ($lc -match 'missing_normals=(\d+)') { [int]$Matches[1] } else { -1 }
	# What levelcheck named, against the disk's answer worked out before the run:
	# `named` it listed although the disk has a normal for it (or it is no
	# albedo at all), `missed` the disk lacks a normal for but it did not list.
	$listed = @($con | ForEach-Object { if ($_ -cmatch '^\s*NO NORMAL MAP (\S+) - it loads flat$') { $Matches[1] } })
	$diskSet = [Collections.Generic.HashSet[string]]::new([string[]]$diskNoNormal, [StringComparer]::Ordinal)
	$listSet = [Collections.Generic.HashSet[string]]::new([string[]]$listed, [StringComparer]::Ordinal)
	$named = @($listed | Where-Object { -not $diskSet.Contains($_) })
	$missed = @($diskNoNormal | Where-Object { -not $listSet.Contains($_) })
	$low = After '--- low ---'; $ultra = After '--- ultra ---'; $again = After '--- low again ---'
	$h0 = Head (After '--- loaded ---'); $hl = Head $low; $hu = Head $ultra; $ha = Head $again
	$pl = Props $low; $pu = Props $ultra; $pa = Props $again
	$moved = @($pl.Keys | Where-Object { $pu.ContainsKey($_) -and $pu[$_] -ne $pl[$_] })
	$back = @($pl.Keys | Where-Object { $pa[$_] -ne $pl[$_] })
	$probe = @($con | Where-Object { $_ -match '^textures: prop zz_selftest_nonormal ' }) | Select-Object -First 1
	$flatWarn = @($life | Where-Object { $_ -match "texture set 'zz_selftest_nonormal' has no normal map" })
	$checker = @($life | Where-Object { $_ -match 'Missing texture .*zz_selftest_nonormal' })
	$checks = @(
		@{ what = 'the WARP run finished'; ok = $lifeRan -and $dev.Warp }
		@{ what = 'C193 the debug layer listened, and saw no error'
		   ok = (-not $debugCfg) -or ($dev.Layer -and $dev.Errors.Count -eq 0)
		   say = $(if (-not $debugCfg) { "($Config build: no layer, not checked)" }
				   elseif (-not $dev.Layer) { 'NO LAYER was listening' }
				   else { "$($dev.Errors.Count) error(s)" + $(if ($dev.Errors.Count) { ': ' + ($dev.Errors[0] -replace '^\[ERROR\] ', '') } else { '' }) }) }
		@{ what = 'C222 no CPU image bytes pinned after the load'
		   ok = ($cacheFirst -match 'pinned_bytes=0 ') -and ($cacheFirst -match ' multi=([1-9]\d*) ') -and
				-not @($cache | Where-Object { $_ -notmatch 'pinned_bytes=0 ' }).Count
		   say = $cacheFirst }
		@{ what = 'C471 levelcheck names exactly the sets with no _n'
		   ok = $listed -ccontains $plantStem -and $named.Count -eq 0 -and $missed.Count -eq 0 -and
				$listed.Count -eq $diskNoNormal.Count -and $lcCount -eq $listed.Count
		   say = "$lcCount listed, $($diskNoNormal.Count) on disk" +
				 $(if ($named.Count) { "; $($named.Count) HAVE a normal (e.g. $($named[0]))" } else { '' }) +
				 $(if ($missed.Count) { "; $($missed.Count) MISSED (e.g. $($missed[0]))" } else { '' }) +
				 $(if (-not ($listed -ccontains $plantStem)) { "; the plant NOT named" } else { '' }) }
		@{ what = 'C471 it loads with a flat normal and ONE warning'
		   ok = ($probe -match 'normal=flat') -and $flatWarn.Count -eq 1 -and $checker.Count -eq 0
		   say = "$probe; warnings $($flatWarn.Count), checker $($checker.Count)" }
		@{ what = 'C154 each swap leaves every set at its tier'
		   ok = $hl -and $hu -and $ha -and $hl.tier -eq '1k' -and $hu.tier -eq '4k' -and $ha.tier -eq '1k' -and
				$hl.stale -eq '0' -and $hu.stale -eq '0' -and $ha.stale -eq '0' -and [int]$hl.props -gt 0
		   say = "tiers $($hl.tier) / $($hu.tier) / $($ha.tier), stale $($hl.stale) / $($hu.stale) / $($ha.stale), props $($hl.props)" }
		@{ what = 'C154 the props change size with the tier, and come back'
		   ok = $moved.Count -gt 0 -and $back.Count -eq 0 -and $pl.Count -gt 0
		   say = "$($moved.Count) of $($pl.Count) prop set(s) resized at Ultra" + $(if ($moved.Count) { " (e.g. $($moved[0]) $($pl[$moved[0]]) -> $($pu[$moved[0]]))" } else { '' }) + "; $($back.Count) not back at Low" }
		@{ what = 'C154 the SRV gauge ends where the load left it'
		   ok = $h0 -and $ha -and $h0.tier -eq '1k' -and $h0.srv_live -eq $ha.srv_live -and $h0.srv_peak -eq $ha.srv_peak
		   say = "loaded at $($h0.tier); live $($h0.srv_live) -> $($hl.srv_live) -> $($hu.srv_live) -> $($ha.srv_live), peak $($h0.srv_peak) -> $($hl.srv_peak) -> $($hu.srv_peak) -> $($ha.srv_peak)" }
	)
	$lifeOk = $true
	foreach ($c in $checks) {
		Write-Host ("  {0,-50} {1}  {2}" -f $c.what, $(if ($c.ok) { 'ok  ' } else { 'FAIL' }), $c.say) `
			-ForegroundColor $(if ($c.ok) { 'Gray' } else { 'Red' })
		if (-not $c.ok) { $lifeOk = $false }
	}

	$ok = ($p.ExitCode -eq 1) -and ($q.ExitCode -eq 2) -and $declineOk -and $gapOk -and $resetOk -and $reproOk -and $awayOk -and $batchOk -and $headOk -and $respondOk -and $guardOk -and $hiddenOk -and $lifeOk
	Write-Host ''
	Write-Host ("eval RESULT={0} self_test=1" -f $(if ($ok) { 'PASS' } else { 'FAIL' }))
	if ($ok) { Write-Host 'the runner reports both failures, counts a declined line and holds a probe to its refusal, stops the clock at a wipe, counts a gap in a batch once, recycling (from any level) and headless change nothing, a load or a stair leaves no gas, wreck or burn behind, a load, a new game or a reset ends a rest, the undo history and the clocks, the numbers still move, a second or killed run does not count, and the load paths leave a clean device' }
	else { Write-Host 'A RUNNER THAT CANNOT FAIL MEANS NOTHING' -ForegroundColor Red }
	exit $(if ($ok) { 0 } else { 1 })
}

$run = if ($Only.Count -gt 0) { $suites | Where-Object { $Only -contains $_.name } } else { $suites }
if (-not $run) { Write-Host "eval: nothing matched -Only"; exit 2 }

# ONE PROCESS FOR EVERY SUITE. A dungeon load is ~12 seconds and a `reset` is
# ~340 ms, so ten suites in one process pay one load instead of ten - measured
# 155s -> 37s. Each script opens with `reset`, which loads for the first one in
# the batch and recycles for the rest (docs/eval-harness.md).
$paths = @($run | ForEach-Object { Join-Path $scripts $_.script })
# NOT `$args`: that is PowerShell's automatic variable, and inside the launch
# block below it would be the block's own (empty) argument list.
$gameArgs = @()
if ($Headless) { $gameArgs += '-headless' }
if ($Warp) { $gameArgs += '-warp' }
$gameArgs += @('-eval') + $paths
$t0 = Get-Date
# The log deleted first, so a game that dies before writing one leaves nothing
# for the "did it finish" check below to mistake for this run's.
Remove-Item $log -ErrorAction SilentlyContinue
$launch = { Start-Process -FilePath $exe -ArgumentList $gameArgs -PassThru -Wait }
$p = if ($Warp) { Invoke-WithIni $warpIni $launch } else { & $launch }
$totalSecs = [int]((Get-Date) - $t0).TotalSeconds

# THE MEASUREMENT. Read back from dungeon.log rather than captured from the
# process: the game writes there, and it is the same file a human opens after a
# run. One source, so the harness cannot show something the log does not
# (docs/eval-harness.md - `logecho`).
#
# With one process the log holds every suite's output end to end, so it is split
# on the runner's own markers - `eval: 'x' queued` opens a section and
# `eval RESULT=... script=x` closes it. Splitting on the RUNNER's lines rather
# than on the suites' own echoes means a suite cannot break the split by
# printing something that looks like a header.
$logLines = ReadLog
# A RUN COUNTS ONLY IF IT FINISHED. Every batch the runner completes - or
# abandons on a timeout - ends in 'eval BATCH RESULT='; a game that crashed or
# was killed writes neither, and its partial log must not be read as a result
# (code-review C430: the exit code used to go unread).
$batchLine = @($logLines | Where-Object { $_ -match 'eval BATCH RESULT=' }) | Select-Object -Last 1
$runDied = -not $batchLine
$section = @{}
$verdicts = @{}
$current = $null
foreach ($line in $logLines) {
	if ($line -match "^\[info \] eval: '([^']+)' queued") { $current = $Matches[1]; $section[$current] = @() ; continue }
	if ($line -match '^\[(info |ERROR)\] eval RESULT=(\w+) script=(\S+)') { $verdicts[$Matches[3]] = $Matches[2]; $current = $null; continue }
	if ($current) { $section[$current] += $line }
}

$failed = 0
$results = @()
foreach ($s in $run) {
	if (-not $Table) {
		Say ''
		Say ('=' * 78)
		Say ("{0} - {1}" -f $s.name, $s.what)
		Say ('=' * 78)
	}
	# A suite whose section is MISSING never ran - the batch was abandoned by a
	# timeout, say. That has to read as a failure and not as a quiet blank.
	$verdict = if ($verdicts.ContainsKey($s.script)) { $verdicts[$s.script] } else { 'NOTRUN' }
	$note = ''

	$shown = 0
	$produced = 0
	if ($section.ContainsKey($s.script)) {
		# Everything the suite SAID, minus the runner's echo of each command it
		# was given - those are the script, not its answers.
		$produced = @($section[$s.script] |
			Where-Object { $_ -cmatch '^\[info \] console: ' -and $_ -cnotmatch '^\[info \] console: > ' }).Count
	}
	if ($s.measure -and $section.ContainsKey($s.script)) {
		if ($Table) { Say ''; Say ("--- {0} ---" -f $s.name) }
		# CASE-SENSITIVE on purpose: `TALLY` is the result and `tally reset` is
		# the command that begins a rung. Without this the table carries a line
		# of bookkeeping for every measurement it prints.
		$hits = @($section[$s.script] |
			Where-Object { $_ -cmatch ("^\[info \] console: ({0})" -f $s.measure) })
		$shown = $hits.Count
		$hits | ForEach-Object { Say ('  ' + ($_ -replace '^\[info \] console: ', '')) }
	}

	# A MEASURE THAT MATCHED NOTHING IS A BROKEN SUITE, NOT A QUIET ONE, and
	# until now the two were indistinguishable: both print a header and blank
	# space, which is exactly what `smoke` and `arena` legitimately do on every
	# run - so a reader is trained to skim past the one shape that means the
	# measurement has been silently lost (docs/eval-audit.md F25). Editing an
	# `echo` a regex keys on is all it takes.
	if ($s.measure -and $shown -eq 0) {
		Say '  MEASURED NOTHING - the suite ran but its measure regex matched no line' 'Red'
		Say ("  regex: {0}" -f $s.measure) 'Red'
		$note = 'measured nothing'
		if ($verdict -eq 'PASS') { $verdict = 'NOMEASURE' }
	}

	# HOW MUCH WAS SUPPRESSED. The report shows what a per-suite regex matched and
	# silently drops the rest, which across the ten suites is about 70% of what the
	# run produced - and that 70% is where every integrity signal in this audit was
	# hiding (docs/eval-audit.md F4). One line per suite is what tells a reader
	# there is a log worth opening, and roughly where the interesting part is.
	if ($produced -gt 0 -and -not $Table) {
		Say ("  [{0} of {1} lines shown; the rest is in dungeon.log]" -f $shown, $produced) 'DarkGray'
	}

	# WARNINGS AND ERRORS THE GAME WROTE MID-SUITE. The measure filter only ever
	# looks at `[info ] console:` lines, so every `[warn ]` and `[ERROR]` in the
	# log was invisible to the report BY CONSTRUCTION - a failed model load or a
	# steady-state allocation violation showed up as ten green PASS lines
	# (docs/eval-audit.md F24). Stack frames are indented under their message, so
	# requiring a non-space right after the tag keeps the message and drops the
	# forty lines of symbols beneath it.
	if ($section.ContainsKey($s.script)) {
		$noise = @($section[$s.script] |
			Where-Object { $_ -cmatch '^\[(warn |ERROR)\] [^\s]' } |
			ForEach-Object { $_ -replace '^\[(warn |ERROR)\] ', '' } |
			Select-Object -Unique)
		if ($noise.Count -gt 0) {
			Say ("  {0} warning/error line(s) in the log:" -f $noise.Count) 'Yellow'
			$noise | Select-Object -First 6 | ForEach-Object { Say ("    ! {0}" -f $_) 'Yellow' }
			if ($noise.Count -gt 6) { Say ("    ... and {0} more (see dungeon.log)" -f ($noise.Count - 6)) 'Yellow' }
			if ($note) { $note += '; ' }
			$note += ("{0} warn/error" -f $noise.Count)
		}
	}

	if ($verdict -ne 'PASS') { $failed++ }
	$results += [pscustomobject]@{ Name = $s.name; Verdict = $verdict; Note = $note }
}

Say ''
Say ('=' * 78)
foreach ($r in $results) {
	Say ("  {0,-12} {1,-10} {2}" -f $r.Name, $r.Verdict, $r.Note) `
		$(if ($r.Verdict -eq 'PASS') { '' } else { 'Red' })
}
Say ''
# THE DEVICE (the header's fifth condition). One line either way, so a reader can
# tell "the layer saw nothing" from "no layer was listening".
$device = Get-DeviceReport $logLines
if (-not $device.Layer) {
	Say ("d3d12 debug layer: not in this build ({0}) - the device was not checked" -f $Config) 'DarkGray'
} elseif ($device.Errors.Count -eq 0) {
	Say ("d3d12 debug layer: no errors{0}" -f $(if ($device.Warp) { ' (WARP)' } else { ' (GPU - run -Warp to see frames still in flight)' }))
} else {
	Say ("d3d12 debug layer: {0} ERROR(S){1} - the run does not count" -f $device.Errors.Count,
		$(if ($device.Warp) { ' (WARP)' } else { '' })) 'Red'
	$device.Errors | Select-Object -First 3 | ForEach-Object { Say ("    ! {0}" -f ($_ -replace '^\[ERROR\] ', '')) 'Red' }
	$failed++
}
if ($Warp -and -not $device.Warp -and -not $runDied) {
	Say 'asked for -Warp, but the game never said it drew on WARP - the run does not count' 'Red'
	$failed++
}
if ($runDied) {
	Say ("THE GAME DID NOT FINISH (exit code {0}): no 'eval BATCH RESULT=' line - nothing above counts" -f $p.ExitCode) 'Red'
	$failed++
} elseif ($p.ExitCode -ne 0 -and $failed -eq 0) {
	Say ("the game exited {0} although every suite passed - the run does not count" -f $p.ExitCode) 'Red'
	$failed++
}
# ONE total rather than a column of per-suite times: they all ran in one process
# now, so a per-suite wall clock would be a number the harness cannot honestly
# produce. The load is paid once and shows up in whichever suite went first.
#
# THE SECONDS ARE DELIBERATELY NOT IN THE SAVED TRANSCRIPT'S COMPARISON VALUE:
# they change run to run on the same build, so a diff of two -OutFile reports
# would always show this line. It stays because a human wants it; a reader
# diffing two runs should expect exactly this one line to differ.
Say ("eval RESULT={0} suites={1} failures={2} seconds={3} self_test=0" -f `
	$(if ($failed -eq 0) { 'PASS' } else { 'FAIL' }), $results.Count, $failed, $totalSecs)
if ($failed -eq 0) {
	Say 'every suite ran; the NUMBERS above are the result, not this line'
}
SaveTranscript
exit $(if ($failed -eq 0) { 0 } else { 1 })
