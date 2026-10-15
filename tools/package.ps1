# Builds dist\MHW-FSR-<version>.zip, laid out to be extracted into the game folder.
param(
    [string]$FfxSdkDir
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$build = Join-Path $root 'build'
$version = (Select-String -LiteralPath (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(MHW-FSR VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$stage = Join-Path $root "dist\stage"
$zip = Join-Path $root "dist\MHW-FSR-$version.zip"

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$plugins = New-Item -ItemType Directory -Force (Join-Path $stage 'nativePC\plugins')
$fsrDir = New-Item -ItemType Directory -Force (Join-Path $plugins 'MHW-FSR')

$dll = if (Test-Path (Join-Path $build 'Release\MHW-FSR.dll')) { Join-Path $build 'Release\MHW-FSR.dll' } else { Join-Path $build 'MHW-FSR.dll' }
Copy-Item $dll $plugins
if (Test-Path (Join-Path $root 'README.md')) {
    Copy-Item (Join-Path $root 'README.md') (Join-Path $stage 'MHW-FSR-README.md')
}
if (Test-Path (Join-Path $root 'LICENSE')) {
    Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'MHW-FSR-LICENSE.txt')
}

# Copy AMD FidelityFX runtime binaries
if (-not $FfxSdkDir) {
    $FfxSdkDir = Join-Path $root '..\FidelityFX-SDK'
}
$signedbin = Join-Path $FfxSdkDir 'Kits\FidelityFX\signedbin'
if (Test-Path $signedbin) {
    Copy-Item (Join-Path $signedbin 'amd_fidelityfx_loader_dx12.dll') $fsrDir -Force
    Copy-Item (Join-Path $signedbin 'amd_fidelityfx_upscaler_dx12.dll') $fsrDir -Force
} else {
    Write-Warning "FidelityFX signedbin not found at $signedbin"
}

# The plugin writes its ini with defaults on first start; ship default ini in package
$text = Get-Content -Raw (Join-Path $root 'src\config.cpp')
$m = [regex]::Match($text, 'kDefaultIni\[\] =\s*((?:\s*L"(?:[^"\\]|\\.)*")+);')
if ($m.Success) {
    $lines = [regex]::Matches($m.Groups[1].Value, 'L"((?:[^"\\]|\\.)*)"') | ForEach-Object {
        [regex]::Replace($_.Groups[1].Value, '\\(.)', { param($e) if ($e.Groups[1].Value -ceq 'n') { "`n" } else { $e.Groups[1].Value } })
    }
    [IO.File]::WriteAllText((Join-Path $plugins 'MHW-FSR.ini'), ($lines -join ''), [Text.Encoding]::ASCII)
}

# Copy CMFM in-game settings page descriptor (for Astera Loader / CMFM users)
$cmfmJson = Join-Path $root 'AsteraLoader\plugins\cmfm\pages\mhw_fsr.json'
if (Test-Path $cmfmJson) {
    $cmfmStage = New-Item -ItemType Directory -Force (Join-Path $stage 'AsteraLoader\plugins\cmfm\pages')
    Copy-Item $cmfmJson $cmfmStage -Force
}

New-Item -ItemType Directory -Force (Split-Path $zip) | Out-Null
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item -Recurse -Force $stage
Get-Item $zip | Select-Object FullName, Length
