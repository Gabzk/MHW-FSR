# MHW-FSR: Technical Notes

For developers. Players only need the [README](../README.md).

## Architecture Overview

Monster Hunter: World (Iceborne v15.2x) links a static NGX 1.3 library and, when DLSS is enabled, builds two NGX evaluation commands in its DirectX 12 pipeline:
1. **`NVSDK_NGX_Feature_SuperSampling`** inside `uTemporalAAFilter2::Draw` (`+0x2392150`), which receives linear HDR colour (`R11G11B10_FLOAT`), dense velocity vectors (`R16G16_FLOAT`), and the `R32_FLOAT` depth copy at internal render resolution (`renderW x renderH`).
2. **`NVSDK_NGX_Feature_ImageSignalProcessing`** (legacy DLISP) at the end of the scene pass (`+0x22993d0`), after `uToneMapFilter` and post-processing, which transitions from internal render resolution (`renderW x renderH`) to display resolution (`outW x outH`) before HUD rendering.

### Why Upscaling Must Run in the TAA Pass

In MT Framework's pipeline, the TAA pass is the only point where:
- Scene colour is still **linear HDR** before tone mapping or post-process blur.
- **Depth** and **Motion Vectors** are aligned with the current frame's sub-pixel camera jitter.

However, the engine's TAA output texture (`taa->history[slot]`) is allocated at `renderW x renderH`, and subsequent filters (`uToneMapFilter`, `cLuminance`, Bloom/DoF) bind low-resolution render targets until the final Scene Resolve (DLISP) pass.

To achieve full-resolution FSR 3.1 / 4.1 super-resolution at the TAA pass without breaking the engine's post-process pipeline, `MHW-FSR` uses a two-stage bridge (`TaaUpscaleBridge` in `src/d3d_util.cpp`):

1. **`EvaluateAtTaa` (`uTemporalAAFilter2::Draw`)**:
   - `d3d::BeginTaaUpscale12` allocates an internal display-resolution `R11G11B10_FLOAT` UAV (`upscaledHdr`, e.g. `2560x1440`).
   - `FsrContext::Dispatch` runs AMD FidelityFX FSR (`renderW x renderH -> outW x outH`) directly into `upscaledHdr` with linear HDR input, reversed-Z depth, velocity vectors, Halton jitter, and RCAS sharpening.
   - `d3d::FinishTaaUpscale12` dispatches `DownsampleCS` to box-downsample `upscaledHdr` back into the engine's low-resolution TAA `output` (`renderW x renderH`) and copies it to `preTonemapLow`. This feeds clean, anti-aliased linear HDR into `cLuminance` and `uToneMapFilter` for accurate auto-exposure and color grading.
2. **`EvaluateAtSceneResolve` (Scene Pass Resolve)**:
   - After `uToneMapFilter` has applied exposure, filmic curve, and the 3D color LUT to the low-resolution buffer (`color`), `d3d::ResolveTaaTonemap12` dispatches `TransferTonemapCS` at display resolution (`outW x outH`).
   - For each high-resolution pixel in `upscaledHdr`, `TransferTonemapCS` samples the bilinearly filtered low-res pre-tonemap (`preTonemapLow`) and post-tonemap (`TonemappedLow`) buffers, transferring the per-channel tonemap ratio `TonemappedLow / PreTonemapLow` (and post-process additive highlights) onto the full-resolution `upscaledHdr` detail.

In **NativeAA** mode (`scale == 1.0`), `upscalingAtTaa` is `false` and `SceneResolveActive()` is `false`: FSR dispatches directly into the native-resolution TAA target with zero bridge overhead.

---

## Fixed Engine Conventions (`src/tuning.h` & `src/game.cpp`)

All MT Framework engine conventions required for FSR are hardcoded so they cannot be misconfigured:

| Constant / Mechanism | Value | Purpose |
| --- | --- | --- |
| `tuning::kDepthInverted` | `true` | MHW uses Reversed-Z depth (`1.0` at near plane, `0.0` at far plane/sky). |
| `tuning::kMotionScaleX/Y` | `0.5f, -0.5f` | Converts engine clip-space velocity (`delta * renderSize`) to UV space (`+X` right, `+Y` down). |
| `tuning::kJitterScaleX/Y` | `0.5f, -0.5f` | Converts camera Halton jitter to pixel offset (`* renderScale`). |
| `FFX_UPSCALE_ENABLE_AUTO_EXPOSURE` | Always On | MHW does not expose a standalone 1x1 exposure texture to NGX; FSR 3/4 requires auto-exposure when no external exposure texture is bound. |
| `EnsureJitter` & `WidenJitter` | Always On | Forces Halton jitter on (even on the title screen) and widens jitter amplitude by `2.0 / renderScale` with `max(20, ceil(8 / scale^2))` phases. |
| `SharpenGuard` | Always On | Disables MT Framework's internal TAA sharpen pass (`taa + 0x187`) while FSR runs so it does not overwrite the output slot. |
| `PatchViewDepth` | Always On | Aliases the colour native (`+0x60`) onto the depth-view native (`+0xf0`) of the R32F depth copy texture with atomic reference counting (`native + 0x8`). |
| `HoldTaaOnly` | Always On | Holds scene AA switches (`scene + 0xe9e4`) at `0x0001` (TAA only) while FSR is active so FXAA never blurs the upscaled output. |
| `HoldReflectionJitterOff` | Always On | Holds `mSSLRGBufferJitter` (`scene + 0xb440`) at `0` while FSR runs to prevent 2x2 block shimmering in screen-space reflections. |
| `HoldMipBias` | Always On | Holds `mSamplerLodBias` (`scene + 0x1a8`) at the sampler bank matching `log2(renderScale) - 1` for sharp textures at sub-native resolutions. |
| `CameraCut` | `> 100m` | Resets FSR temporal history only on genuine camera teleports (`> 10000` cm), avoiding false resets during weapon zoom or fast camera rotation. |

---

## AMD FidelityFX Anti-Lag 2 (`src/antilag.cpp`)

`src/antilag.cpp` loads `amdxc64.dll` from the driver store and initializes the AMD Anti-Lag 2 D3D12 context on the game's `ID3D12Device`. `antilag::Update` is called at the very start of `sMhMain::Update` (`OnFrameStart`), before the engine's internal frame limiter and before waking the render thread, synchronizing CPU input polling with GPU execution and optionally applying the driver-level `AntiLagMaxFPS` pacing limit.
