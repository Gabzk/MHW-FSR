# Changelog

All notable changes to **MHW-FSR** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [1.3.0] - 2026-10-16

### Added
- **AMD FidelityFX Anti-Lag 2 SDK Integration**:
  - Full native driver-level integration of AMD FidelityFX Anti-Lag 2 (`amdxc64.dll`).
  - Drastically minimizes click-to-photon and input-to-present latency on AMD Radeon GPUs (tested drop from ~172 ms down to ~32 ms).
  - Added in-game toggle under the new **Anti-Lag 2 & Latency** tab.
  - Added integrated **Driver Frame Pacing** slider (Driver Max FPS limiter) to prevent GPU queue bloat without external frame limiters.
- **Advanced Latency & Performance Profiler**:
  - Added dedicated hardware and CPU telemetry profiler under the **Diagnostics & Profiler** tab:
    - **Upscaler Pass Latency**: Real-time GPU execution time (via D3D12 hardware timestamp queries) and CPU command recording time.
    - **Anti-Lag 2 CPU Synchronization Wait**: Exact milliseconds spent synchronizing CPU input with GPU dispatch.
    - **GPU Render Queue Latency**: In-flight queue latency between CPU submit and GPU execution completion.
    - **PC Latency (Input-to-Present)**: End-to-end latency from mouse/controller input poll to frame presentation.
    - **Interactive Frame Time Plot**: Real-time smoothed frametime history graph with live FPS and frametime readout.
  - Added optional **Latency Breakdown OSD** displaying Upscale, AL2 Wait, and PC Latency directly on screen.

### Fixed
- **ROG Ally & non-FSR4 GPU Auto-Fallback**:
  - Implemented semantic major version parser (`GetMajorVersion`) in the FSR loader.
  - Fixed an issue where handhelds like ROG Ally or GPUs lacking DirectML FSR 4.1.1 support would falsely match the `.4` suffix of `"2.3.4"` in `Auto` mode.
  - Hardware without FSR 4 now strictly falls back to high-quality **FSR 3.1.5**, never reverting to FSR 2.3.4.
- **Steam Cloud & Save File Loading**:
  - Fixed an accidental `.rdata` pointer overwrite in `FindGetProcAddressSlot` during early NGX shim initialization that previously interfered with Steam Cloud sync and save data loading.
- **OSD & FPS Counter Configuration Persistence**:
  - Added `ShowOSD`, `ShowFPS`, and `ShowLatencyOSD` settings under `[General]` in `MHW-FSR.ini`.
  - User preferences for displaying the corner OSD badge and FPS counter are now correctly saved and restored across game sessions.
- **Controller X / Square Black Box Highlight**:
  - Fixed Dear ImGui gamepad navigation (`NavUpdateWindowing`) drawing a dark highlight box over the corner OSD when holding **X (Xbox)** or **Square (PlayStation)** with the overlay menu closed.
  - Gamepad and keyboard navigation flags are now strictly scoped to when the `[F10]` menu is active.
- **DirectML / FSR 4.1.1 DLL Search Path**:
  - Fixed library search path handling to ensure `amd_fidelityfx_upscaler_dx12.dll` and DirectML providers are always discovered on RDNA 3 / RDNA 4 systems.
- **Diagnostics Tab UI Alignment**:
  - Fixed column offset and label spacing so the PC Latency metric is never obscured by long diagnostic strings.

### Changed
- **DirectX 12 Engine Robustness**:
  - Implemented `BarrierBatcher` for consolidated resource transition barriers.
  - Replaced fixed RTV descriptor heap with dynamic capacity expansion supporting up to 8 swapchain backbuffers during runtime window resizes and fullscreen toggles.
  - Streamlined presentation loop for ultra-low overhead and optimal frametime consistency.

---

## [1.2.0] - 2026-10-15

### Added
- **Native In-Game Overlay & Control Center**:
  - Implemented standalone DirectX 12 overlay using Dear ImGui, hooking `IDXGISwapChain::Present` with zero third-party UI dependencies.
  - Toggled anytime in-game via **[F10]** or **[Insert]**.
  - **Upscaling & Resolution**: On-the-fly quality preset switching (`NativeAA`, `Quality`, `Balanced`, `Performance`, `Ultra Performance`, `Custom`), custom render scale slider (0.50x - 1.00x), and interactive RCAS Sharpness slider with quick presets (`Off`, `Subtle`, `Moderate`, `Crisp`).
  - **FidelityFX Core**: Architecture selection (`Auto`, `Force FSR 4.1.1`, `Force FSR 3.1.5`), dynamic auto-exposure toggle, and screen-space reflection jitter filter fix.
  - **Diagnostics & A/B Comparison**: Real-time bypass toggle to view vanilla Capcom TAA side-by-side with FSR's crisp temporal reconstruction, Halton jitter telemetry, and OSD watermark toggle.
  - **Persist to Disk**: Added "Save to MHW-FSR.ini" button directly in the overlay.
- **Corner OSD Badge**:
  - Minimalist live telemetry badge rendering active upscaler version, render scale percentage, and framerate/frametimes.
- **Dynamic Hardware Capability Detection**:
  - Added `loader::HasFsr4Support()` to query device capabilities from the AMD FidelityFX API at runtime.
  - Proactively auto-detects RDNA 3 and RDNA 4 GPUs (e.g. Radeon RX 7000 / 8000 series like RX 7700 XT) to ensure FSR 4.1.1 (DirectML) is always chosen in `Auto` mode.
  - Dynamically hides the FSR 4 option and displays an informative GPU requirement note on hardware without DirectML / CS 6.6 support.
- **Input Capture & Camera Lock**:
  - Hooked Win32 window messages (`WM_MOUSE*`, `WM_KEY*`, `WM_INPUT`) and MT Framework's internal `sMhMouse` polling loop to completely lock camera rotation and hunter movement while navigating the overlay.
- **Early Loader DLSS Menu Patch**:
  - Enforced `kRendererDlssBits` so the "NVIDIA DLSS" graphics setting is unlocked and visible in MT Framework's options menu on all non-NVIDIA GPUs under Stracker's Loader.

### Changed
- **Pure ASCII UI Rendering**: Eliminated Unicode symbols (`●`, `™`, `│`) that triggered fallback `'?'` glyphs in Dear ImGui's built-in font atlas.
- **F10 Hotkey Debounce**: Removed duplicate present-loop polling and added a 150ms debounce guard to prevent key flapping when opening/closing the overlay.
- **Decoupled Architecture**: Removed external dependencies on CMFM and Astera Loader, restoring full compatibility with standard Stracker's Loader, `mhw-seamless-coop`, and other gameplay mods.

---

## [1.1.1] - 2026-10-15

### Fixed
- Fixed in-game "NVIDIA DLSS" option missing or grayed out on AMD and Intel GPUs by enforcing feature capability bits at early initialization.

---

## [1.0.0] - 2026-10-14

### Added
- Initial release of **MHW-FSR**.
- Native AMD FidelityFX FSR 3.1.5 and FSR 4.1.1 (DirectML) upscalers for Monster Hunter: World (DirectX 12).
- NativeAA mode replacing vanilla MT Framework TAA with FSR temporal anti-aliasing.
- Integrated RCAS post-upscale sharpening.
- Halton (2, 3) sub-pixel jitter extraction from camera viewports.
- Automatic hot-reloading of `MHW-FSR.ini`.
- Universal GPU support for AMD Radeon, NVIDIA GeForce, and Intel Arc.
