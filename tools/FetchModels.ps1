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

# Newest "Blender <version>" under Program Files that actually has the exe.
function Find-Blender {
    Get-ChildItem "$env:ProgramFiles\Blender Foundation" -Directory -ErrorAction SilentlyContinue |
        ForEach-Object {
            $exe = Join-Path $_.FullName "blender.exe"
            if ($_.Name -match '^Blender\s+(\d+(\.\d+)*)$' -and (Test-Path $exe)) {
                [pscustomobject]@{ Version = [version]$Matches[1]; Path = $exe }
            }
        } | Sort-Object Version -Descending | Select-Object -First 1 -ExpandProperty Path
}

$baker = Join-Path $repo "build\release\bin\AssetBaker.exe"
if (-not (Test-Path $baker)) { $baker = Join-Path $repo "build\debug\bin\AssetBaker.exe" }
if (-not (Test-Path $baker)) { throw "Build AssetBaker first (build.cmd release)" }

# Resolve Blender (only needed for fbx/usd sources or Split packs). The NEWEST
# installed version wins - a hardcoded version list silently skips every mesh
# import the day Blender updates itself (it went 5.1 -> 5.2 mid-session once).
if (-not $Blender) { $Blender = Find-Blender }

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
#   Height     UNITS (1.0 = one dungeon square — see game::kUnit); 0 = auto-fit
#              (import-model fits the largest extent to ~0.8 of a square). Every
#              size below is units: the pipeline's normalizers (ConvertMesh
#              --height/--fit, import-model --height/--lift) just scale the mesh
#              to the number they are given, and the engine multiplies models by
#              kUnit, so authoring these in units is what keeps a bought prop the
#              right size whatever a square measures. Divide a real-world metre
#              size by 2.5 to get the number to put here.
#   Rig        $true for a rigged monster: convert with --keep-rig + --height and
#              drop the normalized rigged .glb straight into assets/models
#              (bypasses import-model, which strips joints). Wire via monsters.cat.
#   Objects    comma list -> ConvertMesh --objects: keep ONLY these mesh objects
#              (packs that bundle decorative flame shells/reference junk).
#   Lift       UNITS -> import-model --lift: raise the grounded mesh to a
#              hanging height (wall fixtures render at y=0, the mesh carries it).
#   Wall       $true -> import-model --wall: back face at z=0 (mount wall),
#              room side +Z, instead of centering Z.
#   TexDir     texture folder relative to Src (when the maps aren't beside the
#              mesh, e.g. an extracted zip's textures\ sibling).
#   TexPrefix  filename prefix filter: stage only matching maps to a temp dir
#              before import (a pack whose one folder mixes several sets).
#   TexExclude filename substring to DROP from the staged maps (a prefix that
#              is itself the prefix of another set: Brazier_ vs Brazier_lamp_).
#   SplitWhole $true -> ConvertMesh --split-whole: per-object .glb export but
#              the scene normalizes as ONE (Height on combined bounds), so
#              co-located parts of one prop stay aligned; Object picks the
#              piece. Pair with Raw so import-model doesn't re-fit the piece.
#   Raw        $true -> import-model --raw: trust the glb's placement (no
#              orient/scale/ground/center/lift).
#
# The entries below are the listings we scouted - they install only once the
# matching pack is downloaded to the archive (missing Src is skipped, not fatal).
# ===========================================================================
$modelSets = @(
    # Fantasy Assassin Weapon Pack (Deepanshu) - ships glb+obj+fbx, 18 meshes;
    # one shared material. A multi-item pack -> Split, each weapon its own model,
    # the 4 weapon objects each carry their own multiple materials (steel blade,
    # brass guard, leather/wood grip). MultiMaterial -> split per weapon + keep
    # each weapon's own glTF materials in one embedded-texture .glb (downscaled),
    # rendered by the engine's multi-material path. Height = target longest extent.
    @{ Src = "fab\weapons\fantasy-assassin"; Name = "viking_dagger"; Object = "viking_dagger"; MultiMaterial = $true; Height = 0.22 }
    @{ Src = "fab\weapons\fantasy-assassin"; Name = "khukri";        Object = "khukri";        MultiMaterial = $true; Height = 0.18 }
    @{ Src = "fab\weapons\fantasy-assassin"; Name = "snake_dagger";  Object = "snake_dagger";  MultiMaterial = $true; Height = 0.20 }
    @{ Src = "fab\weapons\fantasy-assassin"; Name = "french_dagger"; Object = "french_dagger"; MultiMaterial = $true; Height = 0.20 }

    # Leather Sentinel armor (fab, free) - a single-object, single-material body
    # of armour (~2 m worn-size); no Object -> converted as one combined .glb,
    # scaled down to a floor-loot size.
    @{ Src = "fab\armor"; Name = "leather_armor"; MultiMaterial = $true; Height = 0.36 }

    # Assets Animated rigged crawlers (fab, 2026-07-10): one mesh + one material
    # each, the 4K PBR maps EMBEDDED in the FBX (the Rig path dumps + imports
    # them), full motion libraries as named actions (Atk/Dead/Hit/Idle/Walk/...).
    # Crawlers size by FIT (longest extent — leg span / body length inside the
    # 2 m cell), not standing height; fine-tune per type with the catalog's
    # modelscale field in the editor's monster dialog.
    @{ Src = "fab\monsters\centipede";    Name = "centipede";    Rig = $true; Fit = 0.72; FlipGreen = $true }
    @{ Src = "fab\monsters\giant_spider"; Name = "giant_spider"; Rig = $true; Fit = 0.68; FlipGreen = $true }

    # Perunir "Medieval Stylized Torch" (fab, 2026-07-11) — the authored wall
    # sconce (fixtures.cat [sconce]): bracket (Holder) + torch, the decorative
    # flame shells + coal dropped (the engine's particle flame burns at the
    # catalog's flame_* point). One material for both kept meshes; the pack's
    # textures folder also carries the coal's separate set, hence TexPrefix.
    # Lifted/wall-aligned to hang like the procedural sconce (y 1.10..1.75).
    @{ Src = "fab\props\medieval-stylized-torch\extracted\source"; Name = "wall_torch"
       Objects = "Holder,Torch"; Height = 0.26; Lift = 0.44; Wall = $true
       TexDir = "..\textures"; TexPrefix = "T_Torch" }

    # Mavas3D "Fantastic brazier lamp" (fab, 2026-07-11) — the authored brazier
    # (fixtures.cat [brazier]), TWO co-located parts with separate materials:
    # the ornate metal bowl + the hot-coals insert (its own set, emissive map
    # unused — the particle fire + light sell the glow). SplitWhole keeps the
    # coals seated in the bowl (each piece re-fit on its own bounds would blow
    # the 0.08 m coal bed up to bowl height); Raw imports trust that placement.
    # The vendor's texture names cross: Brazier_* = the BOWL, Brazier_lamp_* =
    # the COALS (confirmed by eye), and Brazier_ prefixes Brazier_lamp_, hence
    # the exclude.
    @{ Src = "fab\props\brazier_lamp_fbx\extracted"; Name = "brazier_bowl"
       Object = "fantasy_brazier_lamp"; SplitWhole = $true; Raw = $true; Height = 0.30
       TexDir = "Textures_brazier_lamp_2K"; TexPrefix = "Brazier_"; TexExclude = "Brazier_lamp_" }
    @{ Src = "fab\props\brazier_lamp_fbx\extracted"; Name = "brazier_coals"
       Object = "hot_coals"; SplitWhole = $true; Raw = $true; Height = 0.30
       TexDir = "Textures_brazier_lamp_2K"; TexPrefix = "Brazier_lamp_" }

    # Mavas3D "Fantastic brazier" (fab, 2026-07-11) — the same bowl WITHOUT
    # coals, one mesh + one set. Imported as spare assets (the world loads ONE
    # brazier kind — the default id's — so this can't coexist as a separate
    # placeable fixture yet; swap fixtures.cat [brazier] model/texture to use).
    @{ Src = "fab\props\brazier_fbx\extracted"; Name = "brazier_empty"; Height = 0.30
       TexDir = "Textures_brazier_2K" }
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

    # --- authored multi-material model: keep the model's own glTF materials in
    # one downscaled embedded-texture .glb (no texture-set import; the engine
    # renders the embedded textures per material). A multi-piece pack sets Object
    # (--split, one .glb per object, picked by name); a single-object model leaves
    # Object unset and converts to one combined .glb. ----------------------------
    if ($m.MultiMaterial) {
        $modelsDir = Join-Path $assets "models"
        $stage = Join-Path $stageRoot ($m.Src -replace '[\\/:]', '_')
        $glbOut = if ($m.Object) { Join-Path $stage "$($m.Object).glb" } else { $null }
        if (-not $glbOut -or -not (Test-Path $glbOut)) {
            if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
            $h = if ($m.Height) { $m.Height } else { 0.24 } # units
            $maxTex = if ($m.MaxTex) { $m.MaxTex } else { 512 }
            $cargs = @($mesh, $stage)
            if ($m.Object) { $cargs += '--split' }
            $cargs += @('--height', $h, '--max-tex', $maxTex)
            if ((Invoke-Convert @cargs) -ne 0) { throw "Convert failed for $($m.Src)" }
        }
        # Split -> the named object's .glb; single -> the one .glb produced.
        $glbOut = if ($m.Object) { Join-Path $stage "$($m.Object).glb" } `
                  else { (Get-ChildItem $stage -Filter *.glb | Select-Object -First 1).FullName }
        if (-not $glbOut -or -not (Test-Path $glbOut)) {
            Write-Host "  convert produced no '$($m.Object).glb' - available:"
            Get-ChildItem $stage -Filter *.glb | ForEach-Object { Write-Host "    $($_.BaseName)" }
            throw "Expected glb not found in convert of $($m.Src)"
        }
        Copy-Item $glbOut (Join-Path $modelsDir "$($m.Name).glb") -Force
        $mb = [math]::Round((Get-Item (Join-Path $modelsDir "$($m.Name).glb")).Length / 1MB, 1)
        Write-Host "  multi-material model -> models\$($m.Name).glb ($mb MB)"
        $installed++
        continue
    }

    # --- rigged monster: convert (keep rig + normalize) straight to models ----
    if ($m.Rig) {
        $modelsDir = Join-Path $assets "models"
        $stage = Join-Path $stageRoot $m.Name
        if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
        # Size by Fit (longest extent; crawlers) when given, else Height (Z;
        # standing creatures).
        $sizeArgs = if ($m.Fit) { @('--fit', $m.Fit) }
                    else { @('--height', $(if ($m.Height) { $m.Height } else { 0.72 })) } # units
        # fab monster FBXs usually EMBED their PBR maps — have the convert dump
        # them as loose PNGs so the set import below has something to pack.
        $texDump = Join-Path $stage "dumped_textures"
        if ((Invoke-Convert $mesh $stage "--keep-rig" @sizeArgs `
                            "--dump-images" $texDump) -ne 0) { throw "Convert failed for $($m.Name)" }
        $glb = Get-ChildItem -Path $stage -Filter *.glb -File | Select-Object -First 1
        if (-not $glb) { throw "No rigged glb produced for $($m.Name)" }
        Copy-Item $glb.FullName (Join-Path $modelsDir "$($m.Name).gltf") -Force
        Write-Host "  rigged model -> models\$($m.Name).gltf"
        # Its texture set (imported like a normal prop set). Source priority:
        # loose maps beside the mesh, a textures\ subfolder (Sketchfab-style
        # gltf downloads), then the maps dumped out of the embedded FBX.
        $texSrc = $srcDir
        if (-not (Get-ChildItem $srcDir -File -ErrorAction SilentlyContinue |
                  Where-Object { $_.Extension -in '.png', '.jpg', '.jpeg', '.tga' })) {
            if (Test-Path (Join-Path $srcDir 'textures')) { $texSrc = Join-Path $srcDir 'textures' }
            elseif (Test-Path $texDump) { $texSrc = $texDump }
        }
        $flip = if ($m.FlipGreen) { @('--flip-green') } else { @() }
        if ((Invoke-Baker import $texSrc $assets "$($m.Name)_2k" @flip) -ne 0) { throw "Texture import failed for $($m.Name)" }
        $installed++
        continue
    }

    # --- pick the .glb to feed import-model -----------------------------------
    $importMesh = $mesh
    if ($m.Split -or $m.SplitWhole) {
        $stage = Join-Path $stageRoot ($m.Src -replace '[\\/:]', '_')
        if (-not (Test-Path (Join-Path $stage "$($m.Object).glb"))) {
            if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
            $cargs = @($mesh, $stage)
            if ($m.SplitWhole) { $cargs += @('--split-whole', '--height', $m.Height) }
            else { $cargs += '--split' }
            if ($m.Objects) { $cargs += @('--objects', $m.Objects) }
            if ((Invoke-Convert @cargs) -ne 0) { throw "Split convert failed for $($m.Src)" }
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
        $cargs = @($mesh, $stage)
        if ($m.Objects) { $cargs += @('--objects', $m.Objects) }
        if ((Invoke-Convert @cargs) -ne 0) { throw "Convert failed for $($m.Name)" }
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
        # The maps may live in a sibling folder (TexDir) and share it with other
        # sets (TexPrefix stages only the matching files, since DiscoverMaps
        # binds the FIRST match per map kind).
        $texSrc = if ($m.TexDir) { [IO.Path]::GetFullPath((Join-Path $srcDir $m.TexDir)) } else { $srcDir }
        if ($m.TexPrefix) {
            $texStage = Join-Path $stageRoot "$($m.Name)_tex"
            if (Test-Path $texStage) { Remove-Item -Recurse -Force $texStage }
            New-Item -ItemType Directory -Force $texStage | Out-Null
            Get-ChildItem $texSrc -File |
                Where-Object { $_.Name -like "$($m.TexPrefix)*" -and
                               (-not $m.TexExclude -or $_.Name -notlike "$($m.TexExclude)*") } |
                Copy-Item -Destination $texStage
            $texSrc = $texStage
        }
        $flip = if ($m.FlipGreen) { @('--flip-green') } else { @() }
        if ((Invoke-Baker import $texSrc $assets "$($set)_2k" @flip) -ne 0) {
            throw "Texture import failed for set $set"
        }
        [void]$importedSets.Add($set)
    }

    $importArgs = @('import-model', $importMesh, $assets, $m.Name, '--texture-set', $set)
    if ($m.Raw) { $importArgs += @('--raw') }
    else {
        if ($m.Height) { $importArgs += @('--height', $m.Height) }
        if ($m.Lift) { $importArgs += @('--lift', $m.Lift) }
        if ($m.Wall) { $importArgs += @('--wall') }
    }
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
