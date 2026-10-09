# MHW-DLSS4: technical notes

For developers. Players only need the [README](../README.md).

## How it works

The game links a static NGX 1.3 library and, at start-up, initialises the driver's NGX core with
its application id. The plugin is loaded later (with the input system) and:

1. Replaces the entries of the game's internal NGX function table (in `.data`) and the game's
   NGX parameter object, so every DLSS 1 call ends up in the plugin. No game code is patched; the
   TAA filter is hooked through two vtable entries.
2. Shuts the game's NGX 1.x context down and initialises a current NGX with a project id and the
   folder of the chosen `nvngx_dlss.dll` as its only search path. With the game's application id,
   or with the old context still alive, the driver serves NVIDIA's per-game DLSS 1 model over the
   air instead.
3. At the game's DLSS 1 call inside the TAA pass, runs DLSS with the game's colour, dense motion
   vectors and depth copy, plus the camera jitter recorded when the pass was built. The game's
   jitter only covers a fraction of a pixel and scene parameters sometimes switch it off, so the
   plugin keeps it on and widens it to a full render pixel while DLSS runs.
4. In the upscaling modes the TAA-position call passes the jittered frame through and DLSS runs
   where the game calls DLISP, its old upscaler, after post-processing. That is the only place the
   game's pipeline changes resolution.
5. With sharpening on, DLSS writes into a texture of the plugin and a compute pass (NVSharpen,
   Vanilla or RCAS) writes the sharpened result into the game's texture.
6. Keeps the TAA filter's own sharpen pass off while DLSS runs. With it on, the game would write
   the sharpened TAA result over the DLSS output.
7. Works around a crash in the game's never-used DX12 DLSS path: the barrier setup for the DLSS
   command expects a depth view the depth copy texture does not have. The plugin lends it the
   texture's colour view and takes a reference of its own, because the game releases both views
   separately when the texture is rebuilt.
8. Holds the game's anti-aliasing at TAA only while DLSS runs. The game decides its anti-aliasing
   when graphics options are applied: with DLSS below native resolution it switches to TAA only,
   but at native resolution (DLAA, which the original game never had) it kept the greyed-out
   Anti-Aliasing option. FXAA then ran over the DLSS image, and Off or FXAA switched off the TAA
   pass and with it DLSS.
9. Keeps D3D12 textures and DLSS features it replaces alive for 16 game frames before freeing
   them. D3D12 frees resources immediately, even while queued GPU work still uses them, which
   crashed the GPU when the render resolution changed while the game was running.

The game builds the TAA pass on worker threads, twice per frame and at the same time, and runs
the TAA update once per frame on the main thread. The plugin reloads its ini and asks the game to
recompute the render scale from that update (where the game applies graphics options itself),
outside the lock the render thread takes for DLSS. The hooks only act once the NGX side is
installed.

## Conventions

Measured with the scripts in `tools/`:

- The game's motion vectors are the clip-space difference to the previous position times the
  render size, so DLSS gets them scaled by `(0.5, -0.5)` (`MotionScaleX/Y`).
- Depth is reversed (sky = 0).
- The camera jitter `h` moves the image by `(h.x / 2, -h.y / 2) * render scale` render pixels.

`tools/motion_check.py` checks the motion vector scale and the jitter term against frames dumped
while moving (`DebugDumpFrames=8` in the ini; the dump starts 5 seconds after saving, enough to get
back into the game and move the camera).

Versions 0.1 and 0.2 passed the motion vectors unscaled (`MVScaleX/Y=1`). DLSS then put its history
in the wrong place whenever the camera moved and dropped it, which showed as softer, slightly
aliased edges in motion. Those keys are ignored now.

## Building

Visual Studio 2022 or newer (MSVC), CMake 3.24+, Ninja, and `fxc` from the Windows SDK.

- The NVIDIA DLSS SDK is not in the repository; see
  [third_party/dlss_sdk/README.md](../third_party/dlss_sdk/README.md) for which files to copy from
  https://github.com/NVIDIA/DLSS.
- `third_party/nis` holds `NIS_Config.h`, `NIS_Scaler.h` and `NIS_Main.hlsl` from the NVIDIA Image
  Scaling SDK v1.0.3 (https://github.com/NVIDIAGameWorks/NVIDIAImageScaling, MIT licence, see
  `third_party/nis/licence.txt`).
- `src/shaders/rcas.hlsl` is adapted from AMD FidelityFX Super Resolution 1.0 (MIT licence,
  `third_party/amd_fsr/licence.txt`).

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
tools\deploy.ps1          # copy into the game for testing
tools\package.ps1         # dist\MHW-DLSS4-<version>.zip
```

`tools/` also holds the scripts used to verify the conventions against the running game (window
capture without stealing focus, ini-driven A/B runs, jitter and motion vector analysis);
`tools/re` and `tools/live` hold reverse-engineering helpers and live-game test scripts.
