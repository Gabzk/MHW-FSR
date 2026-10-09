# MHW-DLSS4: notes for working on this project

DLSS 4 for Monster Hunter: World (Iceborne, game version 15.2x, tested on 15.23), as a native
plugin DLL loaded by Stracker's Loader from `nativePC\plugins`. C++20, MSVC, CMake + Ninja.
Current version: see `project(... VERSION ...)` in `CMakeLists.txt` (0.8.2 at the time of writing).
Releases are git tags (`v0.8.2`) with the zip attached as a GitHub release; the repo is private on
GitHub (`odt1/MHW-DLSS4`). Licence: BSD-2-Clause (`LICENSE`, "Copyright (c) 2026, odt1"), for the
project's own code. No copyleft code or licence: the DLSS SDK licence (4e) forbids using the SDK in a
way that puts it under terms requiring source release, and the DLL links `nvsdk_ngx_s.lib`.

Called MHWDLSS (`MHWDLSS.dll/.ini/.log`, `MHWDLSS\` folder, repo `odt1/MHWDLSS`) up to 0.7.3;
renamed to MHW-DLSS4 in 0.8.0. The ini, log and data folder names follow the DLL's file name.
Kept on purpose: the C++ namespace `mhwdlss`, the NGX project id, and the local checkout folder
`C:\Projects\MHW DLSS` (Claude's project memory is keyed to that path). If both DLLs are installed,
the first one the loader loads (`MHW-DLSS4.dll` sorts first) hooks the game, the other one fails
`game::Init` ("TAA vtable not found") and stays inactive, and MHW-DLSS4 logs a warning to delete
the old files.

`README.md` is for players and belongs to the user, who rewrote its top part themselves. Keep it
plain and non-technical, and don't restructure it without being asked. Developer detail goes here
or in `docs/`.

## Layout

```
src/                  the plugin
  dllmain.cpp         start-up: config, log, game::Init, ngx::Install
  game.cpp/.h         game RE: renderer/scene/camera globals, TAA filter hooks, jitter, depth-view
                      fix, anti-aliasing hold, render-scale recompute
  ngx_shim.cpp/.h     replaces the game's NGX 1.x function table; runs current DLSS (TAA and DLISP
                      positions), feature lifetime, optimal-settings callback, ini hot reload
  ngx_params.cpp/.h   the NVSDK_NGX_Parameter object handed to the game
  dlss_dll.cpp/.h     which nvngx_dlss.dll to use (driver store / DLLPath / MHW-DLSS4 folder)
  sharpen.cpp/.h      NVSharpen / Vanilla / RCAS compute passes after DLSS (D3D11 + D3D12)
  d3d_util.cpp/.h     texture info, mirror copies, rect copies, D3D12 retire queue, debug dumps
  probe.cpp/.h        DebugProbe / DebugDumpFrames (D3D12 only)
  config.cpp/.h       MHW-DLSS4.ini (default text = kDefaultIni) and graphics_option.ini reading
  crash.cpp, log.cpp, memory.cpp (pattern scan, SafeRead, WritePointer)
  shaders/            nis_sharpen.hlsl, vanilla_sharpen.hlsl, rcas.hlsl (fxc at build time)
third_party/          NIS SDK v1.0.3 (MIT), AMD FSR licence, DLSS SDK (not committed, see its README)
tools/                build/deploy/package scripts and measurement tools (index below)
  re/                 static RE helpers for the unpacked game exe
  live/               scripts that read/poke the running game or drive test runs
images/               before/after comparisons used by README.md
```

Local-only folders (git-ignored, never commit): `re/` (game exe, copyrighted), `graphics/` (raw
screenshots, Affinity files), the other loaders and mods (`Stracker's Loader Binary`,
`SharpPluginLoader`, `MHW-QuestLoader`, `MHWSS-*`, `Performance Booster and Plugin Extender`),
`nvngx_dlss.dll`, `build/`, `dist/`.

## Build, deploy, package

Machine-specific setup is in the user's global CLAUDE.md (pwsh, `devshell` for MSVC, Git Bash).

```
pwsh:  . $PROFILE; devshell; cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
pwsh:  . $PROFILE; devshell; cmake --build build
tools\deploy.ps1       copies build\MHW-DLSS4.dll + .pdb into the game (game must be closed)
tools\package.ps1      dist\MHW-DLSS4-<version>.zip: DLL, default ini (extracted from kDefaultIni in
                       config.cpp), README as MHW-DLSS4-README.md, LICENSE as
                       MHW-DLSS4-LICENSE.txt, THIRD-PARTY notices
```

Bump the version in `CMakeLists.txt` for every release. The zip has no `nvngx_dlss.dll` unless
`package.ps1 -Dlss <path>`. CMakeLists.txt sets `/O2 /Gw` and `NDEBUG` on the target for every
non-Debug build (plus `/Zi /DEBUG` for the PDB): the old `build\` folder had empty cached
`CMAKE_CXX_FLAGS_<CONFIG>` (first configured without the MSVC environment), so 0.7.3 and 0.8.1
shipped unoptimised (`/Od`). Check the `FLAGS` line in `build\build.ninja` after a fresh configure.
Before changing source, keep a copy of the last released `src/`
(the repo is in git now, so a tag per release does the job).

## Game, files, test workflow

- Game folder: `F:\SteamLibrary\steamapps\common\Monster Hunter World`. Launch with
  `steam://rungameid/582010`. The exe is SteamStub-packed; the unpacked copy for RE is
  `re\MonsterHunterWorld.exe.unpacked.exe` (Steamless at `C:\Applications\Steamless 0.5`). Image base
  0x140000000 (no ASLR), so exe addresses below are absolute VAs; the plugin logs RVAs (+0x...).
- Plugin files in `nativePC\plugins`: `MHW-DLSS4.dll`, `MHW-DLSS4.ini` (hot-reloaded, mtime checked
  twice a second from the TAA update), `MHW-DLSS4.log`, and the `MHW-DLSS4\` folder (NGX data, `driver\nvngx_dlss.dll` copy,
  `custom\`). Never put a DLL directly in `nativePC\plugins`: the loader loads every DLL there.
- The user also runs `MonsterHunterFixorld.dll` (their own swapchain/frame-latency/loading plugin;
  its log shows fps, hitches and `ResizeBuffers` with the swapchain format), `!CRCBypass.dll`,
  CutsceneSkip, QuestLoader, MonsterLoader. Don't rely on CRCBypass: the plugin patches no game
  code, only `.rdata` vtable slots and `.data`.
- `graphics_option.ini` (game folder): the game's options. **Never write it with a BOM** (the game
  then ignores it and starts with defaults: DX11, 1080p). Edit it as ASCII and restore it byte for
  byte after tests. After a GPU crash the game writes `DirectX12Enable=Off` itself; tell the user.
- Test runs: launch via Steam, wait for `evaluate #512 ok` in the log (title screen is enough for
  most checks; getting into the hub needs the user), change keys in `MHW-DLSS4.ini` (ASCII), and end the
  run with `Stop-Process -Name MonsterHunterWorld -Force`. Never CloseMainWindow (it shows a dialog
  only the user can answer). Killing a game the user started is fine during active work.
- After a test session, overwrite the user's `MHW-DLSS4.ini` with the packaged default ini of the new
  version (the user's explicit preference), not with their old one.
- Captures: `tools\capture-window.ps1 -Path x.png` (PrintWindow, no focus steal; works in DX12 and
  now also DX11 windows, but sometimes returns all-black frames; retry). `DebugDumpOutput=1` writes
  the DLSS output to `MHW-DLSS4\output.bin` (D3D11 only; `tools\bin2png.py`). `DebugDumpFrames=N`
  dumps TAA-position colour/motion/depth/jitter (D3D12 only; `tools/motion_check.py`).
- GPU faults: check the System log for `nvlddmkm` event 153 (they come in threes) and the game's
  `ERR12 : Graphics device crashed.` handler. `tools/live/modetest.py <cycles>` launches the game if
  needed and switches Mode at run time; it stops at the first driver error, dead game or switch the
  plugin did not reload. The packaged ini has LF line endings, the scripts keep whichever is there.
  `bypasstest.py [switches]` checks the AA give-back with Bypass and VRAM across switches;
  `bypassupscale.py` checks that Bypass in Performance keeps the hold and DLISP running.
- Python: plain `python` has no numpy; use `uv run --quiet --with numpy --with pillow python -I x.py`
  (`--with capstone` for the RE scripts). In Git Bash heredocs `\\` collapses to `\`: write scripts
  with the Write tool instead.

## Working with the user

- Before treating a game crash or odd log line as a plugin bug, report the evidence and ask; the
  user often tests and kills the game themselves. Don't ship a "crash fix" on a guess (a v0.3.1 was
  rejected for that; the same change shipped later in 0.7.1 once memory evidence proved it).
- The user tests in the hub (Astera) and reports visuals; numbers from captures back the claims.
- Global rules (no Co-Authored-By trailers, no TUI programs, etc.) are in the user's global CLAUDE.md.

## How the plugin works

1. **NGX table.** The game links a static NGX 1.3 library and initialises the driver's NGX core with
   its application id before plugins load. `ngx_shim.cpp` finds the library's function table (via
   the `NVSDK_NGX_D3D1x_*` exports' first `mov r64,[rip+x]`) and replaces its 7 entries, re-applying
   them every frame because the game's init rewrites them. The game's DLSS context (device +0x42700,
   `game::DlssContext`) gets the plugin's parameter object.
   **Early load (0.8.1).** With Stracker's Loader alone, plugins load after the game's NGX init and
   before its DLSS apply. SharpPluginLoader 2.0 (`winmm.dll` proxy, `loader-config.json` "SPL"
   with PreloadDLLs `dinput8.dll`) preloads Stracker's Loader at sMhMain construction, so the
   plugin loads before the renderer exists. The game's `NVSDK_NGX_D3D12_Init` export
   (`0x14273f230`) zeroes the table (`0x1451d8808`..`0x1451d883f`), loads the NGX 1.x core and
   fills the 7 entries through the exe's GetProcAddress import (slot `0x142e13118`), then calls the
   Init it got. Left alone, the game set DLSS up with the 1.x core: type 2 (DLISP at 0.75, no DLSS
   pass), so the plugin had nothing to upscale (grey screen). Calling `RecomputeRenderScale` after
   a late adoption switched type 2 to 1 mid-run and crashed the render thread: a null texture in a
   pass binding at `0x14229268b`. So when `game::Device()` is null at install, `HookNgxLookups`
   swaps that import slot (found from the `FF 15` calls in the Init exports) and returns the
   shim entries for `NVSDK_NGX_D3D1x_*` names. The game's NGX init then goes through the plugin,
   the 1.x core is never initialised, and the context is created with the plugin's parameters. The
   shim Init reports failure without an NVIDIA GPU. After a late adoption, the render scale is
   only recomputed when the game already has a DLSS pass; otherwise the log says to toggle DLSS.
2. **Current NGX.** The legacy context is shut down (otherwise the driver serves MHW's per-game OTA
   DLSS 1.2.110 model: zebra garbage, presets ignored) and NGX is initialised with a project id and
   the folder of the chosen `nvngx_dlss.dll` as its only search path.
3. **Optimal settings.** The game asks `DLSSOptimalSettingsCallback` for the render size; the plugin
   answers from `Mode` (DLAA 1.0, Quality 2/3, Balanced 0.58, Performance 0.5, Custom 0.5..1) with
   `DLSSMode=1`. The game clamps render scale to >= 0.5 (UltraPerformance gave a black screen).
   Ini reloads call the game's render-scale recompute (`game::RecomputeRenderScale`).
4. **TAA position.** The game's DLSS 1 call sits inside the TAA filter's draw (uTemporalAAFilter2).
   In DLAA, DLSS runs there on colour, dense motion vectors and the R32F depth copy, writing the TAA
   history texture. In the upscaling modes the TAA position copies the jittered colour through
   (`CopyRect12/11`) and mirrors motion/depth (`Mirror12/11`) for later.
5. **DLISP position.** In the upscaling modes DLSS runs where the game called DLISP (its old
   upscaler), after post-processing, on display-referred colour (max about 1.4), with MVLowRes.
   `StaleFrame` skips frames of the old size after a resolution change.
6. **Sharpening.** With Sharpness > 0, DLSS writes a plugin texture and a compute pass writes the
   game's target: NVSharpen (NIS linear-HDR mode, strength and limit faded by min(1, s/0.5); 1 =
   NVIDIA's 0.3), Vanilla (reimplementation of DLISP's "DLSS Sharpen", strength 1.5*s, on
   sqrt(x/(1+x)), added as a difference), RCAS (FSR 1). All share NIS's root signature.
   `Sharpness=Game` follows `DLSS_Sharpness` in `graphics_option.ini`.
7. **Jitter.** The TAA update writes the camera jitter only under scene conditions; the plugin forces
   it on (`ForceJitter`, `EnsureJitter`) and widens it by 2/renderScale (`FullPixelJitter`). DLSS
   jitter = (0.5, -0.5) * camera jitter * render scale.
8. **TAA sharpen pass** (+0x187) is cleared during the draw while DLSS runs (it would overwrite the
   DLSS output: DLSS was pointed at history[next]).
9. **DX12 depth view.** The game's DX12 DLSS barrier code reads the depth copy texture's depth-view
   native (+0xf0), which is null, and crashed; `PatchViewDepth` aliases the colour native (+0x60)
   there and takes a reference (InterlockedIncrement native+8), because the game releases +0x60 and
   +0xf0 separately. Without that reference (before 0.7.1) every texture rebuild destroyed the native
   twice: "exploded geometry" until a restart.
10. **Anti-aliasing hold (0.7.2).** While DLSS runs, the scene's AA switches are held at TAA only
    (see below), so FXAA never runs after DLSS and Off/FXAA can't switch DLSS off. Released (game
    value given back) only by Bypass in DLAA or when NGX could not be initialised
    (`game::SetDlssUnavailable`).
11. **D3D12 retire queue (0.7.2, clock fixed in 0.7.3).** D3D12 frees resources on the last
    Release even while queued command lists use them. Replaced mirror textures, the sharpen
    target and released NGX D3D12 features go to `d3d::Retire` / `RetireFeature` and are freed 16
    distinct renderer frames later (`RetireTick(game::Frame())` from the update callback). At NGX
    shutdown only the retired features are flushed (NGX would free them anyway); textures and the
    sharpen state stay retired. Freeing at once faulted the GPU (nvlddmkm 153, ERR12) on run-time
    Mode switches. NGX accepts a new feature while the old one still exists. D3D11 releases stay
    immediate. The motion/depth mirrors are retired as soon as the TAA position runs in DLAA.

### Threads (0.7.3)

- The TAA pass is built on worker threads, twice per frame and concurrently (two `HookTaaDraw`
  calls with the same frame and slot on different threads). `OnFrame` (frame callback) therefore
  only re-patches the NGX table, adopts the context and attaches DLISP, all under the shim mutex.
  The depth-view alias uses a compare-exchange, and the TAA sharpen flag is cleared by the first
  concurrent build and restored by the last (`SharpenGuard`).
- The TAA update runs once per frame on the main thread, also while the TAA filter is switched off.
  The update callback (`ngx::OnUpdate`) does the retire tick, the ini and graphics_option.ini
  reloads (under the shim mutex) and then `RecomputeRenderScale` outside the mutex: the game's
  recompute releases/creates DLSS features through the shim and the render thread takes the mutex
  to evaluate. `Install` calls it outside the mutex too.
- `g_config` is only replaced under the shim mutex; ngx_shim reads it under the mutex (including
  `OptimalSettingsCallback`). The game hooks read `HookSettings` atomics published by
  `game::ApplyConfig`. The log level is atomic.
- The game hooks pass straight through until `game::Activate`, which dllmain calls only after
  `ngx::Install` succeeded.
- `Bypass` means DLAA only everywhere (`Bypassed()` = bypass && !DlispActive()): in the upscaling
  modes jitter, the AA hold and the sharpen suppression stay on.

## Conventions (measured)

- Motion vectors: clip-space delta * render size; DLSS gets them scaled by (0.5, -0.5)
  (`MotionScaleX/Y`). Symptom of (1,1): softer, slightly aliased edges only while moving, no ghosting.
  Verify with `tools/motion_check.py` on a `DebugDumpFrames=8` dump taken while moving (5 s delay).
- Depth is reversed (sky = 0), `DepthInverted=1`. MVs are not jittered.
- Camera jitter h (Halton(2,3), 20 entries at scene+0xe6a0) moves the image by
  (h.x/2, -h.y/2) * renderScale render pixels (clip offset = h / output size).
- The game passes `Hint.HDR=1`; colour is R11G11B10_FLOAT (fmt 26); DLSS gets IsHDR at the TAA
  position. DLAA input at the title: median ~0.09, max ~16; the DLISP input is ~10x darker.

## Game RE reference (MHW:I 15.23 exe, absolute VAs)

Globals: renderer `0x1451c4480`, scene `0x1451c4368`, camera `0x1451c4400`, unit list
`[0x1451238c8]+0x15d0` (next at +0x30), option gate `[0x1451c5320]+0x2cda` (1 on PC), DX12 flag
`0x1451cc080`.

Renderer fields: +0x78 device (device+0x42700 DLSS context, +0x42718 ready, +0x1400 D3D11 context),
+0xa5 DLSS on, +0xa6 DLISP on, +0xa8 mode (2 = DLSS on), +0xac user sharpness, +0xb0 DLSS scale,
+0xb4 DLSS type (1 = DLSS at 1.0, 3 = DLSS + DLISP below 1.0), +0x200/+0x204 render scale
requested/applied, +0x1f208 render frame, +0x1f210 frame, +0x1f450 view (+0x58 flags: bits 0x12 /
0x17 skip TAA). Plugin test: DLSS enabled = +0xa5 && (+0xb4 & 1); DLISP active = +0xa6 && (+0xb4 & 2)
&& scale != 1.

TAA filter uTemporalAAFilter2, vtable `0x14351bda8`: +0x48 update `0x1423931e0` (hooked; jitter
when blend +0x180 != 0, type +0xc == 8, flags +0x14 & 0x12 == 0x12, enable +0x184), +0x50 late update
`0x142393260` (syncs flag bit 0x2 from the scene TAA switch, clears +0x187 when DLSS is on), +0x60
draw `0x142392150` (hooked; returns early when the scene TAA switch is 0). Fields: +0x168 history[3]
(frame % 3), +0x187 mSharpen, +0x198 sharpen amount, +0x1a8 resolved velocity. Camera: +0x38
jitter, +0x40 jitter frame.

uFXAAFilter, vtable `0x14351c1a8`: draw `0x1423939d0` (returns early when the scene FXAA switch is
0), +0x168 input texture.

Anti-aliasing option (`Anti-Aliasing` in graphics_option.ini: Off/TAA/FXAA/TAA+FXAA = 0..3, read
from the ini file itself by `0x1404413e0`):
- Apply `0x14043dcb0`: if DLSS is available (`0x1404431f0`), queries the optimal settings
  (`0x142383ef0` -> the plugin's callback) and maps DLSSMode 1 below scale 1 to TAA only, DLSSMode 2
  to TAA+FXAA, DLSSMode 1 at scale 1 (DLAA) to "keep the option". Then the scene setter
  `0x142287a00` writes the word at scene+0xe9e4 (TAA low byte, FXAA high byte), and the unit loop
  sets each filter's bit 0x2.
- Runs only when options are applied (start-up, options menu; `0x14043e220` applies the DLSS
  option and clears the renderer DLSS type via `0x14229bed0` before its AA apply), never on a
  plugin Mode change.
- The plugin's `HoldTaaOnly` (game.cpp, called from the TAA update every frame) keeps the word at
  0x0001 while DLSS runs and gives the game's value back only when the plugin ends the hold
  itself (Bypass in DLAA, NGX unavailable) and the word still holds the plugin's value
  (`g_aaOurs`). The setter is pattern-scanned for its offset only, not patched.
- Measured before the fix (DLAA, TAA+FXAA stored): FXAA ran after DLSS (static detail -0.3%, p99
  edge change ~4/255); scene TAA 0 skipped the TAA draw and with it DLSS (plain jaggies).

Textures: object +0x60 colour native, +0xf0 depth-view native, natives refcounted at +8, released
by `0x142554f90` (lock xadd -1, destroy via vtable[0] at 1). Native +0x18 ID3D11 resource, +0x20
ID3D12 resource. Draw context +0x1d0 view, view +0x628 targets, targets +0xa0 depth copy.

Game DLSS 1 details: DLISP (`nvngx_dlisp`) maps NGX "Sharpness" to clamp(0.75*(S-1), 0, 1.5), so
the stock 0..1 slider did nothing; the game sets Sharpness only on the DLISP evaluate. The game's
DLSS 1 call sets "MV.Scale.X/Y" = 1.0. Fatal GPU handler: `0x1423a9cf0` -> GetDeviceRemovedReason ->
"ERR12 : Graphics device crashed." -> writes 0xdeadbeef to address 0 (`0x1421fd0c9`).

Driver DLSS store: `%ProgramData%\NVIDIA\NGX\models\dlss\versions\<(maj<<16)|(min<<8)|patch>\files\
<id>.bin`; MHW's own OTA entries are app B9D2F5C (DLSS 1.2.110), the NVIDIA App override build is
app E658700; `models\nvngx_config.txt` lists versions. The plugin copies the newest >= 3 to
`MHW-DLSS4\driver\nvngx_dlss.dll`.

## Tools

- `tools/` top level: `deploy.ps1`, `package.ps1`, `capture-window.ps1`, `screenshot.ps1`,
  `try.ps1` (set ini keys, wait, capture; appends missing keys at the end of the ini, i.e. in
  [DLSS]; the config falls back across sections), `experiment.ps1` + `metrics.py` (detail /
  flicker per ini variant), `burst.ps1`, `shift_compare.py`, `jitter_analysis.py`,
  `motion_check.py`, `mv_analysis.py`, `bin2png.py`.
- `tools/re/` (run on `re\MonsterHunterWorld.exe.unpacked.exe`): `strfind.py` (strings + VA),
  `ripxref.py` (RIP-relative refs), `ptrfind.py` (absolute pointers, vtables, tables), `callfind.py`,
  `funcroot.py` (function start via chained unwind info + callers), `dis.py` (capstone, `--func`),
  `qdump.py` (qwords with string resolution), `dispfind.py` (instructions with a given
  displacement), `funcdiff.py` (compare plugin builds function by function via PDB symbols from
  `llvm-pdbutil`), `fatbin_ptx.py` (PTX from CUDA fatbins, used on nvngx_dlisp).
  rizin also works on the unpacked exe (`pD len @ addr`; `af`+`pdf` fail on big functions); in Git
  Bash call `rz-bin.cmd` etc. (the shims are .cmd).
- `tools/live/` (ReadProcessMemory/WriteProcessMemory on the running game): `aapeek.py` (AA
  switches, filter flags, renderer DLSS fields), `aa_ab.py` (toggle a switch and capture),
  `natpeek.py` (texture natives and refcounts), `campeek.py` (camera), `modetest.py` (run-time Mode
  switch test), `bypasstest.py` (AA give-back + VRAM leak check), `bypassupscale.py` (Bypass in
  Performance keeps the hold), `titletest.ps1`, `abmulti.ps1`
  (multi-frame A/B captures), `specshift.py`, `jitregion.py`, `blocky.py` (dump/capture analysis).

## History and open items

- 0.1 first DLSS 4 port; 0.2 RCAS, no UltraPerformance; 0.3 MV scale (0.5,-0.5); 0.4 NVSharpen;
  0.5 driver DLSS store + DLLPath, default sharpness; 0.6/0.7 Sharpener = NVSharpen | Vanilla |
  RCAS, Sharpness 0..1 per filter, default 0.33; 0.7.1 depth-view refcount (exploded geometry);
  0.7.2 D3D12 retire queue (run-time Mode switch GPU fault) and anti-aliasing hold; 0.7.3 fixes
  from a code review: retire clock on renderer frames, per-API NGX shutdown, Bypass DLAA-only,
  thread safety (ini reload on the main thread, recompute outside the mutex, atomic hook settings,
  depth-alias CAS, sharpen-flag guard), hooks inactive until NGX is installed, AA hold released
  when NGX is unavailable, mirrors freed in DLAA, sharpen uses the retire queue, test scripts
  handle LF inis. 0.8.0 rename; 0.8.1 SharpPluginLoader 2.0 (early load); 0.8.2 optimised build,
  ini checked by time (500 ms), developer keys out of the default ini (listed in
  docs/technical.md; `Bypass` moved to [DLSS]), renderer state compared without formatting.
- Not exercised by tests: the NGX shutdown path (only at game exit; the test runs kill the game)
  and the NGX-unavailable release.
- To verify: run-time Mode switches with a real HDR10 swapchain (Windows HDR on, ScreenMode=HDRMode;
  the 0.7.2 tests ran with the swapchain in SDR format 87). The 0.7.1 fault happened in HDR mode.
- Known content limits: weapon/specular shine shows render-pixel steps in the upscaling modes
  (the game's own specular aliasing: highlights change 3-4x more between frames than the jitter
  explains; same in DLAA at 1-pixel scale). Lower sharpening or Quality/DLAA helps; a firefly clamp
  would cost real highlights. The checkerboard on planks is the game's dithered transparency.
- Known limitations: in the upscaling modes DLSS sees the frame after post-processing; texture mip
  bias is not lowered; the game's TAA still runs under DLSS in DLAA (result discarded).
