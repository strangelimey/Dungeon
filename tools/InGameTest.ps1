# ============================================================================
# tools\InGameTest.ps1 - the audits that need a running game.
#
# Checks that were built but never automated (the first two), and one since,
# sharing one launch because the expensive part is loading the dungeon, not
# running them:
#
#   levelcheck  every level file is present and every model a catalog type or
#               a level's palette loads is installed - as the FILE its loader
#               opens (AssetUtil's ModelFileOf / WornBlockFile). The baked pool
#               is GITIGNORED, so a fresh clone or a stale worktree provision
#               has entries whose assets are absent - and a missing model is a
#               LoadModelOrDie that takes the process down at level load,
#               possibly on a level nobody has visited in weeks. Then its
#               MUTATIONS ($levelcheckMutations below), each of which must FAIL
#               naming the file it planted, and its CONTROLS ($levelcheckControls),
#               each an entry that loads, which must not.
#
#   uioverlap   CLAUDE.md says RUN IT AFTER TOUCHING ANY SCREEN, and the one
#               manual sweep found four defects nobody had reported. The command
#               was written to log its findings with a label precisely so a
#               scripted sweep would be collectable, and then nothing ever swept.
#
#   etched gold `uimaterial sweep` (code-review C204): on every UI material the
#               etched symbols' gold follows the ink solve the carved words take,
#               every etch measured; `uimaterial drawn`: on a snow material and
#               a dark stone the etches were really PAINTED in those inks; and
#               the movement pad is photographed on the snow into
#               build\<cfg>\bin\shots for a look. Run-wide checks, before the
#               screens.
#
#   thumbnails  (code-review C158, C111) the asset picker's texture tiles and
#               the editor palette's surface swatches are PHOTOGRAPHED and each
#               one's mean set beside the stored mean of the file it came from
#               (`assetpicker survey`, `editor palette swatches`): a tile drawn
#               through an sRGB view comes out far darker. And on both pickers
#               a cap FORCED below what is in view (`thumbcap 8`), scrolled so
#               eviction must run: nothing in view blank, nothing reloaded, and
#               nothing more evicted once the view stands still. And the asset
#               picker under a heap line FORCED just above what is live
#               (`thumbcap heap`): it stops loading at the line and makes room
#               from what is off screen. Run-wide checks, before the screens.
#   the sheet   (code-review batch 54) three readouts, run-wide, before the
#               screens: the ARMOR TOOLTIP's rows (`sheet armor`) for Brand,
#               unarmored with `avoid` trained, alone and beside the plate
#               cuirass - on each side the Roll's terms (base, DEX, stance,
#               avoidance, armor) add up to the Roll (C372: the avoidance row
#               was gone, and an unarmored member's terms came out short by
#               it) and each CELL shows its value, the avoidance as
#               "+N (lvl 3)" unarmored and "-" armored (the sum alone holds by
#               construction); a SKILL'S NAME on the status bar in its BAR's
#               colour (C477) - the pointer parked on the blade row, `sheet bar
#               colour` must be the row's; and the HUD EFFECT STRIP
#               photographed with three wards up (C263, one DrawEffectIcon)
#               into build\<cfg>\bin\shots for a look, the verdict only that it
#               was taken.
#
#   backdrop    (code-review C365) what Render drew behind the pause menu and
#               the sheet opened from the world map: the world map, with no 3D
#               pass - not the parked dungeon. A drawing fact, so only a run
#               that renders can check it; it is those two screens' status.
#
#   the title   (code-review C366) after a first save, Esc and Return to Main
#               Menu, the title offers Continue and Load - in a world of its
#               own, whose title first offers neither. `title status` is that
#               screen's status, read on the title before, BEHIND THE PAUSE
#               MENU (the way back rebuilds the title whatever its flags say,
#               so only that reading sees them) and on the title after; the
#               screen is the last, since it switches world.
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
#   trims    ONE row, sweep_worldwords, is the exception to "clean": the world
#            map's word faces in Russian must be cut, so its audit must REPORT
#            those cuts - only chrome trims, at least one and no more than the
#            faces the view says it cut, each quoting whole UTF-8 characters
#            and the ".." (code-review C224)
#
# and the run ends by requiring `state` to be playing or worldmap. Logecho stays
# ON for the whole sweep, since the status lines are console output; the verdict
# reads only the lines the game logs ITSELF (`[info ] uioverlap...`), never the
# mirrored `console: uioverlap: auditing...`.
#
#   .\tools\InGameTest.ps1
#   .\tools\InGameTest.ps1 -SelfTest     # see below; exit 0 = the checker works
#
# -SELFTEST is a real sweep with the real labels and THREE named faults injected,
# and it passes only if exactly those three checks fail and everything else
# passes (SpellTest's rule, code-review C419): sweep_gencomplexity's open step
# asks for a tab the dialog does not have, which is REFUSED, so its status line
# never lands; sweep_worlds is opened but never audited, so its label is missing
# while its longer sibling sweep_worldsettings is present; and sweep_worldwords
# turns its word faces off again before the audit, which then finds no trim.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[ValidateSet('debug', 'release')][string]$Config = 'debug',
	[int]$LoadTimeoutSec = 240,
	# Checks the CHECKER: three faults injected into a real sweep (above), and
	# exactly those three checks must fail.
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
# A command that starts a game (`newgame`, a world switch, `newparty`), waited
# out: a command typed while its level loads is refused - so wait for the level
# (or the world map), then for the console.
function Run-Load([string]$c) {
	$ready = '^\[info \] (Level ready: |New game started)'
	$before = Get-LogMatchCount $ready
	Run-Cmd $c
	Wait-ForNewLog $ready $LoadTimeoutSec "the game '$c' starts" $before | Out-Null
	if (-not (Wait-ConsoleReady)) { throw "the console never answered after $c" }
}
# A new game with a CREATED party (party creation, docs/party-creation-plan.md
# phase 2).
function Run-Party([string]$spec) { Run-Load "newparty $spec" }

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

# sweep_titlesaved's world and save, BOTH named for this worktree (Get-WorktreeTag,
# tools/harness_game.py save_name's rule). The save because the saves folder is
# shared with every session and with Michael's own play; the world because a
# `-project` run lists the saves that NAME the world in hand, so under a fixed
# name another worktree's save - a killed run's, or one written by a sweep
# running at the same time - would be counted in this row's "0 saves" control.
# In a world of its own the row starts with no saves; both go on its way out.
$worktreeTag = Get-WorktreeTag $root
$titleWorld = "wt_igttitle_$worktreeTag"
$titleSave = "igt_titlesaved_$worktreeTag"

# The game window's CLIENT area, read and resized from here as dragging its edge
# would resize it (WM_SIZE: the swapchain and the fonts follow). For the hand
# menu sweep's sub-900p window and the sheet's ultrawide and 16:10 ones (each
# row puts the window back in its close step). ASYNC, so a game that stopped
# pumping messages cannot hang the run; the pause lets the fonts settle (0.25 s)
# before the next command. Its own type, not HarnessWin, which is shared with
# other harnesses.
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
# The sheet panel's saved spot and scale, from `hudpanel list` - what a row that
# places it puts back afterwards (settings.ini keeps a placement). Returns the
# `hudpanel sheet` arguments that restore it: "-1 -1 <scale>" for the default
# spot, else "<x> <y> <scale>".
function Get-SheetLook {
	$pattern = '^\[info \] console:\s+sheet\s+\S+\s+px .* saved (default|[\d.]+,[\d.]+)\s+scale ([\d.]+)'
	$before = @(Select-String -Path $log -Pattern $pattern).Count
	Run-Quick 'hudpanel list'
	$deadline = (Get-Date).AddSeconds(5)
	while ((Get-Date) -lt $deadline) {
		$lines = @(Select-String -Path $log -Pattern $pattern)
		if ($lines.Count -gt $before) {
			$m = $lines[-1].Matches[0]
			$spot = if ($m.Groups[1].Value -eq 'default') { '-1 -1' } else { $m.Groups[1].Value -replace ',', ' ' }
			return "$spot $($m.Groups[2].Value)"
		}
		Start-Sleep -Milliseconds 200
	}
	throw 'the console never listed the sheet panel (hudpanel list)'
}

# THE ETCHED GOLD (code-review C204): the material the movement pad is shown and
# photographed on - a snow, the lightest kind, where the old baked gold read
# worst - and where the pictures go (beside the exe, so never into git).
$etchMaterial = 'snow_packed'
# ...and a dark stone the draw record is read on too: there the solve keeps the
# authored gold, and its lit slope (the ink x the sheen) is not clamped white as
# on the snow, so a slope drawn in the bare tint shows.
$sheenMaterial = 'granite_grey'
$shotDir = Join-Path $bin 'shots'
# A PrintWindow of THIS game's client area (PW_CLIENTONLY | PW_RENDERFULLCONTENT:
# the second flag is what captures a D3D swapchain - never a screen grab, which
# photographs whatever window is in front), saved whole to $path and, cut to
# $rect (x, y, w, h in client px) and doubled, to $cropPath. Returns the cut's
# luminance spread (standard deviation, 0..255) - a blank or failed capture is
# flat - or -1 when nothing was captured.
function Save-PadShot([string]$path, [int[]]$rect, [string]$cropPath) {
	Add-Type -AssemblyName System.Drawing
	$r = New-Object HarnessWin+RECT
	[HarnessWin]::GetClientRect($hwnd, [ref]$r) | Out-Null
	if ($r.Right -le 0 -or $r.Bottom -le 0) { return -1 }
	$bmp = New-Object System.Drawing.Bitmap($r.Right, $r.Bottom)
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$hdc = $g.GetHdc()
	$ok = [HarnessWin]::PrintWindow($hwnd, $hdc, 3)
	$g.ReleaseHdc($hdc); $g.Dispose()
	try {
		if (-not $ok) { return -1 }
		New-Item -ItemType Directory -Force $shotDir | Out-Null
		$bmp.Save($path)
		$x = [Math]::Max(0, $rect[0]); $y = [Math]::Max(0, $rect[1])
		$w = [Math]::Min($rect[2], $bmp.Width - $x); $h = [Math]::Min($rect[3], $bmp.Height - $y)
		if ($w -le 0 -or $h -le 0) { return -1 }
		$crop = New-Object System.Drawing.Bitmap(($w * 2), ($h * 2))
		$cg = [System.Drawing.Graphics]::FromImage($crop)
		$cg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
		$cg.DrawImage($bmp, (New-Object System.Drawing.Rectangle(0, 0, ($w * 2), ($h * 2))),
			(New-Object System.Drawing.Rectangle($x, $y, $w, $h)), [System.Drawing.GraphicsUnit]::Pixel)
		$cg.Dispose(); $crop.Save($cropPath); $crop.Dispose()
		$sum = 0.0; $sq = 0.0; $n = 0
		for ($py = $y; $py -lt $y + $h; $py += 3) {
			for ($px = $x; $px -lt $x + $w; $px += 3) {
				$c = $bmp.GetPixel($px, $py)
				$l = 0.2126 * $c.R + 0.7152 * $c.G + 0.0722 * $c.B
				$sum += $l; $sq += $l * $l; $n++
			}
		}
		$mean = $sum / $n
		return [Math]::Sqrt([Math]::Max(0.0, $sq / $n - $mean * $mean))
	} finally {
		$bmp.Dispose()
	}
}

# THE THUMBNAIL SURVEY'S CAMERA (code-review C158): a PrintWindow of THIS game's
# client area, as above, saved to $path, and the mean colour (r, g, b in 0..1) of
# each rect in $rects (x, y, w, h in client px) less a 2 px border, so the
# filtered edge and a rect's fractions (printed as whole pixels) never count -
# and nearly the whole image does, which is what its stored mean is taken over
# (a rune's glyph sits in the middle, so a middle cut alone reads darker). $null
# for a rect outside the picture; $null overall when nothing was captured.
function Measure-Shot([string]$path, $rects) {
	Add-Type -AssemblyName System.Drawing
	$r = New-Object HarnessWin+RECT
	[HarnessWin]::GetClientRect($hwnd, [ref]$r) | Out-Null
	if ($r.Right -le 0 -or $r.Bottom -le 0) { return $null }
	$fmt = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
	$bmp = New-Object System.Drawing.Bitmap($r.Right, $r.Bottom, $fmt)
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$hdc = $g.GetHdc()
	$ok = [HarnessWin]::PrintWindow($hwnd, $hdc, 3)
	$g.ReleaseHdc($hdc); $g.Dispose()
	try {
		if (-not $ok) { return $null }
		New-Item -ItemType Directory -Force $shotDir | Out-Null
		$bmp.Save($path)
		$data = $bmp.LockBits((New-Object System.Drawing.Rectangle(0, 0, $bmp.Width, $bmp.Height)),
			[System.Drawing.Imaging.ImageLockMode]::ReadOnly, $fmt)
		$stride = $data.Stride
		$bytes = New-Object byte[] ($stride * $bmp.Height)
		[System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
		$bmp.UnlockBits($data)
		$out = @()
		foreach ($rc in $rects) {
			$x0 = $rc[0] + 2; $x1 = $rc[0] + $rc[2] - 2
			$y0 = $rc[1] + 2; $y1 = $rc[1] + $rc[3] - 2
			if ($x0 -lt 0 -or $y0 -lt 0 -or $x1 -gt $bmp.Width -or $y1 -gt $bmp.Height -or $x1 -le $x0 -or $y1 -le $y0) {
				$out += ,$null; continue
			}
			$step = [Math]::Max(1, [int](($x1 - $x0) / 48))
			$sr = 0.0; $sg = 0.0; $sb = 0.0; $n = 0
			for ($py = $y0; $py -lt $y1; $py += $step) {
				for ($px = $x0; $px -lt $x1; $px += $step) {
					$i = $py * $stride + $px * 4   # BGRA
					$sb += $bytes[$i]; $sg += $bytes[$i + 1]; $sr += $bytes[$i + 2]; $n++
				}
			}
			$out += ,@(($sr / $n / 255.0), ($sg / $n / 255.0), ($sb / $n / 255.0))
		}
		return ,$out
	} finally {
		$bmp.Dispose()
	}
}
function Luma($c) { return 0.2126 * $c[0] + 0.7152 * $c[1] + 0.0722 * $c[2] }

# A survey's lines (`assetpicker tile ...` / `editor palette swatch ...`, mirrored
# by logecho) past the first $before: each a name, the rect it was drawn in, whether
# its image (not a placeholder) was drawn, and its file's stored mean r,g,b,a
# ($null when the game found no file to read).
function Read-SurveyLines([string]$head, [int]$before) {
	$pat = "console: $head (\S+) rect=(-?\d+),(-?\d+),(\d+),(\d+) drawn=(\d) mean=(\S+)"
	$out = @()
	foreach ($l in @(@(Select-String -Path $log -Pattern $pat -ErrorAction SilentlyContinue) | Select-Object -Skip $before)) {
		$g = $l.Matches[0].Groups
		$mean = if ($g[7].Value -eq '-') { $null } else { @($g[7].Value.Split(',') | ForEach-Object { [double]$_ }) }
		$out += [pscustomobject]@{ name = $g[1].Value; drawn = ($g[6].Value -eq '1'); mean = $mean
			rect = @([int]$g[2].Value, [int]$g[3].Value, [int]$g[4].Value, [int]$g[5].Value) }
	}
	return $out   # unrolled: a caller collects it with @()
}
# The survey's arithmetic: what a correct (linear) draw of an image averages to
# on screen is its STORED mean; an sRGB view decodes that to linear light first,
# so a 0.2 image draws at 0.03 and a 0.5 one at 0.21. A tile within this much
# luminance of its file passes, and the run must measure enough tiles bright
# enough that a darkening could not hide under it.
$thumbTolerance = 0.05
$thumbBright = 0.2

# A picker's status line (`assetpicker status` / `portrait picker status`,
# matching $pattern), asked for now and waited out: the newest such line, or
# $null when none came.
function Read-PickerStatus([string]$cmd, [string]$pattern) {
	$asked = Get-LogMatchCount $pattern
	Run-Cmd $cmd
	return @(Wait-NewLogLines $pattern $asked 1 10) | Select-Object -Last 1
}
# Every `name=<n>` of a status line, as a table; `visible=<first>+<count>` (the
# portrait picker's) reads as the count.
function Read-Fields($line) {
	$f = @{}
	foreach ($m in [regex]::Matches($line.Line, '(\w+)=(\d+)(?:\+(\d+))?')) {
		$v = if ($m.Groups[3].Success) { $m.Groups[3].Value } else { $m.Groups[2].Value }
		$f[$m.Groups[1].Value] = [int]$v
	}
	return $f
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
	# The SHEET IN AN ULTRAWIDE AND A 16:10 WINDOW (code-review C96). It took the
	# window's aspect while its layout was tuned at 16:9, so a backpack cell came
	# out 118x86 px at 2560x1080 and 88x96 at 1920x1200; sized in em now, it
	# keeps one shape. Each row's status is `sheet status` reading a SQUARE cell
	# (slot 0 of the selected bag, rounded to whole pixels) in a window of
	# exactly that size - a resize the desktop refused would prove nothing.
	@{ label = 'sweep_sheetwide'; state = 'sheet'; viaConsole = $true
	   open = { $script:clientBefore = Get-ClientSize; Set-ClientSize 2560 1080; Run-Cmd 'sheet 0' }
	   close = { Run-Cmd 'sheet off'; Set-ClientSize $script:clientBefore[0] $script:clientBefore[1] }
	   probe = @('sheet status')
	   status = @((Answer 'sheet: open member 0 '),
				  (Answer 'sheet size: window 2560x1080 panel [1-9]\d*x[1-9]\d* cell ([1-9]\d*)x\1\s*$')) },
	@{ label = 'sweep_sheet1610'; state = 'sheet'; viaConsole = $true
	   open = { $script:clientBefore = Get-ClientSize; Set-ClientSize 1920 1200; Run-Cmd 'sheet 0' }
	   close = { Run-Cmd 'sheet off'; Set-ClientSize $script:clientBefore[0] $script:clientBefore[1] }
	   probe = @('sheet status')
	   status = @((Answer 'sheet: open member 0 '),
				  (Answer 'sheet size: window 1920x1200 panel [1-9]\d*x[1-9]\d* cell ([1-9]\d*)x\1\s*$')) },
	# ...and a sheet scaled past what its window FITS: at scale 1.3 in 1024x768 it
	# wants 1154 px across, and the floating layer's clamp, which squeezes each
	# axis on its own, would put the window's aspect back (a 71x81 cell).
	# GameUI::SheetFitScale takes the scale that fits instead; the status is a
	# square cell in a panel no wider than the window. The sheet's saved spot and
	# scale are put back as found.
	@{ label = 'sweep_sheetfit'; state = 'sheet'; viaConsole = $true
	   open = {
		   $script:clientBefore = Get-ClientSize
		   $script:sheetLook = Get-SheetLook
		   Set-ClientSize 1024 768
		   Run-Cmd 'hudpanel sheet 0 0 1.3'
		   Run-Cmd 'sheet 0' }
	   close = {
		   Run-Cmd 'sheet off'
		   Run-Cmd "hudpanel sheet $script:sheetLook"
		   Set-ClientSize $script:clientBefore[0] $script:clientBefore[1] }
	   probe = @('sheet status')
	   status = @((Answer 'sheet: open member 0 '),
				  (Answer ('sheet size: window 1024x768 panel (\d{1,3}|10[01]\d|102[0-4])x[1-9]\d* ' +
						   'cell ([1-9]\d*)x\2\s*$'))) },
	# The floating HUD's other shapes (docs/ui-panels-plan.md P3b/P4): the party
	# inventory WINDOW, and the MINIMAL layout - the party bar and the hands
	# folded into one card per member, with the Magic dock (a member knows a
	# symbol first, or it is not shown) moved to the left column. The window
	# opens on its Inventory tab, whose squares are the SHEET'S (Michael, Phase
	# 6): `inventory status` sets member 0's first square beside the sheet's at
	# the window's scale, and they must match (code-review C96 - measured in the
	# drawn face's em, a tenth larger, a card's came out 79 px to the sheet's 73).
	@{ label = 'sweep_inventory'; state = 'playing'; viaConsole = $true
	   open = { Run-Cmd 'inventory' }; close = { Run-Cmd 'inventory off' }
	   probe = @('inventory status')
	   status = @((Answer 'inventory: open tab inventory '),
				  (Answer 'inventory squares: card ([1-9]\d*)x\1 sheet \1x\1\s*$')) },
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
	# THE WORLD MAP'S WORD FACES IN RUSSIAN (code-review C224). Its toolbar is
	# hand-drawn chrome, outside every widget tree, and a button whose art is
	# missing shows its label on the face - a word written for a tooltip, far
	# wider than a disc. `worldview words on` draws every button that way; under
	# ru.lang each label must be FITTED (cut at a whole UTF-8 character, ".."
	# after it - the old byte-at-a-time trim could split a Cyrillic letter) and
	# each cut REPORTED to the audit. So this row's audit must find trims (the
	# `trims` check below) where every other row's must be clean. Its status is
	# the switch and `worldview` seeing the editor's band drawn with word faces;
	# its `trimmed` count is the trims check's ceiling. The language goes back to
	# what settings.ini held at launch (a typed `lang` is saved).
	@{ label = 'sweep_worldwords'; state = 'worldmap'; viaConsole = $true; trims = $true
	   open = { Run-Cmd 'lang ru'; Run-Cmd 'worldmap on'; Run-Cmd 'worldedit on'; Run-Cmd 'worldview words on' }
	   close = { Run-Cmd 'worldview words off'; Run-Cmd 'worldedit off'; Run-Cmd 'worldmap off'
				 Run-Cmd "lang $langAtLaunch" }
	   probe = @('worldview')
	   status = @((Answer 'world view: word faces on\s*$'),
				  (Answer 'world view: travel screen, mode editor, editing yes, toolbar 6, fog \w+, words \w+, trimmed \d+\s*$'))
	   # The -SelfTest fault: the faces switched on and straight off again, so the
	   # screen opens as before but nothing is cut - a clean audit this row must
	   # call a failure.
	   selfTestOpen = { Run-Cmd 'lang ru'; Run-Cmd 'worldmap on'; Run-Cmd 'worldedit on'
						Run-Cmd 'worldview words on'; Run-Cmd 'worldview words off' } },
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
	   probe = @('hudpanel list'); status = @((Answer 'hud layout minimal, ')) },
	# THE TITLE AFTER A FIRST SAVE (code-review C366). The pause list rebuilt on
	# every Esc and shared the title's has-saves flag, so save, Esc, Return to
	# Main Menu showed a title with no Continue and no Load. In a world of its own
	# (no saves): the title first lists neither - the control - then a new game,
	# a save, Esc to the pause menu, back to the title, which must now offer
	# Continue and Load. Its status is the three `title status` readings, each
	# naming the state it was asked in. The one BEHIND THE PAUSE MENU is the
	# check of the fix: the title list catches up there (UpdatePause), from the
	# save's own mark and the title's own flag, while Return to Main Menu
	# rebuilds the title whatever either says - so the reading on the title
	# after it cannot fail on them.
	# LAST, because it switches world: the close deletes its save, switches back
	# to a new game in dungeon-demo and deletes the world it made.
	@{ label = 'sweep_titlesaved'; state = 'menu'; viaConsole = $true
	   open = {
		   # What a killed run left, first (each refused when there is none).
		   Run-Cmd "deletesave $titleSave"; Run-Cmd "worlds delete $titleWorld $titleWorld"
		   Run-Cmd "worlds new $titleWorld"
		   Run-Load "worlds load $titleWorld"
		   Run-Cmd 'title'; Run-Quick 'title status'
		   Run-Load 'newgame'
		   Run-Cmd "save $titleSave"
		   Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 400
		   Send-Key $VK_ESCAPE; Start-Sleep -Milliseconds 800
		   Open-Console
		   Run-Quick 'title status'
		   Run-Cmd 'title' }
	   close = {
		   Run-Cmd "deletesave $titleSave"
		   Run-Load 'worlds load dungeon-demo'
		   Run-Cmd "worlds delete $titleWorld $titleWorld" }
	   probe = @('title status')
	   status = @((Answer "title: entries \[(?!Continue)[^\]]*\] - 0 save\(s\) listed for '$titleWorld' \(asked while menu\)"),
				  (Answer "title: entries \[Continue, Load, [^\]]*\] - 1 save\(s\) listed for '$titleWorld' \(asked while paused\)"),
				  (Answer "title: entries \[Continue, Load, [^\]]*\] - 1 save\(s\) listed for '$titleWorld' \(asked while menu\)")) }
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
$selfTestExpected = @('sweep_gencomplexity: status', 'sweep_worlds: label', 'sweep_worldwords: trims')

# The language settings.ini held at launch, which sweep_worldwords puts back
# after its `lang ru` (a typed `lang` is saved). English when none is set.
$langAtLaunch = 'en'
$settingsIni = Join-Path $bin 'settings.ini'
if (Test-Path $settingsIni) {
	$langLine = @(Select-String -Path $settingsIni -Pattern '^language=(\S+)\s*$') | Select-Object -Last 1
	if ($langLine) { $langAtLaunch = $langLine.Matches[0].Groups[1].Value }
}

# levelcheck's MUTATIONS (code-review C441), run after the real check on every
# run: each plants one fault in what the check reads - never in the files - and
# must come back FAIL with exactly one more model missing than the real run, the
# planted file named. Each is a shape the old stem match passed: a fixture's
# missing empty_model and part2_model, an entry with no `model` (its loader
# opens the id), a door's `trim` naming no doors.cat entry (its loader opens the
# name), and a worn block tier the session did not load - chosen by the command
# apart from the check's own walk, so a check that skips that tier comes back
# PASSED here rather than refused.
$levelcheckMutations = @('empty_model', 'part2_model', 'id', 'trim', 'worn')
# ...and its CONTROLS (code-review C301): each plants an entry that LOADS - a
# model installed only under the extension its loader does not prefer (glb: a
# decoration naming a .glb-only model; gltf: an item naming a .gltf-only one),
# which the loader now opens - and must come back as the real run did, no model
# more missing, with the planted file the one the check's walk RESOLVED. Batch
# 13's `glb` was a mutation that had to FAIL: C301 made that entry a good one,
# and a check still on the old one-extension rule fails this control instead.
$levelcheckControls = @('glb', 'gltf')

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
# What the THUMBNAILS checks collect during the run (judged after it); empty
# until they ran, so a run that died before them fails them rather than throws.
$pickerTiles = @(); $pickerMeans = $null; $pickerShot = ''; $pickerCapLine = $null
$pickerStillLine = $null; $pickerHeapLine = $null; $heapForced = 0
$paletteSwatches = @(); $paletteMeans = $null; $paletteShot = ''; $swatchHeadLine = $null
$portraitCapLine = $null; $portraitStillLine = $null
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
	# Then the check's own mutations and controls, each awaited by its own
	# verdict line.
	foreach ($m in @($levelcheckMutations) + @($levelcheckControls)) {
		Wait-ConsoleReady "levelcheck mutate $m" "^\[info \] levelcheck RESULT=\S+ .* mutate=$m " 5 4000 | Out-Null
	}

	# THE ETCHED GOLD (code-review C204): every material's inks swept into the log
	# (judged below), then the movement pad shown on a snow material and
	# photographed - the picture is Michael's look, the verdict only that it was
	# taken. The pad's place comes from `hudpanel list` (logecho is on all run).
	Run-Cmd 'uimaterial sweep'
	$switched = Get-LogMatchCount "ui material ${etchMaterial}:"
	Run-Cmd "uimaterial $etchMaterial"
	Wait-NewLogLines "ui material ${etchMaterial}:" $switched 1 10 | Out-Null
	$padPattern = 'console:\s+move\s+shown\s+px (-?\d+),(-?\d+) (\d+)x(\d+)'
	$listed = Get-LogMatchCount $padPattern
	Run-Cmd 'hudpanel list'
	$padLine = @(Wait-NewLogLines $padPattern $listed 1 10)
	$padRect = $null; $padSpread = -1
	if ($padLine.Count -gt 0) {
		$m = $padLine[-1].Matches[0].Groups
		$padRect = @([int]$m[1].Value, [int]$m[2].Value, [int]$m[3].Value, [int]$m[4].Value)
		Send-Key 0xC0; Start-Sleep -Milliseconds 1500   # shut, so the HUD shows alone
		$padShot = Join-Path $shotDir "ingametest-movepad-$etchMaterial.png"
		$padSpread = Save-PadShot (Join-Path $shotDir "ingametest-hud-$etchMaterial.png") $padRect $padShot
		Open-Console
	}
	# ...and what the etches were PAINTED with there: the sheet open, so its
	# current tab draws its lit twin beside the plain stones, then the draw
	# record (judged below against the solved inks). The console SHUT a moment
	# first: an open console owns the frame and the UI is drawn but not updated,
	# so no tab is ever marked current and nothing draws lit (the first run of
	# this check read `lit=1 draws=0` exactly so). Then the same on the dark stone.
	Run-Cmd 'sheet 0'
	foreach ($mat in $etchMaterial, $sheenMaterial) {
		if ($mat -ne $etchMaterial) { Run-Cmd "uimaterial $mat" }
		Send-Key 0xC0; Start-Sleep -Milliseconds 1500
		Open-Console
		Run-Cmd 'uimaterial drawn'
	}
	Run-Cmd 'sheet off'
	Run-Cmd 'uimaterial off'

	# THUMBNAILS (code-review C111, C158), judged below. The console is SHUT
	# for every wait: an open console owns the frame, and a picker loads its
	# tiles in its own Update. First the asset picker's texture grid under a cap
	# FORCED to 8, below what is in view, scrolled half way so the screen it
	# opened on must be evicted; then its status, after a further settle - and
	# again after one more with the view standing still, when nothing more may
	# have been evicted (a draw that marks tiles it does not show makes and drops
	# the off-screen ones every frame, and only `evicted=` would say so).
	Run-Cmd 'thumbcap 8'
	Run-Cmd 'assetpicker textures'
	Send-Key 0xC0; Start-Sleep -Milliseconds 3500; Open-Console
	Run-Cmd 'assetpicker scroll 0.5'
	Send-Key 0xC0; Start-Sleep -Milliseconds 3500; Open-Console
	$capPattern = 'console: assetpicker open thumbs=\d+ srv=\d+ peak=\d+ visible=\d+ blank=\d+ missing=\d+ onscreen=\d+ cap=\d+ evicted=\d+ reloads=\d+ refused=\d+ heapline=\d+ heaptop=\d+'
	$pickerCapLine = Read-PickerStatus 'assetpicker status' $capPattern
	Send-Key 0xC0; Start-Sleep -Milliseconds 2000; Open-Console
	$pickerStillLine = Read-PickerStatus 'assetpicker status' $capPattern
	# ...then the cap back, and the same grid PHOTOGRAPHED: each tile wholly in
	# view, set beside its file's stored mean.
	Run-Cmd 'thumbcap off'
	$tileHead = 'console: assetpicker survey: \d+ tiles'
	$asked = Get-LogMatchCount $tileHead
	$tilesBefore = @(Read-SurveyLines 'assetpicker tile' 0).Count
	Run-Cmd 'assetpicker survey'
	Wait-NewLogLines $tileHead $asked 1 10 | Out-Null
	$pickerTiles = @(Read-SurveyLines 'assetpicker tile' $tilesBefore)
	Send-Key 0xC0; Start-Sleep -Milliseconds 1500
	$pickerShot = Join-Path $shotDir 'ingametest-assetpicker.png'
	$pickerMeans = Measure-Shot $pickerShot @($pickerTiles | ForEach-Object { ,$_.rect })
	Open-Console
	# THE HEAP LINE (C111, its review): the same grid, settled, under a line
	# FORCED 4 slots above what is live now, then scrolled back to the top - a
	# screenful of tiles evicted under the forced cap, so more to load than the
	# line leaves room for. It must stop AT the line and make the rest of the
	# room from the screen it left, every tile in view loaded in the end.
	$heapBefore = Read-PickerStatus 'assetpicker status' $capPattern
	if ($heapBefore) {
		$heapForced = (Read-Fields $heapBefore).srv + 4
		Run-Cmd "thumbcap heap $heapForced"
		Run-Cmd 'assetpicker scroll 0'
		Send-Key 0xC0; Start-Sleep -Milliseconds 3500; Open-Console
		$pickerHeapLine = Read-PickerStatus 'assetpicker status' $capPattern
		Run-Cmd 'thumbcap heap off'
	}
	Run-Cmd 'assetpicker off'
	# The PORTRAIT picker under the same forced cap, scrolled half way too, and
	# read again with the view still.
	Run-Cmd 'thumbcap 8'
	Run-Cmd 'sheet 0'
	Run-Cmd 'portrait picker 0'
	Send-Key 0xC0; Start-Sleep -Milliseconds 3500; Open-Console
	Run-Cmd 'portrait picker scroll 0.5'
	Send-Key 0xC0; Start-Sleep -Milliseconds 3500; Open-Console
	$portraitCapPattern = ('console: picker open shown=\d+ of \d+ filter=\S+ visible=\d+\+\d+ thumbs=\d+ srv=\d+ ' +
		'peak=\d+ blank=\d+ missing=\d+ onscreen=\d+ cap=\d+ evicted=\d+ reloads=\d+ refused=\d+ heapline=\d+ heaptop=\d+')
	$portraitCapLine = Read-PickerStatus 'portrait picker status' $portraitCapPattern
	Send-Key 0xC0; Start-Sleep -Milliseconds 2000; Open-Console
	$portraitStillLine = Read-PickerStatus 'portrait picker status' $portraitCapPattern
	Run-Cmd 'portrait picker off'
	Run-Cmd 'sheet off'
	Run-Cmd 'thumbcap off'
	# The editor PALETTE'S swatches: its Surfaces group with every section and
	# sub-group open (`expand` - grouped rows sit in shut sub-groups otherwise)
	# and no filter, photographed like the tiles, then shut again. The grouping
	# and filter are saved settings, so they are put back as they were.
	Run-Cmd 'editor'
	$paletteWas = $null
	$asked = Get-LogMatchCount 'console: editor palette: (stage|kind) (\S+) filter=''([^'']*)'''
	Run-Cmd 'editor palette'
	$was = @(Wait-NewLogLines 'console: editor palette: (stage|kind) (\S+) filter=''([^'']*)''' $asked 1 10) | Select-Object -Last 1
	if ($was) { $paletteWas = $was.Matches[0].Groups }
	Run-Cmd 'editor palette mode kind'
	Run-Cmd 'editor palette group surfaces'
	Run-Cmd 'editor palette filter'
	Run-Cmd 'editor palette expand'
	Send-Key 0xC0; Start-Sleep -Milliseconds 3500; Open-Console
	$swatchHead = 'console: editor palette swatches: (\d+) drawn, dock (\S+)'
	$asked = Get-LogMatchCount $swatchHead
	$swatchesBefore = @(Read-SurveyLines 'editor palette swatch' 0).Count
	Run-Cmd 'editor palette swatches'
	$swatchHeadLine = @(Wait-NewLogLines $swatchHead $asked 1 10) | Select-Object -Last 1
	$paletteSwatches = @(Read-SurveyLines 'editor palette swatch' $swatchesBefore)
	Send-Key 0xC0; Start-Sleep -Milliseconds 1500
	$paletteShot = Join-Path $shotDir 'ingametest-palette.png'
	$paletteMeans = Measure-Shot $paletteShot @($paletteSwatches | ForEach-Object { ,$_.rect })
	Open-Console
	Run-Cmd 'editor palette collapse'
	if ($paletteWas) {
		Run-Cmd "editor palette mode $($paletteWas[1].Value)"
		Run-Cmd "editor palette group $($paletteWas[2].Value)"
		Run-Cmd "editor palette filter $($paletteWas[3].Value)"
	}
	# `editor off` leaves the map open in Player mode, where the next sweep's
	# Esc would close it instead of pausing - so the map is shut too.
	Run-Cmd 'editor off'
	Run-Cmd 'mappage close'

	# THE SHEET'S READOUTS (code-review batch 54), judged below. The ARMOR
	# TOOLTIP's rows for Brand, unarmored with `avoid` trained - so the avoidance
	# is worth points and a row left out shows in the sum - alone and beside the
	# plate cuirass in his pack.
	Run-Cmd 'setskill 0 avoid 3'
	Run-Cmd 'sheet armor 0'
	Run-Cmd 'sheet armor 0 plate_cuirass'
	# A SKILL'S NAME IN ITS BAR'S COLOUR: blade trained (a weapon: its bar is
	# steel, where the name used to take the accent), the Skills tab shown and
	# laid out - the console shut a moment, since an open console's frames
	# update no UI - then the pointer parked on the blade row, where `sheet
	# status` says it is, and the bar read again behind a marker.
	Run-Cmd 'setskill 0 blade 1.5'
	Run-Cmd 'sheet 0'
	Run-Cmd 'sheet tab skills'
	Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 1000
	Open-Console
	$bladePattern = 'console: sheet skill blade at (\d+),(\d+) colour (\S+)'
	$listed = Get-LogMatchCount $bladePattern
	Run-Quick 'sheet status'
	$bladeRow = @(Wait-NewLogLines $bladePattern $listed 1 10)
	if ($bladeRow.Count -gt 0) {
		$g = $bladeRow[-1].Matches[0].Groups
		$at = [int64](([int]$g[2].Value -shl 16) -bor ([int]$g[1].Value -band 0xFFFF))
		Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 400
		Send-Message 0x200 0 $at   # WM_MOUSEMOVE: onto the row
		Start-Sleep -Milliseconds 1000
		Open-Console
		Run-Quick 'echo sheet-hover blade'
		Run-Quick 'sheet status'
		# And away, to the middle of the window - the 3D view once the sheet
		# shuts, where no panel lights up under it in the strip's picture below.
		$size = Get-ClientSize
		Send-Message 0x200 0 ([int64]((([int]($size[1] / 2)) -shl 16) -bor ([int]($size[0] / 2))))
	}
	Run-Cmd 'sheet off'
	# THE HUD EFFECT STRIP (one DrawEffectIcon, the sheet's too): three wards on
	# Brand, laid out with the console shut, the strip found in the HUD tree
	# (member 0's EffectsArea, the first) and photographed, the console shut
	# again - Michael's look; the verdict is only that the icons were up and
	# the picture was taken.
	Run-Cmd 'effect stoneskin 0'
	Run-Cmd 'effect fireshield 0'
	Run-Cmd 'effect windward 0'
	Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 1000
	Open-Console
	$dumped = Get-LogMatchCount 'console: --- hud ---'
	Run-Cmd 'uitree dump hud'
	Wait-NewLogLines 'console: --- hud ---' $dumped 1 10 | Out-Null
	$strip = $null; $stripIcons = 0; $stripSpread = -1; $stripShot = Join-Path $shotDir 'ingametest-effectstrip.png'
	$dump = @(Get-Content $log)
	$top = -1
	for ($i = $dump.Count - 1; $i -ge 0; $i--) { if ($dump[$i] -match '^\[info \] console: --- hud ---\s*$') { $top = $i; break } }
	$stripIndent = -1
	for ($i = $top + 1; $top -ge 0 -and $i -lt $dump.Count; $i++) {
		if ($dump[$i] -notmatch '^\[info \] console: ( *)(\S.*)$') { continue }
		$indent = $Matches[1].Length; $rest = $Matches[2]
		if (-not $strip) {
			if ($rest -match '^EffectsArea  px (-?\d+),(-?\d+) (\d+)x(\d+)') {
				$strip = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4])
				$stripIndent = $indent
			}
		} elseif ($indent -le $stripIndent) {
			break
		} elseif ($rest -match '^EffectIcon  px ') {
			$stripIcons++   # shown (a hidden one reads "EffectIcon (hidden)")
		}
	}
	if ($strip -and $strip[2] -gt 0 -and $strip[3] -gt 0) {
		Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 1500
		$m = 6   # a margin, so the icons' borders show whole
		$stripSpread = Save-PadShot (Join-Path $shotDir 'ingametest-hud-effects.png') @(
			($strip[0] - $m), ($strip[1] - $m), ($strip[2] + 2 * $m), ($strip[3] + 2 * $m)) $stripShot
		Open-Console
	}

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
# The same lines decoded as UTF-8, invalid bytes as U+FFFD (sweep_worldwords'
# trims check). Line for line with $lines: both split on CR LF / LF.
$utf8Lines = @(if (Test-Path $log) { [System.IO.File]::ReadAllLines($log, (New-Object System.Text.UTF8Encoding($false))) })
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
# levelcheck: the real check must PASS, and each mutation must FAIL by exactly
# its planted file. Read from the game's own verdict lines, never the console's
# mirror of them.
function Get-LcMissing([string]$line) {
	$n = 0
	foreach ($f in 'missing_models', 'missing_worn') {
		if ($line -match "\b$f=(\d+)") { $n += [int]$Matches[1] }
	}
	return $n
}
function Fail-Global([string]$why) {
	Write-Host "  [FAIL] $why" -ForegroundColor Red
	$script:global++
}
$lcAt = @(for ($i = 0; $i -lt $lines.Count; $i++) {
	if ($lines[$i] -match '^\[info \] levelcheck RESULT=') { $i }
})
$lcReal = @($lcAt | Where-Object { $lines[$_] -notmatch ' mutate=' }) | Select-Object -Last 1
if ($null -eq $lcReal) {
	Fail-Global 'levelcheck never reported'
} else {
	$lcLine = $lines[$lcReal]
	Write-Host "  $($lcLine.Substring($lcLine.IndexOf('levelcheck')))"
	if ($lcLine -notmatch 'RESULT=PASS') { $global++ }
	$lcBase = Get-LcMissing $lcLine
	foreach ($m in $levelcheckMutations) {
		$at = @($lcAt | Where-Object { $_ -gt $lcReal -and $lines[$_] -match " mutate=$m " }) |
			Select-Object -Last 1
		if ($null -eq $at) { Fail-Global "levelcheck mutate ${m}: never reported (refused?)"; continue }
		$v = $lines[$at]
		$planted = if ($v -match ' planted=(\S+)') { $Matches[1] } else { '' }
		$from = @($lcAt | Where-Object { $_ -lt $at }) | Select-Object -Last 1
		$named = @(@(Lines-Between $from $at) -match
			("^\[warn \] levelcheck: missing .*'" + [regex]::Escape($planted) + "'"))
		$got = Get-LcMissing $v
		if (-not $planted) {
			Fail-Global "levelcheck mutate ${m}: its verdict names no planted file"
		} elseif ($v -notmatch 'RESULT=FAIL') {
			Fail-Global "levelcheck mutate ${m}: PASSED - the check missed the planted $planted"
		} elseif ($got -ne $lcBase + 1) {
			Fail-Global "levelcheck mutate ${m}: $got missing, expected $($lcBase + 1) (the real run's $lcBase and the planted $planted)"
		} elseif ($named.Count -eq 0) {
			Fail-Global "levelcheck mutate ${m}: failed without naming the planted $planted"
		} else {
			Write-Host "  [ok  ] levelcheck mutate ${m}: FAIL, naming the planted $planted"
		}
	}
	# The controls: as the real run, nothing more missing, the planted file the
	# one the walk resolved.
	$lcResult = if ($lcLine -match 'RESULT=(\S+)') { $Matches[1] } else { '' }
	foreach ($m in $levelcheckControls) {
		$at = @($lcAt | Where-Object { $_ -gt $lcReal -and $lines[$_] -match " mutate=$m " }) |
			Select-Object -Last 1
		if ($null -eq $at) { Fail-Global "levelcheck control ${m}: never reported (refused?)"; continue }
		$v = $lines[$at]
		$planted = if ($v -match ' planted=(\S+)') { $Matches[1] } else { '' }
		$resolved = if ($v -match ' resolved=(\S+)') { $Matches[1] } else { '' }
		$result = if ($v -match 'RESULT=(\S+)') { $Matches[1] } else { '' }
		$got = Get-LcMissing $v
		if (-not $planted) {
			Fail-Global "levelcheck control ${m}: its verdict names no planted file"
		} elseif ($resolved -ne $planted) {
			Fail-Global "levelcheck control ${m}: the check resolved '$resolved', not the planted $planted - it does not open the file the loader opens"
		} elseif ($got -ne $lcBase -or $result -ne $lcResult) {
			Fail-Global "levelcheck control ${m}: RESULT=$result with $got missing, expected the real run's RESULT=$lcResult and $lcBase - the planted $planted loads"
		} else {
			Write-Host "  [ok  ] levelcheck control ${m}: as the real run, resolving the planted $planted"
		}
	}
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
	} elseif ($s.trims) {
		# TRIMS (sweep_worldwords): the findings must be chrome trims and nothing
		# else, at least one and no more than the faces `worldview` said that
		# frame cut, each quoting the drawn text with its "..". The quote is what
		# the face PAINTED (ui::FittedFace's text and mark; NoteChromeTrim adds
		# nothing), so a face that drew no mark fails here. Read again as
		# UTF-8 (Get-Content took the log as ANSI): a face cut part-way through a
		# character quotes a broken byte, which decodes as U+FFFD.
		$cut = 0
		$probe = @($before -match '^\[info \] console: world view: travel screen, .*, trimmed (\d+)\s*$') | Select-Object -Last 1
		if ($probe -and $probe -match 'trimmed (\d+)\s*$') { $cut = [int]$Matches[1] }
		$bad = @(); $trimLines = 0
		for ($i = $h.at + 1; $i -lt $next; $i++) {
			if ($lines[$i] -notmatch '^\[info \]   \S') { continue }
			$u = if ($i -lt $utf8Lines.Count) { $utf8Lines[$i] } else { '' }
			if ($u -notmatch '^\[info \]   chrome > .* trims its text by \d+px to fit: "(.*)"$') {
				$bad += "not a chrome trim: $($lines[$i])"; continue
			}
			$shown = $Matches[1]
			$trimLines++
			if (-not $shown.EndsWith('..')) { $bad += "no '..' after the cut: $u" }
			if ($shown.Contains([string][char]0xFFFD)) { $bad += "a character split by the cut: $u" }
		}
		if ($trimLines -lt 1) { $bad += "no trim reported (the view said it cut $cut face(s))" }
		elseif ($trimLines -gt $cut) { $bad += "$trimLines trims reported, but the view said it cut only $cut face(s)" }
		if ($bad.Count -gt 0) {
			Fail-Check $s.label 'trims' 'the word faces were not cut and reported as they should be:'
			$bad | ForEach-Object { Write-Host "     $_" }
			$ok = $false
		} else {
			Write-Host "  [ok  ] $($s.label): $trimLines chrome trims reported ($cut faces cut), every one whole characters and '..'"
		}
	} elseif ($sum -notmatch 'uioverlap: clean') {
		Fail-Check $s.label 'findings' "found overlaps:"
		$after -match '^\[info \]   \S' | ForEach-Object { Write-Host "     $_" }
		$ok = $false
	}
	if ($ok) {
		$audit = if ($s.trims) { 'trims reported' } else { 'clean' }
		Write-Host "  [ok  ] swept $($s.label) ($($h.state), status logged, $audit)"
	}
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

# THE ETCHED GOLD FOLLOWS THE MATERIAL (code-review C204). The sweep logged every
# material its header counted, and on each the etched symbols' gold reads at
# least as well as the authored dark-stone gold would there - and BETTER wherever
# the ink solve moved the gold, which is the etch following the material (the
# baked gold it replaced read 1.2:1 on the snows). With no material moved, the
# follow check would prove nothing, so one must be. And EVERY etch was measured:
# the rows take the weakest of whichever loaded, so a missing or refused one
# (its stone left on the fallback, one warning at load) would read clean.
$etchProblems = @()
$sweepHead = @($lines | Select-String 'uimaterial sweep: (\d+) materials, etches (\d+)/(\d+) lit (\d+)/(\d+)$') |
	Select-Object -Last 1
if (-not $sweepHead) {
	$etchProblems += 'the material sweep never ran'
} else {
	$hg = $sweepHead.Matches[0].Groups
	if ([int]$hg[3].Value -lt 1 -or $hg[2].Value -ne $hg[3].Value -or $hg[4].Value -ne $hg[5].Value) {
		$etchProblems += "measured $($hg[2].Value) of $($hg[3].Value) etches and $($hg[4].Value) of $($hg[5].Value) lit ones"
		foreach ($w in @($lines | Select-String 'etched symbol')) { $etchProblems += "  $($w.Line)" }
	}
	$want = [int]$hg[1].Value
	$rows = @($lines | Select-String ('uimaterial sweep: (\S+) carved=([\d.]+) etched=([\d.]+) lit=([\d.]+) ' +
		'authored=([\d.]+) authoredlit=([\d.]+) moved=(\d) litmoved=(\d)'))
	if ($want -lt 1 -or $rows.Count -ne $want) { $etchProblems += "the sweep logged $($rows.Count) of $want materials" }
	$moved = 0
	foreach ($r in $rows) {
		$g = $r.Matches[0].Groups
		$name = $g[1].Value
		$e = [double]$g[3].Value; $l = [double]$g[4].Value; $a = [double]$g[5].Value; $al = [double]$g[6].Value
		if ($e -lt $a - 0.005 -or $l -lt $al - 0.005) {
			$etchProblems += "${name}: the etched gold reads worse than the authored gold would ($e vs $a, lit $l vs $al)"
		}
		if ($g[7].Value -eq '1') {
			$moved++
			if ($e -lt $a + 0.01) { $etchProblems += "${name}: the ink solve moved the gold and the etch did not follow ($e vs $a)" }
		}
		if ($g[8].Value -eq '1' -and $l -lt $al + 0.01) {
			$etchProblems += "${name}: the ink solve moved the lit gold and the lit etch did not follow ($l vs $al)"
		}
	}
	if ($moved -eq 0) { $etchProblems += 'the solve moved no material''s gold - the follow check proved nothing' }
}
if ($etchProblems.Count -eq 0) {
	Write-Host "  [ok  ] the etched gold followed the material on all $($rows.Count) materials ($moved moved)"
} else {
	Write-Host '  [FAIL] the etched gold does not follow the material:' -ForegroundColor Red
	$etchProblems | ForEach-Object { Write-Host "     $_" }
	$global++
}
# ...and the movement pad was photographed on the snow: the switch landed (its
# own ink line), the pad was found shown with a size, and the picture is not
# flat (a failed PrintWindow comes back blank).
$snowLine = $lines | Select-String "ui material ${etchMaterial}:" | Select-Object -Last 1
if ($snowLine -and $padRect -and $padRect[2] -gt 0 -and $padRect[3] -gt 0 -and $padSpread -gt 4) {
	Write-Host "  [ok  ] the movement pad photographed on $etchMaterial (spread $([Math]::Round($padSpread, 1))): $padShot"
} else {
	$why = if (-not $snowLine) { "$etchMaterial never showed" } elseif (-not $padRect) { 'the pad was not found shown' }
		   else { "the picture is blank (spread $([Math]::Round($padSpread, 1)))" }
	Write-Host "  [FAIL] no picture of the movement pad on ${etchMaterial}: $why" -ForegroundColor Red
	$global++
}
# ...and the etches were PAINTED in those inks. The sweep reads the inks the draw
# is meant to use, through the draw's own EtchInk, so it cannot see a draw that
# stopped using them; nor can the photo, whose only test is that it is not flat.
# `uimaterial drawn` is DrawCutStone's record of its last plain and lit etch on
# each material (the sheet open, so its current tab drew lit): each drew all
# three panels from their own thirds of the strip, the groove in the call's tint,
# the gold floor in the SOLVED ink x tint and its lit slope in that ink lifted by
# the sheen x tint. On the snow the solved ink is not the authored one, or the
# floor could not tell them apart; on the dark stone the slope is not clamped
# white, or it could not tell the lifted ink from the bare tint.
$drawnProblems = @()
foreach ($mat in $etchMaterial, $sheenMaterial) {
	foreach ($k in 0, 1) {
		$kind = if ($k) { 'lit' } else { 'plain' }
		$d = @($lines | Select-String ("uimaterial drawn: $mat lit=$k draws=(\d+) ink=(\S+) authored=(\S+) " +
			'sheen=(\S+) tint=(\S+) uv=(\S+) groove=(\S+) floor=(\S+) slope=(\S+)')) | Select-Object -Last 1
		if (-not $d) { $drawnProblems += "${mat}: no record of a $kind etch"; continue }
		$g = $d.Matches[0].Groups
		if ([int]$g[1].Value -lt 1) { $drawnProblems += "${mat}: no $kind etch was drawn"; continue }
		$v = @{}
		foreach ($i in 2, 3, 5, 7, 8, 9) { $v[$i] = @($g[$i].Value.Split(',') | ForEach-Object { [double]$_ }) }
		$ink = $v[2]; $auth = $v[3]; $tint = $v[5]; $sheen = [double]$g[4].Value
		if ($mat -eq $etchMaterial) {
			$off = 0.0; foreach ($c in 0..2) { $off += [Math]::Abs($ink[$c] - $auth[$c]) }
			if ($off -lt 0.01) { $drawnProblems += "${mat}: the solve kept the authored $kind gold - the floor check proves nothing" }
		} elseif (@(0..2 | Where-Object { $ink[$_] * $sheen -lt 0.99 }).Count -eq 0) {
			$drawnProblems += "${mat}: the $kind slope is clamped white - the slope check proves nothing"
		}
		$wantGroove = @(0..2 | ForEach-Object { $tint[$_] })
		$wantFloor = @(0..2 | ForEach-Object { $ink[$_] * $tint[$_] })
		$wantSlope = @(0..2 | ForEach-Object { [Math]::Min(1.0, $ink[$_] * $sheen) * $tint[$_] })
		foreach ($p in @(@('groove', $v[7], $wantGroove), @('gold floor', $v[8], $wantFloor), @('lit slope', $v[9], $wantSlope))) {
			$bad = $false; foreach ($c in 0..2) { if ([Math]::Abs($p[1][$c] - $p[2][$c]) -gt 0.001) { $bad = $true } }
			if ($bad) {
				$drawnProblems += "${mat}: the $kind $($p[0]) was drawn $($p[1] -join ','), not $(($p[2] | ForEach-Object { [Math]::Round($_, 4) }) -join ',')"
			}
		}
		$uvs = @($g[6].Value.Split('/'))
		for ($i = 0; $i -lt 3; $i++) {
			$u = if ($i -lt $uvs.Count) { @($uvs[$i].Split(',') | ForEach-Object { [double]$_ }) } else { @() }
			if ($u.Count -ne 4 -or [Math]::Abs($u[0] - $i / 3.0) -gt 0.001 -or [Math]::Abs($u[1]) -gt 0.001 -or
				[Math]::Abs($u[2] - 1 / 3.0) -gt 0.001 -or [Math]::Abs($u[3] - 1.0) -gt 0.001) {
				$drawnProblems += "${mat}: the $kind etch's panel $i was drawn from $($uvs[$i]), not its own third of the strip"
			}
		}
	}
}
if ($drawnProblems.Count -eq 0) {
	Write-Host "  [ok  ] the etches were painted in the solved inks, plain and lit, on $etchMaterial and $sheenMaterial"
} else {
	Write-Host '  [FAIL] the etches were not painted in the solved inks:' -ForegroundColor Red
	$drawnProblems | ForEach-Object { Write-Host "     $_" }
	$global++
}

# THUMBNAILS ARE AS BRIGHT AS THEIR FILES (code-review C158). Each tile and swatch
# the survey photographed is set beside its file's stored mean; an image drawn
# through an sRGB view comes out far darker. Opaque images only (a cut-out's
# alpha lets the panel through), and enough of them bright enough that a
# darkening would show - a survey of three near-black sets proves nothing.
function Judge-Brightness([string]$what, $items, $means, [int]$minCount, [string]$shot) {
	$problems = @()
	if ($null -eq $means) {
		Write-Host "  [FAIL] ${what}: nothing was photographed (PrintWindow failed)" -ForegroundColor Red
		return 1
	}
	$measured = 0; $bright = 0; $worst = 0.0
	for ($i = 0; $i -lt @($items).Count; $i++) {
		$it = $items[$i]
		$seen = if ($i -lt @($means).Count) { $means[$i] } else { $null }
		if (-not $it.drawn -or $null -eq $it.mean -or $it.mean[3] -lt 0.98) { continue }
		if ($null -eq $seen) { $problems += "$($it.name): its rect $($it.rect -join ',') is outside the picture"; continue }
		$measured++
		$want = Luma $it.mean; $got = Luma $seen
		if ($want -ge $thumbBright) { $bright++ }
		$worst = [Math]::Max($worst, [Math]::Abs($got - $want))
		if ([Math]::Abs($got - $want) -gt $thumbTolerance) {
			$problems += "$($it.name) drew at luminance $([Math]::Round($got, 3)); its file averages $([Math]::Round($want, 3))"
		}
	}
	if ($measured -lt $minCount) { $problems += "only $measured opaque, drawn images were measured (want $minCount)" }
	if ($bright -lt 2) { $problems += "only $bright measured image(s) average $thumbBright or more - a darkening could hide" }
	if ($problems.Count -eq 0) {
		Write-Host "  [ok  ] $what drew as bright as their files ($measured measured, $bright bright, worst off by $([Math]::Round($worst, 3))): $shot"
	} else {
		Write-Host "  [FAIL] $what did not draw as bright as their files:" -ForegroundColor Red
		$problems | ForEach-Object { Write-Host "     $_" }
	}
	return $problems.Count
}
if ((Judge-Brightness 'the asset picker''s texture tiles' $pickerTiles $pickerMeans 4 $pickerShot) -gt 0) { $global++ }
$swatchDock = if ($swatchHeadLine) { $swatchHeadLine.Matches[0].Groups[2].Value } else { '?' }
if ($swatchDock -ne 'open') {
	Write-Host "  [FAIL] the editor palette's swatches were never surveyed (palette dock: $swatchDock)" -ForegroundColor Red
	$global++
} elseif ((Judge-Brightness 'the editor palette''s surface swatches' $paletteSwatches $paletteMeans 4 $paletteShot) -gt 0) {
	$global++
}

# NO TILE IN VIEW IS EVICTED (code-review C111). Each picker under a cap FORCED
# below what it shows (`thumbcap 8`), scrolled half way: the cap held (8) and is
# below the count in view, eviction RAN (the screen it opened on was dropped -
# else the cap proved nothing), and yet nothing in view is blank and no key was
# loaded twice (a reload, with the view settled, is a tile evicted while shown).
# Read AGAIN after a settle with the view still, nothing more was evicted: a
# draw that marks tiles it does not show (the asset picker's, before its tiles
# drew only in view) has the cache make and drop the off-screen ones each frame
# - never reloaded (they held nothing) and never blank (they are not in view),
# so `evicted=` climbing is the one sign. A tile whose load found NO image
# (`missing=` - a portrait the worktree does not have, the bought pack being
# gitignored) is not blank: its load ran and was kept like any other, so it is
# only noted.
function Judge-Cap([string]$what, $line, $still) {
	if (-not $line -or -not $still) {
		Write-Host "  [FAIL] ${what}: no status line under the forced cap (or none after the settle)" -ForegroundColor Red
		return 1
	}
	$a = Read-Fields $line; $b = Read-Fields $still
	$problems = @()
	if ($b.cap -ne 8) { $problems += "the cap was $($b.cap), not the forced 8" }
	if ($b.visible -le $b.cap) { $problems += "only $($b.visible) in view - the cap is not below it, so this proves nothing" }
	if ($a.evicted -lt 1) { $problems += 'nothing was evicted - the cap proves nothing' }
	foreach ($s in $a, $b) {
		if ($s.blank -gt 0) { $problems += "$($s.blank) of the $($s.visible) in view have no image" }
		if ($s.reloads -gt 0) { $problems += "$($s.reloads) reloads - tiles were evicted while they showed" }
	}
	if ($b.evicted -ne $a.evicted) {
		$problems += "$($b.evicted - $a.evicted) more evicted with the view standing still ($($a.evicted) -> $($b.evicted)) - something marks tiles that are not on screen"
	}
	if ($b.missing -gt 0) {
		Write-Host "  [note] ${what}: $($b.missing) of the $($b.visible) in view have no image installed (their loads ran and found none) - not blank; judged on the loads"
	}
	if ($problems.Count -eq 0) {
		Write-Host "  [ok  ] ${what}: $($b.visible) in view under a cap of $($b.cap), $($b.evicted) evicted and no more with the view still, none of them showing (0 blank, 0 reloads)"
		return 0
	}
	Write-Host "  [FAIL] ${what} evicted what it showed:" -ForegroundColor Red
	$problems | ForEach-Object { Write-Host "     $_" }
	return 1
}
$global += Judge-Cap 'the asset picker' $pickerCapLine $pickerStillLine
$global += Judge-Cap 'the portrait picker' $portraitCapLine $portraitStillLine

# THE HEAP LINE (C111, its review): the asset picker under a line forced 4 SRVs
# above what was live, scrolled to a screen it had to load again. The line was
# the one forced, the most live at any eviction never passed it, a load was
# turned away (or the line was never reached and this proves nothing), and every
# tile in view is loaded all the same - the room came from the screen it left.
function Judge-Heap($line, [int]$forced) {
	if (-not $line) {
		Write-Host '  [FAIL] the heap line: no asset picker status under the forced line' -ForegroundColor Red
		return 1
	}
	$f = Read-Fields $line
	$problems = @()
	if ($f.heapline -ne $forced) { $problems += "the line in force was $($f.heapline), not the forced $forced" }
	if ($f.heaptop -gt $forced) { $problems += "$($f.heaptop) SRVs were live at an eviction - past the line of $forced" }
	if ($f.refused -lt 1) { $problems += 'no load was turned away - the line was never reached, so this proves nothing' }
	if ($f.blank -gt 0) { $problems += "$($f.blank) of the $($f.visible) in view never loaded - no room was made from what is off screen" }
	if ($problems.Count -eq 0) {
		Write-Host "  [ok  ] the heap line: the asset picker stopped at $forced SRVs (top $($f.heaptop), $($f.refused) loads turned away) and loaded all $($f.visible) in view by evicting off screen"
		return 0
	}
	Write-Host '  [FAIL] the heap line did not hold:' -ForegroundColor Red
	$problems | ForEach-Object { Write-Host "     $_" }
	return 1
}
$global += Judge-Heap $pickerHeapLine $heapForced

# THE ARMOR TOOLTIP ADDS UP (code-review C372). Each `sheet armor` readout is
# the tooltip's own rows (CharacterSheet::BuildArmorTipRows), each with the TEXT
# its cell shows and the value that text was formatted from; on every side the
# Roll's terms - base, DEX, stance, avoidance, armor - must sum to the Roll.
# The sum alone holds BY CONSTRUCTION once the rows exist (stance is the live
# roll's remainder, DefenseFor's total - base - stat - avoidance + armor), so
# the CELLS are read too: every term and the Roll must SHOW its value (the
# signed whole number the cell leads with, within rounding of it), and the
# avoidance cell must read "+N (<lvl> 3)" on an unarmored side - the level the
# script trained - and "-" on an armored one, worth 0. A cell built from the
# wrong side, or the avoidance shown as "-" where it counts, fails here and
# nowhere else. Not vacuous: Brand's avoidance must be worth points (a 0 would
# hide a missing row), and beside the plate cuirass that side must be armored
# (its terms take the armor's cost and no avoidance).
$armorProblems = @()
$armorTerms = @('sheet.def.base', 'sheet.def.dex', 'sheet.def.stance', 'sheet.def.avoid', 'sheet.def.armorpen')
$avoidLevel = 3   # `setskill 0 avoid 3` above
foreach ($case in @(@{ name = 'alone'; head = 'sheet armor: member 0 \(\S+\) - (\d+) rows'; sides = 1 },
					@{ name = 'beside the plate cuirass'; head = 'sheet armor: member 0 \(\S+\) with plate_cuirass - (\d+) rows'; sides = 2 })) {
	$hi = -1; $want = 0
	for ($i = $lines.Count - 1; $i -ge 0; $i--) {
		if ($lines[$i] -match "^\[info \] console: $($case.head)\s*$") { $hi = $i; $want = [int]$Matches[1]; break }
	}
	if ($hi -lt 0) { $armorProblems += "no ``sheet armor`` readout $($case.name)"; continue }
	$vals = @{}; $cells = @{}; $got = 0
	for ($i = $hi + 1; $i -lt [Math]::Min($lines.Count, $hi + 40) -and $got -lt $want; $i++) {
		if ($lines[$i] -match '^\[info \] console: sheet armor row (\S+) "[^"]*": (.*?) = (-?[\d.]+)(?: \| (.*?) = (-?[\d.]+))?\s*$') {
			$right = if ($Matches[5]) { [double]$Matches[5] } else { $null }
			$vals[$Matches[1]] = @([double]$Matches[3], $right)
			$cells[$Matches[1]] = @($Matches[2], $Matches[4])
			$got++
		}
	}
	if ($got -ne $want -or -not $vals.ContainsKey('sheet.def.roll')) {
		$armorProblems += "$($case.name): read $got of $want rows, Roll among them: $($vals.ContainsKey('sheet.def.roll'))"
		continue
	}
	if (-not $vals.ContainsKey('sheet.def.avoid')) { $armorProblems += "$($case.name): no avoidance row for an unarmored side" }
	elseif ($vals['sheet.def.avoid'][0] -lt 0.5) {
		$armorProblems += "$($case.name): the avoidance is worth $($vals['sheet.def.avoid'][0]) - the sum check proves nothing"
	}
	for ($side = 0; $side -lt $case.sides; $side++) {
		$sum = 0.0
		foreach ($t in $armorTerms) { if ($vals.ContainsKey($t)) { $sum += $vals[$t][$side] } }
		$roll = $vals['sheet.def.roll'][$side]
		if ([Math]::Abs($sum - $roll) -gt 0.05) {
			$armorProblems += "$($case.name), $(if ($side) { 'with it' } else { 'as worn' }): the terms sum to $([Math]::Round($sum, 2)), the Roll is $roll"
		}
	}
	if ($case.sides -eq 2 -and $vals.ContainsKey('sheet.def.armorpen') -and $vals['sheet.def.armorpen'][1] -gt -0.5) {
		$armorProblems += "$($case.name): the cuirass costs nothing - that side is not armored, and its sum proves nothing"
	}
	# What the cells SAY. Side 0 is Brand as worn (unarmored), side 1 with the
	# cuirass on (armored, checked just above).
	for ($side = 0; $side -lt $case.sides; $side++) {
		$sideName = "$($case.name), $(if ($side) { 'with it' } else { 'as worn' })"
		foreach ($t in @('sheet.def.roll') + $armorTerms) {
			if (-not $cells.ContainsKey($t)) { continue }
			$text = $cells[$t][$side]; $v = $vals[$t][$side]
			if ($t -eq 'sheet.def.avoid' -and $side -eq 1) {
				if ($text -ne '-' -or [Math]::Abs($v) -gt 0.005) {
					$armorProblems += "${sideName}: the avoidance under armor reads '$text' worth $v - want '-' worth 0"
				}
				continue
			}
			if ($text -notmatch '^([+-]?\d+)(?: \((.+) (\d+)\))?$') {
				$armorProblems += "${sideName}: $t reads '$text' - not the number it is worth ($v)"
				continue
			}
			# (Every value is taken out of $Matches before the next -match resets it.)
			$number = $Matches[1]; $level = $Matches[3]
			$shown = [int]$number; $signed = $number.StartsWith('+') -or $number.StartsWith('-')
			if ([Math]::Abs($shown - $v) -gt 0.51) {
				$armorProblems += "${sideName}: $t shows $shown but is worth $v"
			}
			if ($t -eq 'sheet.def.avoid') {
				if (-not $signed -or $shown -lt 0 -or -not $level -or [int]$level -ne $avoidLevel) {
					$armorProblems += "${sideName}: the unarmored avoidance reads '$text' - want '+$([Math]::Round($v)) (lvl $avoidLevel)', its points and level"
				}
			} elseif ($level) {
				$armorProblems += "${sideName}: $t reads '$text' - a level where none belongs"
			}
		}
	}
}
if ($armorProblems.Count -eq 0) {
	Write-Host '  [ok  ] the armor tooltip''s terms add up to the Roll and each cell shows its value - unarmored (avoidance "+N (lvl 3)") and beside the plate cuirass ("-")'
} else {
	Write-Host '  [FAIL] the armor tooltip''s rows do not add up, or do not show what they are worth:' -ForegroundColor Red
	$armorProblems | ForEach-Object { Write-Host "     $_" }
	$global++
}

# A SKILL READS IN ONE COLOUR (code-review C477): with the pointer parked on the
# blade row, the status bar names a skill and draws the name in the colour that
# row's bar wears (`sheet skill blade ... colour`, SkillBarColor). The name used
# to take the accent for every weapon, defence and reserve skill.
$hoverMark = -1
for ($i = $lines.Count - 1; $i -ge 0; $i--) { if ($lines[$i] -match '^\[info \] console: sheet-hover blade\s*$') { $hoverMark = $i; break } }
$bladeLine = if ($hoverMark -ge 0) {
	@($lines[0..$hoverMark] -match '^\[info \] console: sheet skill blade at \d+,\d+ colour (\S+)\s*$') | Select-Object -Last 1
}
$after = if ($hoverMark -ge 0) { Lines-Between $hoverMark ([Math]::Min($lines.Count, $hoverMark + 40)) } else { @() }
$barName = @($after -match '^\[info \] console: sheet bar: (?!\(empty\))') | Select-Object -First 1
$barColour = @($after -match '^\[info \] console: sheet bar colour: (\S+)\s*$') | Select-Object -First 1
if (-not $bladeLine) {
	Write-Host '  [FAIL] the blade row was never placed (`sheet status` on the Skills tab listed no `sheet skill blade`)' -ForegroundColor Red
	$global++
} elseif (-not $barName -or -not $barColour) {
	Write-Host '  [FAIL] the pointer on the blade row named nothing on the status bar - the hover missed the row' -ForegroundColor Red
	$global++
} else {
	$rowColour = ([regex]::Match($bladeLine, 'colour (\S+)')).Groups[1].Value
	$nameColour = ([regex]::Match($barColour, 'colour: (\S+)')).Groups[1].Value
	if ($rowColour -eq $nameColour) {
		Write-Host "  [ok  ] a skill's name takes its bar's colour ($($barName -replace '^\[info \] console: sheet bar: ', '' -replace ' \|.*$', ''): $nameColour)"
	} else {
		Write-Host "  [FAIL] the blade row's bar is $rowColour but the status bar names it in $nameColour" -ForegroundColor Red
		$global++
	}
}

# ...and the HUD effect strip was photographed: three icons up in member 0's
# strip, the picture not flat (a failed PrintWindow comes back blank).
if ($strip -and $stripIcons -ge 3 -and $stripSpread -gt 4) {
	Write-Host "  [ok  ] the HUD effect strip photographed with $stripIcons icons (spread $([Math]::Round($stripSpread, 1))): $stripShot"
} else {
	$why = if (-not $strip) { 'no EffectsArea in the HUD tree' } elseif ($stripIcons -lt 3) { "$stripIcons icons shown, not 3" }
		   else { "the picture is blank (spread $([Math]::Round($stripSpread, 1)))" }
	Write-Host "  [FAIL] no picture of the HUD effect strip: $why" -ForegroundColor Red
	$global++
}

# THE GAME IS DPI AWARE (code-review C201): the boot says what the process is.
# Unaware - an exe that lost src/Main/DpiAware.manifest - sees logical pixels on
# a scaled monitor, so Windows stretches a too-small swapchain over it, blurred,
# and the Video tab's physical resolutions come out too large.
$dpiLine = @($lines -match '^\[info \] dpi: awareness=\S+ window=\d+ scale=\d+%') | Select-Object -First 1
if (-not $dpiLine) {
	Write-Host '  [FAIL] the boot logged no `dpi: awareness=` line' -ForegroundColor Red
	$global++
} elseif ($dpiLine -match 'awareness=permonitorv2 ') {
	Write-Host "  [ok  ] the game is per-monitor-v2 DPI aware: $($dpiLine -replace '^\[info \] ', '')"
} else {
	Write-Host "  [FAIL] the game is not per-monitor-v2 DPI aware: $($dpiLine -replace '^\[info \] ', '')" -ForegroundColor Red
	$global++
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
		Write-Host 'SELF-TEST PASSED - exactly the injected faults failed: a refused open step, a missing label and an uncut word face' -ForegroundColor Green
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
