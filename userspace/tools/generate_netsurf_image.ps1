param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [Parameter(Mandatory = $true)]
    [string]$Name
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$bitmap = [System.Drawing.Bitmap]::new((Resolve-Path -LiteralPath $InputPath).Path)
try {
    $isCursor = $true
    $greenPixels = 0
    $hotX = 0
    $hotY = 0

    for ($x = 0; $x -lt $bitmap.Width; $x++) {
        $pixel = $bitmap.GetPixel($x, 0)
        if ($pixel.A -eq 255) {
            if ($pixel.G -eq 255) {
                $greenPixels++
                $hotX = $x
            }
            if (($pixel.B -ne 0) -or ($pixel.R -ne 0)) {
                $isCursor = $false
                break
            }
        } elseif ($pixel.A -ne 0) {
            $isCursor = $false
            break
        }
    }

    if ($isCursor -and ($greenPixels -eq 1)) {
        for ($y = 0; $y -lt $bitmap.Height; $y++) {
            $pixel = $bitmap.GetPixel(0, $y)
            if ($pixel.A -eq 255) {
                if ($pixel.G -eq 255) {
                    $greenPixels++
                    $hotY = $y
                }
                if (($pixel.B -ne 0) -or ($pixel.R -ne 0)) {
                    $isCursor = $false
                    break
                }
            } elseif ($pixel.A -ne 0) {
                $isCursor = $false
                break
            }
        }
    } else {
        $isCursor = $false
    }

    if ($greenPixels -ne 2) {
        $isCursor = $false
    }

    $firstX = if ($isCursor) { 1 } else { 0 }
    $firstY = if ($isCursor) { 1 } else { 0 }
    $width = $bitmap.Width - $firstX
    $height = $bitmap.Height - $firstY
    $outputHotX = if ($isCursor) { $hotX - 1 } else { 0 }
    $outputHotY = if ($isCursor) { $hotY - 1 } else { 0 }

    $outputDirectory = Split-Path -Parent $OutputPath
    if ($outputDirectory) {
        New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
    }

    $encoding = [System.Text.UTF8Encoding]::new($false)
    $writer = [System.IO.StreamWriter]::new($OutputPath, $false, $encoding)
    try {
        $writer.WriteLine("/* Generated from $InputPath. */")
        $writer.WriteLine('#include <sys/types.h>')
        $writer.WriteLine('#include <stdint.h>')
        $writer.WriteLine('#include <stdbool.h>')
        $writer.WriteLine('#include <libnsfb.h>')
        $writer.WriteLine('#include "netsurf/plot_style.h"')
        $writer.WriteLine('#include "framebuffer/gui.h"')
        $writer.WriteLine('#include "framebuffer/fbtk.h"')
        $writer.WriteLine()
        $writer.WriteLine("static uint8_t ${Name}_pixdata[] = {")

        for ($y = $firstY; $y -lt $bitmap.Height; $y++) {
            $row = [System.Text.StringBuilder]::new("`t")
            for ($x = $firstX; $x -lt $bitmap.Width; $x++) {
                $pixel = $bitmap.GetPixel($x, $y)
                [void]$row.AppendFormat(
                    '0x{0:x2}, 0x{1:x2}, 0x{2:x2}, 0x{3:x2}, ',
                    $pixel.R, $pixel.G, $pixel.B, $pixel.A
                )
            }
            $writer.WriteLine($row.ToString())
        }

        $writer.WriteLine('};')
        $writer.WriteLine()
        $writer.WriteLine("struct fbtk_bitmap $Name = {")
        $writer.WriteLine("`t.width = $width,")
        $writer.WriteLine("`t.height = $height,")
        $writer.WriteLine("`t.hot_x = $outputHotX,")
        $writer.WriteLine("`t.hot_y = $outputHotY,")
        $writer.WriteLine("`t.pixdata = ${Name}_pixdata,")
        $writer.WriteLine('};')
    } finally {
        $writer.Dispose()
    }
} finally {
    $bitmap.Dispose()
}
