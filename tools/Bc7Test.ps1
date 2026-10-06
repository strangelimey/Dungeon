# ============================================================================
# tools\Bc7Test.ps1 - the BC7 encoder regression run.
#
# The texture pipeline's quality used to be a number from a session that is now
# history. This is the version that can be re-run: a fixed corpus, a recorded
# baseline, and an exit code. Exit 0 = PASS.
#
#   .\tools\Bc7Test.ps1                    # release build, full corpus
#   .\tools\Bc7Test.ps1 -Config debug
#   .\tools\Bc7Test.ps1 -SelfTest          # checks the CHECKER
#   .\tools\Bc7Test.ps1 -Audit             # the knob-by-knob measurement table
#   .\tools\Bc7Test.ps1 -UpdateBaseline    # record today's numbers as the bar
#
# The failures mean different things:
#   consistency_bad > 0  The encoder's own error estimate disagrees with a real
#                        decode of the bytes it wrote. This is a CORRECTNESS
#                        bug, not a quality one: that estimate is what picks the
#                        mode, so if it lies, mode selection is a coin toss.
#   thread_diff > 0      The block fan-out changed the output. Blocks are
#                        independent; if this fires, something is shared that
#                        should not be.
#   regressed > 0        A corpus image lost quality against the baseline.
#                        Sometimes legitimate (a deliberate speed trade) - then
#                        re-run with -UpdateBaseline and say so in the commit.
#   matched = 0          Nothing in the baseline matched the corpus. The gate
#                        checked nothing: until 2026-10-05 the loader stopped
#                        at the file's '#' header and EVERY run was this,
#                        reported as a pass.
#   a syn.* gap          A synthetic image with no entry, or a syn.* entry with
#                        no image. That corpus is the same on every machine, so
#                        this is a lost row or a renamed/new image, and that
#                        image is no longer gated. Real-texture rows that do not
#                        match are only listed as notes - which textures get
#                        sampled follows the installed pool.
#
#   a mips: check        The mip filter every level below the first goes through
#                        (assets::Downsample): sRGB colour averaged in linear
#                        light, every other average rounded (code-review C414).
#                        A FAIL here means the baked chains and the runtime
#                        fallback's darken or drift with distance.
#
# -SelfTest injects three faults and requires EXACTLY the three checks they aim
# at to fail: corrupted bytes (the consistency check), a baseline raised 1 dB
# (the quality check, on every matched image) and sRGB mips averaged as stored
# bytes, the filter before C414 (the sRGB mip check). The thread, rounding and
# baseline-reading checks must still pass. A harness that cannot fail is not
# evidence of anything - the same reason AllocTest.ps1 has an inverted mode -
# and one that passes its self-test on ANY failure cannot show which fault it
# caught.
#
# Prefer the RELEASE build: the encode is heavily float-bound and a debug run of
# the same corpus takes minutes rather than seconds. The output is identical.
#
# ASCII ONLY, deliberately: PowerShell 5.1 reads a BOM-less .ps1 as ANSI, so a
# stray em-dash in a comment is a parse error, not a cosmetic issue.
# ============================================================================
[CmdletBinding()]
param(
	[ValidateSet('debug', 'release')][string]$Config = 'release',
	# Checks the CHECKER: damages the packed bytes and raises the baseline, and
	# passes only if exactly the two checks those faults aim at come back FAIL.
	[switch]$SelfTest,
	# Prints what each knob is worth (modes, partition-shape count, p-bit trial)
	# instead of running the regression. Slow - it encodes the corpus many times.
	[switch]$Audit,
	# Records the current PSNR as the new baseline.
	[switch]$UpdateBaseline,
	# Real textures sampled per kind (albedo / normal+height / ORM).
	[int]$PerKind = 3
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\$Config\bin\Bc7Test.exe"
$baseline = Join-Path $root 'tools\bc7-baseline.txt'
$assets = Join-Path $root 'assets'

if (-not (Test-Path $exe)) {
	Write-Host "Bc7Test.exe not found at $exe" -ForegroundColor Red
	Write-Host "Build it first:  .\build.cmd $Config"
	exit 2
}
# Never a stale encoder: a baseline compared against yesterday's binary says
# nothing about today's change (code-review C426; tools\HarnessGame.ps1).
. (Join-Path $PSScriptRoot 'HarnessGame.ps1')
Assert-ExeCurrent $exe

# The self-test proves the gates by tripping them, so it needs the regression
# run and the baseline; with -Audit or -UpdateBaseline it would prove nothing.
if ($SelfTest -and ($Audit -or $UpdateBaseline)) {
	Write-Host '-SelfTest runs with neither -Audit nor -UpdateBaseline.' -ForegroundColor Red
	exit 2
}

$bc7Args = @('--per-kind', $PerKind)

# The real textures are gitignored, so a fresh clone legitimately has none. The
# synthetic corpus alone still exercises every mode and every check - it just
# measures fewer kinds of content, so say which run this was.
$haveTextures = (Test-Path (Join-Path $assets 'textures')) -and
				((Get-ChildItem (Join-Path $assets 'textures') -Filter *.png -ErrorAction SilentlyContinue |
				  Measure-Object).Count -gt 0)
if ($haveTextures) {
	$bc7Args += @('--assets', $assets)
} else {
	Write-Host "No installed textures - running the synthetic corpus only." -ForegroundColor Yellow
	Write-Host "(tools\FetchTextures.ps1 installs them; the baseline covers both sets.)"
}

if ($Audit) {
	& $exe @bc7Args --audit
	exit $LASTEXITCODE
}

# The baseline is committed, so it is ALWAYS passed: a missing file is a FAIL
# (nothing matched), never a run that quietly skipped the quality gate.
if ($UpdateBaseline) { $bc7Args += @('--write-baseline', $baseline) }
else { $bc7Args += @('--baseline', $baseline) }

if ($SelfTest) { $bc7Args += '--self-test' }

# Shown as it comes and kept, so its last line - the shared verdict,
# `bc7test RESULT=.. checks=N failures=M ..` - can be read back against the
# exit code (tools\Verdict.ps1, code-review C425). stderr is left alone: under
# 'Stop' a redirected stderr line would be a terminating error.
. (Join-Path $PSScriptRoot 'Verdict.ps1')
$lines = @(& $exe @bc7Args | ForEach-Object { Write-Host $_; $_ })
$code = Confirm-Verdict $lines 'bc7test' $LASTEXITCODE -SelfTest:$SelfTest

Write-Host ''
if ($code -eq 0) {
	if ($SelfTest) { Write-Host 'SELF-TEST PASS: every deliberate fault was caught, each by its own check.' -ForegroundColor Green }
	else { Write-Host 'PASS' -ForegroundColor Green }
} else {
	if ($SelfTest) { Write-Host 'SELF-TEST FAIL: a fault went undetected, or a check it should not reach failed - see the self-test lines above.' -ForegroundColor Red }
	else { Write-Host 'FAIL - see the checks and the per-image rows above.' -ForegroundColor Red }
}
exit $code
