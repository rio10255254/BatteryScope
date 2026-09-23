# Reproduce the geometric SVG design as a Windows multi-resolution ICO.
# No downloaded assets or third-party drawing dependencies.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$destination = Join-Path $PSScriptRoot '..\src\assets\batteryscope.ico'
$sizes = @(16,20,24,32,40,48,64,128,256)
$frames = @()
function Fill-RoundRect($graphics, $brush, [single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    try {
        $d = 2*$r
        $path.AddArc($x,$y,$d,$d,180,90)
        $path.AddArc(($x+$w-$d),$y,$d,$d,270,90)
        $path.AddArc(($x+$w-$d),($y+$h-$d),$d,$d,0,90)
        $path.AddArc($x,($y+$h-$d),$d,$d,90,90)
        $path.CloseFigure()
        $graphics.FillPath($brush,$path)
    } finally { $path.Dispose() }
}
foreach ($size in $sizes) {
    $large = New-Object System.Drawing.Bitmap ($size*4),($size*4)
    $g = [System.Drawing.Graphics]::FromImage($large)
    $dark = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(24,32,42))
    $mint = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(104,225,188))
    try {
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.ScaleTransform($size/8.0,$size/8.0)
        Fill-RoundRect $g $dark 1 1 30 30 7
        Fill-RoundRect $g $mint 5 10 20 12 3
        Fill-RoundRect $g $dark 7 12 16 8 1
        Fill-RoundRect $g $mint 9 14 10 4 1
        Fill-RoundRect $g $mint 25 13 3 6 1
        $bitmap = New-Object System.Drawing.Bitmap $size,$size
        $scaled = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $scaled.Clear([System.Drawing.Color]::Transparent)
            $scaled.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $scaled.DrawImage($large,0,0,$size,$size)
        } finally { $scaled.Dispose() }
        $stream = New-Object System.IO.MemoryStream
        try {
            if ($size -ge 64) {
                $bitmap.Save($stream,[System.Drawing.Imaging.ImageFormat]::Png)
            } else {
                $writer = New-Object System.IO.BinaryWriter $stream
                $stride = [int]([Math]::Ceiling($size/32.0)*4)
                $writer.Write([uint32]40); $writer.Write([int32]$size); $writer.Write([int32]($size*2))
                $writer.Write([uint16]1); $writer.Write([uint16]32)
                $writer.Write([uint32]0); $writer.Write([uint32]($size*$size*4+$stride*$size))
                1..4 | ForEach-Object { $writer.Write([uint32]0) }
                for ($y=$size-1; $y -ge 0; $y--) { for ($x=0; $x -lt $size; $x++) {
                    $c=$bitmap.GetPixel($x,$y)
                    $writer.Write([byte]$c.B); $writer.Write([byte]$c.G); $writer.Write([byte]$c.R); $writer.Write([byte]$c.A)
                } }
                for ($y=$size-1; $y -ge 0; $y--) {
                    $mask=New-Object byte[] $stride
                    for ($x=0; $x -lt $size; $x++) { if ($bitmap.GetPixel($x,$y).A -eq 0) {
                        $index=[int][Math]::Floor($x/8); $mask[$index]=$mask[$index] -bor (128 -shr ($x%8))
                    } }
                    $writer.Write($mask)
                }
                $writer.Flush()
            }
            $frames += ,$stream.ToArray()
        } finally { $stream.Dispose(); $bitmap.Dispose() }
    } finally { $g.Dispose(); $large.Dispose(); $dark.Dispose(); $mint.Dispose() }
}
$file = [System.IO.File]::Create($destination)
$output = New-Object System.IO.BinaryWriter $file
try {
    $output.Write([uint16]0); $output.Write([uint16]1); $output.Write([uint16]$sizes.Count)
    $offset=6+16*$sizes.Count
    for ($i=0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $output.Write([byte]$dimension); $output.Write([byte]$dimension)
        $output.Write([byte]0); $output.Write([byte]0); $output.Write([uint16]1); $output.Write([uint16]32)
        $output.Write([uint32]$frames[$i].Length); $output.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $output.Write([byte[]]$frame) }
} finally { $output.Dispose(); $file.Dispose() }
Get-Item -LiteralPath $destination | Select-Object FullName,Length
