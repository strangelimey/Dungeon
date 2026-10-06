# Drives a running Dungeon.exe for verification: PostMessage keystrokes and
# mouse clicks (client coords), PrintWindow screenshots into docs/.
#
#   . docs\drive.ps1 -GamePid $proc.Id     # the game this session launched
#   . docs\drive.ps1                       # else: THIS worktree's running game
#   Use-Game <pid>                         # re-target after a relaunch
# then call Key / Send / Click / Shot.
#
# SEVERAL SESSIONS RUN AT ONCE (Michael often has a few going), each with its
# own game, so nothing here may touch "whichever Dungeon" or "whatever is on
# screen":
# - the window is found BY PID - the game window (DungeonWindowClass) that
#   process owns, never Get-Process Dungeon's first hit or the foreground. With
#   no -GamePid it takes the Dungeon.exe running from this repo's build\ and
#   REFUSES when there is none or more than one, rather than guessing.
# - Shot captures with PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT), which
#   asks THAT window to render itself (the full-content flag is what makes a D3D
#   swapchain come out instead of black), so another window on top, or this one
#   being behind, spoils nothing. It used to CopyFromScreen the client rect,
#   which photographed whatever happened to be in front. A MINIMIZED window
#   still captures black - restore it first.
param([int]$GamePid = 0)

# The window type (FindByPid, PostMessage, PrintWindow) is the harnesses' own
# copy in tools\HarnessGame.ps1, so this driver and every harness find the
# game's window the same way.
. (Join-Path (Split-Path $PSScriptRoot -Parent) 'tools\HarnessGame.ps1')
Add-Type -AssemblyName System.Drawing

function Use-Game([int]$id = 0) {
	if ($id -le 0) {
		$root = Split-Path $PSScriptRoot -Parent
		$mine = @(Get-Process Dungeon -ErrorAction SilentlyContinue |
			Where-Object { $_.Path -like (Join-Path $root 'build\*') })
		if ($mine.Count -ne 1) {
			throw "drive.ps1: found $($mine.Count) Dungeon.exe running from $root\build - pass -GamePid / Use-Game <pid>"
		}
		$id = $mine[0].Id
	}
	$h = [HarnessWin]::FindByPid([uint32]$id, $HarnessWindowClass)
	if ($h -eq [IntPtr]::Zero) { throw "drive.ps1: process $id has no game window (yet?)" }
	$script:gamePid = $id
	$script:hwnd = $h
}

Use-Game $GamePid

function Key([int]$vk) { Send-Key $vk }

# Types a string into whatever has focus (the dev console, a text field). Posts
# WM_CHAR per character rather than key-downs, which is what Input::OnChar
# actually reads — so this handles SHIFTED characters, underscores included. (A
# Key-only approach cannot: PostMessage delivers no shift state, which is why
# `wear plate_cuirass 0` was long believed undrivable from a script.)
#
# NOT named Type: that is a built-in PowerShell alias for Get-Content, and a
# function by that name is silently shadowed — "cast 0 fire" comes back as
# "cannot find path .../cast 0 fire" instead of typing anything.
function Send([string]$text) {
	Send-Text $text 30
	Start-Sleep -Milliseconds 150
}

function Click([int]$x, [int]$y) {
	$l = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
	[HarnessWin]::PostMessage($script:hwnd, 0x200, [IntPtr]0, $l) | Out-Null # WM_MOUSEMOVE
	Start-Sleep -Milliseconds 120
	[HarnessWin]::PostMessage($script:hwnd, 0x201, [IntPtr]1, $l) | Out-Null # WM_LBUTTONDOWN
	Start-Sleep -Milliseconds 60
	[HarnessWin]::PostMessage($script:hwnd, 0x202, [IntPtr]0, $l) | Out-Null # WM_LBUTTONUP
	Start-Sleep -Milliseconds 250
}

function Shot([string]$name) {
	if (-not [HarnessWin]::IsWindow($script:hwnd)) { throw "drive.ps1: game $script:gamePid's window is gone" }
	$r = New-Object HarnessWin+RECT; [HarnessWin]::GetClientRect($script:hwnd, [ref]$r) | Out-Null
	$bmp = New-Object System.Drawing.Bitmap($r.Right, $r.Bottom)
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$hdc = $g.GetHdc()
	$ok = [HarnessWin]::PrintWindow($script:hwnd, $hdc, 3) # PW_CLIENTONLY | PW_RENDERFULLCONTENT
	$g.ReleaseHdc($hdc)
	if (-not $ok) { $g.Dispose(); $bmp.Dispose(); throw "drive.ps1: PrintWindow failed for game $script:gamePid" }
	$bmp.Save((Join-Path $PSScriptRoot "$name.png")); $g.Dispose(); $bmp.Dispose()
}
