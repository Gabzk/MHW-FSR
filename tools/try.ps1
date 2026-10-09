# Experiment helper: sets keys in the deployed MHWDLSS.ini (the plugin hot-reloads it), waits,
# then captures the game window.
#   try.ps1 -Set 'AutoExposure=0','HDR=0' -Name exp1 [-Wait 3] [-Scale 0.5]
param(
    [string[]]$Set = @(),
    [Parameter(Mandatory)][string]$Name,
    [double]$Wait = 3,
    [double]$Scale = 0.5,
    [int]$X = 0, [int]$Y = 0, [int]$W = 0, [int]$H = 0,
    [string]$Ini = 'F:\SteamLibrary\steamapps\common\Monster Hunter World\nativePC\plugins\MHWDLSS.ini',
    [string]$OutDir = (Join-Path $env:TEMP 'mhwdlss')
)
New-Item -ItemType Directory -Force $OutDir | Out-Null
$lines = [System.Collections.Generic.List[string]](Get-Content -LiteralPath $Ini)
foreach ($kv in $Set) {
    $key = $kv.Split('=')[0]
    $idx = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match "^$([regex]::Escape($key))=") { $idx = $i } }
    if ($idx -ge 0) { $lines[$idx] = $kv } else { $lines.Add($kv) }
}
if ($Set.Count) { Set-Content -LiteralPath $Ini -Value $lines }
Start-Sleep -Milliseconds ([int]($Wait * 1000))
& (Join-Path $PSScriptRoot 'capture-window.ps1') -Path (Join-Path $OutDir "$Name.png") -Scale $Scale -X $X -Y $Y -W $W -H $H
