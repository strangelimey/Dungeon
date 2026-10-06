# ============================================================================
# tools\CheckAll.ps1 - the whole regression suite, one verdict.
#
# Run this from time to time to find out whether anything has DRIFTED. Not to
# find new bugs - each check below already knows what it is guarding - but to
# notice when something that used to hold has quietly stopped holding.
#
#   .\tools\CheckAll.ps1              # quick tier  (a few minutes)
#   .\tools\CheckAll.ps1 -Full        # everything  (about an hour)
#   .\tools\CheckAll.ps1 -SelfTest    # every checker must FAIL
#   .\tools\CheckAll.ps1 -Only diag,health
#   .\tools\CheckAll.ps1 -List
#   .\tools\CheckAll.ps1 -Only alloc -Plan   # what would run, in order; runs nothing
#
# Exit code 0 = every check in the tier passed (or, under -SelfTest, every
# checker correctly reported failure).
#
# A CHECK RUNS ON THE EXE ITS BUILD ROW MAKES, so whatever is selected - a
# tier, -Only, a self-test - brings its build rows with it, FIRST (each check
# names one in `needs`). `-Only alloc` used to skip the build and judge
# yesterday's binary (code-review C426); and a harness run on its own now asks
# the build system and refuses a stale exe with exit 4 (tools\HarnessGame.ps1
# Assert-ExeCurrent), so the two cannot disagree about what was judged. Build
# rows run as themselves under -SelfTest too: they are preparation, not checks
# with a fail-on-purpose mode.
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
	[switch]$Plan,
	[ValidateSet('debug', 'release')][string]$Config = 'debug'
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"

# Muted ONCE for the whole suite; every harness below that runs this config
# finds its bin muted and leaves the volume to this one, and one that runs
# another config (ProfileTest on release-profile) mutes its own
# (tools\HarnessAudio.ps1).
. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not $List -and -not $Plan -and -not (Test-HarnessMuted $bin)) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }

# ---------------------------------------------------------------------------
# THE SUITE. `tier` is quick or full; `selfTest` is how to ask this check to
# fail on purpose (absent = it has no such mode, and is skipped under -SelfTest
# rather than counted as passing). `needs` names the build row that makes the
# exe a check runs, and `build` marks the build rows themselves.
#
# Kept in ONE table so adding a check is one row, and so -List can print what
# the suite actually covers rather than what a comment claims it covers. The
# /check-* command files carry that list too, GENERATED: after adding or
# changing a row, run tools\CheckDocs.ps1 -Write, or the `docs` check fails.
# ---------------------------------------------------------------------------
$checks = @(
	@{
		name = 'build-debug'; tier = 'quick'; build = $true
		what = 'the debug build compiles clean'
		run  = { & cmd /c ".\build.cmd debug > `"$env:TEMP\checkall-build-debug.txt`" 2>&1"; $LASTEXITCODE }
	},
	@{
		name = 'build-release'; tier = 'full'; build = $true
		what = 'the release build compiles clean (the config that rots unwatched)'
		run  = { & cmd /c ".\build.cmd release > `"$env:TEMP\checkall-build-release.txt`" 2>&1"; $LASTEXITCODE }
	},
	@{
		name = 'diag'; tier = 'quick'; needs = "build-$Config"
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
		name = 'rolls'; tier = 'quick'; needs = "build-$Config"
		what = 'the pure rules: dice, strike, armour, blasts, resources, ledger, carve, party'
		# RollTest links the real Game rules (never a copy) and runs in seconds,
		# so it belongs where it is run most (code-review C213). --self-test
		# injects a broken die and must name exactly the checks that fail.
		run      = { & cmd /c "`"$(Join-Path $bin 'RollTest.exe')`" 2>&1" | Out-Host; $LASTEXITCODE }
		selfTest = { & cmd /c "`"$(Join-Path $bin 'RollTest.exe')`" --self-test 2>&1" | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'docs'; tier = 'quick'
		what = 'the /check-* commands list what CheckAll -List and Eval -List print'
		# Generated blocks in .claude\commands\check*.md (tools\CheckDocs.ps1;
		# -Write regenerates). Needs no build: it reads two tables, runs nothing.
		run      = { & (Join-Path $root 'tools\CheckDocs.ps1') | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\CheckDocs.ps1') -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'threads'; tier = 'full'; needs = "build-$Config"
		what = 'ThreadManager + AI buckets under load: no force-terminate, clean reboots'
		run      = { & cmd /c "`"$(Join-Path $bin 'ThreadStress.exe')`" 2>&1" | Out-Host; $LASTEXITCODE }
		selfTest = { & cmd /c "`"$(Join-Path $bin 'ThreadStress.exe')`" --self-test 2>&1" | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'ingame'; tier = 'quick'; needs = "build-$Config"
		what = 'level files + installed models, and a uioverlap sweep of every screen'
		run      = { & (Join-Path $root 'tools\InGameTest.ps1') -Config $Config | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\InGameTest.ps1') -Config $Config -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'pipeline'; tier = 'quick'; needs = "build-$Config"
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
		name = 'spells'; tier = 'quick'; needs = 'build-debug'
		what = 'every spell tier does what it says: hand spells, bolts, modifiers, wards'
		# The debug build only (it reads build\debug), like LevelBuildTest. Its
		# self-test cuts every cast and demands exactly the spell-free checks pass.
		run      = { python (Join-Path $root 'tools\SpellTest.py') | Out-Host; $LASTEXITCODE }
		selfTest = { python (Join-Path $root 'tools\SpellTest.py') --selftest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'combat'; tier = 'quick'; needs = 'build-debug'
		what = 'the combat rules the code review fixed, judged from combat.eval'
		# The debug build only, like SpellTest. combat.eval's sections each make one
		# claim; the self-test cuts every effect / spawn / equip / wear line and
		# demands exactly the checks resting on one fail.
		run      = { python (Join-Path $root 'tools\CombatTest.py') | Out-Host; $LASTEXITCODE }
		selfTest = { python (Join-Path $root 'tools\CombatTest.py') --selftest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'alloc'; tier = 'full'; needs = "build-$Config"
		what = 'a steady-state frame allocates nothing on the heap'
		run      = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -Seconds 10 | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -Seconds 10 -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'alloc-hand'; tier = 'full'; needs = "build-$Config"
		what = 'the hand spells (light, douse, flare, fill, pebble) allocate nothing'
		run      = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -Hand | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -Hand -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'typing'; tier = 'full'; needs = "build-$Config"
		what = 'typed console text arrives whole and in order (focus loss, heavy frames)'
		# Every harness here drives the game by typing, so a dropped character
		# fails a run for a reason unrelated to what it measures.
		run      = { & (Join-Path $root 'tools\TypingTest.ps1') -Config $Config | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\TypingTest.ps1') -Config $Config -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'health'; tier = 'full'; needs = "build-$Config"
		what = 'crashes, faults and stalls are caught, recorded and explained'
		run      = { & (Join-Path $root 'tools\HealthTest.ps1') -Config $Config | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\HealthTest.ps1') -Config $Config -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'evalrunner'; tier = 'full'; needs = "build-$Config"
		what = 'the eval runner: reset = new game, batched = solo, headless = windowed, knobs move numbers'
		# Eval.ps1's SUITES measure and stay out of every tier (check-eval.md: a
		# green suite means it RAN). Its -SelfTest is a pass/fail check of the
		# RUNNER, and PipelineTest's quick PASS rests on that runner (C213). It
		# has no fail-on-purpose mode of its own: it IS one.
		run = { & (Join-Path $root 'tools\Eval.ps1') -Config $Config -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'editor'; tier = 'full'; needs = 'build-debug'
		what = 'the editor, phase by phase (EditorTest.py, each phase mutation-tested)'
		# The Python judges read build\debug, like SpellTest.
		run = { python (Join-Path $root 'tools\EditorTest.py') | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'world'; tier = 'full'; needs = 'build-debug'
		what = 'the world tier: saves, worlds, dungeons, the world map (WorldTest.py)'
		run = { python (Join-Path $root 'tools\WorldTest.py') | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'levelbuild'; tier = 'full'; needs = 'build-debug'
		what = 'the level generator, measured from the files it writes (LevelBuildTest.py)'
		run = { python (Join-Path $root 'tools\LevelBuildTest.py') | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'quit'; tier = 'full'; needs = "build-$Config"
		what = 'a load can always be quit: `quit` mid-load, Alt+F4 in Borderless'
		run      = { & (Join-Path $root 'tools\QuitTest.ps1') -Config $Config | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\QuitTest.ps1') -Config $Config -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'stale'; tier = 'full'; needs = 'build-debug'
		what = 'a harness refuses a stale exe, and CheckAll builds what it runs (StaleTest)'
		run = { & (Join-Path $root 'tools\StaleTest.ps1') | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'build-profile'; tier = 'full'; build = $true
		what = 'the release-profile build compiles clean (DN_PROFILE rots unwatched too)'
		run  = { & cmd /c ".\build.cmd release-profile > `"$env:TEMP\checkall-build-profile.txt`" 2>&1"; $LASTEXITCODE }
	},
	@{
		name = 'profile'; tier = 'full'; needs = 'build-profile'
		what = 'the frame budget still adds up, and the verdict still reacts to load'
		# release-profile on purpose, and NOT $Config: the budget only exists with
		# DN_PROFILE, and debug's D3D12 debug layer inflates command recording
		# enough to change which term dominates. Depends on build-profile above.
		run      = { & (Join-Path $root 'tools\ProfileTest.ps1') -Config release-profile | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\ProfileTest.ps1') -Config release-profile -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'bc7'; tier = 'full'; needs = 'build-release'
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
		Write-Host ("  {0,-14} {1,-6} {2,-14} {3}" -f $c.name, $c.tier, $st, $c.what)
	}
	exit 0
}

# Selection: -Only wins, then the tier. `-Only a,b` arrives as ONE string
# through `powershell -File` (it binds a,b to [string[]] as one element), so
# the names are split here rather than matching nothing.
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
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
	# A BUILD row is kept: it is not a check with a fail-on-purpose mode but the
	# preparation the self-tests run on.
	$noSelf = @($selected | Where-Object { -not $_.selfTest -and -not $_.build })
	$selected = @($selected | Where-Object { $_.selfTest -or $_.build })
	foreach ($c in $noSelf) { Write-Host "  (skipped in -SelfTest: $($c.name) has no fail-on-purpose mode)" -ForegroundColor Yellow }
}

# THE BUILDS THE SELECTION RUNS ON, first. A selected check's `needs` row joins
# at the front unless it is already selected (a tier lists its own, in order).
$needed = @($selected | ForEach-Object { $_.needs } | Where-Object { $_ } | Select-Object -Unique)
$missing = @($checks | Where-Object { $b = $_; $b.build -and ($needed -contains $b.name) -and
	-not @($selected | Where-Object { $_.name -eq $b.name }).Count })
$selected = @($missing) + @($selected)

if ($Plan) {
	Write-Host 'this run would make, in order:'
	foreach ($c in $selected) {
		$mode = if ($c.build) { 'build' } elseif ($SelfTest) { 'self-test' } else { 'check' }
		Write-Host ("  {0,-14} {1,-9} {2}" -f $c.name, $mode, $c.what)
	}
	exit 0
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
		$code = if ($SelfTest -and -not $c.build) { & $c.selfTest } else { & $c.run }
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
