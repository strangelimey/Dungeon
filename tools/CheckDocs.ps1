# ============================================================================
# tools\CheckDocs.ps1 - the /check-* commands say what the harnesses really run.
#
#   .\tools\CheckDocs.ps1              # compare; exit 0 = every block is current
#   .\tools\CheckDocs.ps1 -Write       # rewrite the blocks from the live lists
#   .\tools\CheckDocs.ps1 -SelfTest    # a block one line short must be CAUGHT
#
# The .claude\commands\check*.md files are what a session reads before it runs
# a check, and they had drifted from the scripts (code-review C481): one listed
# six eval suites of thirteen, one said InGameTest swept four screens, one left
# two commands out of the family table, one named three checks with no
# self-test where there were more. Prose cannot be kept current by hand, so the
# LISTS in them are GENERATED - from `CheckAll.ps1 -List`, `Eval.ps1 -List` and
# each command file's own `description:` - between markers:
#
#   <!-- BEGIN generated: <source> ... -->
#   ...
#   <!-- END generated -->
#
# and this script compares them (CheckAll's `docs` check, quick tier) or
# rewrites them (-Write). Everything outside the markers is hand-written prose.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[switch]$Write,
	[switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$commands = Join-Path $root '.claude\commands'
$utf8 = New-Object System.Text.UTF8Encoding($false)

# --- the live lists ---------------------------------------------------------

# CheckAll's table rows: name, tier, self-testable, what.
function Get-CheckRows {
	$out = @(& (Join-Path $PSScriptRoot 'CheckAll.ps1') -List 6>&1 | ForEach-Object { "$_" })
	return @($out | Where-Object { $_ -match '^  \S' } | ForEach-Object {
		$m = [regex]::Match($_, '^  (\S+)\s+(\S+)\s+(self-testable|no self-test)\s+(.*)$')
		if ($m.Success) {
			[pscustomobject]@{ Name = $m.Groups[1].Value; Tier = $m.Groups[2].Value
				Self = ($m.Groups[3].Value -eq 'self-testable'); What = $m.Groups[4].Value.Trim(); Line = $_ }
		}
	})
}

# Eval.ps1's suites (the measurement suites; the fragments are left out).
function Get-EvalRows {
	$out = @(& (Join-Path $PSScriptRoot 'Eval.ps1') -List 6>&1 | ForEach-Object { "$_" })
	$rows = @()
	$in = $false
	foreach ($l in $out) {
		if ($l -match '^eval suites:') { $in = $true; continue }
		if ($in -and $l.Trim() -eq '') { break }
		if ($in) { $rows += $l }
	}
	return $rows
}

# Each command file's `description:` frontmatter line.
function Get-Family {
	return @(Get-ChildItem $commands -Filter 'check*.md' | Sort-Object Name | ForEach-Object {
		$text = [IO.File]::ReadAllText($_.FullName, $utf8)
		$m = [regex]::Match($text, '(?m)^description:\s*(.+?)\s*$')
		$cmd = '/' + [IO.Path]::GetFileNameWithoutExtension($_.Name)
		"| ``$cmd`` | $(if ($m.Success) { $m.Groups[1].Value } else { '(no description)' }) |"
	})
}

$checks = Get-CheckRows
$blocks = @{
	# check.md: the whole table, and the family of commands.
	'checkall-list' = @('```text') + @($checks | ForEach-Object { $_.Line }) + @('```')
	'family'        = @('| command | covers |', '|---|---|') + (Get-Family)
	# check-full.md: what -Full adds to the quick tier.
	'checkall-full' = @('```text') + @($checks | Where-Object { $_.Tier -eq 'full' } | ForEach-Object { $_.Line }) + @('```')
	# check-selftest.md: what -SelfTest names and skips (build rows run as the
	# preparation, so they are not in it).
	'checkall-noself' = @('```text') + @($checks | Where-Object { -not $_.Self -and $_.Name -notlike 'build-*' } |
		ForEach-Object { $_.Line }) + @('```')
	# check-eval.md: the suites.
	'eval-suites'   = @('```text') + (Get-EvalRows) + @('```')
}

# --- the blocks in the files ------------------------------------------------

$begin = '<!-- BEGIN generated: (\S+)[^>]*-->'
$end = '<!-- END generated -->'

# Every generated block in every check*.md: file, source, inner lines, and the
# text either side, so -Write can splice.
function Get-FileBlocks {
	$found = @()
	foreach ($f in Get-ChildItem $commands -Filter 'check*.md' | Sort-Object Name) {
		$text = [IO.File]::ReadAllText($f.FullName, $utf8)
		$nl = if ($text -match "`r`n") { "`r`n" } else { "`n" }
		# Groups: 1 the BEGIN line, 2 its source, 3 the block, 4 the END line. The
		# region -Write replaces runs from the block's start to the END line, so
		# an empty block (BEGIN and END on consecutive lines) splices correctly.
		foreach ($m in [regex]::Matches($text, "(?s)($begin)\r?\n(.*?)\r?\n?($([regex]::Escape($end)))")) {
			$inner = $m.Groups[3].Value
			$found += [pscustomobject]@{
				File = $f.FullName; Name = $f.Name; Source = $m.Groups[2].Value
				Inner = $(if ($inner -eq '') { @() } else { @($inner -split "\r?\n") })
				Start = $m.Groups[3].Index; Length = $m.Groups[4].Index - $m.Groups[3].Index
				NewLine = $nl
			}
		}
	}
	return $found
}

# The lines that differ, or nothing.
function Compare-Block([string[]]$want, [string[]]$have) {
	$w = @($want | ForEach-Object { $_.TrimEnd() })
	$h = @($have | ForEach-Object { $_.TrimEnd() })
	$diff = @()
	for ($i = 0; $i -lt [Math]::Max($w.Count, $h.Count); $i++) {
		$a = if ($i -lt $w.Count) { $w[$i] } else { '(missing)' }
		$b = if ($i -lt $h.Count) { $h[$i] } else { '(missing)' }
		if ($a -cne $b) { $diff += "line $($i + 1): want '$a' / has '$b'" }
	}
	return ,$diff
}

$fileBlocks = Get-FileBlocks
$unknown = @($fileBlocks | Where-Object { -not $blocks.ContainsKey($_.Source) })
foreach ($u in $unknown) { Write-Host "  $($u.Name): unknown generated block '$($u.Source)'" -ForegroundColor Red }
# Every source must appear somewhere, or a deleted block would pass silently.
$absent = @($blocks.Keys | Where-Object { $k = $_; -not ($fileBlocks | Where-Object { $_.Source -eq $k }) })
foreach ($a in $absent) { Write-Host "  no check*.md carries the generated block '$a'" -ForegroundColor Red }

if ($Write) {
	# Splice from the END of each file backwards so the offsets stay true.
	foreach ($g in ($fileBlocks | Where-Object { $blocks.ContainsKey($_.Source) } | Group-Object File)) {
		$text = [IO.File]::ReadAllText($g.Name, $utf8)
		foreach ($b in ($g.Group | Sort-Object Start -Descending)) {
			$text = $text.Substring(0, $b.Start) + ($blocks[$b.Source] -join $b.NewLine) + $b.NewLine +
				$text.Substring($b.Start + $b.Length)
		}
		[IO.File]::WriteAllText($g.Name, $text, $utf8)
		Write-Host "  wrote $([IO.Path]::GetFileName($g.Name))"
	}
	exit $(if ($unknown.Count -or $absent.Count) { 1 } else { 0 })
}

$drifted = 0
$caught = 0
foreach ($b in $fileBlocks | Where-Object { $blocks.ContainsKey($_.Source) }) {
	$want = $blocks[$b.Source]
	if ($SelfTest) {
		# The block one line short of the truth must be reported - a compare that
		# cannot see a dropped row is how these files drifted in the first place.
		$short = @($want | Select-Object -First ([Math]::Max(0, $want.Count - 1)))
		if ((Compare-Block $want $short).Count -gt 0) { $caught++ }
		else { Write-Host "  self-test: a short '$($b.Source)' in $($b.Name) was NOT caught" -ForegroundColor Red }
		continue
	}
	$diff = Compare-Block $want $b.Inner
	if ($diff.Count) {
		$drifted++
		Write-Host "  $($b.Name) '$($b.Source)' has drifted:" -ForegroundColor Red
		$diff | Select-Object -First 6 | ForEach-Object { Write-Host "    $_" }
	} else {
		Write-Host "  $($b.Name) '$($b.Source)' is current"
	}
}

$problems = $drifted + $unknown.Count + $absent.Count
if ($SelfTest) {
	$n = @($fileBlocks | Where-Object { $blocks.ContainsKey($_.Source) }).Count
	$ok = ($caught -eq $n) -and $n -gt 0
	Write-Host ("checkdocs RESULT={0} blocks={1} caught={2} self_test=1" -f $(if ($ok) { 'FAIL' } else { 'PASS' }), $n, $caught)
	exit $(if ($ok) { 0 } else { 1 })
}
Write-Host ("checkdocs RESULT={0} blocks={1} drifted={2} self_test=0" -f $(if ($problems -eq 0) { 'PASS' } else { 'FAIL' }),
	@($fileBlocks).Count, $problems)
if ($problems) { Write-Host '  re-run with -Write to regenerate them (and read the diff before committing)' }
exit $(if ($problems -eq 0) { 0 } else { 1 })
