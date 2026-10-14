# Measures latency per ini variant with NVIDIA's latency report (LatencyStats=1 in the ini). For each
# variant the keys are set in the running game's MHW-DLSS4.ini (hot reload), then the script waits for
# two reports and prints the second one.
#   latencytest.ps1 -Variants 'LowLatency=Off;ReflexSleep=0', 'LowLatency=On', 'LowLatency=On;FrameLimit=60'
# Keys missing from the ini are appended at its end; the plugin reads keys from any section.
param(
    [Parameter(Mandatory)][string[]]$Variants,
    [int]$Reports = 2,
    [string]$Ini = 'F:\SteamLibrary\steamapps\common\Monster Hunter World\nativePC\plugins\MHW-DLSS4.ini'
)
$log = Join-Path (Split-Path $Ini) 'MHW-DLSS4.log'

function Set-IniKeys([string[]]$pairs) {
    $text = [IO.File]::ReadAllText($Ini)
    $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $lines = [System.Collections.Generic.List[string]]($text -split '\r?\n')
    if ($lines.Count -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
    foreach ($kv in $pairs) {
        $key = $kv.Split('=')[0]
        $idx = -1
        for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match "^$([regex]::Escape($key))=") { $idx = $i } }
        if ($idx -ge 0) { $lines[$idx] = $kv } else { $lines.Add($kv) }
    }
    [IO.File]::WriteAllText($Ini, ($lines -join $nl) + $nl, [Text.Encoding]::ASCII)
}

foreach ($variant in $Variants) {
    if (-not (Get-Process MonsterHunterWorld -ErrorAction SilentlyContinue)) { 'game is not running'; break }
    $start = (Get-Content $log).Count
    Set-IniKeys ($variant -split ';')
    $deadline = (Get-Date).AddSeconds(8 + 6 * $Reports)
    $lines = @()
    while ((Get-Date) -lt $deadline) {
        Start-Sleep 1
        $new = Get-Content $log | Select-Object -Skip $start
        $reload = ($new | Select-String 'ini reloaded' | Select-Object -First 1)
        if (-not $reload) { continue }
        $after = $new | Select-Object -Skip ($reload.LineNumber)
        $lines = @($after | Select-String 'latency over')
        if ($lines.Count -ge $Reports) { break }
    }
    $line = if ($lines.Count) { $lines[$lines.Count - 1].Line } else { '(no report)' }
    "{0,-40} {1}" -f $variant, ($line -replace '^.*latency over ', '')
}
