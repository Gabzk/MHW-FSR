![MHW-FSR: AMD FidelityFX FSR 3.1 & 4.1 for Monster Hunter: World](images/banner_fsr.jpg)

# MHW-FSR: AMD FidelityFX FSR 3.1 & 4.1 for Monster Hunter: World

**MHW-FSR** is an open-source DirectX 12 plugin for [Stracker's Loader](https://www.nexusmods.com/monsterhunterworld/mods/1982) that brings native **AMD FidelityFX FSR 3.1** and next-gen **FSR 4.1** (ML upscaler) to *Monster Hunter: World*.

By intercepting MT Framework's internal TAA pass and camera projection matrices in DirectX 12, the mod replaces the legacy DLSS 1.0 implementation with modern AMD FidelityFX upscaling.

---

## Highlights

- **Universal GPU Support**: Runs natively on AMD Radeon (RDNA 1/2/3/4), Intel Arc, and NVIDIA GeForce GPUs.
- **AMD FidelityFX Anti-Lag 2**: Native driver-level integration (`amdxc64.dll`). Includes driver-level frame pacing limiter.
- **FSR 4 ML Upgrade for RDNA 3 & 4**: Automatically leverages AMD's machine-learning upscaler (`FSR 4.1.1`) on RDNA 3 (e.g. RX 7000 series like RX 7700 XT) and RDNA 4 GPUs, while providing high-quality **FSR 3.1.5** temporal upscaling on earlier architectures.
- **Advanced Latency & Performance Profiler**: Hardware timestamp queries and CPU profiler tracking upscale time, driver pacing sync delay, queue latency, and end-to-end PC Latency.
- **NativeAA (Native Resolution Anti-Aliasing)**: Replaces MT Framework's blurry legacy TAA with crisp FSR temporal anti-aliasing at 100% native resolution.
- **Native In-Game Overlay & OSD**: Standalone, hardware-accelerated DirectX 12 overlay built with Dear ImGui. Press **[F10]** or **[Insert]** to open the Control Center anytime in-game.
- **Real-Time Hot-Reloading**: Seamlessly switch quality presets, customize render scales, adjust RCAS sharpness, toggle FSR architectures, or do live A/B comparisons against vanilla Capcom TAA.
- **Minimalist Corner OSD Badge**: Displays active upscaler engine, current render resolution %, framerate, and frametimes in real time with configurable INI persistence.

---

## Visual Comparison

![FSR Visual Comparison](images/comparison.png)

*Comparison demonstrating reconstructed details, texture clarity, and temporal anti-aliasing stability with AMD FidelityFX FSR in Monster Hunter: World.*

---

## Requirements

1. **Monster Hunter: World** (v15.23) running in **DirectX 12** mode.
2. [Stracker's Loader](https://www.nexusmods.com/monsterhunterworld/mods/1982) (supporting `nativePC\plugins\`).
3. Windows 10/11 64-bit with current graphics drivers.

---

## Installation

1. Download or extract the release archive (`MHW-FSR-v1.2.0.zip`) into your game directory (`Monster Hunter World`):
   ```
   nativePC\plugins\MHW-FSR.dll
   nativePC\plugins\MHW-FSR.ini
   nativePC\plugins\MHW-FSR\amd_fidelityfx_loader_dx12.dll
   nativePC\plugins\MHW-FSR\amd_fidelityfx_upscaler_dx12.dll
   ```
   > **Note:** The FSR runtime binaries must stay inside `nativePC\plugins\MHW-FSR\`. Do not place them directly in `plugins\`, as Stracker's Loader will attempt to treat them as game mods.

2. Launch the game.
3. In **Options -> Title Menu -> Display -> Advanced Graphics Settings**:
   - Set **DirectX 12 API** to **On**.
   - Set **NVIDIA DLSS** to **On** (the plugin patches MT Framework's UI check so this option is enabled on all AMD/Intel cards).

---

## Configuration File (`MHW-FSR.ini`)

All settings can also be modified in `nativePC\plugins\MHW-FSR.ini`, which hot-reloads automatically:

```ini
; =============================================================================
; MHW-FSR - AMD FidelityFX FSR 3.1 & FSR 4.1 for Monster Hunter: World (DX12)
; =============================================================================
; Quick Start:
; 1. In game Display Settings, enable DirectX 12 API.
; 2. Turn ON "NVIDIA DLSS" (this plugin intercepts and runs native AMD FSR instead).
; 3. This file is automatically hot-reloaded in real-time while the game is running.

[General]
; Enabled: 1 = Plugin enabled, 0 = Disabled completely (requires game restart).
Enabled=1

; ShowOSD: 1 = Show the top-left corner overlay (OSD) on startup, 0 = Hidden by default.
ShowOSD=1

; ShowFPS: 1 = Show the FPS & frametime counter inside the OSD, 0 = Hide FPS counter.
ShowFPS=1

; ShowLatencyOSD: 1 = Show detailed GPU/CPU latency breakdown in OSD, 0 = Compact OSD.
ShowLatencyOSD=0

; LogLevel: Detail level written to nativePC\plugins\MHW-FSR.log
; Options: Debug, Info, Warn, Error.
; (Use 'Debug' to log per-frame render resolutions and full diagnostics).
LogLevel=Info

[FSR]
; -----------------------------------------------------------------------------
; QualityPreset: Internal rendering resolution and upscaling mode.
; Options:
;   NativeAA         -> Renders at 100% native screen resolution (no upscaling).
;                       Replaces the game's blurry TAA with FSR's ultra-crisp temporal
;                       anti-aliasing pass. Highest possible image clarity.
;   Quality          -> Quality mode (~1.5x scale: e.g., 1706x960 upscaled to 1440p).
;                       Substantial FPS boost with near-native visual fidelity.
;   Balanced         -> Balanced mode (~1.7x scale: e.g., 1506x847 upscaled to 1440p).
;   Performance      -> Performance mode (2.0x scale: e.g., 1280x720 upscaled to 1440p).
;                       Maximum performance / framerate.
;   UltraPerformance -> Runs at 0.5 scale (the game engine clamps render scale to >= 0.5).
;   Custom           -> Uses the custom scale specified in RenderScale below.
QualityPreset=Quality

; RenderScale: Used only when QualityPreset=Custom (values from 0.50 to 1.00).
RenderScale=1.0

; -----------------------------------------------------------------------------
; Version: Upscaler version from the AMD FidelityFX API.
; Options:
;   Auto      -> Recommended. Automatically detects your GPU:
;                - RDNA 3 (e.g. RX 7000 series) & RDNA 4: runs FSR 4 (ML/DirectML).
;                - Pre-RDNA 3 (RX 6000, 5000, Intel, Nvidia): runs FSR 3.1.5 (Temporal).
;   FSR 4     -> Forces the FSR 4 Machine Learning upscaler (also accepts: 4.1, FSR4).
;   FSR 3.1   -> Forces the FSR 3.1 temporal upscaler (also accepts: 3.1.5, FSR3).
;   0x...     -> Exact hexadecimal provider ID listed in MHW-FSR.log.
Version=Auto

; -----------------------------------------------------------------------------
; Sharpness: Integrated post-upscale sharpening (AMD FidelityFX RCAS).
;   0.00      -> Sharpening off.
;   0.33      -> Subtle touch of sharpness (recommended for 1440p / 4K).
;   0.66      -> Moderate sharpness.
;   1.00      -> Maximum sharpness.
;   Game      -> Follows the in-game "DLSS Sharpness" slider value.
Sharpness=0.33

; -----------------------------------------------------------------------------
; AutoExposure: Automatic exposure estimation by FSR.
;   0 = Off (uses MT Framework engine's internal exposure/luminance buffer).
;   1 = On  (allows FSR to compute its own exposure dynamically).
AutoExposure=0

; -----------------------------------------------------------------------------
; DLLPath: Directory containing amd_fidelityfx_loader_dx12.dll & amd_fidelityfx_upscaler_dx12.dll.
; Leave empty to use default: nativePC\plugins\MHW-FSR\
DLLPath=

; -----------------------------------------------------------------------------
; Bypass: Comparison mode (only active when QualityPreset=NativeAA).
;   0 = FSR active.
;   1 = Displays vanilla MT Framework TAA to compare sharpness side by side.
Bypass=0

[AntiLag]
; -----------------------------------------------------------------------------
; AntiLag2: AMD FidelityFX Anti-Lag 2 driver-level latency reduction (Radeon).
;   1 = Enabled (synchronizes CPU and GPU pacing, reducing click-to-photon latency).
;   0 = Disabled.
AntiLag2=1

; AntiLagMaxFPS: Framerate limiter via driver pacing (0 = Unlimited).
AntiLagMaxFPS=0
```

---

## Logging & Diagnostics

Detailed logs are written to:
```
nativePC\plugins\MHW-FSR.log
```
The log displays the detected GPU, active D3D12 device, queried FSR versions, selected upscaler model, and per-frame dispatch resolutions.

---

## Building from Source

### Prerequisites
- Visual Studio 2022 (with "Desktop development with C++" workload)
- CMake 3.24 or newer
- [AMD FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)

### Build Steps
```powershell
git clone https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git FidelityFX-SDK
git clone <this-repo> MHW-FSR
cd MHW-FSR

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFFX_SDK_DIR="path/to/FidelityFX-SDK"
cmake --build build --config Release
```

To automatically package or deploy into your game installation:
```powershell
.\tools\package.ps1
.\tools\deploy.ps1
```

---

## Credits & Acknowledgments

- **[odt1](https://github.com/odt1/MHW-DLSS4)** for **MHW-DLSS4**: This project is a direct fork and evolution of `MHW-DLSS4`. All original engine reverse-engineering, MT Framework TAA pass hooking, projection matrix analysis, and memory scanning techniques are credited to **odt1**.
- **[Stracker](https://www.nexusmods.com/monsterhunterworld/mods/1982)** for Stracker's Loader.
- **[wowo-dot](https://github.com/wowo-dot)** for [CMFM](https://www.nexusmods.com/monsterhunterworld/mods/8856) and [Astera Loader](https://github.com/wowo-dot/AsteraLoader).
- **[AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)**: AMD's open-source graphics technologies (FSR 3.1, FSR 4, and RCAS).

---

## License

This project is licensed under the BSD 2-Clause License (matching the original `MHW-DLSS4` license, copyright (c) 2026, odt1). AMD FidelityFX SDK components are copyright (c) Advanced Micro Devices, Inc. and licensed under the MIT License.
