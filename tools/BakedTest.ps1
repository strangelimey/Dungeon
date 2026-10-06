# ============================================================================
# tools\BakedTest.ps1 - a baked file stands in for its source only while CURRENT.
#
#   .\tools\BakedTest.ps1              # exit 0 = PASS
#   .\tools\BakedTest.ps1 -SelfTest    # nothing planted - the planted checks must FAIL
#
# The game draws a texture from its BC7 .dds rather than its PNG, and a model's
# embedded images from their baked sidecars (<model>.<index>.dds). The texture
# loader never compared the dates, so after `AssetBaker runes` rewrote only the
# PNGs the game went on drawing the old tablets from the .dds beside them
# (code-review C410); and a MISSING sidecar decoded in silence, ~50 ms an image
# at every load, while only a stale one warned - once per image (C437). Both
# loaders now ask one rule, assets::BakedIsCurrent, and say what they refused.
#
# One headless game (tools\EvalScripts\bakedload.eval: a level load, which
# builds every item kind) with two faults planted before it starts:
#
#   load      the level load finished clean. A game that crashed, hit an assert
#             (killed at -TimeoutSec) or failed the eval is a FAIL - the loader
#             under test is what it was running - and nothing else is judged.
#   texture   rune_fire_2k.png is made NEWER than its .dds - the TIMESTAMP only.
#             The loader must warn "rune_fire_2k.dds is older than its PNG".
#   sidecar   leather_armor.glb is made newer than its sidecars AND its first
#             sidecar is hidden (renamed beside itself). The game must say so in
#             ONE line for the model, counting a missing and a stale image.
#   current   nothing ELSE is reported stale, missing or unreadable: a rule that
#             called everything stale would satisfy both checks above.
#
# A TREE THAT IS BEHIND IS REFUSED, NOT FAILED (exit 2, nothing judged): the
# planted faults are evidence only against a tree whose OTHER bakes are current.
# Before planting, every .dds in assets\textures and assets\portraits is held to
# its PNG and every model sidecar to its model - the loaders' own rule. A MISSING
# sidecar cannot be seen from here (which images get one depends on their sides,
# a multiple of 4, and on the loader's order), so after the run every OTHER bake
# the game refused is held against the disk: when the sidecars a model's line
# counts missing or stale are missing or stale on disk too, the TREE is behind
# (a fresh fetch, or a provision from a tree that never baked them) and the run
# is refused, naming them - "run AssetBaker model-images". A refusal the disk
# does not bear out is the loader's error, and `current` FAILs on it.
#
# Everything planted is put back in a finally (the times restored, the sidecar
# renamed back), however the run ends. A hidden sidecar left by a run that was
# killed outright is put back before anything else.
#
# -SelfTest plants neither fault, and EXACTLY the two planted checks must then
# fail while `load` and `current` pass - the evidence that neither passes on a
# line the game writes for some other reason.
#
# Exit: 0 PASS, 1 FAIL, 2 nothing judged (no build, nothing to plant, the tree
# behind), 3 this worktree's game is running, 4 the exe is stale.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[switch]$SelfTest,
	[ValidateSet('debug', 'release')][string]$Config = 'debug',
	[int]$TimeoutSec = 300
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"
$assets = Join-Path $root 'assets'

# Muted for the whole run, restored however it ends (tools\HarnessAudio.ps1).
. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not (Test-HarnessMuted $bin)) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }
# The stale-exe and already-running refusals (tools\HarnessGame.ps1).
. (Join-Path $PSScriptRoot 'HarnessGame.ps1')

$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'
$evalScript = Join-Path $root 'tools\EvalScripts\bakedload.eval'

if (-not (Test-Path $exe)) { Write-Host "baked: no build at $exe - run build.cmd $Config first"; exit 2 }
Assert-ExeCurrent $exe
Assert-NotRunning $exe

# ---------------------------------------------------------------------------
# Is the tree behind? (assets::BakedIsCurrent, restated: a baked file no older
# than its source is current, and a missing source cannot make it stale)
# ---------------------------------------------------------------------------

# Every baked file on disk that is OLDER than its source: a texture set's or a
# portrait's .dds against its PNG, a model sidecar against its model.
function Find-StaleBakes {
	$found = @()
	foreach ($dir in 'textures', 'portraits') {
		$d = Join-Path $assets $dir
		if (-not (Test-Path $d)) { continue }
		$pngs = @{}
		foreach ($f in Get-ChildItem $d -Filter *.png -File) { $pngs[$f.BaseName] = $f.LastWriteTimeUtc }
		foreach ($f in Get-ChildItem $d -Filter *.dds -File) {
			if ($f.Extension -ne '.dds') { continue } # the filter's 8.3 match is loose
			$src = $pngs[$f.BaseName]
			if ($null -ne $src -and $src -gt $f.LastWriteTimeUtc) { $found += "$dir\$($f.Name)" }
		}
	}
	$m = Join-Path $assets 'models'
	$models = @{}
	foreach ($f in Get-ChildItem $m -File) { if ($f.Extension -in '.gltf', '.glb') { $models[$f.Name] = $f.LastWriteTimeUtc } }
	foreach ($f in Get-ChildItem $m -Filter *.dds -File) {
		if ($f.Name -notmatch '^(.+\.(gltf|glb))\.\d+\.dds$') { continue }
		$src = $models[$Matches[1]]
		if ($null -ne $src -and $src -gt $f.LastWriteTimeUtc) { $found += "models\$($f.Name)" }
	}
	return , $found
}

# The game's two refusals of a bake it found behind, any file.
$anyDdsRx = '^\[warn \] (.+\.dds) is older than its PNG'
$anyModelRx = '^\[warn \] (.+): (\d+) of its (\d+) embedded images decoded at load - (\d+) with no baked sidecar, (\d+) older than the model'

# Whether the disk bears a refusal out - the tree, not the rule, is behind.
function Test-TreeBehind([string]$line) {
	if ($line -match $anyDdsRx) {
		$dds = $Matches[1]
		$png = [IO.Path]::ChangeExtension($dds, '.png')
		if (-not (Test-Path -LiteralPath $dds) -or -not (Test-Path -LiteralPath $png)) { return $false }
		return (Get-Item -LiteralPath $png).LastWriteTimeUtc -gt (Get-Item -LiteralPath $dds).LastWriteTimeUtc
	}
	if ($line -match $anyModelRx) {
		$path = $Matches[1]; $images = [int]$Matches[3]; $missing = [int]$Matches[4]; $stale = [int]$Matches[5]
		if ($missing + $stale -eq 0) { return $false } # names nothing behind
		if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path $assets $path }
		if (-not (Test-Path -LiteralPath $path)) { return $false }
		$modelTime = (Get-Item -LiteralPath $path).LastWriteTimeUtc
		# Images the bake skips (sides not a multiple of 4) have no sidecar either,
		# so this can only say the disk has AT LEAST what the line counted.
		$m = 0; $s = 0
		for ($i = 0; $i -lt $images; $i++) {
			$sc = "$path.$i.dds"
			if (-not (Test-Path -LiteralPath $sc)) { $m++ }
			elseif ((Get-Item -LiteralPath $sc).LastWriteTimeUtc -lt $modelTime) { $s++ }
		}
		return $m -ge $missing -and $s -ge $stale
	}
	return $false # a reader's refusal: the bake is there, current and unreadable
}

# ---------------------------------------------------------------------------
# What gets planted
# ---------------------------------------------------------------------------
$runeName = 'rune_fire_2k'
$runePng = Join-Path $assets "textures\$runeName.png"
$runeDds = Join-Path $assets "textures\$runeName.dds"
$modelName = 'leather_armor.glb'
$model = Join-Path $assets "models\$modelName"
$sidecar = "$model.0.dds"
$hidden = "$sidecar.bakedtest-hidden"

# A run killed between the rename and its finally leaves the sidecar hidden.
if ((Test-Path $hidden) -and -not (Test-Path $sidecar)) {
	Move-Item $hidden $sidecar
	Write-Host "baked: put back $sidecar, hidden by a run that never finished"
}

foreach ($f in $runePng, $runeDds, $model, $sidecar) {
	if (-not (Test-Path $f)) { Write-Host "baked: nothing to plant - $f is missing (provision the assets, run AssetBaker mips)"; exit 2 }
}
# Planted faults are only evidence on a tree that was current to begin with -
# the planted files' own bakes included.
$staleBakes = Find-StaleBakes
if ($staleBakes.Count) {
	Write-Host "baked: the tree is not current to start - $($staleBakes.Count) baked file(s) older than their source; nothing was judged:"
	$staleBakes | Select-Object -First 5 | ForEach-Object { Write-Host "  assets\$_" }
	if ($staleBakes.Count -gt 5) { Write-Host "  ... and $($staleBakes.Count - 5) more" }
	Write-Host '  run AssetBaker mips (the release baker), then re-run'
	exit 2
}
$sidecars = @(Get-ChildItem (Join-Path $assets 'models') -Filter "$modelName.*.dds" -File)

$results = @()
function Record([string]$check, [bool]$ok, [string]$detail) {
	$script:results += [pscustomobject]@{ Check = $check; Ok = $ok; Detail = $detail }
	Write-Host ("  [{0}] {1,-8} {2}" -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $check, $detail)
}
function Find-Result([string]$check) { return $script:results | Where-Object { $_.Check -eq $check } }

# The last line, and the exit code it stands for. Under -SelfTest it passes
# only when exactly the planted checks failed: `load` and `current` must hold.
function Exit-WithVerdict {
	$failures = @($script:results | Where-Object { -not $_.Ok }).Count
	$verdict = if ($failures -eq 0) { 'PASS' } else { 'FAIL' }
	Write-Host ''
	if ($SelfTest) {
		$load = Find-Result 'load'; $texture = Find-Result 'texture'
		$side = Find-Result 'sidecar'; $current = Find-Result 'current'
		$caught = $load -and $load.Ok -and $texture -and -not $texture.Ok -and
			$side -and -not $side.Ok -and $current -and $current.Ok
		Write-Host ("bakedtest RESULT={0} checks={1} failures={2} self_test=1 caught={3}" -f `
			$verdict, $script:results.Count, $failures, [int]$caught)
		Write-Host $(if ($caught) { 'SELF-TEST PASSED - with nothing planted, exactly the planted checks failed' }
			else { 'SELF-TEST FAILED - the load did not finish, a planted check passed on nothing, or `current` failed' })
		exit $(if ($caught) { 0 } else { 1 })
	}
	Write-Host ("bakedtest RESULT={0} checks={1} failures={2} self_test=0" -f $verdict, $script:results.Count, $failures)
	exit $(if ($failures -eq 0) { 0 } else { 1 })
}

Write-Host ''
Write-Host ("=== a baked file stands in for its source only while current{0} ===" -f $(if ($SelfTest) { ' (self-test: nothing planted)' } else { '' }))

$pngWas = (Get-Item $runePng).LastWriteTimeUtc
$modelWas = (Get-Item $model).LastWriteTimeUtc
$lines = @()
$ran = $false
$loadDetail = ''
try {
	if (-not $SelfTest) {
		# Newer by a margin no file system's timestamp resolution rounds away.
		$newest = ($sidecars | Measure-Object -Property LastWriteTimeUtc -Maximum).Maximum
		(Get-Item $runePng).LastWriteTimeUtc = (Get-Item $runeDds).LastWriteTimeUtc.AddMinutes(1)
		(Get-Item $model).LastWriteTimeUtc = $newest.AddMinutes(1)
		Move-Item $sidecar $hidden
		Write-Host "  planted: $runeName.png newer than its .dds; $modelName newer than its sidecars, sidecar 0 hidden"
	}
	Remove-Item $log -ErrorAction SilentlyContinue
	$p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru `
		-ArgumentList @('-project', 'dungeon-demo', '-headless', '-eval', $evalScript)
	$null = $p.Handle # or ExitCode reads $null once it ends
	if (-not $p.WaitForExit($TimeoutSec * 1000)) {
		# A debug assert parks a modal dialog and the process looks alive: killed
		# BY PID, never by name (other worktrees' games are Dungeon.exe too).
		$p.Kill(); $p.WaitForExit(5000) | Out-Null
		$loadDetail = "the game was still running after $TimeoutSec s - killed (pid $($p.Id)): an assert, or a hang"
	} else {
		$lines = if (Test-Path $log) { @(Get-Content $log -Encoding UTF8) } else { @() }
		$ran = [bool]($lines -match 'eval RESULT=PASS script=bakedload\.eval')
		$loadDetail = 'bakedload.eval ran clean'
		if (-not $ran) { $loadDetail = "bakedload.eval did not finish clean (exit $($p.ExitCode)): a crash, or the eval failed - see $log" }
	}
} finally {
	if ((Test-Path $hidden) -and -not (Test-Path $sidecar)) { Move-Item $hidden $sidecar }
	(Get-Item $runePng).LastWriteTimeUtc = $pngWas
	(Get-Item $model).LastWriteTimeUtc = $modelWas
}

# A game that ran and did not finish is a FAIL, not "nothing judged": the
# loader under test is what it was running. Nothing after it can be read.
Record 'load' $ran $loadDetail
if (-not $ran) { Exit-WithVerdict }

# ---------------------------------------------------------------------------
# What the loaders said
# ---------------------------------------------------------------------------
$warns = @($lines | Where-Object { $_ -match '^\[warn ' })
$runeRx = [regex]::Escape("$runeName.dds") + ' is older than its PNG'
$modelRx = [regex]::Escape($modelName) + ': (\d+) of its (\d+) embedded images decoded at load - (\d+) with no baked sidecar, (\d+) older than the model'

# Nothing else: another stale or missing bake, or one the readers rejected.
$others = @($warns | Where-Object {
	($_ -match 'is older than its PNG' -and $_ -notmatch $runeRx) -or
	($_ -match 'embedded images decoded at load' -and $_ -notmatch $modelRx) -or
	$_ -match ' - loading the PNG instead' -or
	$_ -match ' - decoding the embedded image instead' })
$behind = @($others | Where-Object { Test-TreeBehind $_ })
$unexplained = @($others | Where-Object { -not (Test-TreeBehind $_) })

# Every other refusal borne out by the disk: the tree is behind, and the planted
# checks are no evidence of anything. Refused before they are read.
if ($behind.Count -and -not $unexplained.Count) {
	Write-Host ''
	Write-Host "baked: the tree is not current - the game refused $($behind.Count) other bake(s) that are missing or stale on disk too; nothing was judged:"
	$behind | Select-Object -First 5 | ForEach-Object { Write-Host "  $($_ -replace '^\[warn \] ', '')" }
	if ($behind.Count -gt 5) { Write-Host "  ... and $($behind.Count - 5) more" }
	Write-Host '  run AssetBaker model-images (a sidecar) or AssetBaker mips (a texture), then re-run'
	exit 2
}

$runeHits = @($warns | Where-Object { $_ -match $runeRx })
Record 'texture' ($runeHits.Count -ge 1) $(if ($runeHits.Count) { ($runeHits[0] -replace '^\[warn \] ', '') } else { "no '$runeName.dds is older than its PNG' line" })

# One line for the model however many of its images fell back, counting both.
$modelHits = @($warns | Where-Object { $_ -match $modelRx })
$detail = "$($modelHits.Count) line(s) for $modelName (want 1)"
$ok = $false
if ($modelHits.Count -eq 1 -and $modelHits[0] -match $modelRx) {
	$missing = [int]$Matches[3]; $stale = [int]$Matches[4]
	$ok = $missing -ge 1 -and $stale -ge 1
	$detail = "$($Matches[1]) of $($Matches[2]) decoded: $missing missing, $stale stale (want both >= 1)"
}
Record 'sidecar' $ok $detail

# A refusal the disk does not bear out is the loader's error; the tree's own
# (borne out, beside one that is not) are only counted.
$detail = if ($unexplained.Count) {
	"$($unexplained.Count) other the disk does not bear out: $($unexplained[0] -replace '^\[warn \] ', '')" +
		$(if ($behind.Count) { " (and $($behind.Count) the tree's own, behind on disk)" } else { '' })
} else { 'no other .dds or sidecar refused' }
Record 'current' ($unexplained.Count -eq 0) $detail

Exit-WithVerdict
