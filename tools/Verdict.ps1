# ============================================================================
# tools\Verdict.ps1 - the one reader of the native judges' last line.
#
# RollTest, DiagTest, ThreadStress and Bc7Test end with the shared verdict
# (tools\Common\Verdict.h):
#   <tool> RESULT=PASS|FAIL checks=N failures=M [fields] self_test=0
#   <tool> RESULT=PASS|FAIL checks=N failures=M [fields] self_test=1 caught=0|1
# CheckAll.ps1 and Bc7Test.ps1 read it back rather than trusting the exit code
# alone (code-review C425: two of the four printed a format of their own, so
# nothing could). A run whose last line is missing or malformed, counted no
# checks, or disagrees with its own exit code FAILS, with a line saying why:
# a judge that cannot say what it judged has not judged.
#
# A self-test reads RESULT=FAIL - the fault was given and the checks failed on
# it - and `caught` says whether exactly the expected checks were the ones;
# its exit code follows caught. So under -SelfTest a RESULT=PASS is itself a
# failure: the injected fault reached nothing.
#
#   . (Join-Path $PSScriptRoot 'Verdict.ps1')
#   $code = Confirm-Verdict $lines 'rolltest' $exitCode [-SelfTest]
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================

# The exit code to report: the tool's own when its last line agrees with it,
# else 1. `$lines` is everything the tool printed to STDOUT, in order - never
# stderr, where a tool's log warnings may land after its verdict (a worker
# joined or force-terminated as main returns).
function Confirm-Verdict([object[]]$lines, [string]$tool, [int]$code, [switch]$SelfTest) {
	$last = @($lines | ForEach-Object { "$_".TrimEnd() } | Where-Object { $_ -ne '' }) | Select-Object -Last 1
	$pattern = '^{0} RESULT=(PASS|FAIL) checks=(\d+) failures=(\d+)(?: \S+=\S+)*? self_test=([01])(?: caught=([01]))?$' -f [regex]::Escape($tool)
	$why = $null
	if ("$last" -match $pattern) {
		$result = $Matches[1]
		$checks = [int]$Matches[2]
		$failures = [int]$Matches[3]
		$self = $Matches[4]
		$caught = $Matches[5]
		if ($checks -le 0) {
			$why = 'it counted no checks'
		} elseif (($result -eq 'PASS') -ne ($failures -eq 0)) {
			$why = "RESULT=$result with failures=$failures"
		} elseif ($SelfTest) {
			if ($self -ne '1' -or -not $caught) { $why = 'a self-test run without self_test=1 caught=N' }
			elseif ($result -ne 'FAIL') { $why = 'every check passed under the self-test: the fault reached nothing' }
			elseif (($caught -eq '1') -ne ($code -eq 0)) { $why = "caught=$caught but exit code $code" }
		} else {
			if ($self -ne '0' -or $caught) { $why = 'a normal run that says self_test=1' }
			elseif (($result -eq 'PASS') -ne ($code -eq 0)) { $why = "RESULT=$result but exit code $code" }
		}
	} else {
		$why = "the last line is not '$tool RESULT=... checks=N failures=M ... self_test=N' (it reads: '$last')"
	}
	if ($why) {
		Write-Host "  verdict line: $why" -ForegroundColor Red
		return 1
	}
	return $code
}
