# Builds dist\MHW-DLSS4-<version>.zip, laid out to be extracted into the game folder.
#   package.ps1 [-Dlss <path to nvngx_dlss.dll>]
# The plugin uses the DLSS the NVIDIA driver has downloaded. -Dlss also ships a nvngx_dlss.dll in
# nativePC\plugins\MHW-DLSS4, which the plugin uses when the driver has none or an older one.
param(
    [string]$Dlss
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$build = Join-Path $root 'build'
$version = (Select-String -LiteralPath (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(MHW-DLSS4 VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$stage = Join-Path $root "dist\stage"
$zip = Join-Path $root "dist\MHW-DLSS4-$version.zip"

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$plugins = New-Item -ItemType Directory -Force (Join-Path $stage 'nativePC\plugins')
Copy-Item (Join-Path $build 'MHW-DLSS4.dll') $plugins
Copy-Item (Join-Path $root 'README.md') (Join-Path $stage 'MHW-DLSS4-README.md')

# Licences of what the plugin and the package contain: NVSharpen (NVIDIA Image Scaling SDK, MIT)
# and RCAS (AMD FidelityFX FSR 1.0, MIT)
# are compiled into the DLL, the DLSS SDK is linked (and nvngx_dlss.dll ships beside it with -Dlss).
$notices = @(
    "Third-party software in MHW-DLSS4",
    "",
    "== NVIDIA Image Scaling SDK v1.0.3 (NVSharpen shader, compiled into MHW-DLSS4.dll)",
    "https://github.com/NVIDIAGameWorks/NVIDIAImageScaling",
    "",
    (Get-Content -Raw (Join-Path $root 'third_party\nis\licence.txt')),
    "",
    "== AMD FidelityFX Super Resolution 1.0 (RCAS, adapted in src/shaders/rcas.hlsl, compiled into MHW-DLSS4.dll)",
    "https://github.com/GPUOpen-Effects/FidelityFX-FSR",
    "",
    (Get-Content -Raw (Join-Path $root 'third_party\amd_fsr\licence.txt')),
    "",
    "== NVIDIA DLSS SDK (NGX library linked into MHW-DLSS4.dll$(if ($Dlss) { ', nvngx_dlss.dll' }))",
    "https://github.com/NVIDIA/DLSS",
    "",
    (Get-Content -Raw (Join-Path $root 'third_party\dlss_sdk\LICENSE.txt'))
)
Set-Content -LiteralPath (Join-Path $stage 'MHW-DLSS4-THIRD-PARTY.txt') -Value ($notices -join "`r`n") -Encoding utf8

# The plugin writes its ini with defaults on first start; ship the same text so it can be edited first.
$text = Get-Content -Raw (Join-Path $root 'src\config.cpp')
$m = [regex]::Match($text, 'kDefaultIni\[\] =\s*((?:\s*L"(?:[^"\\]|\\.)*")+);')
$lines = [regex]::Matches($m.Groups[1].Value, 'L"((?:[^"\\]|\\.)*)"') | ForEach-Object {
    # C escapes: \n is a line break, \\ and \" stand for the character itself.
    [regex]::Replace($_.Groups[1].Value, '\\(.)', { param($e) if ($e.Groups[1].Value -ceq 'n') { "`n" } else { $e.Groups[1].Value } })
}
[IO.File]::WriteAllText((Join-Path $plugins 'MHW-DLSS4.ini'), ($lines -join ''), [Text.Encoding]::ASCII)

if ($Dlss) {
    if (-not (Test-Path -LiteralPath $Dlss)) { throw "nvngx_dlss.dll not found at $Dlss" }
    $data = New-Item -ItemType Directory -Force (Join-Path $plugins 'MHW-DLSS4')
    Copy-Item -LiteralPath $Dlss (Join-Path $data 'nvngx_dlss.dll')
}

New-Item -ItemType Directory -Force (Split-Path $zip) | Out-Null
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item -Recurse -Force $stage
Get-Item $zip | Select-Object FullName, Length
