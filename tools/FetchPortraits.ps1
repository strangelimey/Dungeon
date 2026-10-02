# Installs the party portrait set from the local OneDrive archive.
#
# The bought packs (docs/costs.md) are archived as their original zips in
# OneDrive\DungeonAssets\ui\<folder>\ - the $sources table below, which must
# agree with tools\BuildPortraitCatalog.py's. This script extracts the image for
# EVERY id assets\portraits\portraits.cat lists - so the catalog decides what
# ships, and the full-body figures it leaves out never land - then bakes the BC7
# .dds chains beside them (AssetBaker portrait-mips, which skips any .dds
# already current). Each source maps a zip entry to an id:
#   magory  256square/256x256/<id>.png (the plain set, from the main zip or the
#           bonus one; the dithered and other sizes are not shipped)
#   corax   "coraxdigitalart-realistic-human-heroes (N).png" -> corax<NNN>
#           (512x512, kept at native size)
#
# None of the images are committed (250 MB) except the starter party's four
# defaults, so a fresh clone has faces before this is run. Run after cloning;
# the game reads assets\portraits directly.
#
# Usage:  powershell -File tools\FetchPortraits.ps1 [-Force] [-NoBake]
#   -Force   overwrite images already present
#   -NoBake  extract only (no .dds)

param(
    [switch] $Force,
    [switch] $NoBake
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$assets = Join-Path $repo "assets"
$dest = Join-Path $assets "portraits"
$catalog = Join-Path $dest "portraits.cat"
if (-not (Test-Path $catalog)) { throw "Catalog not found: $catalog (tools\BuildPortraitCatalog.py writes it)" }

$oneDrive = if ($env:OneDrive) { $env:OneDrive } else { Join-Path $env:USERPROFILE "OneDrive" }
$ui = Join-Path $oneDrive "DungeonAssets\ui"

# Folder, zips, and IdOf: a zip entry's FullName -> its portrait id, or $null
# for an entry that is not a shipped image.
$sources = @(
    @{ Folder = "magory-fantasy-portraits"
       Zips = @("fantasy-portraits-256.zip", "fantasy-portraits-bonus.zip")
       IdOf = { param($n) if ($n -match '^256square/256x256/([^/]+)\.png$') { $Matches[1] } } },
    @{ Folder = "corax-human-heroes"
       Zips = @("_coraxdigitalart-realistic-human-heroes.zip")
       IdOf = { param($n) if ($n -match '^coraxdigitalart-realistic-human-heroes \((\d+)\)\.png$') { 'corax{0:D3}' -f [int]$Matches[1] } } }
)

# Every slot that draws a portrait is square, and BC7 needs sides that are a
# multiple of 4, but some of Magory's images are a few pixels off (253x256,
# 256x250) and two are tall crops (182x256). Square those: crop to the short
# side, centred across and anchored at the TOP (a portrait's face sits high, so
# a centred crop would take the hair), then scale to 256 - or, for a bigger
# image, to the short side rounded down to a multiple of 4. A square image whose
# side is already a multiple of 4 is left byte-for-byte alone. Returns $true
# when it changed the file.
Add-Type -AssemblyName System.Drawing
function Square-Portrait([string] $path) {
    $img = [System.Drawing.Image]::FromFile($path)
    try {
        $w = $img.Width; $h = $img.Height
        if ($w -eq $h -and $w % 4 -eq 0) { return $false }
        $side = [Math]::Min($w, $h)
        $size = if ($side -le 256) { 256 } else { $side - ($side % 4) }
        $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $src = New-Object System.Drawing.Rectangle ([int](($w - $side) / 2)), 0, $side, $side
        $dst = New-Object System.Drawing.Rectangle 0, 0, $size, $size
        $attr = New-Object System.Drawing.Imaging.ImageAttributes
        $attr.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY) # no dark fringe at the edges
        $g.DrawImage($img, $dst, $src.X, $src.Y, $src.Width, $src.Height, [System.Drawing.GraphicsUnit]::Pixel, $attr)
        $g.Dispose()
    }
    finally { $img.Dispose() }
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $true
}

$ids = Select-String -Path $catalog -Pattern '^\[(.+)\]\s*$' | ForEach-Object { $_.Matches[0].Groups[1].Value }
if (-not $ids) { throw "No [id] entries in $catalog" }
Write-Host "$($ids.Count) portraits in the catalog"

Add-Type -AssemblyName System.IO.Compression.FileSystem
$opened = @()
$entries = @{}
try {
    foreach ($s in $sources) {
        foreach ($name in $s.Zips) {
            $z = Join-Path (Join-Path $ui $s.Folder) $name
            if (-not (Test-Path $z)) { throw "Portrait archive not found: $z" }
            $zip = [IO.Compression.ZipFile]::OpenRead($z)
            $opened += $zip
            foreach ($e in $zip.Entries) {
                $id = & $s.IdOf $e.FullName
                if ($id) { $entries[$id] = $e }
            }
        }
    }

    $missing = @($ids | Where-Object { -not $entries.ContainsKey($_) })
    if ($missing.Count) { throw "$($missing.Count) catalog ids are not in the archive, e.g. $($missing[0..4] -join ', ')" }

    $written = 0; $kept = 0; $squared = 0
    foreach ($id in $ids) {
        $out = Join-Path $dest "$id.png"
        if ((Test-Path $out) -and -not $Force) { $kept++; continue }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entries[$id], $out, $true)
        if (Square-Portrait $out) { $squared++ }
        # Extraction keeps the zip entry's (old) timestamp, which would make a
        # freshly written image look older than the .dds beside it - and the
        # bake skips a .dds that is current. Stamp it now so -Force rebakes.
        [IO.File]::SetLastWriteTime($out, [DateTime]::Now)
        $written++
    }
    Write-Host "Extracted $written ($squared squared up), $kept already present"
}
finally {
    foreach ($zip in $opened) { $zip.Dispose() }
}

if (-not $NoBake) {
    $baker = Join-Path $repo "build\release\bin\AssetBaker.exe"
    if (-not (Test-Path $baker)) { $baker = Join-Path $repo "build\debug\bin\AssetBaker.exe" }
    if (-not (Test-Path $baker)) { throw "Build AssetBaker first (build.cmd release)" }
    # Merge stderr as plain text (PS 5.1 would turn a logged warning into a
    # terminating error under Stop) and key success off the exit code alone.
    # The per-image "Wrote ..." lines are dropped: there are thousands.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $baker portrait-mips $assets 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -notmatch 'Wrote .*BC7 mips' } | ForEach-Object { Write-Host $_ } }
    finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "AssetBaker portrait-mips failed (exit $LASTEXITCODE)" }
}

$png = @(Get-ChildItem $dest -Filter *.png).Count
$dds = @(Get-ChildItem $dest -Filter *.dds).Count
Write-Host "assets\portraits: $png .png, $dds .dds (catalog: $($ids.Count))"
if ($png -lt $ids.Count) { throw "Fewer images than catalog entries" }
# A .png without its .dds still loads (the PNG fallback), which is exactly how a
# skipped bake would go unnoticed - so count them.
if (-not $NoBake -and $dds -lt $png) { throw "$($png - $dds) images have no .dds (see the baker's warnings above)" }
