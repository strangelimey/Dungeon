# Installs mesh assets bought from fab.com (or any authored-model source) into
# the engine, the way FetchTextures.ps1 installs scanned PBR sets.
#
# WHY a separate script: the runtime loader reads only glTF/GLB + OBJ, but fab
# listings most often ship FBX (and sometimes USD), and a single listing is
# frequently a PACK of many meshes. So this script chains:
#     source mesh --(Blender ConvertMesh.py, if fbx/usd or a pack)--> .glb
#                 --(AssetBaker import-model)--> assets/models/<name>.gltf
#                 --(AssetBaker import)--------> assets/textures/<set>_2k.*
# then you wire a catalog [id] at it (decorations.cat / monsters.cat / items.cat)
# and place it in a level.
#
# SELECTION RULE (read before buying): a fab listing's "Included formats" MUST
# include glb, obj, or fbx. Unreal-Engine-ONLY listings are .uasset packs the
# engine cannot read - do not buy them. Prefer glb/obj (no conversion); fbx/usd
# go through Blender. Multi-mesh packs need Split=$true so each item is its own
# model instead of one merged blob.
#
# ARCHIVE LAYOUT (not committed - the .glb/.dds are gitignored, regenerated here):
#   OneDrive\DungeonAssets\fab\<category>\<pack-name>\
#       <mesh>.fbx|.glb|.obj|.usd  +  the PBR maps (albedo/normal/roughness/...)
# Single-object FreePBR props under DungeonAssets\2k\models\ work too (point Src
# straight at them).
#
# Usage:
#   powershell -File tools\FetchModels.ps1                 # all table entries
#   powershell -File tools\FetchModels.ps1 -Materials dagger,kukri
#   powershell -File tools\FetchModels.ps1 -Blender "C:\...\blender.exe"

param(
    [string[]] $Materials = @(),
    [string]   $Blender = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$assets = Join-Path $repo "assets"
$convert = Join-Path $PSScriptRoot "ConvertMesh.py"

$oneDrive = if ($env:OneDrive) { $env:OneDrive } else { Join-Path $env:USERPROFILE "OneDrive" }
$archive = Join-Path $oneDrive "DungeonAssets"
if (-not (Test-Path $archive)) { throw "Asset archive not found: $archive" }

$baker = Join-Path $repo "build\release\bin\AssetBaker.exe"
if (-not (Test-Path $baker)) { $baker = Join-Path $repo "build\debug\bin\AssetBaker.exe" }
if (-not (Test-Path $baker)) { throw "Build AssetBaker first (build.cmd release)" }

# Resolve Blender (only needed for fbx/usd sources or Split packs).
if (-not $Blender) {
    $candidates = @(
        "$env:ProgramFiles\Blender Foundation\Blender 5.1\blender.exe",
        "$env:ProgramFiles\Blender Foundation\Blender 5.0\blender.exe"
    )
    $Blender = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

# Run AssetBaker keying success ONLY off the process exit code (its stderr
# warnings would otherwise abort the batch under -ErrorActionPreference Stop).
# Verbatim from FetchTextures.ps1.
function Invoke-Baker {
    param([Parameter(ValueFromRemainingArguments = $true)] $bakerArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $baker @bakerArgs 2>&1 | ForEach-Object { Write-Host "$_" } }
    finally { $ErrorActionPreference = $prev }
    return $LASTEXITCODE
}

# Run Blender's ConvertMesh.py headless; same exit-code-only discipline.
function Invoke-Convert {
    param([Parameter(ValueFromRemainingArguments = $true)] $scriptArgs)
    if (-not $Blender) { throw "Blender not found - pass -Blender <path> for fbx/usd/Split sources" }
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Blender --background --factory-startup --python $convert -- @scriptArgs 2>&1 |
            ForEach-Object { Write-Host "$_" }
    } finally { $ErrorActionPreference = $prev }
    return $LASTEXITCODE
}

# Find the source mesh inside a pack folder: prefer the formats that need no
# conversion, fall back to the ones that do.
function Find-Mesh {
    param([string] $dir)
    foreach ($ext in @("*.glb", "*.gltf", "*.obj", "*.fbx", "*.usdz", "*.usd", "*.usda", "*.usdc")) {
        $hit = Get-ChildItem -Path $dir -Filter $ext -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

# ===========================================================================
# The model table. Each entry is one OUTPUT model. Edit this when you buy a
# pack. Fields:
#   Src        archive folder (relative to DungeonAssets) holding the source
#              mesh + PBR maps
#   Name       output model name -> assets/models/<Name>.gltf and the catalog id
#              you point at it
#   TextureSet shared PBR set base name (files <TextureSet>_2k.*). The FIRST
#              entry that names a given set imports it from Src; later entries
#              with the same TextureSet reuse it (import-model --texture-set).
#              Omit to let import-model pack this folder's own maps as <Name>_2k.
#   Split      $true for a multi-mesh pack -> ConvertMesh --split; then Object
#              picks which split piece becomes this model.
#   Object     (Split only) the split glb basename (lowercased object name) to
#              import as Name.
#   FlipGreen  $true if the PBR normals are OpenGL (most fab/textures.com sets).
#   Height     meters; 0 = auto-fit (import-model fits largest extent to ~2 m).
#   Rig        $true for a rigged monster: convert with --keep-rig + --height and
#              drop the normalized rigged .glb straight into assets/models
#              (bypasses import-model, which strips joints). Wire via monsters.cat.
#
# The entries below are the listings we scouted - they install only once the
# matching pack is downloaded to the archive (missing Src is skipped, not fatal).
# ===========================================================================
$modelSets = @(
    # Fantasy Assassin Weapon Pack (Deepanshu) - ships glb+obj+fbx, 18 meshes;
    # one shared material. A multi-item pack -> Split, each weapon its own model,
    # all sharing the 'assassin' set (first entry imports it, rest reuse).
    @{ Src = "fab\weapons\fantasy-assassin"; Name = "dagger";        Object = "dagger";        Split = $true; TextureSet = "assassin"; FlipGreen = $true }
    @{ Src = "fab\weapons\fantasy-assassin"; Name = "kukri";         Object = "kukri";         Split = $true; TextureSet = "assassin"; FlipGreen = $true }
    @{ Src = "fab\weapons\fantasy-assassin"; Name = "poison_knife";  Object = "poison_knife";  Split = $true; TextureSet = "assassin"; FlipGreen = $true }
    @{ Src = "fab\weapons\fantasy-assassin"; Name = "throwing_blade"; Object = "throwing_blade"; Split = $true; TextureSet = "assassin"; FlipGreen = $true }

    # Single-object example (a free Deepanshu shield, glb/obj) - no Split, packs
    # its own maps as 'viking_shield_2k'.
    @{ Src = "fab\props\viking-shield"; Name = "viking_shield"; FlipGreen = $true }

    # Rigged-monster scaffold (left commented until a rigged character is bought):
    # @{ Src = "fab\monsters\skeleton-warrior"; Name = "skeleton_warrior"; Rig = $true; Height = 1.9; FlipGreen = $true }
)

$wanted = $Materials.Count -gt 0
$installed = 0
$importedSets = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$stageRoot = Join-Path $env:TEMP "DungeonMeshImport"

foreach ($m in $modelSets) {
    if ($wanted -and ($Materials -notcontains $m.Name)) { continue }

    $srcDir = Join-Path $archive $m.Src
    if (-not (Test-Path $srcDir)) { Write-Host "$($m.Name): $($m.Src) missing - skipped"; continue }
    $mesh = Find-Mesh $srcDir
    if (-not $mesh) { Write-Host "$($m.Name): no mesh in $($m.Src) - skipped"; continue }

    Write-Host ""
    Write-Host "=== $($m.Name)  <-  $($m.Src) ($(Split-Path $mesh -Leaf)) ==="
    $ext = [IO.Path]::GetExtension($mesh).ToLower()

    # --- rigged monster: convert (keep rig + normalize) straight to models ----
    if ($m.Rig) {
        $modelsDir = Join-Path $assets "models"
        $stage = Join-Path $stageRoot $m.Name
        if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
        $h = if ($m.Height) { $m.Height } else { 1.8 }
        if ((Invoke-Convert $mesh $stage "--keep-rig" "--height" $h) -ne 0) { throw "Convert failed for $($m.Name)" }
        $glb = Get-ChildItem -Path $stage -Filter *.glb -File | Select-Object -First 1
        if (-not $glb) { throw "No rigged glb produced for $($m.Name)" }
        Copy-Item $glb.FullName (Join-Path $modelsDir "$($m.Name).gltf") -Force
        Write-Host "  rigged model -> models\$($m.Name).gltf"
        # Its texture set (imported like a normal prop set).
        $flip = if ($m.FlipGreen) { @('--flip-green') } else { @() }
        if ((Invoke-Baker import $srcDir $assets "$($m.Name)_2k" @flip) -ne 0) { throw "Texture import failed for $($m.Name)" }
        $installed++
        continue
    }

    # --- pick the .glb to feed import-model -----------------------------------
    $importMesh = $mesh
    if ($m.Split) {
        $stage = Join-Path $stageRoot ($m.Src -replace '[\\/:]', '_')
        if (-not (Test-Path (Join-Path $stage "$($m.Object).glb"))) {
            if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
            if ((Invoke-Convert $mesh $stage "--split") -ne 0) { throw "Split convert failed for $($m.Src)" }
        }
        $importMesh = Join-Path $stage "$($m.Object).glb"
        if (-not (Test-Path $importMesh)) {
            Write-Host "  split produced no '$($m.Object).glb' - available:"
            Get-ChildItem -Path $stage -Filter *.glb | ForEach-Object { Write-Host "    $($_.BaseName)" }
            throw "Object '$($m.Object)' not found in split of $($m.Src)"
        }
    } elseif ($ext -in @(".fbx", ".usd", ".usda", ".usdc", ".usdz")) {
        # Non-pack but needs conversion (fbx/usd) -> one combined glb.
        $stage = Join-Path $stageRoot ($m.Src -replace '[\\/:]', '_')
        if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
        if ((Invoke-Convert $mesh $stage) -ne 0) { throw "Convert failed for $($m.Name)" }
        $importMesh = (Get-ChildItem -Path $stage -Filter *.glb -File | Select-Object -First 1).FullName
    }

    # --- import the texture set, then the model -------------------------------
    # Import the PBR set from the SOURCE folder (always, not via import-model's
    # own packing): once converted/split, the mesh lives in a temp stage dir with
    # no maps beside it, so import-model could not find them. The set base name is
    # the entry's shared TextureSet, or the model Name for a standalone prop;
    # import once per set, then point import-model at it with --texture-set.
    $set = if ($m.TextureSet) { $m.TextureSet } else { $m.Name }
    if (-not $importedSets.Contains($set)) {
        $flip = if ($m.FlipGreen) { @('--flip-green') } else { @() }
        if ((Invoke-Baker import $srcDir $assets "$($set)_2k" @flip) -ne 0) {
            throw "Texture import failed for set $set"
        }
        [void]$importedSets.Add($set)
    }

    $importArgs = @('import-model', $importMesh, $assets, $m.Name, '--texture-set', $set)
    if ($m.Height) { $importArgs += @('--height', $m.Height) }
    if ((Invoke-Baker @importArgs) -ne 0) { throw "Model import failed for $($m.Name)" }
    $installed++
}

# (AssetBaker's `import` already bakes each set's .dds mip chain, so no separate
# mips pass is needed here.)

Write-Host ""
if ($installed -eq 0) {
    Write-Host "No models installed (download a pack into $archive\fab\... first,"
    Write-Host "or check the names against the table in this script)."
} else {
    Write-Host "$installed model(s) installed. Wire a catalog [id] at each (model=<Name>,"
    Write-Host "texture=<set>), place it in a level, then build (or robocopy assets)."
}
