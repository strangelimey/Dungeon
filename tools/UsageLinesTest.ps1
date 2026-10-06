# ============================================================================
# tools\UsageLinesTest.ps1 - the fetch scripts' usage lines do what they say.
#
#   .\tools\UsageLinesTest.ps1              # dry-run every usage line
#   .\tools\UsageLinesTest.ps1 -SelfTest    # planted faults must be CAUGHT
#
# Exit code 0 = PASS (or, under -SelfTest, every planted fault caught); 1 =
# FAIL; 2 = SKIP, the OneDrive asset archive the scripts read is not on this
# machine (the static rule still runs, and still FAILs). One machine-readable
# verdict line.
#
# WHY (code-review C482): the usage lines in FetchTextures.ps1, FetchModels.ps1
# and SortTextureDownloads.ps1 passed comma lists through `powershell -File`,
# which hands `a,b,c` to a [string[]] parameter as ONE element - so the line a
# reader copies imports nothing, and FetchModels' example also named models its
# table did not have. A usage line is code that is never run, so this runs it.
#
# What is checked: every comment line of every tools\*.ps1 that INVOKES a
# script through powershell (or pwsh) - `-File` followed by a .ps1 path, or
# `-Command` followed by a quote, `&`, `{` or a .ps1 path. Prose that only
# names the switches is not an invocation. Any indent before the '#'.
#   1. STATIC: a -File invocation may not pass a comma list (the trap itself),
#      and a -Command invocation must be in the one form rule 2 can run:
#        #   powershell -Command "& { .\tools\<Script>.ps1 <args> }"
#      with an optional `Word:` label after the '#' and an optional trailing
#      `# note`. A -Command line in any other shape is a FAIL, not a skip: a
#      line the judge cannot read is a line it does not check.
#   2. DRY RUN: every such -Command line runs AS WRITTEN from the repo root,
#      with -WhatIf added inside the braces (both fetch scripts take it:
#      ShouldProcess, nothing converted or baked), and must exit 0 having
#      selected something - and every name its -Materials list gives:
#      <name>_<res> for each of its -Resolutions (FetchTextures; 1k,2k,4k when
#      absent), <name> for FetchModels. A comma list after any parameter but
#      -Materials / -Resolutions is a FAIL (this judge would not read it), and
#      a -Command line for a script with no rule below is a FAIL, so a new one
#      cannot go unchecked.
#   3. PALETTES: a FetchTextures line with no -Materials and no -All - the
#      default fetch - must select every texture set a level PALETTE draws
#      (each world's levels\*.map `palette` ids, through its walls / floors /
#      ceilings.cat `texture`), at one of the line's resolutions at least.
#      The default list once came from a level format nothing read any more,
#      and a fresh clone drew 10 of the crypts' 12 surfaces magenta while the
#      fetch reported success (code-review C402). The same failure would pass
#      this rule if its own reading came back empty, so the READING is judged
#      first, archive or not: no level found, a map without all three
#      `palette` records (the game asserts on one, so the format moved under
#      this reader) or no set at all is a FAIL, and so is a run in which rule 3
#      judged no line. The verdict says how many sets it read (palettes=N) and
#      how many lines it judged (judged=N).
#
# -SELFTEST checks the checker. Every dry-run line is first run AS WRITTEN and
# must pass - the CONTROL, so a plant that fails for some unrelated reason (the
# archive moved, a script throwing at startup, a timeout) cannot count as
# caught. Then, on each line whose control passed:
#   - COMMA: a line passing a comma list is run in the -File form, which must
#     fail the way the trap fails (nothing selected, "Nothing imported", or a
#     name not selected), and rule 1 must flag that -File text;
#   - NAME: a line with a -Materials list gets a name the archive does not have
#     added to the list, and must fail as "... but not: <that name>" - the
#     every-name check, which no passing control exercises;
#   - PALETTE: a default FetchTextures line is judged against the reading of a
#     scratch copy of a real level whose `palette wall` line names one more id,
#     which its walls.cat copy resolves through `texture` to a set nothing
#     fetches - so the plant goes through the reader, not around it - and must
#     fail as "... but not: <that set>" and nothing else.
# Two GUARD plants for rule 3's reading, on the same scratch copy: every
# `palette` record renamed (FORMAT) and the levels folder renamed (MOVED) must
# each be refused as unfit to judge by.
# And four SHAPE plants for rule 1: an indented, labelled -File list and two
# -Command lines it cannot parse must be flagged, and a -Command line with a
# trailing `# note` must be PARSED (dry-run), not skipped.
# With nothing to plant in, the self-test fails - it would prove nothing.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$oneDrive = if ($env:OneDrive) { $env:OneDrive } else { Join-Path $env:USERPROFILE 'OneDrive' }
$archive = Join-Path $oneDrive 'DungeonAssets'

# The scripts a usage line may dry-run, and what "selected <name>" means for each:
# the ShouldProcess TARGETS it must print for one -Materials name.
$rules = @{
	'FetchTextures' = {
		param([string]$name, [string[]]$res)
		$r = if ($res.Count) { $res } else { @('1k', '2k', '4k') }
		@($r | ForEach-Object { "$($name)_$_" })
	}
	'FetchModels' = { param([string]$name, [string[]]$res) @($name) }
}
# The parameters whose comma lists the dry run reads.
$listParams = @('-Materials', '-Resolutions')

# Read-Blocks (the catalog block format), shared with the fetch scripts.
. (Join-Path $PSScriptRoot 'Pipeline.ps1')

# Rule 3's READING: every texture set a level palette draws, in every world
# under $projectsDir - a `palette <surface> <id> ...` record of a
# <world>\levels\*.map, each id resolved through the world's <surface>s.cat
# `texture` (absent = the id, as the game's ModelAndTexture does). Also how many
# maps it read, and each map lacking a surface's record.
function Read-Palettes([string]$projectsDir) {
	$sets = @{}
	$maps = 0
	$bare = @()
	foreach ($world in Get-ChildItem $projectsDir -Directory -ErrorAction SilentlyContinue) {
		$textureOf = @{}
		foreach ($surface in 'wall', 'floor', 'ceiling') {
			foreach ($b in Read-Blocks (Join-Path $world.FullName "catalog\$($surface)s.cat")) {
				$textureOf["$surface/$($b.id)"] = if ($b.Contains('texture')) { $b['texture'] } else { $b.id }
			}
		}
		foreach ($map in Get-ChildItem (Join-Path $world.FullName 'levels') -Filter '*.map' -ErrorAction SilentlyContinue) {
			$maps++
			$declared = @{}
			foreach ($line in [IO.File]::ReadAllLines($map.FullName)) {
				$t = @(-split $line)
				if ($t.Count -lt 3 -or $t[0] -ne 'palette') { continue }
				$declared[$t[1]] = $true
				foreach ($id in $t[2..($t.Count - 1)]) {
					$key = "$($t[1])/$id"
					$set = if ($textureOf.ContainsKey($key)) { $textureOf[$key] } else { $id }
					$sets[$set] = $true
				}
			}
			$lack = @('wall', 'floor', 'ceiling' | Where-Object { -not $declared.ContainsKey($_) })
			if ($lack.Count) { $bare += "$($world.Name)\levels\$($map.Name) (no $($lack -join ', '))" }
		}
	}
	return [pscustomobject]@{ Sets = @($sets.Keys | Sort-Object); Maps = $maps; Bare = $bare }
}

# Why a reading is unfit to judge by - each one a FAIL, since rule 3 passes
# whatever it does not read: no level found (levels moved out of
# <world>\levels, or the projects folder did), a map lacking a surface's
# `palette` record (DungeonMap asserts on that, so the format moved under this
# reader), or no set at all.
function Get-PaletteProblems($reading, [string]$where) {
	$p = @()
	if ($reading.Maps -eq 0) { $p += "no level .map under $where\<world>\levels - rule 3 would judge nothing" }
	foreach ($b in $reading.Bare) {
		$p += "$b reads with no 'palette <surface>' record - the game asserts on that, so the format moved under this reader"
	}
	if ($reading.Maps -gt 0 -and $reading.Sets.Count -eq 0) { $p += "the level palettes under $where name no texture set" }
	return $p
}

# The self-test's palette plants: a scratch copy, under <scratch>\<kind>\projects,
# of one real level (the first map, by world and name, with a `palette wall`
# line) and its world's three surface catalogs, mutated:
#   extra  - one more wall id on that real `palette wall` line, which the
#            walls.cat copy resolves through `texture` to a set nothing fetches;
#   format - every `palette` record renamed (a level-format change);
#   moved  - the levels folder renamed (levels moved out of <world>\levels).
# Returns that projects folder, or $null with no real level to copy.
function New-PalettePlant([string]$kind, [string]$scratch) {
	foreach ($world in Get-ChildItem (Join-Path $root 'assets\projects') -Directory | Sort-Object Name) {
		$levels = Join-Path $world.FullName 'levels'
		foreach ($map in Get-ChildItem $levels -Filter '*.map' -ErrorAction SilentlyContinue | Sort-Object Name) {
			$lines = @([IO.File]::ReadAllLines($map.FullName))
			$at = -1
			for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*palette\s+wall\s') { $at = $i; break } }
			if ($at -lt 0) { continue }
			$dir = Join-Path $scratch "$kind\projects\$($world.Name)"
			New-Item -ItemType Directory -Force (Join-Path $dir 'catalog'), (Join-Path $dir 'levels') | Out-Null
			foreach ($cat in 'walls.cat', 'floors.cat', 'ceilings.cat') {
				$src = Join-Path $world.FullName "catalog\$cat"
				if (Test-Path $src) { Copy-Item $src (Join-Path $dir "catalog\$cat") }
			}
			if ($kind -eq 'extra') {
				$lines[$at] += ' usagelines_planted_wall'
				[IO.File]::AppendAllText((Join-Path $dir 'catalog\walls.cat'),
					"`r`n[usagelines_planted_wall]`r`ntexture = usagelines_planted_palette`r`n")
			} elseif ($kind -eq 'format') {
				$lines = @($lines | ForEach-Object { $_ -replace '^(\s*)palette\b', '$1surfaces' })
			}
			[IO.File]::WriteAllLines((Join-Path $dir "levels\$($map.Name)"), [string[]]$lines)
			if ($kind -eq 'moved') { Rename-Item (Join-Path $dir 'levels') 'maps' }
			return (Join-Path $scratch "$kind\projects")
		}
	}
	return $null
}

# The sets rule 3 judges by (the real reading; the PALETTE plant swaps in its
# own) and how many lines it judged.
$script:paletteSets = @()
$script:paletteJudged = 0

# A comment line that INVOKES something through powershell: which switch, and
# everything after it.
$invocation = '^\s*#.*?\b(?:powershell|pwsh)(?:\.exe)?\b.*?\s-(?<form>File|Command)\s+(?<rest>.*)$'
# The one -Command form the dry run reads.
$commandLine = '^\s*#\s+(?:\w+:\s+)?powershell(?:\.exe)?\s+-Command\s+"&\s*\{\s*\.\\tools\\(?<script>\w+)\.ps1(?<args>[^}"]*)\}"(?:\s+#.*)?\s*$'
$ic = [System.Text.RegularExpressions.RegexOptions]::IgnoreCase

# --- rule 1: what is wrong with one line's shape -------------------------------
# $null (fine, or not an invocation), 'trap' (a -File comma list) or 'unparsed'
# (a -Command invocation the dry run cannot read).
function Get-ShapeProblem([string]$line) {
	$m = [regex]::Match($line, $invocation, $ic)
	if (-not $m.Success) { return $null }
	$rest = $m.Groups['rest'].Value
	if ($m.Groups['form'].Value -eq 'File') {
		if ($rest -notmatch '^["'']?[^"''\s]*\.ps1\b') { return $null } # prose about -File
		if ($rest -match '\w,\w') { return 'trap' }
		return $null
	}
	if ($rest -notmatch '^(["''&{]|[^"''\s]*\.ps1\b)') { return $null } # prose about -Command
	if (-not [regex]::IsMatch($line, $commandLine, $ic)) { return 'unparsed' }
	return $null
}

# A dry-runnable usage line, or $null.
function Get-UsageLine([string]$file, [string]$line) {
	$m = [regex]::Match($line, $commandLine, $ic)
	if (-not $m.Success) { return $null }
	return [pscustomobject]@{ File = $file; Line = $line.Trim()
		Script = $m.Groups['script'].Value; Args = $m.Groups['args'].Value.Trim() }
}

# --- rule 2: the dry run -------------------------------------------------------

# A line's argument tokens (the lines are simple: no quoted spaces in a list).
function Get-ListArg([string]$argText, [string]$param) {
	$tokens = @(-split $argText)
	for ($i = 0; $i -lt $tokens.Count - 1; $i++) {
		if ($tokens[$i] -eq $param) { return @($tokens[$i + 1] -split ',' | Where-Object { $_ }) }
	}
	return @()
}

# Runs powershell.exe with exactly `argString` from the repo root; the exit code
# and everything it printed. The dry runs take seconds; 180 s is a hang.
function Invoke-PowerShell([string]$argString) {
	$psi = New-Object System.Diagnostics.ProcessStartInfo
	$psi.FileName = 'powershell.exe'
	$psi.Arguments = $argString
	$psi.WorkingDirectory = $root
	$psi.UseShellExecute = $false
	$psi.RedirectStandardOutput = $true
	$psi.RedirectStandardError = $true
	$psi.CreateNoWindow = $true
	$p = [System.Diagnostics.Process]::Start($psi)
	$err = $p.StandardError.ReadToEndAsync()
	$out = $p.StandardOutput.ReadToEndAsync()
	if (-not $p.WaitForExit(180000)) {
		$p.Kill()
		return [pscustomobject]@{ Code = -1; Out = 'timed out after 180 s' }
	}
	$p.WaitForExit()
	return [pscustomobject]@{ Code = $p.ExitCode; Out = $out.Result + "`n" + $err.Result }
}

# One dry run of `.\tools\<script>.ps1 <argText>`, in the -Command form the
# usage lines use or (a self-test plant) the -File form, judged. Returns $null
# when it selected everything it names, else why not.
function Test-DryRun([string]$scriptName, [string]$argText, [bool]$fileForm) {
	$argText = $argText.Trim()
	# A list this judge would not read is a name it would not check.
	$tokens = @(-split $argText)
	for ($i = 0; $i -lt $tokens.Count; $i++) {
		if ($tokens[$i] -match '\w,\w' -and ($i -eq 0 -or $listParams -notcontains $tokens[$i - 1])) {
			return "a comma list this judge does not read: $(if ($i) { $tokens[$i - 1] }) $($tokens[$i])"
		}
	}
	$invoke = if ($fileForm) { "-NoProfile -File .\tools\$($scriptName).ps1 $argText -WhatIf" }
			  else { "-NoProfile -Command `"& { .\tools\$($scriptName).ps1 $argText -WhatIf }`"" }
	$r = Invoke-PowerShell $invoke
	$targets = @([regex]::Matches($r.Out, 'What if: .*? on target "(?<t>[^"]+)"') |
		ForEach-Object { $_.Groups['t'].Value })
	$want = @()
	foreach ($n in (Get-ListArg $argText '-Materials')) {
		$want += & $rules[$scriptName] $n (Get-ListArg $argText '-Resolutions')
	}
	$missing = @($want | Where-Object { $targets -notcontains $_ })
	# Rule 3: the default fetch selects every set a palette draws.
	if ($scriptName -eq 'FetchTextures' -and -not @(Get-ListArg $argText '-Materials').Count -and
		$argText -notmatch '(^|\s)-All\b') {
		$script:paletteJudged++
		$res = @(Get-ListArg $argText '-Resolutions')
		if (-not $res.Count) { $res = @('1k', '2k', '4k') }
		foreach ($set in $script:paletteSets) {
			$at = '^' + [regex]::Escape($set) + '_(' + ($res -join '|') + ')$'
			if (-not @($targets | Where-Object { $_ -match $at }).Count) { $missing += $set }
		}
	}
	if ($r.Code -ne 0) {
		$tail = (@($r.Out -split "`r?`n" | Where-Object { $_.Trim() }) | Select-Object -Last 2) -join ' / '
		return "exit $($r.Code): $tail"
	}
	if ($targets.Count -eq 0) { return 'selected nothing' }
	if ($missing.Count) { return "selected $($targets.Count), but not: $($missing -join ', ')" }
	return $null
}

# --- the usage lines --------------------------------------------------------
$usage = @()
$shapeFails = @()
foreach ($f in Get-ChildItem (Join-Path $root 'tools') -Filter '*.ps1' | Sort-Object Name) {
	if ($f.Name -eq 'UsageLinesTest.ps1') { continue } # its own header quotes the forms
	foreach ($l in [IO.File]::ReadAllLines($f.FullName)) {
		$why = Get-ShapeProblem $l
		if ($why) { $shapeFails += [pscustomobject]@{ File = $f.Name; Line = $l.Trim(); Why = $why } }
		$u = Get-UsageLine $f.Name $l
		if ($u) { $usage += $u }
	}
}

$failures = 0
foreach ($s in $shapeFails) {
	$what = if ($s.Why -eq 'trap') { 'a -File usage line passes a comma list' }
			else { 'a -Command usage line not in the checked form (powershell -Command "& { .\tools\X.ps1 ... }")' }
	Write-Host "  FAIL  $what - $($s.File): $($s.Line)" -ForegroundColor Red
	$failures++
}
# The three scripts C482 is about must each carry a -Command usage line, or a
# deleted block would pass silently.
foreach ($s in 'FetchTextures.ps1', 'FetchModels.ps1', 'SortTextureDownloads.ps1') {
	if (-not ($usage | Where-Object { $_.File -eq $s })) {
		Write-Host "  FAIL  $s carries no -Command usage line" -ForegroundColor Red
		$failures++
	}
}
foreach ($u in $usage | Where-Object { -not $rules.ContainsKey($_.Script) }) {
	Write-Host "  FAIL  $($u.File): no dry-run rule for $($u.Script).ps1 - add one to `$rules: $($u.Line)" -ForegroundColor Red
	$failures++
}
$runnable = @($usage | Where-Object { $rules.ContainsKey($_.Script) })

# Rule 3's reading, judged before anything is run by it (no archive needed).
$projects = Join-Path $root 'assets\projects'
$reading = Read-Palettes $projects
foreach ($p in @(Get-PaletteProblems $reading $projects)) {
	Write-Host "  FAIL  palette reading: $p" -ForegroundColor Red
	$failures++
}
$script:paletteSets = @($reading.Sets)
Write-Host "  palette reading: $($reading.Sets.Count) sets from $($reading.Maps) level maps"

if (-not (Test-Path $archive)) {
	Write-Host "  the asset archive is not here ($archive) - the scripts select from it" -ForegroundColor Yellow
	$verdict = if ($failures) { 'FAIL' } else { 'SKIP' }
	Write-Host ("usagelines RESULT={0} lines={1} palettes={2} failures={3} self_test={4}" -f $verdict, $usage.Count,
		$reading.Sets.Count, $failures, [int]$SelfTest.IsPresent)
	exit $(if ($failures) { 1 } else { 2 })
}

if ($SelfTest) {
	$planted = 0
	$caught = 0
	$commaPlants = 0
	$namePlants = 0
	$palettePlants = 0
	$controlsOk = $true
	function Write-Plant([bool]$ok, [string]$text) {
		if ($ok) { $script:caught++; Write-Host "  caught  $text" -ForegroundColor Green }
		else { Write-Host "  MISSED  $text" -ForegroundColor Red }
	}

	# SHAPE: rule 1 on lines no real file needs to contain.
	$shapePlants = @(
		@{ Want = 'trap'; Text = '    # Then:   powershell -File .\tools\FetchTextures.ps1 -Materials wall_stone,floor_cobble' }
		@{ Want = 'unparsed'; Text = '#   powershell -Command "& { .\tools\FetchModels.ps1 -Materials khukri }" one model' }
		@{ Want = 'unparsed'; Text = '#   powershell -Command ".\tools\FetchModels.ps1 -Materials khukri"' }
		@{ Want = 'parsed'; Text = '#   powershell -Command "& { .\tools\FetchModels.ps1 -Materials khukri }"   # one model' }
	)
	foreach ($p in $shapePlants) {
		$planted++
		$shape = Get-ShapeProblem $p.Text
		$got = if ($shape) { $shape } elseif (Get-UsageLine 'plant' $p.Text) { 'parsed' } else { 'ignored' }
		Write-Plant ($got -eq $p.Want) "shape: read as $got (want $($p.Want)): $($p.Text.Trim())"
	}

	# The palette plants' readings, each of a mutated scratch copy of a real
	# level (New-PalettePlant), read by the reader the real run uses; the copies
	# are gone before any dry run.
	$scratch = Join-Path ([IO.Path]::GetTempPath()) "usagelines-$PID"
	$plantReadings = @{}
	try {
		foreach ($kind in 'extra', 'format', 'moved') {
			$dir = New-PalettePlant $kind $scratch
			if ($dir) { $plantReadings[$kind] = Read-Palettes $dir }
		}
	} finally { Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue }
	# GUARD: a reading of a moved format or a moved folder must be refused.
	foreach ($kind in 'format', 'moved') {
		$planted++
		$probs = @(if ($plantReadings.ContainsKey($kind)) { Get-PaletteProblems $plantReadings[$kind] 'the plant' })
		Write-Plant ($probs.Count -gt 0) ("palette reading, $kind - " +
			$(if (-not $plantReadings.ContainsKey($kind)) { 'no real level to copy' }
			  elseif ($probs.Count) { "refused: $($probs[0])" } else { 'judged fit' }))
	}
	# The PALETTE plant's own reading must be fit, or its dry run proves nothing.
	$extra = $plantReadings['extra']
	$extraProbs = @(if ($extra) { Get-PaletteProblems $extra 'the plant' } else { 'no real level to copy' })
	if ($extraProbs.Count) {
		$controlsOk = $false
		Write-Host "  CONTROL FAILED  the PALETTE plant's reading: $($extraProbs -join '; ')" -ForegroundColor Red
	}

	$plantName = 'usagelines_planted_missing'
	foreach ($u in $runnable) {
		# The CONTROL: as written, it must pass, or nothing planted in it counts.
		$control = Test-DryRun $u.Script $u.Args $false
		if ($control) {
			$controlsOk = $false
			Write-Host "  CONTROL FAILED  $($u.File): '$($u.Line)' as written: $control" -ForegroundColor Red
			continue
		}
		Write-Host "  control ok  $($u.File): $($u.Line)"
		# COMMA: the -File form of a comma list.
		if ($u.Args -match '\w,\w') {
			$planted++
			$commaPlants++
			$why = Test-DryRun $u.Script $u.Args $true
			$trapLike = $why -and ($why -eq 'selected nothing' -or $why -match 'but not: ' -or
				$why -match 'Nothing imported')
			$fileText = "#   powershell -File .\tools\$($u.Script).ps1 $($u.Args)"
			$flagged = (Get-ShapeProblem $fileText) -eq 'trap'
			Write-Plant ($trapLike -and $flagged) ("$($u.File): the -File form - dry run: " +
				$(if ($why) { $why } else { 'selected everything' }) + "; rule 1 flagged it: $flagged")
		}
		# NAME: a name the archive lacks, added to the -Materials list.
		$mat = [regex]::Match($u.Args, '(?<=(^|\s)-Materials\s+)\S+', $ic)
		if ($mat.Success) {
			$planted++
			$namePlants++
			$cut = $mat.Index + $mat.Length
			$plantArgs = $u.Args.Substring(0, $cut) + ",$plantName" + $u.Args.Substring($cut)
			$why = Test-DryRun $u.Script $plantArgs $false
			$named = $why -match ('^selected \d+, but not: .*' + [regex]::Escape($plantName))
			Write-Plant $named ("$($u.File): -Materials ...,$plantName - dry run: " +
				$(if ($why) { $why } else { 'selected everything' }))
		}
		# PALETTE: a default fetch, judged by the plant's reading - a real level
		# whose wall palette names one more id, resolved through its catalog
		# copy to a set nothing fetches. Only that set may be missed: the rest
		# are the real level's, which the control selected.
		if ($u.Script -eq 'FetchTextures' -and $u.Args -notmatch '(^|\s)-(Materials|All)\b') {
			$planted++
			$palettePlants++
			$why = $null
			if ($extra) {
				$script:paletteSets = @($extra.Sets)
				try { $why = Test-DryRun $u.Script $u.Args $false } finally { $script:paletteSets = @($reading.Sets) }
			}
			$named = $why -match '^selected \d+, but not: usagelines_planted_palette$'
			Write-Plant $named ("$($u.File): a level palette drawing usagelines_planted_palette - dry run: " +
				$(if (-not $extra) { 'no plant' } elseif ($why) { $why } else { 'selected everything' }))
		}
	}
	$ok = $controlsOk -and $commaPlants -gt 0 -and $namePlants -gt 0 -and $palettePlants -gt 0 -and
		$caught -eq $planted -and $failures -eq 0
	Write-Host ''
	Write-Host ("usagelines RESULT={0} planted={1} caught={2} controls={3} palettes={4} self_test=1" -f
		$(if ($ok) { 'FAIL' } else { 'PASS' }), $planted, $caught, $(if ($controlsOk) { 'ok' } else { 'failed' }),
		$reading.Sets.Count)
	Write-Host $(if ($ok) { 'SELF-TEST PASSED - every planted fault was caught, and every control passed' }
				 else { 'SELF-TEST FAILED - a plant went unseen, a control failed, or the real lines are broken' })
	exit $(if ($ok) { 0 } else { 1 })
}

foreach ($u in $runnable) {
	$why = Test-DryRun $u.Script $u.Args $false
	if ($why) {
		Write-Host "  FAIL  $($u.File): $($u.Line)" -ForegroundColor Red
		Write-Host "        $why"
		$failures++
	} else {
		Write-Host "  ok    $($u.File): $($u.Line)"
	}
}
# Rule 3 is the C402 check: a run that judged no default fetch checked nothing.
if ($script:paletteJudged -eq 0) {
	Write-Host "  FAIL  rule 3 judged no line - no FetchTextures usage line without -Materials and -All" -ForegroundColor Red
	$failures++
}

Write-Host ''
Write-Host ("usagelines RESULT={0} lines={1} palettes={2} judged={3} failures={4} self_test=0" -f
	$(if ($failures -eq 0) { 'PASS' } else { 'FAIL' }), $usage.Count, $reading.Sets.Count, $script:paletteJudged, $failures)
exit $(if ($failures -eq 0) { 0 } else { 1 })
