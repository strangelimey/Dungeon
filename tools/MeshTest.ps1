# ============================================================================
# tools\MeshTest.ps1 - the script-built meshes are closed, face out and run
# their u one way (code-review C435, C436, C404). Finds Blender and runs
# tools\MeshTest.py inside it, which reads the shipped files in assets\models.
#
#   .\tools\MeshTest.ps1              # exit 0 = PASS
#   .\tools\MeshTest.ps1 -SelfTest    # every planted fault must be CAUGHT
#   .\tools\MeshTest.ps1 -Blender 'D:\Blender\blender.exe'
#   .\tools\MeshTest.ps1 -Files a.glb,b.gltf -Detail   # judge other files, say where
#
# The checks (closed / outward / normals / uspan) and the self-test's three
# faults are described in MeshTest.py's header; this wrapper only discovers
# Blender (FetchModels' own Find-Blender, lifted out by the parser - never
# pinned), runs it headless with --python-exit-code (a traceback must not read
# as a pass), relays the judge's lines and ends on its verdict.
#
# Needs no build and writes nothing. Exit: 0 PASS (or, under -SelfTest, every
# planted fault caught and nothing else); 1 FAIL; 2 nothing judged (no Blender
# here, or no file to read). One verdict line:
# `meshtest RESULT=... checks=N failures=M self_test=N [caught=N]`.
#
# ASCII ONLY: PS 5.1 reads a BOM-less .ps1 as ANSI.
# ============================================================================
[CmdletBinding()]
param(
	[switch]$SelfTest,
	[switch]$Detail,
	[string[]]$Files = @(),
	[string]$Blender = ''
)

$ErrorActionPreference = 'Stop'
$tool = 'meshtest'

# --- FetchModels' own Find-Blender, lifted by the parser ----------------------
$fetchModels = Join-Path $PSScriptRoot 'FetchModels.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($fetchModels, [ref]$null, [ref]$null)
$fn = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
	$n.Name -eq 'Find-Blender' }, $true)
if (-not $fn) { throw 'FetchModels.ps1 has no function Find-Blender to lift' }
. ([ScriptBlock]::Create($fn.Extent.Text))

if (-not $Blender) { $Blender = Find-Blender }
if (-not $Blender -or -not (Test-Path $Blender)) {
	Write-Host "  no Blender here (looked under $env:ProgramFiles\Blender Foundation) - nothing judged" -ForegroundColor Yellow
	Write-Host "$tool RESULT=FAIL checks=0 failures=0 self_test=$([int]$SelfTest.IsPresent)"
	exit 2
}
Write-Host "Blender: $Blender"

# `-Files a,b` arrives as ONE string through `powershell -File`; split it here.
$Files = @($Files | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$judgeArgs = @()
if ($SelfTest) { $judgeArgs += '--selftest' }
if ($Detail) { $judgeArgs += '--detail' }
$judgeArgs += $Files

$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
	$lines = @(& $Blender --background --factory-startup --python-exit-code 1 `
		--python (Join-Path $PSScriptRoot 'MeshTest.py') -- @judgeArgs 2>&1 | ForEach-Object { "$_" })
} finally { $ErrorActionPreference = $prev }
$code = $LASTEXITCODE

# The judge's own lines; Blender's start-up and quit chatter is left out, but a
# traceback is shown whole, since it is the answer to "why no verdict".
$verdict = $null
$traceback = $false
foreach ($l in $lines) {
	if ($l -match "^$tool RESULT=") { $verdict = $l; continue }
	if ($l -match '^Traceback') { $traceback = $true }
	if ($traceback -or $l -match "^(  \[|  selftest:|      |$($tool):)") { Write-Host $l }
}
if (-not $verdict) {
	Write-Host "  the judge printed no verdict (Blender exit $code)" -ForegroundColor Red
	Write-Host "$tool RESULT=FAIL checks=0 failures=0 self_test=$([int]$SelfTest.IsPresent)"
	exit $(if ($code -eq 2) { 2 } else { 1 })
}
Write-Host ''
Write-Host $verdict
exit $code
