# Copies the built plugin into the game for testing, or removes it again.
#   deploy.ps1            install build\MHW-DLSS4.dll
#   deploy.ps1 -Dlss <p>  also copy a nvngx_dlss.dll into the plugin folder (the plugin prefers the
#                         driver's DLSS unless this one is newer)
#   deploy.ps1 -Remove    delete everything this script installed
param(
    [string]$GameDir = 'F:\SteamLibrary\steamapps\common\Monster Hunter World',
    [string]$Dlss,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$plugins = Join-Path $GameDir 'nativePC\plugins'
$dataDir = Join-Path $plugins 'MHW-DLSS4'
$backupDir = Join-Path $PSScriptRoot '..\re\backup'

if ($Remove) {
    foreach ($p in @("$plugins\MHW-DLSS4.dll", "$plugins\MHW-DLSS4.pdb")) {
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; "removed $p" }
    }
    # The ini, log and the DLSS folder are left in place on purpose; delete by hand if wanted.
    return
}

New-Item -ItemType Directory -Force $backupDir | Out-Null
$opt = Join-Path $GameDir 'graphics_option.ini'
$optBackup = Join-Path $backupDir 'graphics_option.ini'
if ((Test-Path -LiteralPath $opt) -and -not (Test-Path -LiteralPath $optBackup)) {
    Copy-Item -LiteralPath $opt $optBackup
    "backed up $opt"
}

$build = Join-Path $PSScriptRoot '..\build'
Copy-Item -LiteralPath (Join-Path $build 'MHW-DLSS4.dll') $plugins -Force
Copy-Item -LiteralPath (Join-Path $build 'MHW-DLSS4.pdb') $plugins -Force
if ($Dlss) {
    New-Item -ItemType Directory -Force $dataDir | Out-Null
    Copy-Item -LiteralPath $Dlss (Join-Path $dataDir 'nvngx_dlss.dll') -Force
}
"installed into $plugins"
