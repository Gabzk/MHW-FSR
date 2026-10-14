# One restart configuration of an input-latency comparison: sets the game's Frame Rate option and the
# driver's Max Frame Rate, starts the game into the world (mhwharness.py to-game), then for each ini
# variant measures key press -> player movement (mhwharness.py react) and takes the plugin's latency
# report (LatencyStats=1). Prints one result line per variant.
#   latencymatrix.ps1 -Label user -FrameRate 60 -DriverCap 0 -Variants 'LowLatency=Off;ReflexSleep=0', 'LowLatency=On'
# -FrameRate: the game option ('No Limit', 30, 60, 90, 120, 144, 165, 240). -DriverCap: driver Max Frame
# Rate in the game's NVIDIA profile, 0 = inherit the global value. Both need a game start, so the game is
# restarted. graphics_option.ini is rewritten from -OptionsBackup (ASCII, no BOM) and restored by
# -Restore; the driver value is deleted again by -Restore.
param(
    [string]$Label = 'run',
    [string]$FrameRate = 'No Limit',
    [int]$DriverCap = 0,
    [string[]]$Variants = @('LowLatency=Off;ReflexSleep=0;FrameLimit=0', 'LowLatency=On;ReflexSleep=1;FrameLimit=0'),
    [int]$Count = 15,
    # The untouched graphics_option.ini: copied there by the first run, put back and deleted by -Restore.
    [string]$OptionsBackup = (Join-Path $PSScriptRoot '..\..\re\backup\graphics_option.latencymatrix.ini'),
    [switch]$Dx11,       # start the game in DirectX 11 (DirectX12Enable=Off)
    [switch]$NoRestart,  # the game already runs in the world with these settings
    [switch]$Restore
)
$ErrorActionPreference = 'Stop'
$game = 'F:\SteamLibrary\steamapps\common\Monster Hunter World'
$options = Join-Path $game 'graphics_option.ini'
$ini = Join-Path $game 'nativePC\plugins\MHW-DLSS4.ini'
$log = Join-Path $game 'nativePC\plugins\MHW-DLSS4.log'
$here = $PSScriptRoot
function Harness { & uv run --quiet python -I (Join-Path $here 'mhwharness.py') @args }
function NvProfile { & python -I (Join-Path $here 'nvprofile.py') @args }

function Set-IniKeys([string[]]$pairs) {
    $text = [IO.File]::ReadAllText($ini)
    $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $lines = [System.Collections.Generic.List[string]]($text -split '\r?\n')
    if ($lines.Count -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
    foreach ($kv in $pairs) {
        $key = $kv.Split('=')[0]
        $idx = -1
        for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match "^$([regex]::Escape($key))=") { $idx = $i } }
        if ($idx -ge 0) { $lines[$idx] = $kv } else { $lines.Add($kv) }
    }
    [IO.File]::WriteAllText($ini, ($lines -join $nl) + $nl, [Text.Encoding]::ASCII)
}

if ($Restore) {
    Harness kill | Out-Null
    NvProfile delete frl
    if (Test-Path -LiteralPath $OptionsBackup) {
        Copy-Item -LiteralPath $OptionsBackup $options -Force
        "graphics_option.ini restored: $((Get-FileHash $options).Hash -eq (Get-FileHash $OptionsBackup).Hash)"
        Remove-Item -LiteralPath $OptionsBackup
    }
    return
}
if (-not (Test-Path -LiteralPath $OptionsBackup)) {
    New-Item -ItemType Directory -Force (Split-Path $OptionsBackup) | Out-Null
    Copy-Item -LiteralPath $options $OptionsBackup
}

Set-IniKeys @('LatencyStats=1')
if (-not $NoRestart) {
    Harness kill | Out-Null
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($OptionsBackup))
    $text = [regex]::Replace($text, '(?m)^FrameRate=.*?(\r?)$', "FrameRate=$FrameRate`$1")
    if ($Dx11) { $text = [regex]::Replace($text, '(?m)^DirectX12Enable=On', 'DirectX12Enable=Off') }
    [IO.File]::WriteAllBytes($options, [Text.Encoding]::ASCII.GetBytes($text))
    if ($DriverCap -gt 0) { NvProfile set frl $DriverCap | Out-Null } else { NvProfile delete frl | Out-Null }
    Set-IniKeys ($Variants[0] -split ';')
    Start-Sleep 3  # let Steam see the game exit before starting it again
    Harness to-game | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) {
        Harness kill | Out-Null
        Start-Sleep 5
        Harness to-game | Select-Object -Last 1
        if ($LASTEXITCODE -ne 0) { throw 'to-game failed twice' }
    }
}

foreach ($variant in $Variants) {
    $before = (Get-Content $log).Count
    Set-IniKeys ($variant -split ';')
    # The plugin reloads the ini within 0.5 s; two 5 s reports later the averages are of the new settings.
    $deadline = (Get-Date).AddSeconds(25)
    do {
        Start-Sleep 1
        $new = @(Get-Content $log | Select-Object -Skip $before)
        $reloaded = ($new | Select-String 'ini reloaded' | Select-Object -First 1)
        $reports = if ($reloaded) { @($new | Select-Object -Skip $reloaded.LineNumber | Select-String 'latency over') } else { @() }
    } while ($reports.Count -lt 2 -and (Get-Date) -lt $deadline)
    if ($variant -eq $Variants[0] -and -not $reloaded) { Start-Sleep 6 }  # first variant was set before the start
    $out = Harness react w --count $Count 2>&1 | Out-String
    $pos = [regex]::Match($out, 'position change:\s+min [\d.]+\s+median ([\d.]+)').Groups[1].Value
    $read = [regex]::Match($out, 'game reads key:\s+min [\d.]+\s+median ([\d.]+)').Groups[1].Value
    $fps = [regex]::Match($out, 'frame interval during the run: ([\d.]+) ms').Groups[1].Value
    $line = (Get-Content $log | Select-String 'latency over' | Select-Object -Last 1).Line
    $gpuDone = [regex]::Match("$line", 'input to GPU done ([\d.]+) ms').Groups[1].Value
    $busy = [regex]::Match("$line", 'GPU busy ([\d.]+)').Groups[1].Value
    "{0,-6} {1} game={2,-8} driver={3,-3} {4,-48} frame {5,5} ms | key->read {6,6} | key->moved {7,6} | read->GPU done {8,5} | GPU {9,4}" -f `
        $Label, $(if ($Dx11) { 'DX11' } else { 'DX12' }), $FrameRate, $DriverCap, $variant, $fps, $read, $pos, $gpuDone, $busy
}
