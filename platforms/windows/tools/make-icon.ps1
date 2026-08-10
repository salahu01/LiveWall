<#
.SYNOPSIS
    Draws res/AppIcon.ico and res/TrayIcon.ico.

.DESCRIPTION
    Generated rather than committed as a binary, so the icon is reviewable as
    code and the repository carries no opaque assets. The mark is the same one
    the macOS app draws: a rounded square with a soft diagonal sweep, which at
    16x16 in the notification area resolves to a legible two-tone tile.

    The tray icon is drawn separately and flatter — Windows renders it at 16x16
    against a taskbar that may be light or dark, and the detail that reads well
    in a 256x256 app icon turns to mud at that size.

    Uses System.Drawing, which ships with Windows PowerShell 5.1 and with the
    .NET desktop runtime. On PowerShell 7 without that runtime, run this under
    `powershell.exe` rather than `pwsh`.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$resources = Join-Path $root 'res'
New-Item -ItemType Directory -Force -Path $resources | Out-Null

function New-AppBitmap {
    param([int]$Size)

    $bitmap = New-Object System.Drawing.Bitmap($Size, $Size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D]::SmoothingMode::AntiAlias
    $graphics.Clear([System.Drawing.Color]::Transparent)

    $inset = [Math]::Max(1, [int]($Size * 0.06))
    $radius = [Math]::Max(2, [int]($Size * 0.22))
    $rect = New-Object System.Drawing.Rectangle($inset, $inset,
        ($Size - 2 * $inset), ($Size - 2 * $inset))

    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $radius * 2
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
    $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    # The same deep blue-violet the procedural gradient uses, so the icon and
    # the wallpaper it stands for are recognisably the same thing.
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 10, 13, 28),
        [System.Drawing.Color]::FromArgb(255, 41, 26, 77),
        45.0)
    $graphics.FillPath($brush, $path)

    # The sweep: one soft diagonal band, which is what makes the tile read as a
    # moving surface rather than a flat swatch.
    $sweep = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(0, 8, 48, 61),
        [System.Drawing.Color]::FromArgb(190, 8, 92, 112),
        20.0)
    $graphics.SetClip($path)
    $graphics.FillEllipse($sweep,
        [float]($rect.X - $rect.Width * 0.3), [float]($rect.Y + $rect.Height * 0.35),
        [float]($rect.Width * 1.4), [float]($rect.Height * 0.9))
    $graphics.ResetClip()

    $brush.Dispose(); $sweep.Dispose(); $path.Dispose(); $graphics.Dispose()
    return $bitmap
}

function New-TrayBitmap {
    param([int]$Size)

    $bitmap = New-Object System.Drawing.Bitmap($Size, $Size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D]::SmoothingMode::AntiAlias
    $graphics.Clear([System.Drawing.Color]::Transparent)

    # White with alpha, so it reads on both a light and a dark taskbar — the
    # notification area does not offer a template-image mechanism the way the
    # macOS menu bar does, and a coloured glyph is illegible on one of the two.
    $pen = New-Object System.Drawing.Pen(
        [System.Drawing.Color]::FromArgb(230, 255, 255, 255),
        [float]([Math]::Max(1.0, $Size / 12.0)))
    $inset = [float]($Size * 0.14)
    $rect = New-Object System.Drawing.RectangleF(
        $inset, $inset, [float]($Size - 2 * $inset), [float]($Size - 2 * $inset))
    $graphics.DrawRectangle($pen, $rect.X, $rect.Y, $rect.Width, $rect.Height)

    # A single wave inside the frame: "a moving picture", at a scale where two
    # would merge into a smudge.
    $wave = New-Object System.Drawing.Drawing2D.GraphicsPath
    $points = @()
    for ($i = 0; $i -le 12; $i++) {
        $t = $i / 12.0
        $x = $rect.X + $rect.Width * $t
        $y = $rect.Y + $rect.Height * (0.5 + 0.22 * [Math]::Sin($t * 2 * [Math]::PI))
        $points += New-Object System.Drawing.PointF([float]$x, [float]$y)
    }
    $wave.AddCurve($points)
    $graphics.DrawPath($pen, $wave)

    $wave.Dispose(); $pen.Dispose(); $graphics.Dispose()
    return $bitmap
}

# Writes a multi-resolution .ico. System.Drawing has no ICO encoder, so the
# container is assembled by hand: a 6-byte header, a 16-byte directory entry per
# image, then each image as a PNG. PNG-in-ICO is understood by every Windows
# since Vista and avoids hand-rolling a BMP with an AND mask.
function Write-Icon {
    param(
        [System.Drawing.Bitmap[]]$Bitmaps,
        [string]$Path
    )

    $streams = @()
    foreach ($bitmap in $Bitmaps) {
        $memory = New-Object System.IO.MemoryStream
        $bitmap.Save($memory, [System.Drawing.Imaging.ImageFormat]::Png)
        $streams += , $memory.ToArray()
        $memory.Dispose()
    }

    $output = [System.IO.File]::Create($Path)
    $writer = New-Object System.IO.BinaryWriter($output)

    $writer.Write([UInt16]0)                    # reserved
    $writer.Write([UInt16]1)                    # 1 = icon
    $writer.Write([UInt16]$Bitmaps.Count)

    $offset = 6 + 16 * $Bitmaps.Count
    for ($i = 0; $i -lt $Bitmaps.Count; $i++) {
        $size = $Bitmaps[$i].Width
        # 256 is stored as 0 in a single byte; every other size fits.
        $writer.Write([Byte]($(if ($size -ge 256) { 0 } else { $size })))
        $writer.Write([Byte]($(if ($size -ge 256) { 0 } else { $size })))
        $writer.Write([Byte]0)                  # palette entries
        $writer.Write([Byte]0)                  # reserved
        $writer.Write([UInt16]1)                # colour planes
        $writer.Write([UInt16]32)               # bits per pixel
        $writer.Write([UInt32]$streams[$i].Length)
        $writer.Write([UInt32]$offset)
        $offset += $streams[$i].Length
    }

    foreach ($stream in $streams) { $writer.Write($stream) }

    $writer.Flush(); $writer.Dispose(); $output.Dispose()
}

Write-Host "==> Drawing AppIcon.ico" -ForegroundColor Cyan
$appSizes = @(16, 24, 32, 48, 64, 128, 256)
$appBitmaps = $appSizes | ForEach-Object { New-AppBitmap -Size $_ }
Write-Icon -Bitmaps $appBitmaps -Path (Join-Path $resources 'AppIcon.ico')
$appBitmaps | ForEach-Object { $_.Dispose() }

Write-Host "==> Drawing TrayIcon.ico" -ForegroundColor Cyan
$traySizes = @(16, 20, 24, 32)
$trayBitmaps = $traySizes | ForEach-Object { New-TrayBitmap -Size $_ }
Write-Icon -Bitmaps $trayBitmaps -Path (Join-Path $resources 'TrayIcon.ico')
$trayBitmaps | ForEach-Object { $_.Dispose() }

Write-Host ""
Write-Host "Wrote res/AppIcon.ico and res/TrayIcon.ico - rebuild to embed them." -ForegroundColor Green
