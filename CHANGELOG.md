# Changelog

All notable changes to **MHW-FSR** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
