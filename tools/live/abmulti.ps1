# A/B with several captures per setting (hot reload), cropped to a region, for averaging out
# animation. Each run's settings are applied on top of the previous ones (list everything that
# matters). Writes <prefix>_<run>_<n>.png.
param(
    [Parameter(Mandatory)][System.Collections.Specialized.OrderedDictionary]$Runs,
    [string]$Prefix = 'mm',
    [int]$Frames = 4, [double]$Settle = 2.5, [double]$Interval = 0.3,
    [int]$X = 1060, [int]$Y = 600, [int]$W = 360, [int]$H = 640,
    [string]$Ini = 'F:\SteamLibrary\steamapps\common\Monster Hunter World\nativePC\plugins\MHW-DLSS4.ini',
    [string]$OutDir = (Join-Path $env:TEMP 'mhw-dlss4')
)
$s = $OutDir
New-Item -ItemType Directory -Force $s | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class PW2 {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@
$hwnd = (Get-Process MonsterHunterWorld).MainWindowHandle
function Grab([string]$path) {
    $r = New-Object PW2+RECT
    [PW2]::GetClientRect($hwnd, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $gr = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gr.GetHdc(); [PW2]::PrintWindow($hwnd, $hdc, 3) | Out-Null; $gr.ReleaseHdc($hdc); $gr.Dispose()
    $crop = $bmp.Clone((New-Object System.Drawing.Rectangle $X, $Y, $W, $H), $bmp.PixelFormat)
    $crop.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $crop.Dispose(); $bmp.Dispose()
}
function SetKeys([string[]]$kv) {
    $lines = [System.Collections.Generic.List[string]](Get-Content -LiteralPath $Ini)
    foreach ($p in $kv) {
        $key = $p.Split('=')[0]; $idx = -1
        for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match "^$([regex]::Escape($key))=") { $idx = $i } }
        if ($idx -ge 0) { $lines[$idx] = $p } else { $lines.Add($p) }
    }
    [IO.File]::WriteAllLines($Ini, $lines, [Text.Encoding]::ASCII)
}
foreach ($k in $Runs.Keys) {
    SetKeys $Runs[$k]
    Start-Sleep -Milliseconds ([int]($Settle * 1000))
    for ($n = 0; $n -lt $Frames; $n++) {
        Grab "$s\${Prefix}_${k}_$n.png"
        Start-Sleep -Milliseconds ([int]($Interval * 1000))
    }
    "$k $(Get-Date -Format HH:mm:ss.fff)"
}
