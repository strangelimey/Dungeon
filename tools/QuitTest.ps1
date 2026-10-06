# ============================================================================
# tools\QuitTest.ps1 - a load can always be quit.
#
#   .\tools\QuitTest.ps1              # exit 0 = PASS
#   .\tools\QuitTest.ps1 -SelfTest    # near-misses must NOT quit - must FAIL
#
# During a load the console refused EVERY command, `quit` included, and
# WM_SYSKEYDOWN never reached DefWindowProc, so Alt+F4 never became WM_CLOSE.
# Borderless is WS_POPUP (no close box) and Exclusive covers the screen, so a
# load in either - or a wedged one - could only be left through Task Manager
# (code-review C392). Two cases, each launching its own game:
#
#   quitload   `newgame`, then - once `state` answers "unavailable while
#              loading", proof the load is under way - `quit`. The process must
#              end cleanly (exit 0, "Dungeon shutting down.") with no
#              `Level ready:` before the shutdown line.
#   altf4      Borderless saved in settings.ini (put back however the run ends),
#              then Alt+F4 posted to the game window. It must end the same way.
#              The window covers a monitor for a few seconds: that is the mode.
#
# -SelfTest sends a near-miss instead (`qiut`; Alt+F3), and each case must then
# FAIL - the game must still be running when the wait runs out - or the checks
# could pass on a process that ended for some other reason. NOT `qui`: Enter on
# the type-ahead's highlighted `quit` completes it and, a command with no
# arguments, runs it (through the same gate) - its first self-test quit the game.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[switch]$SelfTest,
	[ValidateSet('debug', 'release')][string]$Config = 'debug',
	[int]$LoadTimeoutSec = 180
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Config\bin"

# Muted for the whole run, restored however it ends (tools\HarnessAudio.ps1).
. (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
if (-not (Test-HarnessMuted $bin)) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }
# Launch, input and log waits: the one shared copy (tools\HarnessGame.ps1).
. (Join-Path $PSScriptRoot 'HarnessGame.ps1')

$exe = Join-Path $bin 'Dungeon.exe'
$log = Join-Path $bin 'dungeon.log'
$ini = Join-Path $bin 'settings.ini'

if (-not (Test-Path $exe)) { throw "no build at $exe - run build.cmd $Config first" }
Assert-ExeCurrent $exe
Assert-NotRunning $exe

$WM_SYSKEYDOWN = 0x104; $WM_SYSKEYUP = 0x105
$VK_F3 = 0x72; $VK_F4 = 0x73
$exitWaitMs = 20000

$results = @()
function Record([string]$case, [bool]$ok, [string]$detail) {
	$script:results += [pscustomobject]@{ Case = $case; Ok = $ok; Detail = $detail }
	Write-Host ("  [{0}] {1,-10} {2}" -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $case, $detail)
}

# Whether THIS run's game ended cleanly within the wait: exited, code 0, and the
# shutdown line logged. Kills it (by PID) if it is still up, so the next case
# starts clean.
function Wait-CleanExit([string]$what) {
	$ended = $script:proc.WaitForExit($exitWaitMs)
	if (-not $ended) {
		$script:proc.Kill(); $script:proc.WaitForExit(5000) | Out-Null
		return @{ ok = $false; detail = "still running $($exitWaitMs / 1000)s after $what" }
	}
	$code = $script:proc.ExitCode
	$down = [bool](Select-String -Path $script:log -Pattern 'Dungeon shutting down\.' -EA SilentlyContinue)
	return @{ ok = ($code -eq 0 -and $down); detail = "exited $code after $what$(if (-not $down) { ', no shutdown line' })" }
}

Write-Host ''
Write-Host ("=== a load can always be quit{0} ===" -f $(if ($SelfTest) { ' (self-test: near-misses)' } else { '' }))

# --- quitload ----------------------------------------------------------------
try {
	Start-HarnessGame $exe $bin $log $LoadTimeoutSec
	Send-Key $VK_CONSOLE; Start-Sleep -Milliseconds 500
	if (-not (Wait-ConsoleReady)) { throw 'the console never answered on the title screen' }
	Send-Text 'newgame'; Send-Key $VK_RETURN
	# The load is under way once the console refuses a command. Probe until it
	# does (a fast load could be over before the first probe: then say so).
	$gate = 'commands are unavailable while loading'
	$before = Get-LogMatchCount $gate
	$loading = $false
	for ($i = 0; $i -lt 20 -and -not $loading; $i++) {
		Send-Text 'state'; Send-Key $VK_RETURN
		$loading = (Wait-NewLogLines $gate $before 1 1.0).Count -gt 0
		if (Select-String -Path $log -Pattern '^\[info \] Level ready: ' -EA SilentlyContinue) { break }
	}
	if (-not $loading) {
		Record 'quitload' $false 'the load finished before the console ever refused a command - nothing tested'
	} else {
		$word = if ($SelfTest) { 'qiut' } else { 'quit' }
		Send-Text $word; Send-Key $VK_RETURN
		$r = Wait-CleanExit "``$word`` during the load"
		$lines = @(Get-Content $log -EA SilentlyContinue)
		$readyAt = [array]::FindIndex($lines, [Predicate[string]] { param($l) $l -match '^\[info \] Level ready: ' })
		$downAt = [array]::FindIndex($lines, [Predicate[string]] { param($l) $l -match 'Dungeon shutting down\.' })
		$inLoad = $downAt -ge 0 -and ($readyAt -lt 0 -or $readyAt -gt $downAt)
		Record 'quitload' ($r.ok -and $inLoad) ($r.detail + $(if ($r.ok -and -not $inLoad) { ' - but the level was ready first' } else { '' }))
	}
} catch {
	Record 'quitload' $false "harness error: $($_.Exception.Message)"
} finally {
	if ($script:proc -and -not $script:proc.HasExited) { $script:proc.Kill(); $script:proc.WaitForExit(5000) | Out-Null }
}

# --- altf4 -------------------------------------------------------------------
$iniBefore = if (Test-Path $ini) { [IO.File]::ReadAllText($ini) } else { $null }
try {
	$keep = @((("$iniBefore") -split "\r?\n") | Where-Object { $_ -ne '' -and $_ -notmatch '^fullscreen=' })
	[IO.File]::WriteAllText($ini, ((@($keep) + 'fullscreen=1') -join "`n") + "`n")
	Start-HarnessGame $exe $bin $log $LoadTimeoutSec
	Start-Sleep -Seconds 1
	# Alt held: bit 29 of lParam is the context code DefWindowProc reads.
	$key = if ($SelfTest) { $VK_F3 } else { $VK_F4 }
	Send-Message $WM_SYSKEYDOWN $key 0x20000001
	Start-Sleep -Milliseconds 60
	Send-Message $WM_SYSKEYUP $key 0xE0000001
	$r = Wait-CleanExit "Alt+$(if ($SelfTest) { 'F3' } else { 'F4' }) in Borderless"
	Record 'altf4' $r.ok $r.detail
} catch {
	Record 'altf4' $false "harness error: $($_.Exception.Message)"
} finally {
	if ($script:proc -and -not $script:proc.HasExited) { $script:proc.Kill(); $script:proc.WaitForExit(5000) | Out-Null }
	if ($null -eq $iniBefore) { Remove-Item $ini -ErrorAction SilentlyContinue }
	else { [IO.File]::WriteAllText($ini, $iniBefore) }
}

$failures = @($results | Where-Object { -not $_.Ok }).Count
Write-Host ''
Write-Host ("quittest RESULT={0} cases={1} failures={2} self_test={3}" -f $(if ($failures -eq 0) { 'PASS' } else { 'FAIL' }),
	$results.Count, $failures, [int]$SelfTest.IsPresent)
if ($SelfTest) {
	# Every case must have failed, and for the RIGHT reason: the game was still
	# up when the wait ran out. A harness error is not a caught near-miss.
	$right = @($results | Where-Object { -not $_.Ok -and $_.Detail -match '^still running' }).Count
	$caught = ($right -eq 2)
	Write-Host $(if ($caught) { 'SELF-TEST PASSED - neither near-miss quit the game' } else { 'SELF-TEST FAILED - a near-miss quit the game, or the harness broke' })
	exit $(if ($caught) { 0 } else { 1 })
}
exit $(if ($failures -eq 0) { 0 } else { 1 })
