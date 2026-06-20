# Drives the running Dungeon.exe for verification: PostMessage keystrokes and
# mouse clicks (client coords), CopyFromScreen screenshots into docs/.
# Dot-source, then call Key / Click / Shot.
if (-not ([System.Management.Automation.PSTypeName]'Win').Type) {
	Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Win {
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
	[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
	public struct RECT { public int Left, Top, Right, Bottom; }
	public struct POINT { public int X, Y; }
}
'@
}
Add-Type -AssemblyName System.Drawing

$script:hwnd = [IntPtr](Get-Process Dungeon).MainWindowHandle

function Key([int]$vk) {
	[Win]::PostMessage($script:hwnd, 0x100, [IntPtr]$vk, [IntPtr]1) | Out-Null
	Start-Sleep -Milliseconds 60
	[Win]::PostMessage($script:hwnd, 0x101, [IntPtr]$vk, [IntPtr][int64]0xC0000001) | Out-Null
	Start-Sleep -Milliseconds 250
}

function Click([int]$x, [int]$y) {
	$l = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
	[Win]::PostMessage($script:hwnd, 0x200, [IntPtr]0, $l) | Out-Null # WM_MOUSEMOVE
	Start-Sleep -Milliseconds 120
	[Win]::PostMessage($script:hwnd, 0x201, [IntPtr]1, $l) | Out-Null # WM_LBUTTONDOWN
	Start-Sleep -Milliseconds 60
	[Win]::PostMessage($script:hwnd, 0x202, [IntPtr]0, $l) | Out-Null # WM_LBUTTONUP
	Start-Sleep -Milliseconds 250
}

function Shot([string]$name) {
	$r = New-Object Win+RECT; [Win]::GetClientRect($script:hwnd, [ref]$r) | Out-Null
	$p = New-Object Win+POINT; [Win]::ClientToScreen($script:hwnd, [ref]$p) | Out-Null
	$bmp = New-Object System.Drawing.Bitmap($r.Right, $r.Bottom)
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$g.CopyFromScreen($p.X, $p.Y, 0, 0, $bmp.Size)
	$bmp.Save((Join-Path $PSScriptRoot "$name.png")); $g.Dispose(); $bmp.Dispose()
}
