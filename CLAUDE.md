# MHW-FSR: Developer Reference

AMD FidelityFX Super Resolution 3.1 & 4.1 for Monster Hunter: World (Iceborne, v15.2x, DirectX 12), loaded by Stracker's Loader from `nativePC\plugins`. C++20, MSVC, CMake. Licence: BSD-2-Clause (`LICENSE`).

## Layout

```
src/                  the plugin
  dllmain.cpp         start-up: config, log, loader::Init, game::Init, ngx::Install, game::Activate
  tuning.h            fixed MT Framework engine constants for FSR (reversed-Z depth, MV/jitter scale)
  config.cpp/.h       MHW-FSR.ini (default text = kDefaultIni) and graphics_option.ini reading
  game.cpp/.h         game RE: renderer/scene/camera globals, TAA filter hooks, jitter, depth-view
                      fix, anti-aliasing/reflection/mip-bias holds, render-scale recompute
  ngx_shim.cpp/.h     replaces the game's NGX 1.x function table; runs FSR at the TAA position and
                      tonemap transfer at the SceneResolve position, optimal-settings callback, ini reload
  ngx_params.cpp/.h   the NVSDK_NGX_Parameter object handed to the game
  fsr_loader.cpp/.h   dynamic loading of amd_fidelityfx_loader_dx12.dll & amd_fidelityfx_upscaler_dx12.dll,
                      version queries (FSR 3.1.5, FSR 4.1.1 DirectML, FSR 4.1.2 driver ML)
  fsr_context.cpp/.h  FidelityFX Upscale context lifecycle and dispatch
  d3d_util.cpp/.h     texture info, D3D12 retire queue, TaaUpscaleBridge (DownsampleCS & TransferTonemapCS)
  antilag.cpp/.h      AMD FidelityFX Anti-Lag 2 driver integration (amdxc64.dll)
  telemetry.cpp/.h    GPU timestamp queries and CPU frame/latency profiler
  overlay/            Dear ImGui D3D12 overlay, OSD badge, WndProc cycle protection (wndproc_chain.h)
  crash.cpp, log.cpp, memory.cpp (pattern scan, SafeRead, WritePointer)
tools/                build/deploy/package scripts, selftests, and RE/live helpers
```

## Build, Test, Deploy, Package

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\Release\wndproc_selftest.exe
.\build\Release\config_selftest.exe
.\tools\deploy.ps1
.\tools\package.ps1
```

## How the Plugin Works

1. **NGX table interception (`src/ngx_shim.cpp`)**: Replaces the game's internal 7-entry NGX function table and patches renderer feature bits (`renderer + 0xb4 |= 3`) so the in-game "NVIDIA DLSS" menu toggle is unlocked on all AMD, Intel, and NVIDIA GPUs.
2. **Optimal settings (`OptimalSettingsCallback`)**: Reports the render resolution for the selected preset (`NativeAA` 1.0, `Quality` 2/3, `Balanced` 0.58, `Performance` 0.50, `Custom` 0.50..1.00) with `DLSSMode=1`.
3. **TAA-Pass Super-Resolution (`EvaluateAtTaa`)**:
   - In **NativeAA** (`scale == 1.0`), FSR dispatches directly into the engine's full-resolution TAA history target (`output`).
   - In **Upscaling modes** (`scale < 1.0`), the engine's TAA pass provides clean linear HDR color (`color`), reversed-Z depth (`depth`), dense velocity vectors (`motion`), and Halton sub-pixel jitter. `d3d::BeginTaaUpscale12` allocates an internal full-resolution `R11G11B10_FLOAT` UAV (`upscaledHdr`), and `f.fsr.Dispatch(dp)` upscales directly from render resolution (`inW x inH`) to display resolution (`upW x upH`).
   - `d3d::FinishTaaUpscale12` then box-downsamples `upscaledHdr` back into the low-resolution TAA `output` buffer (`DownsampleCS`) so subsequent low-res post-process passes (`uToneMapFilter`, `cLuminance`, Bloom/DoF) receive an anti-aliased image.
4. **Scene Resolve Tonemap Transfer (`EvaluateAtSceneResolve`)**:
   - At the end of the scene pass (where legacy DLISP ran after `uToneMapFilter`), `d3d::ResolveTaaTonemap12` runs `TransferTonemapCS` at full display resolution (`upW x upH`). It transfers the local exposure, filmic curve, and 3D LUT color grading ratio from the engine's low-res tonemapped buffer onto the high-frequency detail of `upscaledHdr`, writing the final display-referred result to the pre-HUD output target.
5. **Engine Holds (`src/game.cpp`)**:
   - Forces camera Halton jitter on (`EnsureJitter`) and widens it to a full render pixel (`WidenJitter`) with `max(20, ceil(8 / scale^2))` phases.
   - Suppresses the game's internal TAA sharpen pass (`SharpenGuard`) while FSR runs.
   - Aliases the colour native view onto the R32F depth copy texture's depth view (`PatchViewDepth`) with proper reference counting.
   - Holds scene anti-aliasing at TAA-only (`HoldTaaOnly`), disables the 2x2 screen-space reflection history jitter (`HoldReflectionJitterOff`), and selects the optimal negative mip-bias sampler bank (`HoldMipBias`).
   - Detects camera teleports (`> 100m` in `CameraCut`) to reset FSR temporal history without false resets during weapon zoom or fast camera turns.
