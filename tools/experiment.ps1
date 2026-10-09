# Runs a list of ini variants against the running game and prints detail/flicker metrics for each.
#   experiment.ps1 -Variants @{ taa='Bypass=1'; dlss='Bypass=0' } [-Order taa,dlss]
param(
    [Parameter(Mandatory)][hashtable]$Variants,
    [string[]]$Order,
    [double]$Settle = 4,
    [int]$Count = 6,
    [string]$OutDir = (Join-Path $env:TEMP 'mhwdlss\exp'),
    # name = x,y,w,h in window pixels
    [hashtable]$Regions = @{ bark = @(200, 500, 300, 300); rock = @(1940, 580, 200, 200) }
)
$tools = $PSScriptRoot
New-Item -ItemType Directory -Force $OutDir | Out-Null
Get-ChildItem $OutDir -Filter '*.png' | Remove-Item
if (-not $Order) { $Order = $Variants.Keys }
foreach ($name in $Order) {
    $set = @($Variants[$name])
    & "$tools\try.ps1" -Set $set -Name '_settle' -Wait $Settle -X 0 -Y 0 -W 8 -H 8 | Out-Null
    foreach ($r in $Regions.Keys) {
        $g = $Regions[$r]
        & "$tools\burst.ps1" -Prefix "$OutDir\$($r)_$name" -Count $Count -X $g[0] -Y $g[1] -W $g[2] -H $g[3] | Out-Null
    }
}
$prefixes = foreach ($name in $Order) { foreach ($r in $Regions.Keys) { "$OutDir\$($r)_$name" } }
uv run --quiet --with pillow --with numpy python -I "$tools\metrics.py" @prefixes
