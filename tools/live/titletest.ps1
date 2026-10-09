# Launches the game to the title, probes DLSS input ranges (Performance at the DLISP position, DLAA at
# the TAA position), then switches Performance <-> Quality repeatedly to try to reproduce the
# device loss. Prints relevant log lines.
param([int]$Cycles = 15)
$g = 'F:\SteamLibrary\steamapps\common\Monster Hunter World\nativePC\plugins'
$L = "$g\MHW-DLSS4.log"
$try = Join-Path $PSScriptRoot '..\try.ps1'
function Alive { [bool](Get-Process MonsterHunterWorld -ErrorAction SilentlyContinue) }
function LogFrom([int]$n) { (Get-Content $L | Select-Object -Skip $n) }

if (-not (Alive)) {
    $t0 = Get-Date
    Start-Process 'steam://rungameid/582010'
    while (((Get-Date) - $t0).TotalSeconds -lt 120) {
        if ((Test-Path $L) -and (Get-Item $L).LastWriteTime -gt $t0 -and (Select-String -Path $L -Pattern 'evaluate #512 ok' -Quiet)) { break }
        Start-Sleep 2
    }
}
"game alive: $(Alive)"

$n = (Get-Content $L).Count
& $try -Set 'Mode=Performance', 'Sharpness=0', 'AutoExposure=0', 'Preset=K', 'DebugProbe=1' -Name tt_perf -Wait 5 -Scale 0.5 | Out-Null
"--- Performance probe (DLISP input)"
LogFrom $n | Select-String 'probe (?!dump)' | ForEach-Object { $_.Line.Substring(0, [Math]::Min(260, $_.Line.Length)) }

$n = (Get-Content $L).Count
& $try -Set 'Mode=DLAA' -Name tt_dlaa -Wait 5 -Scale 0.5 | Out-Null
"--- DLAA probe (TAA input)"
LogFrom $n | Select-String 'probe (?!dump)' | ForEach-Object { $_.Line.Substring(0, [Math]::Min(260, $_.Line.Length)) }

& $try -Set 'DebugProbe=0' -Name tt_noprobe -Wait 1 -Scale 0.25 | Out-Null
"--- switching Performance <-> Quality x $Cycles"
$n = (Get-Content $L).Count
for ($i = 1; $i -le $Cycles -and (Alive); $i++) {
    foreach ($m in 'Performance', 'Quality') {
        & $try -Set "Mode=$m" -Name tt_cycle -Wait 2.5 -Scale 0.25 | Out-Null
        if (-not (Alive)) { "game died after switch to $m in cycle $i"; break }
    }
}
"cycles done, alive: $(Alive)"
LogFrom $n | Select-String '\[(ERR|WRN)\]|skipping|feature created' | Select-Object -Last 12 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }
