# ============================================================================
# tools\PipelineTest.ps1 - the one-pipeline invariant, checked.
#
#   .\tools\PipelineTest.ps1              # the suite must pass
#   .\tools\PipelineTest.ps1 -SelfTest    # a deliberate violation must be CAUGHT
#   .\tools\PipelineTest.ps1 -Config release
#
# Exit 0 = PASS.
#
# THIS IS NOT tools\Eval.ps1, and the difference is the reason it exists
# separately. Eval MEASURES - its suites produce tables nobody has decided the
# right answer for, which is why it sits outside CheckAll's tiers. This guards a
# RULE (docs/effects.md: outside the fx::ITarget adapters nothing writes health),
# so it belongs with the other checked rules and it has a verdict.
#
# IT DOES NOT TRUST 'RESULT=PASS' ON ITS OWN, and that is not paranoia - it is
# the first thing the suite taught. The opening run wiped the party on a
# T-junction blast, dropped the app to the title screen, and then printed a
# confident PASS for four more sections while nothing simulated at all. A check
# that ran no damage cannot report that damage behaved. So three things are
# demanded, not one:
#
#   1. the final PIPELINE line says PASS with violations=0
#   2. the app is still `playing` - nothing voided the back half of the run
#   3. every sanctioned ROUTE moved health: pipeline, exertion, regen, growth
#      and stabilize each have a non-zero total
#
# (3) is what makes the pass mean something. Each row is a route the suite
# claims to exercise, and a zero in any of them is a section that quietly
# stopped working - which is how the over-exertion row was caught doing nothing
# at all, an untrained fighter being unable to over-exert by design.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[switch]$SelfTest,
	[ValidateSet('debug', 'release')][string]$Config = 'debug'
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"
$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'
$scripts = Join-Path $root 'tools\EvalScripts'

# The routes the suite exercises, in the order the readout prints them. Adding a
# Reason to Game/DamageLedger.h means adding a section to pipeline.eval and a row
# here - a reason nothing takes would otherwise sit at zero forever and nobody
# would notice it had stopped being reachable.
$routes = @(
	@{ name = 'pipeline';  what = 'melee, bolts, blasts, DoTs, breakables, the wall bump' },
	@{ name = 'exertion';  what = 'over-exertion (the DECLARED exception to the rule)' },
	@{ name = 'regen';     what = 'resource regeneration' },
	@{ name = 'growth';    what = 'a stat or practice level growing the pool' },
	@{ name = 'stabilize'; what = 'an unconscious member coming round' }
)

if (-not (Test-Path $exe)) {
	Write-Host "pipeline: no exe at $exe - build first" -ForegroundColor Red
	exit 2
}

$checks = 0
$failed = 0
function Expect($what, $ok, $detail) {
	$script:checks++
	if (-not $ok) { $script:failed++ }
	Write-Host ("  {0,-56} {1,-22} {2}" -f $what, $detail, $(if ($ok) { 'ok' } else { 'FAIL' }))
}

# ---------------------------------------------------------------------------
# Read back from dungeon.log, not from the process: the game writes there, and
# it is the same file a human opens after a run (docs/eval-harness.md).
# ---------------------------------------------------------------------------
function Run-Script($name) {
	$path = Join-Path $scripts $name
	$p = Start-Process -FilePath $exe -ArgumentList '-eval', $path -PassThru -Wait
	$lines = @(Get-Content $log)
	return @{
		exit = $p.ExitCode
		lines = $lines
		# The LAST verdict in the log: the suite prints one per section, and only
		# the final one has seen the whole run.
		verdict = @($lines | Select-String -Pattern 'PIPELINE RESULT=(\w+) checks=(\d+) violations=(\d+)' |
					ForEach-Object { $_.Matches[0] }) | Select-Object -Last 1
		state = @($lines | Select-String -Pattern 'console: state (\w+)' |
				  ForEach-Object { $_.Matches[0].Groups[1].Value }) | Select-Object -Last 1
	}
}

# A route's total, taken from the LAST readout block in the log.
function Route-Total($lines, $name) {
	$hits = @($lines | Select-String -Pattern ("console:   {0}\s+([-+][0-9.]+)" -f $name) |
			  ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
	if ($hits.Count -eq 0) { return 0.0 }
	return $hits[-1]
}

Write-Host ''
if ($SelfTest) {
	# ---------------------------------------------------------------------
	# The check must CATCH a write that goes around the pipeline. A clean run
	# here means the whole thing is decoration.
	# ---------------------------------------------------------------------
	Write-Host '=== self-test: a health write outside the pipeline must be CAUGHT ==='
	$r = Run-Script 'pipeline-poke.eval'
	$violations = if ($r.verdict) { [int]$r.verdict.Groups[3].Value } else { -1 }
	$reported = @($r.lines | Select-String -Pattern 'one-pipeline violation').Count

	Expect 'the poked write is counted as a violation' ($violations -ge 1) "violations=$violations"
	Expect 'the poked write is REPORTED, with a victim and a phase' ($reported -ge 1) "$reported line(s)"
	Expect 'and the verdict flips to FAIL' `
		($r.verdict -and $r.verdict.Groups[1].Value -eq 'FAIL') `
		$(if ($r.verdict) { $r.verdict.Groups[1].Value } else { 'no verdict' })

	Write-Host ''
	Write-Host ("pipeline RESULT={0} checks={1} failures={2} self_test=1" -f `
		$(if ($failed -eq 0) { 'PASS' } else { 'FAIL' }), $checks, $failed)
	if ($failed -ne 0) {
		Write-Host 'A CHECK THAT CANNOT FAIL MEANS NOTHING' -ForegroundColor Red
	}
	exit $(if ($failed -eq 0) { 0 } else { 1 })
}

Write-Host '=== the one-pipeline invariant: every source of damage goes through fx::Deal ==='
$r = Run-Script 'pipeline.eval'

Expect 'the suite ran to completion' ($r.exit -eq 0) "exit $($r.exit)"

$verdictOk = $r.verdict -and $r.verdict.Groups[1].Value -eq 'PASS'
$counted = if ($r.verdict) { [int]$r.verdict.Groups[2].Value } else { 0 }
$violations = if ($r.verdict) { [int]$r.verdict.Groups[3].Value } else { -1 }
Expect 'nothing wrote health outside the pipeline' ($violations -eq 0) "violations=$violations"
Expect 'and the ledger says so itself' $verdictOk $(if ($r.verdict) { $r.verdict.Groups[1].Value } else { 'no verdict' })

# THE GUARD AGAINST AN EMPTY PASS. A wipe drops the app to the title screen and
# every section after it measures nothing while still printing PASS.
Expect 'the world was still simulating at the end' ($r.state -eq 'playing') "state $($r.state)"
Expect 'and it actually looked at something' ($counted -gt 1000) "checks=$counted"

Write-Host ''
Write-Host '  routes exercised (a zero here is a section that stopped working):'
foreach ($route in $routes) {
	$total = Route-Total $r.lines $route.name
	Expect ("    {0} - {1}" -f $route.name, $route.what) `
		($total -ne 0.0) ("{0:+0.00;-0.00}" -f $total)
}

Write-Host ''
Write-Host ("pipeline RESULT={0} checks={1} failures={2} self_test=0" -f `
	$(if ($failed -eq 0) { 'PASS' } else { 'FAIL' }), $checks, $failed)
if ($failed -eq 0) {
	Write-Host ("  {0} health values verified across {1} checkpoints" -f $counted, `
		$(@($r.lines | Select-String -Pattern 'checkpoints=(\d+)' |
			ForEach-Object { $_.Matches[0].Groups[1].Value }) | Select-Object -Last 1))
}
exit $(if ($failed -eq 0) { 0 } else { 1 })
