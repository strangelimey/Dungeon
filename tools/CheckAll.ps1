# ============================================================================
# tools\CheckAll.ps1 - the whole regression suite, one verdict.
#
# Run this from time to time to find out whether anything has DRIFTED. Not to
# find new bugs - each check below already knows what it is guarding - but to
# notice when something that used to hold has quietly stopped holding.
#
#   .\tools\CheckAll.ps1              # quick tier  (~4 min)
#   .\tools\CheckAll.ps1 -Full        # everything  (~20 min)
#   .\tools\CheckAll.ps1 -SelfTest    # every checker must FAIL (~15 min)
#   .\tools\CheckAll.ps1 -Only diag,health
#   .\tools\CheckAll.ps1 -List
#
# Exit code 0 = every check in the tier passed (or, under -SelfTest, every
# checker correctly reported failure).
#
# WHY THE SELF-TEST TIER EXISTS. Every check here can be run in a mode where it
# is GIVEN a failure and must report one. That is the only defence against the
# thing this suite is most likely to become: a green wall that has stopped
# looking. It is not hypothetical - ThreadStress computed all its pass/fail
# conditions, printed them as prose, and returned 0 unconditionally, so a whole
# phase had drifted into measuring an empty world (see its commit). A check
# nobody has watched fail is a check nobody should trust.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[switch]$Full,
	[switch]$SelfTest,
	[string[]]$Only = @(),
	[switch]$List,
	[ValidateSet('debug', 'release')][string]$Config = 'debug'
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"

# ---------------------------------------------------------------------------
# THE SUITE. `tier` is quick or full; `selfTest` is how to ask this check to
# fail on purpose (absent = it has no such mode, and is skipped under -SelfTest
# rather than counted as passing).
#
# Kept in ONE table so adding a check is one row, and so -List can print what
# the suite actually covers rather than what a comment claims it covers.
# ---------------------------------------------------------------------------
$checks = @(
	@{
		name = 'build-debug'; tier = 'quick'
		what = 'the debug build compiles clean'
		run  = { & cmd /c ".\build.cmd debug > `"$env:TEMP\checkall-build-debug.txt`" 2>&1"; $LASTEXITCODE }
	},
	@{
		name = 'build-release'; tier = 'full'
		what = 'the release build compiles clean (the config that rots unwatched)'
		run  = { & cmd /c ".\build.cmd release > `"$env:TEMP\checkall-build-release.txt`" 2>&1"; $LASTEXITCODE }
	},
	@{
		name = 'diag'; tier = 'quick'
		what = 'the health record: ring, wrap, cross-thread writes, torn reads'
		# Streams merged by CMD, not by PowerShell. These tools log warnings to
		# stderr, and without merging they arrive unbuffered AFTER the suite
		# summary, reading like a late failure. But PowerShell's own `2>&1` wraps
		# every stderr line in a four-line NativeCommandError banner, which is far
		# worse than the ordering it fixes. cmd merges before PowerShell ever sees
		# the stream, so the output is both ordered and clean.
		run  = { & cmd /c "`"$(Join-Path $bin 'DiagTest.exe')`" 2>&1" | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'threads'; tier = 'full'
		what = 'ThreadManager + AI buckets under load: no force-terminate, clean reboots'
		run      = { & cmd /c "`"$(Join-Path $bin 'ThreadStress.exe')`" 2>&1" | Out-Host; $LASTEXITCODE }
		selfTest = { & cmd /c "`"$(Join-Path $bin 'ThreadStress.exe')`" --self-test 2>&1" | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'ingame'; tier = 'quick'
		what = 'level files + installed models, and a uioverlap sweep of every screen'
		run      = { & (Join-Path $root 'tools\InGameTest.ps1') -Config $Config | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\InGameTest.ps1') -Config $Config -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'pipeline'; tier = 'quick'
		what = 'every source of damage goes through fx::Deal; nothing else writes health'
		# QUICK despite driving the whole game: 16 seconds, because the eval
		# harness recycles the world with `reset` instead of reloading it. It is
		# also the check most worth running often - the rule it guards is one a
		# perfectly reasonable new feature breaks by accident, which is exactly
		# how the resource-practice growth route arrived unaccounted for.
		run      = { & (Join-Path $root 'tools\PipelineTest.ps1') -Config $Config | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\PipelineTest.ps1') -Config $Config -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'alloc'; tier = 'full'
		what = 'a steady-state frame allocates nothing on the heap'
		run      = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -Seconds 10 | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -Seconds 10 -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'health'; tier = 'full'
		what = 'crashes, faults and stalls are caught, recorded and explained'
		run      = { & (Join-Path $root 'tools\HealthTest.ps1') -Config $Config | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\HealthTest.ps1') -Config $Config -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'build-profile'; tier = 'full'
		what = 'the release-profile build compiles clean (DN_PROFILE rots unwatched too)'
		run  = { & cmd /c ".\build.cmd release-profile > `"$env:TEMP\checkall-build-profile.txt`" 2>&1"; $LASTEXITCODE }
	},
	@{
		name = 'profile'; tier = 'full'
		what = 'the frame budget still adds up, and the verdict still reacts to load'
		# release-profile on purpose, and NOT $Config: the budget only exists with
		# DN_PROFILE, and debug's D3D12 debug layer inflates command recording
		# enough to change which term dominates. Depends on build-profile above.
		run      = { & (Join-Path $root 'tools\ProfileTest.ps1') -Config release-profile | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\ProfileTest.ps1') -Config release-profile -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'bc7'; tier = 'full'
		what = 'the BC7 encoder error estimate against an independent decoder'
		# Release on purpose: the debug encoder is too slow to be worth the wait,
		# and its own script defaults the same way.
		run      = { & (Join-Path $root 'tools\Bc7Test.ps1') -Config release | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\Bc7Test.ps1') -Config release -SelfTest | Out-Host; $LASTEXITCODE }
	}
)

if ($List) {
	Write-Host 'checks in this suite:'
	foreach ($c in $checks) {
		$st = if ($c.selfTest) { 'self-testable' } else { 'no self-test' }
		Write-Host ("  {0,-14} {1,-6} {2,-13} {3}" -f $c.name, $c.tier, $st, $c.what)
	}
	exit 0
}

# Selection: -Only wins, then the tier.
$selected = if ($Only.Count -gt 0) {
	$hit = @($checks | Where-Object { $Only -contains $_.name })
	$unknown = @($Only | Where-Object { $n = $_; -not ($checks | Where-Object { $_.name -eq $n }) })
	if ($unknown) { throw "unknown check(s): $($unknown -join ', ') - try -List" }
	$hit
} elseif ($Full) { $checks } else { @($checks | Where-Object { $_.tier -eq 'quick' }) }

if ($SelfTest) {
	# A check with no self-test mode is NAMED and skipped, never counted as a
	# pass. Silently dropping it would make the self-test tier claim a coverage
	# it does not have, which is the exact failure the tier exists to prevent.
	$noSelf = @($selected | Where-Object { -not $_.selfTest })
	$selected = @($selected | Where-Object { $_.selfTest })
	foreach ($c in $noSelf) { Write-Host "  (skipped in -SelfTest: $($c.name) has no fail-on-purpose mode)" -ForegroundColor Yellow }
}

Push-Location $root
$results = @()
$startAll = Get-Date
foreach ($c in $selected) {
	Write-Host ''
	Write-Host ('=' * 78)
	Write-Host "$($c.name) - $($c.what)"
	Write-Host ('=' * 78)
	$t0 = Get-Date
	$code = 1
	try {
		$code = if ($SelfTest) { & $c.selfTest } else { & $c.run }
		# A script that returns extra pipeline output alongside its exit code
		# would otherwise be read as an array; take the last value.
		if ($code -is [array]) { $code = $code[-1] }
	} catch {
		Write-Host "  harness error: $($_.Exception.Message)" -ForegroundColor Red
		$code = 1
	}
	$secs = [math]::Round(((Get-Date) - $t0).TotalSeconds)
	# Each check's own script already inverts its verdict under -SelfTest, so a
	# zero here means "behaved as asked" in both modes.
	$results += [pscustomobject]@{ Name = $c.name; Ok = ($code -eq 0); Seconds = $secs }
}
Pop-Location

$totalSecs = [math]::Round(((Get-Date) - $startAll).TotalSeconds)
$failed = @($results | Where-Object { -not $_.Ok })

Write-Host ''
Write-Host ('=' * 78)
foreach ($r in $results) {
	$mark = if ($r.Ok) { 'PASS' } else { 'FAIL' }
	$col = if ($r.Ok) { 'Green' } else { 'Red' }
	Write-Host ("  {0,-14} {1}  {2,4}s" -f $r.Name, $mark, $r.Seconds) -ForegroundColor $col
}
$tier = if ($Only.Count -gt 0) { 'selected' } elseif ($Full) { 'full' } else { 'quick' }
Write-Host ''
Write-Host ("checkall RESULT={0} tier={1} checks={2} failures={3} seconds={4} self_test={5}" -f `
	$(if ($failed.Count -eq 0) { 'PASS' } else { 'FAIL' }), $tier, $results.Count,
	$failed.Count, $totalSecs, [int]$SelfTest.IsPresent)
if ($failed.Count -eq 0) {
	Write-Host 'nothing has drifted' -ForegroundColor Green
} else {
	Write-Host "drifted: $(($failed | ForEach-Object { $_.Name }) -join ', ')" -ForegroundColor Red
}
exit ([int]($failed.Count -ne 0))
