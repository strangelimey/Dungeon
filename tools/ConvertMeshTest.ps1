# ============================================================================
# tools\ConvertMeshTest.ps1 - ConvertMesh --keep-rig de-duplicates an FBX's
# takes on this Blender, and FetchModels hears a traceback (code-review C434).
#
#   .\tools\ConvertMeshTest.ps1              # exit 0 = PASS
#   .\tools\ConvertMeshTest.ps1 -SelfTest    # the planted faults must be CAUGHT
#   .\tools\ConvertMeshTest.ps1 -Blender 'D:\Blender\blender.exe'
#
# An FBX importer makes one action per take on every animated object, so a
# bought monster arrives with "Armature|Atk" beside a mesh's "Atk", and
# ConvertMesh keeps the SKELETAL one of each pair (its is_skeletal). That test
# only ran when two actions shared a base name - no pack converted so far had
# one - and it read `action.fcurves`, which Blender 5.x removed: a latent
# AttributeError. And FetchModels ran Blender without --python-exit-code, under
# which a Python traceback EXITS 0, so the failure would have surfaced only as
# "No rigged glb produced" further on.
#
# Everything runs in a scratch folder under %TEMP%; the tree is never written.
# Blender is DISCOVERED (FetchModels' own Find-Blender), never pinned, and the
# convert goes through FetchModels' own Invoke-Convert - both lifted out of
# FetchModels.ps1 by the parser, so this judges the functions the import runs,
# not copies of them.
#
#   fixture    tools\BuildRigFixture.py writes a two-bone rig with a skinned
#              column and an unskinned stowaway, one take; it re-imports the FBX
#              and refuses (exit 1) unless the take arrives as two actions, one
#              skeletal and one not - the shape that reaches is_skeletal.
#   convert    Invoke-Convert <fixture> --keep-rig --height 0.5 exits 0.
#   rigged     the .glb it writes has a skin of the rig's 2 joints and a mesh
#              carrying JOINTS_0 and WEIGHTS_0.
#   clips      exactly one animation, named for the take ("Scene"), and every
#              channel drives one of the skin's joints - the skeletal action won.
#   traceback  Invoke-Convert on a script that raises returns non-zero.
#
# -SelfTest plants the two faults C434 is about - a copy of ConvertMesh.py whose
# is_skeletal reads `action.fcurves` again, and an Invoke-Convert without
# --python-exit-code for the traceback run - and EXACTLY convert, rigged, clips
# and traceback must fail while the fixture still passes.
#
# Exit: 0 PASS (or, under -SelfTest, every planted fault caught and nothing
# else); 1 FAIL; 2 nothing judged (no Blender here). One verdict line:
# `convertmeshtest RESULT=... checks=N failures=M self_test=N [caught=N]`.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[switch]$SelfTest,
	[string]$Blender = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tool = 'convertmeshtest'

# --- FetchModels' own functions, lifted by the parser -------------------------
$fetchModels = Join-Path $PSScriptRoot 'FetchModels.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($fetchModels, [ref]$null, [ref]$null)
function Get-FunctionText([string]$name) {
	$fn = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
		$n.Name -eq $name }, $true)
	if (-not $fn) { throw "FetchModels.ps1 has no function $name to lift" }
	return $fn.Extent.Text
}
. ([ScriptBlock]::Create((Get-FunctionText 'Find-Blender')))
$invokeConvertText = Get-FunctionText 'Invoke-Convert'
. ([ScriptBlock]::Create($invokeConvertText))
if ($invokeConvertText -notmatch '--python-exit-code') {
	Write-Host '  note: FetchModels'' Invoke-Convert passes no --python-exit-code' -ForegroundColor Yellow
}

if (-not $Blender) { $Blender = Find-Blender }
if (-not $Blender -or -not (Test-Path $Blender)) {
	Write-Host "  no Blender here (looked under $env:ProgramFiles\Blender Foundation) - nothing judged" -ForegroundColor Yellow
	Write-Host "$tool RESULT=FAIL checks=0 failures=0 self_test=$([int]$SelfTest.IsPresent)"
	exit 2
}
Write-Host "Blender: $Blender"

$results = New-Object System.Collections.Generic.List[object]
function Test-Check([bool]$ok, [string]$label, [string]$detail = '') {
	$results.Add([pscustomobject]@{ Label = $label; Ok = $ok })
	$mark = if ($ok) { 'ok  ' } else { 'FAIL' }
	$col = if ($ok) { 'Gray' } else { 'Red' }
	Write-Host ("  [{0}] {1}{2}" -f $mark, $label, $(if ($detail) { "  ($detail)" } else { '' })) -ForegroundColor $col
}

# The JSON chunk of a .glb (12-byte header, then chunk 0: length, 'JSON', text).
function Read-GlbJson([string]$path) {
	$bytes = [IO.File]::ReadAllBytes($path)
	if ($bytes.Length -lt 20 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'glTF') { throw "$path is not a .glb" }
	$len = [BitConverter]::ToUInt32($bytes, 12)
	if ([Text.Encoding]::ASCII.GetString($bytes, 16, 4) -ne 'JSON') { throw "$path has no JSON chunk first" }
	return ([Text.Encoding]::UTF8.GetString($bytes, 20, $len) | ConvertFrom-Json)
}

$scratch = Join-Path $env:TEMP ("convertmeshtest-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $scratch | Out-Null
$expected = @()
try {
	# --- fixture ---------------------------------------------------------------
	$fbx = Join-Path $scratch 'rig_fixture.fbx'
	$prev = $ErrorActionPreference
	$ErrorActionPreference = 'Continue'
	try {
		$fixLog = @(& $Blender --background --factory-startup --python-exit-code 1 `
			--python (Join-Path $PSScriptRoot 'BuildRigFixture.py') -- $fbx 2>&1 | ForEach-Object { "$_" })
	} finally { $ErrorActionPreference = $prev }
	$fixCode = $LASTEXITCODE
	$reach = @($fixLog | Where-Object { $_ -match '^BuildRigFixture: (take|REFUSED)' }) -join ' / '
	Test-Check ($fixCode -eq 0 -and (Test-Path $fbx)) 'fixture' "exit $fixCode; $reach"

	# --- convert ---------------------------------------------------------------
	# Invoke-Convert reads $convert and $Blender from this scope, as it reads
	# FetchModels' script variables.
	$convert = Join-Path $PSScriptRoot 'ConvertMesh.py'
	if ($SelfTest) {
		# FAULT: is_skeletal reads the flat list Blender 5.x removed (C434).
		$text = [IO.File]::ReadAllText($convert)
		$planted = $text.Replace('for fc in all_fcurves(action))', 'for fc in action.fcurves)')
		if ($planted -eq $text) { throw 'the self-test could not plant the old is_skeletal - ConvertMesh.py changed' }
		$convert = Join-Path $scratch 'ConvertMesh_planted.py'
		[IO.File]::WriteAllText($convert, $planted)
		$expected += @('convert', 'rigged', 'clips')
	}
	$out = Join-Path $scratch 'out'
	$code = Invoke-Convert $fbx $out --keep-rig --height 0.5
	if ($code -is [array]) { $code = $code[-1] }
	Test-Check ($code -eq 0) 'convert' "exit $code"

	$glb = @(Get-ChildItem $out -Filter *.glb -ErrorAction SilentlyContinue)
	if ($glb.Count -ne 1) {
		Test-Check $false 'rigged' "$($glb.Count) .glb written"
		Test-Check $false 'clips' 'no .glb to read'
	} else {
		$j = Read-GlbJson $glb[0].FullName
		$skins = @($j.skins)
		$joints = if ($skins.Count) { @($skins[0].joints) } else { @() }
		$skinned = @($j.meshes | ForEach-Object { $_.primitives } |
			Where-Object { $_.attributes.PSObject.Properties.Name -contains 'JOINTS_0' -and
				$_.attributes.PSObject.Properties.Name -contains 'WEIGHTS_0' })
		Test-Check ($skins.Count -ge 1 -and $joints.Count -eq 2 -and $skinned.Count -ge 1) 'rigged' `
			"$($skins.Count) skin(s), $($joints.Count) joints, $($skinned.Count) skinned primitive(s)"
		$anims = @($j.animations)
		$names = @($anims | ForEach-Object { $_.name })
		$channels = @($anims | ForEach-Object { $_.channels })
		$offJoint = @($channels | Where-Object { $joints -notcontains $_.target.node })
		Test-Check ($anims.Count -eq 1 -and $names[0] -eq 'Scene' -and $channels.Count -gt 0 -and $offJoint.Count -eq 0) 'clips' `
			"$($anims.Count) animation(s) [$($names -join ', ')], $($channels.Count) channel(s), $($offJoint.Count) off the skin's joints"
	}

	# --- traceback -------------------------------------------------------------
	$convert = Join-Path $scratch 'raises.py'
	[IO.File]::WriteAllText($convert, "raise RuntimeError('ConvertMeshTest: a traceback, on purpose')`n")
	if ($SelfTest) {
		# FAULT: Invoke-Convert without --python-exit-code, as it was.
		$stripped = $invokeConvertText.Replace('--python-exit-code 1 ', '')
		if ($stripped -eq $invokeConvertText) { throw 'the self-test could not strip --python-exit-code - Invoke-Convert changed' }
		. ([ScriptBlock]::Create($stripped))
		$expected += 'traceback'
	}
	$code = Invoke-Convert (Join-Path $scratch 'nothing.fbx') (Join-Path $scratch 'out-raise')
	if ($code -is [array]) { $code = $code[-1] }
	Test-Check ($code -ne 0) 'traceback' "a raising script exits $code"
} finally {
	Remove-Item -Recurse -Force -LiteralPath $scratch -ErrorAction SilentlyContinue
}

$failed = @($results | Where-Object { -not $_.Ok } | ForEach-Object { $_.Label })
$result = if ($failed.Count) { 'FAIL' } else { 'PASS' }
Write-Host ''
if ($SelfTest) {
	$wrong = @(@($expected | Where-Object { $failed -notcontains $_ }) + @($failed | Where-Object { $expected -notcontains $_ }))
	foreach ($w in $wrong) {
		$why = if ($expected -contains $w) { 'passed with its fault' } else { 'failed with no fault' }
		Write-Host "  selftest: '$w' $why" -ForegroundColor Red
	}
	$caught = [int]($wrong.Count -eq 0)
	Write-Host "$tool RESULT=$result checks=$($results.Count) failures=$($failed.Count) self_test=1 caught=$caught"
	exit $(if ($caught) { 0 } else { 1 })
}
Write-Host "$tool RESULT=$result checks=$($results.Count) failures=$($failed.Count) self_test=0"
exit $(if ($failed.Count) { 1 } else { 0 })
