# Captures several consecutive window grabs (client area crop) for stability measurements.
param(
    [string]$Process = 'MonsterHunterWorld',
    [Parameter(Mandatory)][string]$Prefix,
    [int]$Count = 6,
    [int]$IntervalMs = 40,
    [int]$X = 0, [int]$Y = 0, [int]$W = 0, [int]$H = 0
)
Add-Type -AssemblyName System.Drawing
if (-not ('PWB' -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class PWB {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@
}
$p = Get-Process $Process -ErrorAction Stop | Select-Object -First 1
$hwnd = $p.MainWindowHandle
$r = New-Object PWB+RECT
[void][PWB]::GetClientRect($hwnd, [ref]$r)
for ($i = 0; $i -lt $Count; $i++) {
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][PWB]::PrintWindow($hwnd, $hdc, 3)
    $g.ReleaseHdc($hdc); $g.Dispose()
    if ($W -gt 0 -and $H -gt 0) {
        $crop = $bmp.Clone((New-Object System.Drawing.Rectangle $X, $Y, $W, $H), $bmp.PixelFormat)
        $bmp.Dispose(); $bmp = $crop
    }
    $bmp.Save("$Prefix-$i.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Start-Sleep -Milliseconds $IntervalMs
}
"$Prefix x$Count"
