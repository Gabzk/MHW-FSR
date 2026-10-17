# Copies the built plugin into the game for testing, or removes it again.
#   deploy.ps1            install build\MHW-FSR.dll and FSR binaries
#   deploy.ps1 -Remove    delete everything this script installed
param(
    [string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Monster Hunter World',
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$plugins = Join-Path $GameDir 'nativePC\plugins'
$dataDir = Join-Path $plugins 'MHW-FSR'
$backupDir = Join-Path $PSScriptRoot '..\re\backup'

if ($Remove) {
    foreach ($p in @("$plugins\MHW-FSR.dll", "$plugins\MHW-FSR.pdb")) {
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; "removed $p" }
    }
    if (Test-Path -LiteralPath $dataDir) { Remove-Item -LiteralPath $dataDir -Recurse -Force; "removed $dataDir" }
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
$dll = if (Test-Path (Join-Path $build 'Release\MHW-FSR.dll')) { Join-Path $build 'Release\MHW-FSR.dll' } else { Join-Path $build 'MHW-FSR.dll' }
$pdb = if (Test-Path (Join-Path $build 'Release\MHW-FSR.pdb')) { Join-Path $build 'Release\MHW-FSR.pdb' } else { Join-Path $build 'MHW-FSR.pdb' }
Copy-Item -LiteralPath $dll $plugins -Force
if (Test-Path $pdb) {
    Copy-Item -LiteralPath $pdb $plugins -Force
}

# Deploy AMD FidelityFX runtime binaries into plugins\MHW-FSR
$sdkBins = Join-Path $PSScriptRoot '..\..\FidelityFX-SDK\Kits\FidelityFX\signedbin'
if (Test-Path $sdkBins) {
    New-Item -ItemType Directory -Force $dataDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $sdkBins 'amd_fidelityfx_loader_dx12.dll') $dataDir -Force
    Copy-Item -LiteralPath (Join-Path $sdkBins 'amd_fidelityfx_upscaler_dx12.dll') $dataDir -Force
    $oldFg = Join-Path $dataDir 'amd_fidelityfx_framegeneration_dx12.dll'
    if (Test-Path -LiteralPath $oldFg) { Remove-Item -LiteralPath $oldFg -Force }
}

# Deploy initial MHW-FSR.ini if not present in plugins
$iniDest = Join-Path $plugins 'MHW-FSR.ini'
if (-not (Test-Path -LiteralPath $iniDest)) {
    $text = Get-Content -Raw (Join-Path $PSScriptRoot '..\src\config.cpp')
    $m = [regex]::Match($text, 'kDefaultIni\[\] =\s*((?:\s*L"(?:[^"\\]|\\.)*")+);')
    if ($m.Success) {
        $lines = [regex]::Matches($m.Groups[1].Value, 'L"((?:[^"\\]|\\.)*)"') | ForEach-Object {
            [regex]::Replace($_.Groups[1].Value, '\\(.)', { param($e) if ($e.Groups[1].Value -ceq 'n') { "`n" } else { $e.Groups[1].Value } })
        }
        [IO.File]::WriteAllText($iniDest, ($lines -join ''), [Text.Encoding]::ASCII)
    }
}

"installed into $plugins"
