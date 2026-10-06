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

# A build row's full output, named for THIS worktree's folder: several
# sessions run CheckAll at once, and one shared %TEMP% name made a second
# worktree's build fail in 0 s on "being used by another process".
function Get-BuildLog([string]$cfg) {
	Join-Path $env:TEMP "checkall-build-$cfg-$(Split-Path -Leaf $root).txt"
}

# Muted ONCE for the whole suite; every harness below that runs this config
# finds its bin muted and leaves the volume to this one, and one that runs
# another config (ProfileTest on release-profile) mutes its own
# (tools\HarnessAudio.ps1).
. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not $List -and -not $Plan -and -not (Test-HarnessMuted $bin)) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }

# THE NATIVE JUDGES (DiagTest, RollTest, ThreadStress) are read by their LAST
# LINE as well as their exit code: the shared `<tool> RESULT=.. checks=N
# failures=M .. self_test=N` (tools\Common\Verdict.h), checked against the exit
# code by tools\Verdict.ps1, so a run that lost its verdict line, counted
# nothing or contradicts itself fails here (code-review C425). Bc7Test.ps1 reads
# its own exe's line the same way.
#
# The verdict is the last line of STDOUT, so the two streams are kept APART.
# These tools log warnings to stderr, and some of it lands after the verdict:
# ThreadStress's manager joins - and may force-terminate - its workers as main
# returns, so merging the streams (cmd's 2>&1, as these rows once did) put a
# FORCE-TERMINATED warning where the verdict line should be. PowerShell's own
# 2>&1 separates them by type, and each stderr line is shown as plain text in
# arrival order - not as the four-line NativeCommandError banner its default
# formatting would print, and not saved up until after the suite summary.
. (Join-Path $PSScriptRoot 'Verdict.ps1')
function Invoke-NativeJudge([string]$exeName, [string]$tool, [switch]$Self) {
	$exe = Join-Path $bin $exeName
	$exeArgs = @()
	if ($Self) { $exeArgs += '--self-test' }
	$stdout = New-Object System.Collections.Generic.List[string]
	& $exe @exeArgs 2>&1 | ForEach-Object {
		if ($_ -is [System.Management.Automation.ErrorRecord]) { Write-Host "$_" }
		else { Write-Host $_; $stdout.Add("$_") }
	}
	$code = $LASTEXITCODE
	return (Confirm-Verdict $stdout.ToArray() $tool $code -SelfTest:$Self)
}

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
		run  = { & cmd /c ".\build.cmd debug > `"$(Get-BuildLog debug)`" 2>&1"; $LASTEXITCODE }
	},
	@{
		name = 'build-release'; tier = 'full'; build = $true
		what = 'the release build compiles clean (the config that rots unwatched)'
		run  = { & cmd /c ".\build.cmd release > `"$(Get-BuildLog release)`" 2>&1"; $LASTEXITCODE }
	},
	@{
		name = 'diag'; tier = 'quick'; needs = "build-$Config"
		what = 'the health record: ring, wrap, cross-thread writes, torn reads'
		# A native judge: stderr shown apart from stdout, and stdout's last line
		# read back as its verdict (Invoke-NativeJudge, above).
		run  = { Invoke-NativeJudge 'DiagTest.exe' 'diagtest' }
	},
	@{
		name = 'rolls'; tier = 'quick'; needs = "build-$Config"
		what = 'the pure rules: dice, strike, armour, blasts, resources, ledger, carve, party'
		# RollTest links the real Game rules (never a copy) and runs in seconds,
		# so it belongs where it is run most (code-review C213). --self-test
		# injects a broken die and must name exactly the checks that fail.
		run      = { Invoke-NativeJudge 'RollTest.exe' 'rolltest' }
		selfTest = { Invoke-NativeJudge 'RollTest.exe' 'rolltest' -Self }
	},
	@{
		name = 'anim'; tier = 'quick'; needs = "build-$Config"
		what = "the Animator keeps Play's promises: a held clip, mid-fade too, never restarts"
		# On a rig AnimTest builds itself (no assets), against a reference animator
		# that is never re-Played (code-review C395). --self-test feeds every no-op
		# case a Play that restarts by contract and must name exactly those.
		run      = { & cmd /c "`"$(Join-Path $bin 'AnimTest.exe')`" --contract 2>&1" | Out-Host; $LASTEXITCODE }
		selfTest = { & cmd /c "`"$(Join-Path $bin 'AnimTest.exe')`" --contract --self-test 2>&1" | Out-Host; $LASTEXITCODE }
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
		name = 'verdict'; tier = 'quick'
		what = 'the native judges'' last-line reader refuses every bad or contradictory line'
		# tools\VerdictTest.ps1 feeds Confirm-Verdict lines no judge prints today;
		# a reader decayed to trusting the exit code would leave every other row
		# green. Needs no build. Its self-test plants exactly that decay.
		run      = { & (Join-Path $root 'tools\VerdictTest.ps1') | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\VerdictTest.ps1') -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'lang'; tier = 'quick'
		what = 'every language key the code names is in every .lang file, holes matching'
		# Reads src\ and assets\lang and needs no build (code-review C107): a key
		# a translation lacks shows as the raw key and fails nothing at run time.
		# Its self-test plants a dropped key, an unknown key in a loc call and in
		# a table, a hole short and a key defined twice, and must report exactly
		# those - and a run over no source must be refused (exit 2), not passed.
		run      = { python (Join-Path $root 'tools\LangTest.py') | Out-Host; $LASTEXITCODE }
		selfTest = { python (Join-Path $root 'tools\LangTest.py') --selftest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'template'; tier = 'quick'
		what = 'the new-world template is what BuildTemplate.py makes of dungeon-demo: its picks, its items, byte for byte'
		# tools\TemplateTest.py (code-review C439) runs BuildTemplate.py into a
		# scratch folder - seconds, no build - and holds it to dungeon-demo's
		# inherited manifest fields, to items its catalogs define and to the
		# template on disk (not the git index: both sides are the working tree).
		# A FAIL after a dungeon-demo change means: re-run BuildTemplate.py, which
		# clears it, and commit the rebuilt template with the change. Its
		# self-test plants a fault per check group.
		run      = {
			$l = @(python (Join-Path $root 'tools\TemplateTest.py') | ForEach-Object { Write-Host $_; $_ })
			Confirm-Verdict $l 'templatetest' $LASTEXITCODE
		}
		selfTest = {
			$l = @(python (Join-Path $root 'tools\TemplateTest.py') --selftest | ForEach-Object { Write-Host $_; $_ })
			Confirm-Verdict $l 'templatetest' $LASTEXITCODE -SelfTest
		}
	},
	@{
		name = 'threads'; tier = 'full'; needs = "build-$Config"
		what = 'ThreadManager + AI buckets under load: no force-terminate, clean reboots'
		run      = { Invoke-NativeJudge 'ThreadStress.exe' 'threadstress' }
		selfTest = { Invoke-NativeJudge 'ThreadStress.exe' 'threadstress' -Self }
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
		# claim; the self-test cuts every line that sets one up (CombatTest.py's
		# CUT list) and demands exactly the checks resting on one fail.
		run      = { python (Join-Path $root 'tools\CombatTest.py') | Out-Host; $LASTEXITCODE }
		selfTest = { python (Join-Path $root 'tools\CombatTest.py') --selftest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'ai'; tier = 'quick'; needs = 'build-debug'
		what = 'monsters stand where they can (sides, kiters, pits); a new game or load forgets the last fight; a world switch leaves no dead AI worker; a resting chaser walks and thinks as an awake one, and a 60x shot skips neither the party nor a wall'
		# The debug build only, like SpellTest. Three scripts, three processes (the
		# async one needs a cold start). Its self-test cuts the scripts' steps and
		# play frames (no time passes) and demands exactly the time-free checks pass.
		run      = { python (Join-Path $root 'tools\AITest.py') | Out-Host; $LASTEXITCODE }
		selfTest = { python (Join-Path $root 'tools\AITest.py') --selftest | Out-Host; $LASTEXITCODE }
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
		name = 'alloc-rest'; tier = 'full'; needs = "build-$Config"
		what = 'resting, a monster behind a shut door: its inline searches allocate nothing'
		# Rest forces lockstep, so the AI's searches run on the main thread in
		# guarded frames (code-review C62); a failed search was a deque a frame.
		run      = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -Rest | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -Rest -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'alloc-rest-reach'; tier = 'full'; needs = "build-$Config"
		what = 'resting, a frozen monster with a way through: its inline paths allocate nothing'
		# -Rest's other half: a search that FINDS a path writes a plan's path,
		# which the inline compute's batches are sized for at level load.
		run      = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -RestReach | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -RestReach -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'alloc-lights'; tier = 'full'; needs = "build-$Config"
		what = '64 lights allocate nothing; a full light list, element floor glows; a door or a walking Firelight re-renders its shadow cube'
		# The shadow cache's checks (code-review C178 / C187), the candidate
		# ceiling (C181) and the floor glows (C190) ride -Lights. Its self-test
		# mutates the CACHE (-ShadowSelfTest), not the guard - the 'alloc' row
		# already hands the guard its failure.
		run      = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -Lights | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -Lights -ShadowSelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'alloc-items'; tier = 'full'; needs = "build-$Config"
		what = 'moving an item allocates nothing; no glow from a shut niche; every click target hits where it is drawn'
		# The item pose and the click picks (code-review C180 / C359 / C258,
		# itempose.eval) ride -Items, before its window. Its self-test is the
		# guard's (allocpoke), which skips those checks - they were mutation-
		# checked when they landed (batch 63).
		run      = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -Items | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\AllocTest.ps1') -Config $Config -Items -SelfTest | Out-Host; $LASTEXITCODE }
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
		what = 'the eval runner: reset = new game, batched = solo, headless = windowed, knobs move numbers; load paths leave a clean device on WARP'
		# Eval.ps1's SUITES measure and stay out of every tier (check-eval.md: a
		# green suite means it RAN). Its -SelfTest is a pass/fail check of the
		# RUNNER, and PipelineTest's quick PASS rests on that runner (C213). It
		# has no fail-on-purpose mode of its own: it IS one. It also carries the
		# load-path lifetime checks (code-review batch 64: lifetimes.eval on
		# WARP - no D3D12 error through reset and arena, every texture set at its
		# tier across a quality round trip, no CPU image bytes pinned, a set with
		# no normal map loaded flat).
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
		name = 'paths'; tier = 'full'; needs = "build-$Config"
		what = 'the game and an AssetBaker import run from a folder outside ASCII (UTF-8 code page)'
		# Copies of the two exes in %TEMP%, so nothing beside bin is touched. Its
		# self-test strips the UTF-8 code page from those copies' manifests and
		# demands every case fail.
		run      = { & (Join-Path $root 'tools\PathsTest.ps1') -Config $Config | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\PathsTest.ps1') -Config $Config -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'baked'; tier = 'full'; needs = "build-$Config"
		what = 'a .dds older than its PNG, or a model sidecar missing or stale, is refused and said (once a model)'
		# One headless level load with a rune PNG made newer than its .dds and an
		# item model's sidecars made stale, one hidden (code-review C410 / C437);
		# put back however it ends. Its self-test plants nothing and demands
		# exactly the two planted checks fail.
		run      = { & (Join-Path $root 'tools\BakedTest.ps1') -Config $Config | Out-Host; $LASTEXITCODE }
		selfTest = { & (Join-Path $root 'tools\BakedTest.ps1') -Config $Config -SelfTest | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'stale'; tier = 'full'; needs = 'build-debug'
		what = 'a harness refuses a stale exe, and CheckAll builds what it runs (StaleTest)'
		run = { & (Join-Path $root 'tools\StaleTest.ps1') | Out-Host; $LASTEXITCODE }
	},
	@{
		name = 'build-profile'; tier = 'full'; build = $true
		what = 'the release-profile build compiles clean (DN_PROFILE rots unwatched too)'
		run  = { & cmd /c ".\build.cmd release-profile > `"$(Get-BuildLog profile)`" 2>&1"; $LASTEXITCODE }
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
		name = 'worn'; tier = 'full'; needs = 'build-release'
		what = 'the worn-block bake has one authority: models = wornblock = the committed files; wear 0 is flat'
		# tools\WornBakeTest.py bakes into a scratch folder with the RELEASE baker
		# (the tool everyone runs; a few seconds) and prints the shared verdict
		# line, read back like the native judges'. "Committed" is git's copy, not
		# the working tree a bake may have rewritten. Its self-test gives every
		# check group a fault and demands exactly those checks fail.
		run      = {
			$l = @(python (Join-Path $root 'tools\WornBakeTest.py') | ForEach-Object { Write-Host $_; $_ })
			Confirm-Verdict $l 'wornbaketest' $LASTEXITCODE
		}
		selfTest = {
			$l = @(python (Join-Path $root 'tools\WornBakeTest.py') --selftest | ForEach-Object { Write-Host $_; $_ })
			Confirm-Verdict $l 'wornbaketest' $LASTEXITCODE -SelfTest
		}
	},
	@{
		name = 'bakerwrites'; tier = 'full'; needs = 'build-release'
		what = 'the asset baker fails loudly: a read-only target is an error saying why, names are escaped, a bad map is said'
		# tools\BakerWriteTest.py (code-review C416) runs the RELEASE baker into a
		# scratch folder: `models`, `sounds` and an `import` over a read-only target
		# each exit non-zero naming it and why and leave it alone; the WAVs stay
		# the committed bytes; a found map that will not load is said; `rig-names`
		# writes strict JSON. Its self-test gives every check group a fault and
		# demands exactly those checks fail.
		run      = {
			$l = @(python (Join-Path $root 'tools\BakerWriteTest.py') | ForEach-Object { Write-Host $_; $_ })
			Confirm-Verdict $l 'bakerwritetest' $LASTEXITCODE
		}
		selfTest = {
			$l = @(python (Join-Path $root 'tools\BakerWriteTest.py') --selftest | ForEach-Object { Write-Host $_; $_ })
			Confirm-Verdict $l 'bakerwritetest' $LASTEXITCODE -SelfTest
		}
	},
	@{
		name = 'convertmesh'; tier = 'full'
		what = 'ConvertMesh --keep-rig keeps the skeletal one of a take''s two actions, and FetchModels hears a traceback'
		# tools\ConvertMeshTest.ps1 (code-review C434) needs Blender (discovered,
		# never pinned; exit 2 without one) and no build: a fixture FBX from
		# BuildRigFixture.py, converted through FetchModels' own Invoke-Convert.
		# Its self-test plants the old is_skeletal and a Blender call without
		# --python-exit-code and demands exactly the four checks resting on them fail.
		run      = {
			$l = @(& (Join-Path $root 'tools\ConvertMeshTest.ps1') 6>&1 | ForEach-Object { Write-Host $_; "$_" })
			Confirm-Verdict $l 'convertmeshtest' $LASTEXITCODE
		}
		selfTest = {
			$l = @(& (Join-Path $root 'tools\ConvertMeshTest.ps1') -SelfTest 6>&1 | ForEach-Object { Write-Host $_; "$_" })
			Confirm-Verdict $l 'convertmeshtest' $LASTEXITCODE -SelfTest
		}
	},
	@{
		name = 'meshes'; tier = 'full'
		what = 'the script-built arches, fountains, potions, rock and door frames are closed, face out and run their u one way'
		# tools\MeshTest.ps1 (code-review C435 / C436 / C404) needs Blender
		# (discovered, never pinned; exit 2 without one) and no build: seconds,
		# reading the shipped files in assets\models (its ray caster is Blender's).
		# A FAIL after a Build*.py change means the re-run asset is open, wound
		# in or spans u backwards. Its self-test plants a flipped triangle, a
		# hole and a reversed span in every file and demands exactly their checks fail.
		run      = {
			$l = @(& (Join-Path $root 'tools\MeshTest.ps1') 6>&1 | ForEach-Object { Write-Host $_; "$_" })
			Confirm-Verdict $l 'meshtest' $LASTEXITCODE
		}
		selfTest = {
			$l = @(& (Join-Path $root 'tools\MeshTest.ps1') -SelfTest 6>&1 | ForEach-Object { Write-Host $_; "$_" })
			Confirm-Verdict $l 'meshtest' $LASTEXITCODE -SelfTest
		}
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
