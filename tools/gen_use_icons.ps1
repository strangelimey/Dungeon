# Generates the pictures a HUD hand box shows when it is SET to a bare-hand
# verb (punch, kick) as transparent PNGs: assets/ui/use_<verb>.png. The hand
# box looks the set verb up by name, so a verb gains a picture by gaining a
# shape here - no code. Same outline style as gen_slot_icons.ps1 (the paper
# doll's slots), drawn to read as the ACTION rather than the body part: a fist
# driving forward, a boot kicking up, each with motion lines behind and an
# impact burst ahead. Run from the repo; -Montage <png> also writes a review
# sheet of both on a slot-dark background.
param([string]$OutDir = "$PSScriptRoot\..\assets\ui", [string]$Montage = '')

Add-Type -AssemblyName System.Drawing
$S = 100  # canvas size

function New-Canvas {
	$bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
	$g = [System.Drawing.Graphics]::FromImage($bmp)
	$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
	$g.Clear([System.Drawing.Color]::FromArgb(0,0,0,0))
	,@($bmp,$g)
}
function New-Pen([float]$width = 3.5) {
	$p = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(230,215,215,215)), $width
	$p.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
	$p.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
	$p.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
	$p
}
function P([float]$x,[float]$y) { New-Object System.Drawing.PointF($x,$y) }
function Poly($g,$pen,$pts) { $g.DrawPolygon($pen, [System.Drawing.PointF[]]$pts) }
function Curve($g,$pen,$pts) { $g.DrawCurve($pen, [System.Drawing.PointF[]]$pts, 0.5) }
function Line($g,$pen,[float]$x1,[float]$y1,[float]$x2,[float]$y2) { $g.DrawLine($pen, $x1,$y1, $x2,$y2) }

$shapes = [ordered]@{
	# A right fist seen from the thumb side, driving right: forearm from the
	# left, the knuckle block ahead, the curled fingers as stacked bands on its
	# front face, the thumb folded across them. Motion lines trail the arm and
	# a burst marks the strike.
	punch = {
		param($g,$pen,$thin)
		Poly $g $pen @(
			(P 14 50),(P 38 45),(P 44 35),(P 74 31),(P 83 36),   # arm top, knuckles
			(P 86 48),(P 85 68),(P 78 77),(P 48 78),(P 40 71),   # front face, fingers underneath
			(P 14 71))                                           # heel of the hand back to the wrist
		Line $g $thin 64 44 85 44                               # finger bands
		Line $g $thin 62 55 86 55
		Line $g $thin 62 66 85 66
		Curve $g $pen @((P 44 60),(P 55 54),(P 68 56))           # the thumb, folded across
		Line $g $thin 3 40 22 40                                 # motion lines behind
		Line $g $thin 6 81 24 81
		Line $g $thin 2 61 10 61
		Line $g $thin 91 30 97 23                                # impact burst ahead
		Line $g $thin 92 52 98 52
		Line $g $thin 91 73 97 80
	}
	# A boot kicking up and to the right: shaft, heel, and a foot that ends in a
	# rounded toe, tilted so the toe leads. Motion lines trail below-left and a
	# burst marks the toe.
	kick = {
		param($g,$pen,$thin)
		$state = $g.Save()
		$g.TranslateTransform(50, 52)
		$g.RotateTransform(-28)
		$g.ScaleTransform(0.85, 0.85) # room for the burst inside the canvas
		$g.TranslateTransform(-50, -52)
		Poly $g $pen @(
			(P 30 14),(P 50 14),(P 50 50),                       # shaft
			(P 72 54),(P 83 58),(P 87 66),(P 84 74),             # instep, rounded toe
			(P 28 74),(P 27 60))                                 # sole back to the heel
		Line $g $thin 28 67 86 67                                # the sole's welt
		Line $g $thin 30 22 50 22                                # the boot top's cuff
		$g.Restore($state)
		Line $g $thin 4 66 20 58                                 # motion lines behind
		Line $g $thin 8 82 26 72
		Line $g $thin 18 92 34 84
		# Impact burst at the toe, which the transform above lands near (83, 48).
		Line $g $thin 88 37 93 31
		Line $g $thin 91 48 97 48
		Line $g $thin 88 58 93 64
	}
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force $OutDir | Out-Null }
$sheet = $null
if ($Montage) {
	$sheet = New-Object System.Drawing.Bitmap(($S * $shapes.Count), $S)
	$sg = [System.Drawing.Graphics]::FromImage($sheet)
	$sg.Clear([System.Drawing.Color]::FromArgb(255,30,30,34)) # dark like a slot
}
$i = 0
foreach ($name in $shapes.Keys) {
	$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
	# Heavier than the doll's 3.5: these draw at about half size in a HUD hand
	# box, where a 3.5 outline thins to under 2px and reads faint.
	$pen = New-Pen 5.5; $thin = New-Pen 4
	& $shapes[$name] $g $pen $thin
	$bmp.Save((Join-Path $OutDir "use_$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
	if ($sheet) { $sg.DrawImage($bmp, $i * $S, 0) }
	$pen.Dispose(); $thin.Dispose(); $g.Dispose(); $bmp.Dispose()
	$i++
}
if ($sheet) {
	$sheet.Save($Montage, [System.Drawing.Imaging.ImageFormat]::Png)
	$sg.Dispose(); $sheet.Dispose()
	"Montage: $Montage"
}
"Wrote $($shapes.Count) use_*.png to $OutDir"
