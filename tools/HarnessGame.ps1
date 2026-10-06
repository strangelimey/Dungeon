# ============================================================================
# tools\HarnessGame.ps1 - the one copy of "drive the game this harness started".
#
# AllocTest, InGameTest, HealthTest, ProfileTest and TypingTest each used to
# carry their own PostMessage class, Send-Key, Send-Text and Wait-ForLog, and
# fixes reached only some of the copies (code-review C401): two never retried
# the window lookup, all five took Process.MainWindowHandle (a debug build also
# owns a CONSOLE window), TypingTest still pressed Enter on the title screen -
# Continue, whenever a save exists - and the "console answers" loops matched an
# echo the title screen had already written, so they never waited (C428).
#
#   . (Join-Path $PSScriptRoot 'HarnessGame.ps1')
#   Assert-NotRunning $exe
#   Start-HarnessGame $exe $bin $log $LoadTimeoutSec      # sets $proc, $hwnd
#   Start-NewGame $LoadTimeoutSec                         # console left CLOSED
#   ...
#   Stop-HarnessGame                                      # quit, else kill BY PID
#
# STATE lives in the caller's script scope, where a dot-sourced file defines
# its functions: $proc (the process THIS run started), $hwnd (its game window)
# and $log. Every harness already read those names; nothing else is global.
#
# THE PID RULE (CLAUDE.md, several sessions run at once): the window is found
# BY PID and window class, input is posted to that window only, and the game is
# stopped through its own process object - never by name, never the foreground.
# docs\drive.ps1 dot-sources this file for the same window type.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================

if (-not ([System.Management.Automation.PSTypeName]'HarnessWin').Type) {
	Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class HarnessWin {
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
	[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
	[DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
	[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
	[DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
	[DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
	[DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
	delegate bool EnumProc(IntPtr h, IntPtr l);
	public struct RECT { public int Left, Top, Right, Bottom; }

	// The top-level window of class `cls` owned by process `pid`, or zero. Not
	// Process.MainWindowHandle: a debug build also owns a console window, and
	// that property returns whichever the OS considers main.
	public static IntPtr FindByPid(uint pid, string cls) {
		IntPtr found = IntPtr.Zero;
		EnumWindows((h, l) => {
			uint p; GetWindowThreadProcessId(h, out p);
			if (p != pid) return true;
			var sb = new StringBuilder(64); GetClassName(h, sb, sb.Capacity);
			if (sb.ToString() != cls) return true;
			found = h; return false;
		}, IntPtr.Zero);
		return found;
	}
}
'@
}

$HarnessWindowClass = 'DungeonWindowClass'
# Pause after each typed character. AllocTest types at 40 ms (set it after the
# dot-source); everything else at 30.
if (-not (Get-Variable HarnessCharMs -Scope Script -ErrorAction SilentlyContinue)) { $HarnessCharMs = 30 }

$WM_KEYDOWN = 0x100; $WM_KEYUP = 0x101; $WM_CHAR = 0x102; $WM_KILLFOCUS = 0x0008
$VK_RETURN = 0x0D; $VK_ESCAPE = 0x1B; $VK_CONSOLE = 0xC0

# ---------------------------------------------------------------------------
# Launch and stop
# ---------------------------------------------------------------------------

# THIS build's exe only: another worktree's game is a different process with
# its own log, but this one's writes the SAME dungeon.log - truncated on open -
# so two runs of one build interleave each other's verdict source (C430).
# Refuses with exit 3, harness_game.py's code for the same refusal: a refusal
# is not a verdict, so it must not read as FAIL. (ProfileTest keeps a global
# check on purpose - a second game on the GPU would be part of what it
# measures.)
function Assert-NotRunning([string]$exe) {
	$running = @(Get-Process Dungeon -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe })
	if ($running) {
		$pids = ($running | ForEach-Object { $_.Id }) -join ', '
		Write-Host "refused: $exe is already running (pid $pids) - it writes the same dungeon.log; close it or wait for it" -ForegroundColor Red
		exit 3
	}
}

# Launches $exe (truncating its log first unless -KeepLog), waits for the boot
# load's table - the menu is up - and finds the game window by PID, retrying for
# 30 s because the boot line can beat the window becoming findable. Sets
# $proc and $hwnd in the caller's scope.
function Start-HarnessGame([string]$exe, [string]$bin, [string]$logPath, [int]$loadTimeoutSec,
		[string[]]$ExtraArgs = @(), [switch]$KeepLog) {
	if (-not $KeepLog) { Remove-Item $logPath -ErrorAction SilentlyContinue }
	Write-Host "launching $exe"
	$script:log = $logPath
	$script:hwnd = [IntPtr]::Zero
	$script:proc = Start-Process -FilePath $exe -WorkingDirectory $bin `
		-ArgumentList (@('-project', 'dungeon-demo') + $ExtraArgs) -PassThru
	Wait-ForLog '--- load: ' $loadTimeoutSec 'the boot load' | Out-Null
	$deadline = (Get-Date).AddSeconds(30)
	while ((Get-Date) -lt $deadline) {
		if ($script:proc.HasExited) { throw "the game exited early (code $($script:proc.ExitCode)) before showing its window" }
		$script:hwnd = [HarnessWin]::FindByPid([uint32]$script:proc.Id, $HarnessWindowClass)
		if ($script:hwnd -ne [IntPtr]::Zero) { return }
		Start-Sleep -Milliseconds 300
	}
	throw "process $($script:proc.Id) never showed a game window"
}

# Quits through the console so shutdown runs (it logs whole-run totals), then
# kills THIS process if it has not gone. -OpenConsole first when the console is
# closed. Safe to call from a finally whatever state the run reached.
function Stop-HarnessGame([int]$waitMs = 5000, [switch]$OpenConsole) {
	if (-not $script:proc -or $script:proc.HasExited) { return }
	if ($script:hwnd -ne [IntPtr]::Zero) {
		if ($OpenConsole) { Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 400 }
		Send-Text 'quit'; Send-Key $VK_RETURN
	}
	if (-not $script:proc.WaitForExit($waitMs)) {
		$script:proc.Kill()
		$script:proc.WaitForExit(5000) | Out-Null
	}
}

# ---------------------------------------------------------------------------
# Input: posted to THIS game's window only
# ---------------------------------------------------------------------------

function Send-Message([uint32]$msg, [int64]$w, [int64]$l) {
	[HarnessWin]::PostMessage($script:hwnd, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null
}

# Down, a beat, up (ALWAYS the up: without it the next down of the same key does
# not register as a press), then a pause.
function Send-Key([int]$vk) {
	Send-Message $WM_KEYDOWN $vk 1
	Start-Sleep -Milliseconds 60
	Send-Message $WM_KEYUP $vk 0xC0000001
	Start-Sleep -Milliseconds 250
}

# WM_CHAR per character - what the console and text fields read, shifted
# characters included. -BlurAt posts WM_KILLFOCUS straight after that
# character, with no pause, so both land in one frame (TypingTest's FOCUS
# phase).
function Send-Text([string]$text, [int]$ms = $HarnessCharMs, [int]$BlurAt = -1) {
	$i = 0
	foreach ($c in $text.ToCharArray()) {
		Send-Message $WM_CHAR ([int]$c) 1
		if ($i -eq $BlurAt) { Send-Message $WM_KILLFOCUS 0 0 }
		Start-Sleep -Milliseconds $ms
		$i++
	}
}

# ---------------------------------------------------------------------------
# The log
# ---------------------------------------------------------------------------

# The LAST line matching $pattern anywhere in the log, waiting for one to land.
# Throws if the game exits or the time runs out, naming $what.
function Wait-ForLog([string]$pattern, [int]$timeoutSec, [string]$what) {
	$deadline = (Get-Date).AddSeconds($timeoutSec)
	while ((Get-Date) -lt $deadline) {
		if ($script:proc.HasExited) {
			throw "the game exited early (code $($script:proc.ExitCode)) while waiting for $what"
		}
		if (Test-Path $script:log) {
			$hit = Select-String -Path $script:log -Pattern $pattern -ErrorAction SilentlyContinue |
				Select-Object -Last 1
			if ($hit) { return $hit.Line }
		}
		Start-Sleep -Milliseconds 400
	}
	throw "timed out after ${timeoutSec}s waiting for $what"
}

# How many log lines match $pattern right now. Take this BEFORE an action and
# wait for the count to grow: a pattern searched over the whole log is satisfied
# by any earlier line that happens to match (C428).
function Get-LogMatchCount([string]$pattern) {
	if (-not (Test-Path $script:log)) { return 0 }
	return @(Select-String -Path $script:log -Pattern $pattern -ErrorAction SilentlyContinue).Count
}

# POLL FOR A CONSOLE ANSWER, NEVER SLEEP A FIXED TIME AND READ. A command's
# output lands when the game gets round to it, and one heavy debug frame puts it
# past any fixed sleep. The lines matching $pattern past the first $before, once
# there are at least $count of them - or whatever there is when $timeoutSec runs
# out, so the caller's own check (and its own failure message) still decides.
function Wait-NewLogLines([string]$pattern, [int]$before, [int]$count = 1, [double]$timeoutSec = 5) {
	$deadline = (Get-Date).AddSeconds($timeoutSec)
	while ($true) {
		$new = @(@(Select-String -Path $script:log -Pattern $pattern -ErrorAction SilentlyContinue) |
			Select-Object -Skip $before)
		if ($new.Count -ge $count -or (Get-Date) -gt $deadline) { return ,$new }
		if ($script:proc -and $script:proc.HasExited) { return ,$new }
		Start-Sleep -Milliseconds 200
	}
}

# Waits for a NEW line matching $pattern (one written after this call began),
# throwing like Wait-ForLog if none lands. Returns the line.
function Wait-ForNewLog([string]$pattern, [int]$timeoutSec, [string]$what, [int]$before = -1) {
	if ($before -lt 0) { $before = Get-LogMatchCount $pattern }
	$deadline = (Get-Date).AddSeconds($timeoutSec)
	while ((Get-Date) -lt $deadline) {
		if ($script:proc.HasExited) {
			throw "the game exited early (code $($script:proc.ExitCode)) while waiting for $what"
		}
		$new = @(@(Select-String -Path $script:log -Pattern $pattern -ErrorAction SilentlyContinue) |
			Select-Object -Skip $before)
		if ($new.Count -gt 0) { return $new[-1].Line }
		Start-Sleep -Milliseconds 400
	}
	throw "timed out after ${timeoutSec}s waiting for $what"
}

# ---------------------------------------------------------------------------
# The console
# ---------------------------------------------------------------------------

# Types $command until the console answers with a NEW line matching $pattern,
# so a dropped keystroke or a console still gated off by a level load cannot
# fail a run. The count is taken once, before the first try, so a line the title
# screen wrote earlier never satisfies it (C428). The default accepts both lines
# `logecho on` can leave: its answer "console: logecho on", which is all a first
# call with echo OFF logs (the "> logecho on" echo is printed before the command
# turns mirroring on), and the echo itself once mirroring is on. The console
# must already be OPEN. Returns whether it answered.
function Wait-ConsoleReady([string]$command = 'logecho on', [string]$pattern = 'console: (> )?logecho on\s*$',
		[int]$tries = 10, [int]$waitMs = 2000) {
	$before = Get-LogMatchCount $pattern
	for ($try = 1; $try -le $tries; $try++) {
		Send-Text $command; Send-Key $VK_RETURN
		$new = Wait-NewLogLines $pattern $before 1 ($waitMs / 1000.0)
		if ($new.Count -gt 0) { return $true }
		if ($script:proc.HasExited) { return $false }
	}
	return $false
}

# START A NEW GAME THROUGH THE CONSOLE, never the landing page: Enter there is
# the FIRST entry, which is Continue whenever a loadable save exists, and the
# eval suites and other sessions leave saves in the one shared
# Documents\DungeonSaves. `newgame` (or `newparty <spec>`) calls the menu
# entry's own callback, so it is the same new game whatever the menu holds.
#
# Then waits for the level, NOT for 'Game loaded:' - that comes from a load TASK
# before the starting level's own load has begun, while every console command is
# still refused (C429). A level load ends with 'Level ready:'; a new game that
# lands without one (a level already in memory, the world map) says 'New game
# started'. Both are counted from before the command, so a line from an earlier
# game in the same log never satisfies it.
#
# Then waits until the console really answers, and leaves it CLOSED with
# logecho ON (each harness sets the state it needs). Returns the ready line.
function Start-NewGame([int]$loadTimeoutSec, [string]$PartySpec = '') {
	$ready = '^\[info \] (Level ready: |New game started)'
	Write-Host 'starting a new game'
	Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 500
	if (-not (Wait-ConsoleReady)) { throw 'the console never accepted a command on the title screen' }
	$before = Get-LogMatchCount $ready
	if ($PartySpec) {
		Send-Text "newparty $PartySpec"; Send-Key $VK_RETURN
	} else {
		Send-Text 'newgame'; Send-Key $VK_RETURN
	}
	Start-Sleep -Milliseconds 300
	Send-Key $VK_CONSOLE # closed while the level loads
	$line = Wait-ForNewLog $ready $loadTimeoutSec 'the dungeon load' $before
	Write-Host "  $($line -replace '^\[info \] ', '')"
	# If the backtick was ever dropped, an Enter above reached the MENU, whose
	# first entry is Continue whenever a save exists: refuse that run loudly
	# rather than measure whichever save was newest.
	$loaded = Select-String -Path $script:log -Pattern 'Loaded game from ' -EA SilentlyContinue | Select-Object -First 1
	if ($loaded) { throw "a save was loaded instead of a new game (a dropped console key?): $($loaded.Line)" }
	# The level being ready does not prove the console takes commands yet (a
	# save or a stair can stage a second load). Retry a harmless command until a
	# NEW echo lands, then shut the console again.
	Start-Sleep -Milliseconds 500
	Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 500
	if (-not (Wait-ConsoleReady)) { throw 'the console never accepted a command after the new game' }
	Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 400
	return $line
}
