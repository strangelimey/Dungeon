# ============================================================================
# tools\HarnessAudio.ps1 - every harness runs the game MUTED.
#
# A harness launches Dungeon.exe over and over (Eval alone runs ten suites), and
# each launch plays at the developer's own master volume. This mutes it for the
# run and puts it back afterwards:
#
#   . (Join-Path $PSScriptRoot 'HarnessAudio.ps1')
#   if (-not $env:DN_HARNESS_MUTED) { exit (Invoke-Muted $bin $PSCommandPath $PSBoundParameters) }
#
# The volume lives in `volume=` in settings.ini beside the exe - the same key the
# Audio tab's slider saves - so muting is an edit of that one line, and so is the
# restore. Only that line: a run may legitimately rewrite the rest of the file
# (the game saves its settings), and a wholesale restore would undo that too.
#
# WHY THE SCRIPT RE-RUNS ITSELF instead of wrapping its body. The restore has to
# happen however the run ends - an `exit` from any branch, a throw, a Ctrl+C -
# and only a `finally` sees all of those. Invoke-Muted runs the calling script
# once more INSIDE its own try/finally, and the inner run sees DN_HARNESS_MUTED
# and skips straight to the tests. The same variable makes it nest: CheckAll
# mutes once for the whole suite, and every harness it calls finds the flag set
# and leaves the volume to it.
#
# Muting is the MASTERING VOICE only (AudioEngine::SetMasterVolume), so every
# sound still plays through the same code - nothing a harness measures changes.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================

# Read / write the file as UTF-8 without a BOM, the way the game writes it.
function Read-HarnessIni([string]$path) {
	if (-not (Test-Path $path)) { return $null }
	return [IO.File]::ReadAllText($path)
}

function Write-HarnessIni([string]$path, [string]$text) {
	[IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding $false))
}

# Replace the volume line in place (or append one); $line = $null removes it.
function Set-VolumeLine([string]$text, $line) {
	$lines = @()
	if ($text) { $lines = @($text -split "`r?`n" | Where-Object { $_ -ne '' }) }
	$out = @()
	$placed = $false
	foreach ($l in $lines) {
		if ($l -notmatch '^volume=') { $out += $l; continue }
		if ($null -ne $line -and -not $placed) { $out += $line; $placed = $true }
	}
	if ($null -ne $line -and -not $placed) { $out += $line }
	if ($out.Count -eq 0) { return '' }
	return ($out -join "`n") + "`n"
}

# Mute, run the calling script, restore. Returns the script's exit code.
function Invoke-Muted([string]$bin, [string]$script, $params) {
	$ini = Join-Path $bin 'settings.ini'
	$before = Read-HarnessIni $ini
	$original = $null
	if ($before) { $original = @($before -split "`r?`n" | Where-Object { $_ -match '^volume=' }) | Select-Object -First 1 }

	$shown = if ($original) { $original -replace '^volume=', '' } else { 'default' }
	Write-Host "audio: master volume muted for the run (was $shown)"
	if (Test-Path $bin) { Write-HarnessIni $ini (Set-VolumeLine $before 'volume=0') }
	$env:DN_HARNESS_MUTED = '1'
	try {
		& $script @params | Out-Default
		return $LASTEXITCODE
	} finally {
		Remove-Item Env:\DN_HARNESS_MUTED -ErrorAction SilentlyContinue
		$now = Read-HarnessIni $ini
		if ($null -ne $now) {
			$restored = Set-VolumeLine $now $original
			# A file the run created only to hold the mute goes again.
			if ($null -eq $before -and $restored -eq '') { Remove-Item $ini }
			else { Write-HarnessIni $ini $restored }
		}
		Write-Host "audio: master volume restored ($shown)"
	}
}
