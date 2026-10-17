# Release Notes — MHW-FSR v1.3.0

---

## 1. GitHub Release Notes (Markdown)

```markdown
# MHW-FSR v1.3.0: AMD FidelityFX Anti-Lag 2, Latency Profiler & Community Fixes

This release brings official, native **AMD FidelityFX Anti-Lag 2 SDK** integration, a full real-time **hardware timestamp & latency profiler**, and resolves key community feedback reported on Nexus Mods (including ROG Ally fallback fixes, Steam Cloud/save isolation, and controller navigation improvements).

---

### Highlights & What's New

#### ⚡ AMD FidelityFX Anti-Lag 2 SDK Integration
- **Direct Driver Synchronization**: Integrates the native `amdxc64.dll` driver pipeline. By synchronizing CPU input polling with the GPU dispatch pacing, click-to-photon latency is reduced drastically on AMD Radeon GPUs (tested drop from **~172 ms down to ~32 ms**).
- **In-Game Toggle**: Toggle Anti-Lag 2 anytime from the new **Anti-Lag 2 & Latency** tab in the **[F10]** Control Center.
- **Integrated Driver Max FPS Limiter**: Built-in frame rate limiter executed by the Radeon driver, preventing GPU render queue bloat and frame time spikes without external tools (RTSS, Special K, etc.).

#### 📊 Advanced Latency & Performance Profiler
- **Hardware D3D12 Timestamp Queries**: Accurate GPU execution measurement via `ID3D12QueryHeap` alongside high-precision CPU timers.
- **Diagnostics & Profiler Tab**:
  - **Upscaler Pass Latency**: Real-time breakdown of GPU execution vs. CPU command recording (`GPU ms / CPU ms`).
  - **Anti-Lag 2 Wait Time**: Milliseconds spent synchronizing the CPU input frame with GPU pacing.
  - **GPU Render Queue Latency**: Latency of in-flight commands waiting in the GPU execution queue.
  - **PC Latency (Input-to-Present)**: True end-to-end latency from input poll to swapchain presentation.
  - **Live Frametime History**: Interactive smoothed frame time graph with real-time FPS readout.
- **On-Screen Latency Breakdown**: Optional corner OSD display showing Upscale, AL2 Wait, and PC Latency in real time.

#### 🛠️ Community Feedback & Bug Fixes
- **ROG Ally & non-FSR4 GPU Auto-Fallback**: Fixed an issue where handhelds and GPUs lacking DirectML FSR 4 support would match the `.4` suffix of `"2.3.4"` in `Auto` mode. The loader now features a semantic version parser strictly falling back to high-quality **FSR 3.1.5**.
- **Steam Cloud & Save File Isolation**: Eliminated an unintended `.rdata` pointer overwrite in `FindGetProcAddressSlot` during early hook initialization that previously affected save loading and Steam Cloud synchronization.
- **OSD & FPS Counter INI Persistence**: Added `ShowOSD`, `ShowFPS`, and `ShowLatencyOSD` settings to `[General]` in `MHW-FSR.ini`. Display toggles are now permanently preserved across game restarts.
- **Controller X / Square Highlight Fix**: Fixed Dear ImGui's `NavUpdateWindowing` highlighting the corner OSD with a dark box when holding **X (Xbox)** or **Square (PlayStation)** with the overlay closed.
- **DirectML / FSR 4.1.1 Discovery**: Restored dynamic DLL search directory behavior so RDNA 3 / RDNA 4 systems reliably detect and initialize FSR 4.1.1 (DirectML).
- **Diagnostics Tab UI Alignment**: Corrected label widths and column offsets to prevent text truncation on long telemetry strings.

---

### Installation

1. Ensure **Monster Hunter: World** is set to **DirectX 12** mode.
2. Ensure [Stracker's Loader](https://www.nexusmods.com/monsterhunterworld/mods/1982) is installed.
3. Extract `MHW-FSR-1.3.0.zip` directly into your game folder (`Monster Hunter World`):
   ```
   nativePC\plugins\MHW-FSR.dll
   nativePC\plugins\MHW-FSR.ini
   nativePC\plugins\MHW-FSR\amd_fidelityfx_loader_dx12.dll
   nativePC\plugins\MHW-FSR\amd_fidelityfx_upscaler_dx12.dll
   nativePC\plugins\MHW-FSR\amd_fidelityfx_framegeneration_dx12.dll
   ```
4. In-game, press **[F10]** or **[Insert]** to open the FidelityFX Control Center.
```

---

## 2. Nexus Mods Update Description (BBCode)

```bbcode
[size=5][b]MHW-FSR v1.3.0: AMD FidelityFX Anti-Lag 2, Latency Profiler & Community Bug Fixes[/b][/size]

[b]v1.3.0 is a major update focused on ultra-low latency, hardware diagnostics, and resolving community-reported issues![/b]

[size=4][color=#ff4d4d][b]⚡ What's New in v1.3.0[/b][/color][/size]

[list]
[*] [b]AMD FidelityFX Anti-Lag 2 SDK Integration[/b]: Native driver-level latency reduction via [i]amdxc64.dll[/i]. Tested click-to-photon latency drops from [b]~172 ms down to ~32 ms[/b] on AMD Radeon GPUs! Toggle it on the fly under the [b]Anti-Lag 2 & Latency[/b] tab.
[*] [b]Integrated Driver Frame Pacing[/b]: Built-in Radeon driver frame limiter ([b]Driver Max FPS[/b]) to maintain smooth pacing and eliminate GPU render queue bloat without third-party limiters.
[*] [b]Advanced Latency & Performance Profiler[/b]: Comprehensive hardware telemetry under the [b]Diagnostics & Profiler[/b] tab:
    [list]
    [*] Upscaler Pass latency (GPU ms & CPU ms via D3D12 hardware timestamp queries).
    [*] Anti-Lag 2 driver sync wait delay.
    [*] In-flight GPU queue latency.
    [*] End-to-end PC Latency (Input-to-Present).
    [*] Real-time smoothed frametime history graph with FPS readout.
    [*] Optional live Latency OSD toggle.
    [/list]
[*] [b]ROG Ally & Handheld Fallback Fix[/b]: Handhelds and GPUs without FSR 4.1.1 (DirectML) support will now cleanly and strictly select [b]FSR 3.1.5[/b] in Auto mode (fixing the bug where it fell back to 2.3.4).
[*] [b]Steam Cloud & Save Data Fix[/b]: Fixed early memory hook isolation that previously caused issues with save loading or Steam Cloud sync for some users.
[*] [b]OSD & FPS Preferences Saved[/b]: Your settings for hiding/showing the corner OSD watermark and FPS counter are now permanently saved to [i]MHW-FSR.ini[/i].
[*] [b]Controller X / Square Black Box Fix[/b]: Fixed the Dear ImGui navigation box highlighting the top-left OSD when holding X (Xbox) or Square (PlayStation) on a gamepad while the menu was closed.
[*] [b]FSR 4.1.1 Detection Fix[/b]: Fixed DLL search paths so RDNA 3 / RDNA 4 systems always find and load FSR 4.1.1 seamlessly.
[/list]

[size=4][color=#ff4d4d][b]📥 Installation[/b][/color][/size]
[olist]
[*] Make sure the game is running in [b]DirectX 12[/b] and you have [url=https://www.nexusmods.com/monsterhunterworld/mods/1982]Stracker's Loader[/url] installed.
[*] Extract the zip into your main game directory ([i]Monster Hunter World[/i]).
[*] In-game, press [b][F10][/b] or [b][Insert][/b] to open the Control Center anytime!
[/olist]
```
