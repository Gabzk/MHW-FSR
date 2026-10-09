# Captures a window's content (DirectX included) with PrintWindow(PW_RENDERFULLCONTENT), without
# bringing it to the foreground.
param(
    [string]$Process = 'MonsterHunterWorld',
    [Parameter(Mandatory)][string]$Path,
    [double]$Scale = 1.0,
    [int]$X = 0, [int]$Y = 0, [int]$W = 0, [int]$H = 0  # optional crop in window pixels
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class PW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@
$p = Get-Process $Process -ErrorAction Stop | Select-Object -First 1
$hwnd = $p.MainWindowHandle
$r = New-Object PW+RECT
[void][PW]::GetClientRect($hwnd, [ref]$r)
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [PW]::PrintWindow($hwnd, $hdc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc); $g.Dispose()
if ($W -gt 0 -and $H -gt 0) {
    $crop = $bmp.Clone((New-Object System.Drawing.Rectangle $X, $Y, $W, $H), $bmp.PixelFormat)
    $bmp.Dispose(); $bmp = $crop
}
if ($Scale -ne 1.0) {
    $out = New-Object System.Drawing.Bitmap ([int]($bmp.Width * $Scale)), ([int]($bmp.Height * $Scale))
    $g2 = [System.Drawing.Graphics]::FromImage($out)
    $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g2.DrawImage($bmp, 0, 0, $out.Width, $out.Height); $g2.Dispose(); $bmp.Dispose(); $bmp = $out
}
$bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"{0} ({1})" -f $Path, $(if ($ok) { 'ok' } else { 'PrintWindow failed' })
