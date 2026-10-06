# ============================================================================
# tools\PathsTest.ps1 - the game and the baker run from a folder outside ASCII.
#
#   .\tools\PathsTest.ps1              # exit 0 = PASS
#   .\tools\PathsTest.ps1 -SelfTest    # the same run on copies whose manifest
#                                      # lost its UTF-8 code page - must FAIL
#
# Every path in the engine is UTF-8, and the narrow file APIs - fopen,
# std::ifstream, std::filesystem, stb_image, cgltf, dr_wav - read a char* in
# the PROCESS code page. That page was CP1252, so a UTF-8 name past ASCII
# opened the wrong file, and path::string() THREW on a character CP1252 cannot
# hold, from crash::Install before any handler was up (code-review C384). Every
# exe now carries src/Core/Utf8CodePage.manifest, which makes the page UTF-8.
# Four cases, each judged from what landed on disk:
#
#   manifest  every exe in bin (and every copy this run launches) declares
#             activeCodePage UTF-8 - read back out of its RT_MANIFEST resource.
#   game      Dungeon.exe, copied into %TEMP%\dn-paths-<pid>-<name outside
#             ASCII>, runs headless with an eval script beside it (also outside
#             ASCII): exit 0, its dungeon.log THERE, the batch PASS, no
#             code-page warning, its shadercache THERE, and through the
#             `readfile` dev command the real assets::ReadBinaryFile gives a
#             file's exact byte count, and NONE for a directory (C231).
#   import    AssetBaker.exe, copied there too, imports a PBR set whose maps
#             (names outside ASCII) sit in a folder outside ASCII, into an
#             assets folder outside ASCII: exit 0, three PNGs and three DDS.
#   model     import-model of the committed rock.glb, copied under a name
#             outside ASCII: exit 0 and the .gltf written.
#
# -SelfTest rewrites the two COPIES' manifests without the activeCodePage
# element (the UpdateResource API; the build's own exes are not touched) and
# demands that every case then FAIL, and for the reason a missing code page
# gives - the check must see the manifest's effect, not pass on a run that
# would have worked anyway:
#
#   manifest  the copies are named as lacking it.
#   game      EXIT 2: its first eval script did not open (a UTF-8 name read as
#             CP1252). That code is returned after crash::Install, the device
#             and the Game, so it also proves Core/Paths' half of C384: a
#             path::string() there throws from crash::Install, before any
#             handler, and the game ABORTS - exit 3 / a fast-fail in release, a
#             modal dialog killed at the timeout in debug. Any failure is not
#             enough, or that abort would count as caught.
#   import    EXIT 1: the baker refused a path it could not open (the same
#   model     abort, from its first log line, otherwise).
#
# NOT Assert-NotRunning: the game here is a copy with its own log and settings
# beside it, so it shares nothing with a game run from bin. Muted by its own
# settings.ini (volume=0) for the same reason, not by HarnessAudio.
#
# The scratch folder is deleted however the run ends; so is anything else under
# %TEMP% starting dn-paths-<pid>- (a self-test run reads the UTF-8 name as
# CP1252 and can create a mojibake twin of it).
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI. The names outside ASCII are
# built from code points below.
# ============================================================================
[CmdletBinding()]
param(
	[switch]$SelfTest,
	[ValidateSet('debug', 'release')][string]$Config = 'debug',
	[int]$GameTimeoutSec = 600,
	[int]$BakerTimeoutSec = 120
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"

# The stale-exe refusal (exit 4) the other harnesses share.
. (Join-Path $PSScriptRoot 'HarnessGame.ps1')

$gameExe = Join-Path $bin 'Dungeon.exe'
$bakerExe = Join-Path $bin 'AssetBaker.exe'
foreach ($e in @($gameExe, $bakerExe)) {
	if (-not (Test-Path $e)) { Write-Host "no build at $e - run build.cmd $Config first" -ForegroundColor Red; exit 2 }
	Assert-ExeCurrent $e
}
$rock = Join-Path $root 'assets\models\rock.glb'
if (-not (Test-Path $rock)) { throw "no $rock - the model case imports it" }

if (-not ([System.Management.Automation.PSTypeName]'PathsTestRes').Type) {
	Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class PathsTestRes {
	[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr LoadLibraryExW(string f, IntPtr h, uint flags);
	[DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr h);
	[DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr FindResourceW(IntPtr h, IntPtr name, IntPtr type);
	[DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr LoadResource(IntPtr h, IntPtr res);
	[DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr LockResource(IntPtr h);
	[DllImport("kernel32.dll", SetLastError = true)] static extern uint SizeofResource(IntPtr h, IntPtr res);
	[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr BeginUpdateResourceW(string f, bool deleteAll);
	[DllImport("kernel32.dll", SetLastError = true)] static extern bool UpdateResourceW(IntPtr h, IntPtr type, IntPtr name, ushort lang, byte[] data, uint size);
	[DllImport("kernel32.dll", SetLastError = true)] static extern bool EndUpdateResourceW(IntPtr h, bool discard);
	const int RT_MANIFEST = 24, EXE_MANIFEST_ID = 1;

	// The exe's own manifest (RT_MANIFEST, id 1) as text, or null.
	public static string Read(string exe) {
		IntPtr h = LoadLibraryExW(exe, IntPtr.Zero, 0x2 | 0x20); // AS_DATAFILE | AS_IMAGE_RESOURCE
		if (h == IntPtr.Zero) return null;
		try {
			IntPtr r = FindResourceW(h, (IntPtr)EXE_MANIFEST_ID, (IntPtr)RT_MANIFEST);
			if (r == IntPtr.Zero) return null;
			uint n = SizeofResource(h, r);
			IntPtr p = LockResource(LoadResource(h, r));
			if (p == IntPtr.Zero) return null;
			byte[] b = new byte[n];
			Marshal.Copy(p, b, 0, (int)n);
			return Encoding.UTF8.GetString(b);
		} finally { FreeLibrary(h); }
	}

	// Replaces the manifest. Every other resource goes with it (these exes carry
	// no other), which spares finding the old one's language. 0, or the Win32
	// error of the step that failed.
	public static int Replace(string exe, string manifest) {
		byte[] b = Encoding.UTF8.GetBytes(manifest);
		IntPtr h = BeginUpdateResourceW(exe, true);
		if (h == IntPtr.Zero) return Marshal.GetLastWin32Error();
		if (!UpdateResourceW(h, (IntPtr)RT_MANIFEST, (IntPtr)EXE_MANIFEST_ID, 0, b, (uint)b.Length)) {
			int e = Marshal.GetLastWin32Error();
			EndUpdateResourceW(h, true);
			return e;
		}
		return EndUpdateResourceW(h, false) ? 0 : Marshal.GetLastWin32Error();
	}
}
'@
}

$utf8 = New-Object Text.UTF8Encoding $false
$codePageRx = '<activeCodePage[^>]*>\s*UTF-8\s*</activeCodePage>'

# A string from code points, so this file stays ASCII.
function U([int[]]$cps) { -join ($cps | ForEach-Object { [char]::ConvertFromUtf32($_) }) }
# "Uni-Puti-lujing-hwair": Latin-1 (CP1252 holds it), Cyrillic and CJK (it does
# not) and U+10348, a surrogate pair in UTF-16 and four bytes in UTF-8.
$name = U 0xDC,0x6E,0xEF,0x2D,0x41F,0x443,0x442,0x438,0x2D,0x8DEF,0x5F84,0x2D,0x10348
$prefix = "dn-paths-$PID-"
$scratch = Join-Path $env:TEMP ($prefix + $name)
$srcDir = Join-Path $scratch ((U 0x51,0x75,0x65,0x6C,0x6C,0x65,0x2D) + (U 0x438,0x441,0x442,0x43E,0x447,0x43D,0x438,0x43A))
$outDir = Join-Path $scratch ((U 0x5A,0x69,0x65,0x6C,0x2D) + (U 0x446,0x435,0x43B,0x44C))
$stem = (U 0x53,0x74,0x65,0x69,0x6E,0x2D) + (U 0x43A,0x430,0x43C,0x435,0x43D,0x44C)

$results = @()
function Record([string]$case, [bool]$ok, [string]$detail) {
	$script:results += [pscustomobject]@{ Case = $case; Ok = $ok; Detail = $detail }
	Write-Host ("  [{0}] {1,-9} {2}" -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $case, $detail)
}

# Starts $exe hidden, waits up to $sec, kills it BY ITS OWN PROCESS if it has
# not gone (a debug abort parks on a modal dialog). Returns the exit code, or
# $null on a timeout.
function Invoke-Timed([string]$exe, [string[]]$argv, [string]$dir, [int]$sec) {
	$quoted = @($argv | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } })
	$p = Start-Process -FilePath $exe -ArgumentList $quoted -WorkingDirectory $dir -WindowStyle Hidden -PassThru
	$null = $p.Handle # PS 5.1: without the handle cached, ExitCode can read back empty
	if (-not $p.WaitForExit($sec * 1000)) {
		$p.Kill(); $p.WaitForExit(5000) | Out-Null
		return $null
	}
	return $p.ExitCode
}

function Remove-Scratch {
	Get-ChildItem $env:TEMP -Filter "$prefix*" -Force -ErrorAction SilentlyContinue |
		ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
}

# Synthetic 64x64 maps: a pattern for the albedo, a flat normal, a ramp for the
# height (an even map is "absent" to the importer) and a mid roughness.
Add-Type -AssemblyName System.Drawing
function Save-Map([string]$path, [scriptblock]$pixel) {
	$bmp = New-Object System.Drawing.Bitmap 64, 64
	try {
		for ($y = 0; $y -lt 64; $y++) { for ($x = 0; $x -lt 64; $x++) { $bmp.SetPixel($x, $y, (& $pixel $x $y)) } }
		$bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
	} finally { $bmp.Dispose() }
}

Write-Host ''
Write-Host ("=== a folder outside ASCII{0} ===" -f $(if ($SelfTest) { ' (self-test: copies without the UTF-8 code page)' } else { '' }))
Write-Host "  scratch: $scratch"

try {
	Remove-Scratch
	New-Item -ItemType Directory -Force -Path $scratch, $srcDir | Out-Null
	$game = Join-Path $scratch 'Dungeon.exe'
	$baker = Join-Path $scratch 'AssetBaker.exe'
	Copy-Item -LiteralPath $gameExe -Destination $game
	Copy-Item -LiteralPath $bakerExe -Destination $baker

	if ($SelfTest) {
		foreach ($c in @($game, $baker)) {
			$m = [PathsTestRes]::Read($c)
			if (-not $m -or $m -notmatch $codePageRx) { throw "self-test: $c has no UTF-8 code page to take out" }
			# Retried: a just-copied exe is often held a moment by a virus scan,
			# which fails the update with a sharing violation.
			$err = 0
			for ($try = 1; $try -le 20; $try++) {
				$err = [PathsTestRes]::Replace($c, ($m -replace $codePageRx, ''))
				if ($err -eq 0) { break }
				Start-Sleep -Milliseconds 500
			}
			if ($err -ne 0) { throw "self-test: could not rewrite the manifest of $c (Win32 error $err)" }
			if ([PathsTestRes]::Read($c) -match $codePageRx) { throw "self-test: the rewrite of $c did not take" }
		}
		Write-Host '  self-test: the copies'' manifests no longer name a code page'
	}

	# --- manifest --------------------------------------------------------------
	$exes = @(Get-ChildItem (Join-Path $bin '*.exe') | ForEach-Object { $_.FullName }) + @($game, $baker)
	$lacking = @($exes | Where-Object { -not ([PathsTestRes]::Read($_) -match $codePageRx) })
	if ($lacking.Count -eq 0) {
		Record 'manifest' $true "$($exes.Count) exes declare activeCodePage UTF-8"
	} else {
		Record 'manifest' $false ("no UTF-8 code page in: " + (($lacking | ForEach-Object { Split-Path -Leaf $_ }) -join ', '))
	}

	# --- game ------------------------------------------------------------------
	try {
		# Its own settings: muted, and nothing from a developer's ini.
		[IO.File]::WriteAllText((Join-Path $scratch 'settings.ini'), "volume=0`n", $utf8)
		$probe = Join-Path $scratch ((U 0x50,0x72,0x6F,0x62,0x65,0x2D) + (U 0x43F,0x440,0x43E,0x431,0x430) + '.txt')
		$probeText = (U 0x47,0x72,0xFC,0xDF,0x65,0x2C,0x20,0x43C,0x438,0x440,0x20,0x10348) + "`n"
		[IO.File]::WriteAllText($probe, $probeText, $utf8)
		$probeBytes = $utf8.GetByteCount($probeText)
		$script = Join-Path $scratch ((U 0x50,0x66,0x61,0x64,0x2D) + (U 0x43F,0x443,0x442,0x44C) + '.eval')
		$evalText = @(
			'; written by tools\PathsTest.ps1 - it ends in play, as every eval script must'
			'reset'
			"readfile $probe"
			"readfile $scratch"
		) -join "`n"
		[IO.File]::WriteAllText($script, $evalText + "`n", $utf8)

		Write-Host '  game: a headless run from the scratch folder (a cold shader cache - a minute or two)'
		$code = Invoke-Timed $game @('-headless', '-project', 'dungeon-demo', '-eval', $script) $scratch $GameTimeoutSec
		$log = Join-Path $scratch 'dungeon.log'
		$text = if (Test-Path -LiteralPath $log) { [IO.File]::ReadAllText($log, $utf8) } else { $null }
		$why = @()
		if ($null -eq $code) { $why += "still running after ${GameTimeoutSec}s (killed)" }
		elseif ($code -ne 0) { $why += "exited $code" }
		if ($null -eq $text) {
			$why += 'no dungeon.log in the scratch folder'
		} else {
			if ($text -notmatch 'eval BATCH RESULT=PASS') { $why += 'no eval BATCH RESULT=PASS' }
			if ($text -match 'the process code page is') { $why += 'the game warned its code page is not UTF-8' }
			$fileLine = "console: readfile: $probeBytes bytes from $probe"
			if (-not $text.Contains($fileLine)) { $why += "no '$fileLine'" }
			$dirLine = "console: readfile: none - not a file, a directory: $scratch"
			if (-not $text.Contains($dirLine)) { $why += "no '$dirLine'" }
		}
		$cache = @(Get-ChildItem -LiteralPath (Join-Path $scratch 'shadercache') -Filter '*.dxbc' -ErrorAction SilentlyContinue)
		if ($cache.Count -eq 0) { $why += 'no shadercache\*.dxbc written in the scratch folder' }
		if ($why.Count -eq 0) {
			Record 'game' $true "exit 0, log + $($cache.Count) cached shaders there; readfile: $probeBytes bytes, and none for the directory"
		} else {
			Record 'game' $false ($why -join '; ')
		}
	} catch {
		Record 'game' $false "harness error: $($_.Exception.Message)"
	}

	# --- import ----------------------------------------------------------------
	try {
		Save-Map (Join-Path $srcDir "$($stem)_albedo.png") { param($x, $y) [System.Drawing.Color]::FromArgb(255, 4 * $x, 4 * $y, 128) }
		Save-Map (Join-Path $srcDir "$($stem)_normal.png") { param($x, $y) [System.Drawing.Color]::FromArgb(255, 128, 128, 255) }
		Save-Map (Join-Path $srcDir "$($stem)_height.png") { param($x, $y) [System.Drawing.Color]::FromArgb(255, 4 * $x, 4 * $x, 4 * $x) }
		Save-Map (Join-Path $srcDir "$($stem)_roughness.png") { param($x, $y) [System.Drawing.Color]::FromArgb(255, 160, 160, 160) }
		Write-Host '  import: AssetBaker import from and into folders outside ASCII'
		$code = Invoke-Timed $baker @('import', $srcDir, $outDir, 'pathtest') $scratch $BakerTimeoutSec
		$tex = Join-Path $outDir 'textures'
		$want = @('pathtest.png', 'pathtest_n.png', 'pathtest_mr.png', 'pathtest.dds', 'pathtest_n.dds', 'pathtest_mr.dds')
		$missing = @($want | Where-Object { -not (Test-Path -LiteralPath (Join-Path $tex $_)) })
		$bakerLog = Test-Path -LiteralPath (Join-Path $scratch 'assetbaker.log')
		$why = @()
		if ($null -eq $code) { $why += "still running after ${BakerTimeoutSec}s (killed)" }
		elseif ($code -ne 0) { $why += "exited $code" }
		if ($missing.Count -gt 0) { $why += "missing $($missing -join ', ')" }
		if (-not $bakerLog) { $why += 'no assetbaker.log in the scratch folder' }
		if ($why.Count -eq 0) { Record 'import' $true 'exit 0, three maps and three BC7 chains, its log beside it' }
		else { Record 'import' $false ($why -join '; ') }
	} catch {
		Record 'import' $false "harness error: $($_.Exception.Message)"
	}

	# --- model -----------------------------------------------------------------
	try {
		$glb = Join-Path $srcDir ((U 0x46,0x65,0x6C,0x73,0x2D) + (U 0x441,0x43A,0x430,0x43B,0x430) + '.glb')
		Copy-Item -LiteralPath $rock -Destination $glb
		Write-Host '  model: AssetBaker import-model of a .glb named outside ASCII'
		$code = Invoke-Timed $baker @('import-model', $glb, $outDir, 'pathrock', '--texture-set', 'pathtest') $scratch $BakerTimeoutSec
		$gltf = Join-Path $outDir 'models\pathrock.gltf'
		$size = if (Test-Path -LiteralPath $gltf) { (Get-Item -LiteralPath $gltf).Length } else { 0 }
		$why = @()
		if ($null -eq $code) { $why += "still running after ${BakerTimeoutSec}s (killed)" }
		elseif ($code -ne 0) { $why += "exited $code" }
		if ($size -le 0) { $why += 'no models\pathrock.gltf written' }
		if ($why.Count -eq 0) { Record 'model' $true "exit 0, pathrock.gltf ($size bytes)" }
		else { Record 'model' $false ($why -join '; ') }
	} catch {
		Record 'model' $false "harness error: $($_.Exception.Message)"
	}
} catch {
	Record 'setup' $false "harness error: $($_.Exception.Message)"
} finally {
	Remove-Scratch
	if (Get-ChildItem $env:TEMP -Filter "$prefix*" -Force -ErrorAction SilentlyContinue) {
		Write-Host "  WARNING: could not delete everything under $env:TEMP\$prefix*" -ForegroundColor Yellow
	}
}

$failures = @($results | Where-Object { -not $_.Ok }).Count
Write-Host ''
Write-Host ("pathstest RESULT={0} cases={1} failures={2} self_test={3}" -f $(if ($failures -eq 0) { 'PASS' } else { 'FAIL' }),
	$results.Count, $failures, [int]$SelfTest.IsPresent)
if ($SelfTest) {
	# Every case must have failed FOR THE RIGHT REASON (see the header): not a
	# harness error, which is no caught regression, and not just any failure -
	# a game that aborted before its handlers fails too.
	$expect = @{
		manifest = '^no UTF-8 code page in: '
		game     = '^exited 2(;|$)'
		import   = '^exited 1(;|$)'
		model    = '^exited 1(;|$)'
	}
	$right = 0
	foreach ($r in $results) {
		if (-not $r.Ok -and $expect.ContainsKey($r.Case) -and $r.Detail -match $expect[$r.Case]) { $right++; continue }
		$why = if ($r.Ok) { 'passed' } else { "failed, but not as /$($expect[$r.Case])/: $($r.Detail)" }
		Write-Host "  self-test: $($r.Case) $why" -ForegroundColor Yellow
	}
	$caught = ($right -eq $expect.Count -and $results.Count -eq $expect.Count)
	Write-Host $(if ($caught) { 'SELF-TEST PASSED - without the UTF-8 code page every case failed, as it should (game exit 2, imports exit 1)' } else { 'SELF-TEST FAILED - a case passed without the UTF-8 code page, failed for another reason, or the harness broke' })
	exit $(if ($caught) { 0 } else { 1 })
}
exit $(if ($failures -eq 0) { 0 } else { 1 })
