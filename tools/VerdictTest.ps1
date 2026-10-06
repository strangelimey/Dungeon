# ============================================================================
# tools\VerdictTest.ps1 - the reader of the native judges' last line, judged.
#
#   .\tools\VerdictTest.ps1              # every case; exit 0 = all as expected
#   .\tools\VerdictTest.ps1 -SelfTest    # a reader that refuses nothing must be CAUGHT
#
# tools\Verdict.ps1 (Confirm-Verdict) is what makes CheckAll and Bc7Test.ps1
# read a native judge's last line instead of trusting its exit code alone
# (code-review C425). Every branch it has is a REFUSAL - a missing or old-format
# line, no checks counted, RESULT against failures or the exit code, a self-test
# the fault never reached - and a refusal is invisible in a green run: if the
# reader decayed to `return $code`, every tier and every self-test would stay
# green, because the judges it reads all print good lines. So this feeds it
# lines no judge prints today and demands each one be refused, BY NAME, while
# the good lines pass through with their own exit code untouched.
#
# A case is judged on two things: the code returned, and whether the reader
# SAID why it refused (its `verdict line: ...` row). The second matters because
# a refusal returns 1, and so does a good line whose tool exited 1 - only the
# message tells "refused" from "passed through" there.
#
# -SelfTest plants exactly that decay (a Confirm-Verdict that returns the exit
# code and says nothing) and requires exactly the refusal cases to fail and
# every pass-through case to pass - the tools\SpellTest.py rule.
#
# The last line is the shared verdict (tools\Common\Verdict.h):
#   verdicttest RESULT=PASS|FAIL checks=N failures=M self_test=0
#   verdicttest RESULT=FAIL checks=N failures=M self_test=1 caught=0|1
#
# Needs no build and runs no game: CheckAll's `verdict` row, quick tier.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Verdict.ps1')

if ($SelfTest) {
	# The decay this harness exists to catch: the reader trusts the exit code.
	function Confirm-Verdict([object[]]$lines, [string]$tool, [int]$code, [switch]$SelfTest) {
		return $code
	}
}

# ---------------------------------------------------------------------------
# THE CASES. `l` is the tool's stdout, `t` the tool it should be, `c` its exit
# code, `s` whether it ran under --self-test; `refuse` is what the reader must
# do. A refused case must return 1 and say why; a passed one must return `c`
# and say nothing.
# ---------------------------------------------------------------------------
$good = 'rolltest RESULT=PASS checks=441 failures=0 self_test=0'
$caughtLine = 'rolltest RESULT=FAIL checks=441 failures=31 self_test=1 caught=1'
$cases = @(
	# Passed through, exit code untouched.
	@{ n = 'a good normal PASS'; l = @('  [ok  ] x', $good); t = 'rolltest'; c = 0; s = $false; refuse = $false },
	@{ n = 'a good normal FAIL keeps its code'; l = @('rolltest RESULT=FAIL checks=441 failures=2 self_test=0'); t = 'rolltest'; c = 1; s = $false; refuse = $false },
	@{ n = 'trailing blank lines are ignored'; l = @($good, '', '  '); t = 'rolltest'; c = 0; s = $false; refuse = $false },
	@{ n = 'bc7 fields between counts and mode'; l = @('bc7test RESULT=PASS checks=7 failures=0 images=16 consistency_bad=0 thread_diff=0 regressed=0 matched=16 minpsnr=18.58 self_test=0'); t = 'bc7test'; c = 0; s = $false; refuse = $false },
	@{ n = 'a caught self-test'; l = @($caughtLine); t = 'rolltest'; c = 0; s = $true; refuse = $false },
	@{ n = 'an uncaught self-test keeps its code'; l = @('rolltest RESULT=FAIL checks=441 failures=24 self_test=1 caught=0'); t = 'rolltest'; c = 1; s = $true; refuse = $false },
	@{ n = 'a bc7 self-test with its fields'; l = @('bc7test RESULT=FAIL checks=7 failures=2 images=16 consistency_bad=40 thread_diff=0 regressed=16 matched=16 minpsnr=18.58 self_test=1 caught=1'); t = 'bc7test'; c = 0; s = $true; refuse = $false },

	# Refused: the line is not the shared verdict, or not this tool's.
	@{ n = 'the OLD RollTest last line'; l = @('PASS - 441 checks, 0 failed'); t = 'rolltest'; c = 0; s = $false; refuse = $true },
	@{ n = 'the OLD Bc7Test last line'; l = @('BC7TEST VERDICT=PASS images=16 consistency_bad=0 thread_diff=0 regressed=0 matched=16 minpsnr=18.58'); t = 'bc7test'; c = 0; s = $false; refuse = $true },
	@{ n = 'no output at all'; l = @(); t = 'rolltest'; c = 0; s = $false; refuse = $true },
	@{ n = 'another tool''s line'; l = @('diagtest RESULT=PASS checks=54 failures=0 self_test=0'); t = 'rolltest'; c = 0; s = $false; refuse = $true },
	@{ n = 'the verdict is not the last line'; l = @($good, 'FORCE-TERMINATED worker 3'); t = 'rolltest'; c = 0; s = $false; refuse = $true },
	@{ n = 'a line without self_test'; l = @('rolltest RESULT=PASS checks=441 failures=0'); t = 'rolltest'; c = 0; s = $false; refuse = $true },

	# Refused: the line contradicts itself or its exit code.
	@{ n = 'no checks counted'; l = @('diagtest RESULT=PASS checks=0 failures=0 self_test=0'); t = 'diagtest'; c = 0; s = $false; refuse = $true },
	@{ n = 'RESULT=PASS with failures'; l = @('rolltest RESULT=PASS checks=441 failures=3 self_test=0'); t = 'rolltest'; c = 0; s = $false; refuse = $true },
	@{ n = 'RESULT=FAIL but exit 0'; l = @('rolltest RESULT=FAIL checks=441 failures=2 self_test=0'); t = 'rolltest'; c = 0; s = $false; refuse = $true },
	@{ n = 'RESULT=PASS but exit 1'; l = @($good); t = 'rolltest'; c = 1; s = $false; refuse = $true },
	@{ n = 'a normal run saying self_test=1'; l = @($caughtLine); t = 'rolltest'; c = 0; s = $false; refuse = $true },

	# Refused: a self-test that cannot have caught what it says.
	@{ n = 'a self-test the fault never reached'; l = @('rolltest RESULT=PASS checks=441 failures=0 self_test=1 caught=1'); t = 'rolltest'; c = 0; s = $true; refuse = $true },
	@{ n = 'a self-test that ran as a normal run'; l = @($good); t = 'rolltest'; c = 0; s = $true; refuse = $true },
	@{ n = 'a self-test line without caught'; l = @('rolltest RESULT=FAIL checks=441 failures=31 self_test=1'); t = 'rolltest'; c = 0; s = $true; refuse = $true },
	@{ n = 'caught=1 but exit 1'; l = @($caughtLine); t = 'rolltest'; c = 1; s = $true; refuse = $true },
	@{ n = 'caught=0 but exit 0'; l = @('rolltest RESULT=FAIL checks=441 failures=24 self_test=1 caught=0'); t = 'rolltest'; c = 0; s = $true; refuse = $true }
)

# ---------------------------------------------------------------------------
# Run them. Write-Host lands on the information stream (6), which is how the
# reader's refusal row is captured beside its return value.
# ---------------------------------------------------------------------------
$failed = @()
foreach ($k in $cases) {
	$out = @(Confirm-Verdict $k.l $k.t $k.c -SelfTest:$k.s 6>&1)
	$said = @($out | Where-Object { $_ -is [System.Management.Automation.InformationRecord] } | ForEach-Object { "$_".Trim() })
	$codes = @($out | Where-Object { $_ -isnot [System.Management.Automation.InformationRecord] })
	$got = if ($codes.Count -eq 1) { [int]$codes[0] } else { $null }
	$refused = @($said | Where-Object { $_ -like 'verdict line:*' }).Count -gt 0
	$ok = if ($k.refuse) { $refused -and $got -eq 1 } else { -not $said.Count -and $got -eq $k.c }
	if (-not $ok) { $failed += $k.n }
	$what = if ($said.Count) { $said -join ' / ' } else { 'passed through' }
	Write-Host ("  [{0}] {1,-38} want {2,-7} returned {3}: {4}" -f $(if ($ok) { 'ok  ' } else { 'FAIL' }),
		$k.n, $(if ($k.refuse) { 'refused' } else { "code $($k.c)" }), $got, $what)
}

# ---------------------------------------------------------------------------
# The verdict.
# ---------------------------------------------------------------------------
$fail = if ($failed.Count) { 'FAIL' } else { 'PASS' }
if ($SelfTest) {
	# Exactly the refusal cases fail under the planted decay, and nothing else:
	# a pass-through case failing would mean the harness, not the reader, broke.
	$want = @($cases | Where-Object { $_.refuse } | ForEach-Object { $_.n })
	$unexpected = @($failed | Where-Object { $want -notcontains $_ })
	$missed = @($want | Where-Object { $failed -notcontains $_ })
	foreach ($u in $unexpected) { Write-Host "  self-test: '$u' FAILED but is not an expected failure" -ForegroundColor Red }
	foreach ($m in $missed) { Write-Host "  self-test: '$m' was expected to FAIL and passed" -ForegroundColor Red }
	$caught = ($unexpected.Count -eq 0) -and ($missed.Count -eq 0)
	Write-Host ("SELF-TEST {0} - {1} of {2} expected failures, {3} unexpected" -f $(if ($caught) { 'PASS' } else { 'FAIL' }),
		($want.Count - $missed.Count), $want.Count, $unexpected.Count)
	Write-Host ("verdicttest RESULT={0} checks={1} failures={2} self_test=1 caught={3}" -f $fail, $cases.Count, $failed.Count, [int]$caught)
	exit $(if ($caught) { 0 } else { 1 })
}
Write-Host ("verdicttest RESULT={0} checks={1} failures={2} self_test=0" -f $fail, $cases.Count, $failed.Count)
exit $(if ($failed.Count) { 1 } else { 0 })
