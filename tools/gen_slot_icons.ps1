# Generates equipment-slot outline silhouettes (head/body/legs/feet/cloak/
# amulet/hand/ring) as transparent PNGs for the inventory doll's empty slots.
# Outline only (a "ghost" the equipped item's icon later covers). Run from repo;
# writes assets/textures/slot_<type>.png. Also writes a montage for review.
param([string]$OutDir = "$PSScriptRoot\..\assets\textures")

Add-Type -AssemblyName System.Drawing
$S = 100  # canvas size

function New-Canvas {
	$bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
	$g.Clear([System.Drawing.Color]::FromArgb(0,0,0,0))
	,@($bmp,$g)
}
function New-Pen {
	$p = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(200,210,210,210)), 3.5
	$p.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
	$p.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
	$p.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
	$p
}
function P([float]$x,[float]$y) { New-Object System.Drawing.PointF($x,$y) }
function Poly($g,$pen,$pts) { $g.DrawPolygon($pen, [System.Drawing.PointF[]]$pts) }
function Curve($g,$pen,$pts) { $g.DrawCurve($pen, [System.Drawing.PointF[]]$pts, 0.5) }

$shapes = @{
	# Person bust: head circle + a smooth shoulders curve.
	head = {
		param($g,$pen)
		$g.DrawEllipse($pen, 36,12,28,28)
		Curve $g $pen @((P 16 90),(P 22 64),(P 38 54),(P 50 52),(P 62 54),(P 78 64),(P 84 90))
	}
	# Short-sleeve shirt: neckline dip, shoulders, sleeves, down to the waist.
	body = {
		param($g,$pen)
		Poly $g $pen @((P 40 18),(P 32 22),(P 18 32),(P 26 46),(P 30 48),(P 30 84),
			(P 70 84),(P 70 48),(P 74 46),(P 82 32),(P 68 22),(P 60 18),(P 50 30))
	}
	# Trousers: waistband and two legs with a centre notch.
	legs = {
		param($g,$pen)
		Poly $g $pen @((P 32 16),(P 68 16),(P 68 86),(P 54 86),(P 50 50),(P 46 86),(P 32 86))
	}
	# A single boot: vertical shaft + foot.
	feet = {
		param($g,$pen)
		Poly $g $pen @((P 38 16),(P 56 16),(P 56 62),(P 72 62),(P 72 82),(P 38 82))
	}
	# Cape: collar V flaring to a wide hem.
	cloak = {
		param($g,$pen)
		Poly $g $pen @((P 38 18),(P 50 26),(P 62 18),(P 66 32),(P 82 84),(P 18 84),(P 34 32))
	}
	# Pendant: a V chain to a diamond.
	amulet = {
		param($g,$pen)
		$g.DrawLine($pen, 34,20, 50,58)
		$g.DrawLine($pen, 66,20, 50,58)
		Poly $g $pen @((P 50 56),(P 60 68),(P 50 80),(P 40 68))
	}
	# Glove/mitten: a rounded hand blob with a thumb.
	hand = {
		param($g,$pen)
		$g.DrawClosedCurve($pen, [System.Drawing.PointF[]]@(
			(P 42 80),(P 42 50),(P 46 48),(P 48 34),(P 53 34),(P 55 48),
			(P 62 48),(P 66 40),(P 70 44),(P 66 56),(P 60 58),(P 60 80)), 0.4,
			[System.Drawing.Drawing2D.FillMode]::Alternate)
	}
	# Ring: an annulus with a gem.
	ring = {
		param($g,$pen)
		$g.DrawEllipse($pen, 36,42,28,28)
		$g.DrawEllipse($pen, 44,50,12,12)
		Poly $g $pen @((P 50 30),(P 58 40),(P 50 48),(P 42 40))
	}
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force $OutDir | Out-Null }
$montage = New-Object System.Drawing.Bitmap(($S*4), ($S*2))
$mg = [System.Drawing.Graphics]::FromImage($montage)
$mg.Clear([System.Drawing.Color]::FromArgb(255,30,30,34)) # dark like a slot
$i = 0
foreach ($name in @('head','body','legs','feet','cloak','amulet','hand','ring')) {
	$c = New-Canvas; $bmp = $c[0]; $g = $c[1]; $pen = New-Pen
	& $shapes[$name] $g $pen
	$path = Join-Path $OutDir "slot_$name.png"
	$bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
	$mg.DrawImage($bmp, ($i % 4)*$S, [int]($i/4)*$S)
	$pen.Dispose(); $g.Dispose(); $bmp.Dispose()
	$i++
}
$montagePath = Join-Path $PSScriptRoot "..\docs\slot_icons_montage.png"
$montage.Save($montagePath, [System.Drawing.Imaging.ImageFormat]::Png)
$mg.Dispose(); $montage.Dispose()
"Wrote 8 slot_*.png to $OutDir"
"Montage: $montagePath"
