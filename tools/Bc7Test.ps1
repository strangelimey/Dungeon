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
# Three failures are possible and they mean different things:
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
#
# -SelfTest corrupts the encoded bytes on purpose and requires the run to come
# back FAIL. A harness that cannot fail is not evidence of anything - the same
# reason AllocTest.ps1 has an inverted mode.
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
	# Checks the CHECKER: damages the packed bytes and passes only if the run
	# comes back FAIL.
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

if ($UpdateBaseline) { $bc7Args += @('--write-baseline', $baseline) }
elseif (Test-Path $baseline) { $bc7Args += @('--baseline', $baseline) }

if ($SelfTest) { $bc7Args += '--self-test' }

& $exe @bc7Args
$code = $LASTEXITCODE

Write-Host ''
if ($code -eq 0) {
	if ($SelfTest) { Write-Host 'SELF-TEST PASS: the harness caught deliberate corruption.' -ForegroundColor Green }
	else { Write-Host 'PASS' -ForegroundColor Green }
} else {
	if ($SelfTest) { Write-Host 'SELF-TEST FAIL: corruption went UNDETECTED - the checks are not reading the bytes.' -ForegroundColor Red }
	else { Write-Host 'FAIL - see the per-image rows above.' -ForegroundColor Red }
}
exit $code
