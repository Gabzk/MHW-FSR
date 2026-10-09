# Saves a screenshot of the primary screen (optionally scaled down) for quick checks.
param(
    [Parameter(Mandatory)][string]$Path,
    [double]$Scale = 0.5,
    [int]$X = -1, [int]$Y = -1, [int]$W = 0, [int]$H = 0  # optional crop, full-resolution coordinates
)
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$g.Dispose()
if ($W -gt 0 -and $H -gt 0) {
    $crop = $bmp.Clone((New-Object System.Drawing.Rectangle $X, $Y, $W, $H), $bmp.PixelFormat)
    $bmp.Dispose(); $bmp = $crop; $Scale = 1.0
}
if ($Scale -ne 1.0) {
    $out = New-Object System.Drawing.Bitmap ([int]($bmp.Width * $Scale)), ([int]($bmp.Height * $Scale))
    $g2 = [System.Drawing.Graphics]::FromImage($out)
    $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g2.DrawImage($bmp, 0, 0, $out.Width, $out.Height)
    $g2.Dispose(); $bmp.Dispose(); $bmp = $out
}
$bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
$Path
