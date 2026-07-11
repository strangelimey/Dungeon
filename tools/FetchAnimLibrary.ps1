# Bakes a creature's STATE-ORGANIZED animation library (one folder per
# CreatureState, >=1 .fbx each) onto its mesh and into assets/models, then prints
# the monsters.cat rows to paste under the creature's [id] block. The mesh analog
# of FetchModels.ps1 for the animation side.
#
# It chains, per creature:
#     <mesh.fbx> + <library_root>  --(Blender ImportAnimLibrary.py)-->
#         assets/models/<Name>.gltf   (state-named clips on the bound mesh)
#       + <Name>.anim.cat             (the states + anim_<state> rows)
#
# Every Mixamo clip shares one standard skeleton, so a single rigid bind takes any
# number of clips — populate the state folders and re-run; no re-binding.
#
# ARCHIVE LAYOUT (not committed; the raw .fbx live in OneDrive):
#   OneDrive\DungeonAssets\anim\<library>\         <- shared/humanoid clip library
#       Idle\ ...fbx   InCombat\ ...fbx   Attack\ ...fbx   Walk\ Run\ Flee\ ...
#   OneDrive\DungeonAssets\fab\monsters\<pack>\    <- the creature's source mesh
#       <mesh>.fbx
#
# Usage:
#   powershell -File tools\FetchAnimLibrary.ps1                 # all table entries
#   powershell -File tools\FetchAnimLibrary.ps1 -Creatures skel_human
#   powershell -File tools\FetchAnimLibrary.ps1 -Plan          # dry run (no Blender)
#   powershell -File tools\FetchAnimLibrary.ps1 -Blender "C:\...\blender.exe"

param(
    [string[]] $Creatures = @(),
    [switch]   $Plan,
    [string]   $Blender = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$assets = Join-Path $repo "assets"
$script = Join-Path $PSScriptRoot "ImportAnimLibrary.py"

$oneDrive = if ($env:OneDrive) { $env:OneDrive } else { Join-Path $env:USERPROFILE "OneDrive" }
$archive = Join-Path $oneDrive "DungeonAssets"
if (-not $Plan -and -not (Test-Path $archive)) { throw "Asset archive not found: $archive" }

# Resolve Blender (only needed for the actual bake, not -Plan).
if (-not $Plan -and -not $Blender) {
    $candidates = @(
        "$env:ProgramFiles\Blender Foundation\Blender 5.1\blender.exe",
        "$env:ProgramFiles\Blender Foundation\Blender 5.0\blender.exe"
    )
    $Blender = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Blender) { throw "Blender not found - pass -Blender <path>" }
}

# Run Blender headless; key success off the exit code only (its stderr warnings
# would otherwise abort under -ErrorActionPreference Stop). Same as FetchModels.
function Invoke-Blender {
    param([Parameter(ValueFromRemainingArguments = $true)] $scriptArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Blender --background --factory-startup --python $script -- @scriptArgs 2>&1 |
            ForEach-Object { Write-Host "$_" }
    } finally { $ErrorActionPreference = $prev }
    return $LASTEXITCODE
}

# Find python for the -Plan dry run (Blender not required for plan mode).
function Resolve-Python {
    foreach ($c in @("python", "py")) {
        $p = Get-Command $c -ErrorAction SilentlyContinue
        if ($p) { return $p.Source }
    }
    throw "Python not found for -Plan; install python or run without -Plan"
}

# ===========================================================================
# The creature table. Each entry is one OUTPUT model. Fields:
#   Name     output model -> assets/models/<Name>.gltf and the monsters.cat id
#   Mesh     creature source mesh, relative to the archive (the bind target)
#   Library  state-organized clip library root, relative to the archive
#            (one subfolder per CreatureState; see ImportAnimLibrary.py)
#   Ref      existing model in assets/ to match in-engine height/ground to
#            (relative to assets/), e.g. models\skeleton.gltf
#   MeshYaw  degrees to rotate the mesh to co-face the +Y Mixamo armature
#            (default 90; a mesh that faces the armature already wants 0)
# Entries install only once their archive paths exist (missing = skipped).
# ===========================================================================
$animSets = @(
    @{ Name = "skel_human";
       Mesh = "fab\monsters\lowpoly-human-skeleton\skeleton_mixamo_upload.fbx";
       Library = "anim\humanoid";
       Ref = "models\skeleton.gltf";
       MeshYaw = 90 }

    # Skeleton Army Kit (Konjo Design, fab 2026-07-10): four PRE-BUILT armed
    # variants. The unrigged A-pose OBJs defeated the scripted rigid bind
    # (shield bound to the pelvis; the arm-drop re-rest stole hip geometry), so
    # each variant goes through MIXAMO'S AUTO-RIGGER instead: upload FBXs live
    # in the archive's mixamo_upload\, the skinned T-pose downloads in
    # mixamo_rigged\<name>_rigged.fbx (Skinned=$true skips the bind — the clip
    # actions attach straight onto the download's own Mixamo skeleton).
    # Textures maps material slot -> albedo[:normal] under TexDir (the kit's
    # MTLs point at the author's desktop and omit normals); the bake keeps the
    # material slots and embeds the images — the engine's multi-material
    # monster path renders one primitive per material.
    $(
        $kitTex = @{
            'Skeleton_Bones_Mat' = 'Skeleton_C1.png:Skeleton_N1.png'
            'Skelly_armor'       = 'Skelly_armor_C2.png:Skelly_armor_N2.png'
            'Berserker_weps'     = 'Berserker_weps1_albedo.png:Berserker_weps1_normal.png'
            'Skel_war_weps'      = 'Skel_warrior_wep_C.png:Skel_warrior_wep_N1.png'
        }
        $kitDir = "fab\monsters\skeleton-army"
        foreach ($v in @("skel_warrior", "skel_berserker", "skel_spearman", "skel_bare")) {
            @{ Name = $v;
               Mesh = "$kitDir\mixamo_rigged\$($v)_rigged.fbx";
               Library = "anim\humanoid";
               Ref = "models\skeleton.gltf";
               Skinned = $true;
               RigidIslands = $true; # bones + plate: every island is a hard piece
               # The clean pre-Mixamo art: repairs rest positions Mixamo's
               # weight-driven re-posing displaced (arm plates at the chest).
               Original = "$kitDir\mixamo_upload\$($v)_mixamo_upload.fbx";
               Textures = $kitTex;
               TexDir = "$kitDir\obj\Skeleton_obj\Skeletal_textures" }
        }
    )
)

$wanted = $Creatures.Count -gt 0
$done = 0
foreach ($c in $animSets) {
    if ($wanted -and ($Creatures -notcontains $c.Name)) { continue }

    $library = Join-Path $archive $c.Library
    $modelsDir = Join-Path $assets "models"
    $outGltf = Join-Path $modelsDir "$($c.Name).gltf"
    $catOut = Join-Path $modelsDir "$($c.Name).anim.cat"

    Write-Host ""
    Write-Host "=== $($c.Name)  <-  $($c.Library) ==="

    if (-not (Test-Path $library)) { Write-Host "  library $($c.Library) missing - skipped"; continue }

    if ($Plan) {
        $py = Resolve-Python
        & $py $script --plan $library --catalog-out $catOut
        if ($LASTEXITCODE -ne 0) { throw "Plan failed for $($c.Name)" }
        $done++
        continue
    }

    $mesh = Join-Path $archive $c.Mesh
    if (-not (Test-Path $mesh)) { Write-Host "  mesh $($c.Mesh) missing - skipped"; continue }
    $ref = Join-Path $assets $c.Ref
    if (-not (Test-Path $ref)) { throw "Ref model not found: $ref" }
    $yaw = if ($null -ne $c.MeshYaw) { $c.MeshYaw } else { 90 }

    # Multi-material kits: compose the material->texture wiring for the bake.
    $texArgs = @()
    if ($c.Textures) {
        $texSpec = ($c.Textures.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ';'
        $texArgs = @('--textures', $texSpec, '--tex-dir', (Join-Path $archive $c.TexDir))
    }
    if ($c.ArmDrop) { $texArgs += @('--arm-drop', $c.ArmDrop) }
    if ($c.Skinned) { $texArgs += @('--skinned') }
    if ($c.RigidIslands) { $texArgs += @('--rigid-islands') }
    if ($c.Original) { $texArgs += @('--original', (Join-Path $archive $c.Original)) }

    $rc = Invoke-Blender $mesh $library $outGltf $ref "--catalog-out" $catOut "--mesh-yaw" $yaw @texArgs
    if ($rc -ne 0) { throw "Bake failed for $($c.Name)" }
    Write-Host "  model -> models\$($c.Name).gltf"
    Write-Host "  catalog rows -> models\$($c.Name).anim.cat"
    $done++
}

Write-Host ""
if ($done -eq 0) {
    Write-Host "Nothing baked. Populate a state-organized library under"
    Write-Host "$archive\anim\<library>\ (Idle\ Attack\ Walk\ ...), then re-run."
} else {
    Write-Host "$done creature(s) processed. Paste each <Name>.anim.cat block's rows"
    Write-Host "into its monsters.cat [id] (or check the boxes in the editor's monster"
    Write-Host "config dialog), then build (or robocopy assets) to pick up the model."
}
