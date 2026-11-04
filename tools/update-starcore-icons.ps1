<#
.SYNOPSIS
Rebuilds Windows and WebUI icon formats from the unmodified StarCore Android artwork.
.PARAMETER Source
Path to StarCore Android's icon_app.png.
#>
param([string]$Source = 'E:/project/github/StarCoreAndroid/app/src/main/res/mipmap-xhdpi/icon_app.png')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$repo = Split-Path $PSScriptRoot -Parent
$images = Join-Path $repo 'src_assets/common/assets/web/public/images'
$original = [System.Drawing.Image]::FromFile($Source)
try {
    Copy-Item -LiteralPath $Source -Destination (Join-Path $repo 'sunshine.png')
    foreach ($state in @('default', 'playing', 'pausing', 'locked')) {
        $bitmap = [System.Drawing.Bitmap]::new(256, 256)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.DrawImage($original, 0, 0, 256, 256)
        if ($state -ne 'default') {
            $graphics.FillEllipse([System.Drawing.Brushes]::Black, 158, 158, 96, 96)
            $graphics.FillEllipse([System.Drawing.Brushes]::Gold, 164, 164, 84, 84)
            if ($state -eq 'playing') {
                $points = [System.Drawing.Point[]]@([System.Drawing.Point]::new(190,180), [System.Drawing.Point]::new(190,231), [System.Drawing.Point]::new(227,206))
                $graphics.FillPolygon([System.Drawing.Brushes]::Black, $points)
            } elseif ($state -eq 'pausing') {
                $graphics.FillRectangle([System.Drawing.Brushes]::Black, 187, 184, 13, 44)
                $graphics.FillRectangle([System.Drawing.Brushes]::Black, 213, 184, 13, 44)
            } else {
                $pen = [System.Drawing.Pen]::new([System.Drawing.Color]::Black, 7)
                $graphics.DrawArc($pen, 192, 180, 28, 34, 180, 180)
                $graphics.FillRectangle([System.Drawing.Brushes]::Black, 186, 201, 40, 30)
                $pen.Dispose()
            }
        }
        $graphics.Dispose()
        $stem = if ($state -eq 'default') { 'logo-sunshine' } else { "sunshine-$state" }
        $iconName = if ($state -eq 'default') { 'sunshine.ico' } else { "sunshine-$state.ico" }
        $frames = @()
        foreach ($size in @(16, 24, 32, 45, 48, 64, 128, 256)) {
            $scaled = [System.Drawing.Bitmap]::new($size, $size)
            $g = [System.Drawing.Graphics]::FromImage($scaled)
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.DrawImage($bitmap, 0, 0, $size, $size)
            $g.Dispose()
            $stream = [System.IO.MemoryStream]::new()
            $scaled.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
            $bytes = $stream.ToArray()
            if ($size -in @(16, 45)) { [System.IO.File]::WriteAllBytes((Join-Path $images "$stem-$size.png"), $bytes) }
            if ($size -eq 256) {
                if ($state -ne 'default') { [System.IO.File]::WriteAllBytes((Join-Path $images "$stem.png"), $bytes) }
                $base64 = [Convert]::ToBase64String($bytes)
                $svg = '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="256" height="256" viewBox="0 0 256 256"><image width="256" height="256" xlink:href="data:image/png;base64,' + $base64 + '"/></svg>'
                [System.IO.File]::WriteAllText((Join-Path $images "$stem.svg"), $svg)
                if ($state -eq 'default') { [System.IO.File]::WriteAllText((Join-Path $repo 'sunshine.svg'), $svg) }
            }
            $frames += @{ Size = $size; Bytes = $bytes }
            $stream.Dispose()
            $scaled.Dispose()
        }
        $output = [System.IO.MemoryStream]::new()
        $writer = [System.IO.BinaryWriter]::new($output)
        $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$frames.Count)
        $offset = 6 + 16 * $frames.Count
        foreach ($frame in $frames) {
            $dimension = if ($frame.Size -eq 256) { 0 } else { $frame.Size }
            $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
            $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]32)
            $writer.Write([uint32]$frame.Bytes.Length); $writer.Write([uint32]$offset)
            $offset += $frame.Bytes.Length
        }
        foreach ($frame in $frames) { $writer.Write([byte[]]$frame.Bytes) }
        $iconBytes = $output.ToArray()
        [System.IO.File]::WriteAllBytes((Join-Path $images $iconName), $iconBytes)
        if ($state -eq 'default') { [System.IO.File]::WriteAllBytes((Join-Path $repo 'sunshine.ico'), $iconBytes) }
        $writer.Dispose()
        $output.Dispose()
        $bitmap.Dispose()
    }
} finally {
    $original.Dispose()
}
