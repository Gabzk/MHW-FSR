#pragma once

#include <cstdint>
#include <optional>

struct ID3D12Resource;

namespace mhwfsr::game {

// Layout of the game's DLSS context object (0x48 bytes), the one stored at
// device + 0x42700 and created by the game's NGX init wrapper.
struct DlssContext {
    void* params;          // NVSDK_NGX_Parameter*
    void* d3d11Context;    // ID3D11DeviceContext* (engine struct slot)
    uint8_t lock[0x28];    // CRITICAL_SECTION
    void* dlssHandle;      // NVSDK_NGX_Handle* for SuperSampling
    void* dlispHandle;     // NVSDK_NGX_Handle* for ImageSignalProcessing
};
static_assert(sizeof(DlssContext) == 0x48);

struct Jitter {
    float x = 0.0f;
    float y = 0.0f;
    uint64_t frame = 0;
    bool cut = false;  // the camera jumped since the previous frame: FSR history must not be used
    float fovY = 0.785398f;
    float cameraNear = 0.1f;
    float cameraFar = 1000.0f;
    bool infiniteDepth = false;
    float cameraPosition[3] = {0.0f, 0.0f, 0.0f};
    float cameraUp[3] = {0.0f, 1.0f, 0.0f};
    float cameraRight[3] = {1.0f, 0.0f, 0.0f};
    float cameraForward[3] = {0.0f, 0.0f, 1.0f};
};

// Finds the game globals and hooks the TAA filter. Safe to call under the loader lock. The hooks
// pass straight through to the game until Activate.
bool Init();
// Makes the hooks act, once the NGX side is installed too.
void Activate();

// The renderer's device wrapper and the DLSS context living in it.
uint8_t* Device();
DlssContext* Context();
void SetContext(DlssContext* ctx);

// The game's renderer API; nullopt when the flag could not be located.
std::optional<bool> IsDx12();

// Allocates through the game's operator new, so the game may free the block itself.
void* GameAlloc(size_t size);

// Called at every TAA pass build, before the pass is built. The game builds the pass on worker
// threads, possibly more than once per frame and at the same time.
void SetFrameCallback(void (*callback)());
// Called once per frame from the TAA filter's update (which also runs while the filter is switched
// off), part of the main thread's update: mostly on the main thread, sometimes on a job thread.
void SetUpdateCallback(void (*callback)());
// Called at the start of every game frame on the main thread, before the game's frame rate limiter
// and its input polling.
void SetFrameStartCallback(void (*callback)());
// Called when the game polls the mouse, once per frame on the main thread.
void SetInputCallback(void (*callback)());

// The swap chain the game presents through (IDXGISwapChain*), null before it has one.
void* SwapChain();
// The events the main thread starts the render thread with and the render thread signals when it
// has submitted the frame (null when the game renders on the main thread).
struct RenderEvents {
    void* start = nullptr;
    void* done = nullptr;
};
RenderEvents RenderThreadEvents();
// The exe's import slots for SetEvent and WaitForSingleObject, which the game sets and waits for
// the render thread events through (null when not found).
void** SetEventImport();
void** WaitForSingleObjectImport();

// The renderer's frame counter (0 before the renderer exists).
uint64_t Frame();

// Re-runs the game's "DLSS settings changed" path so render scale and scene resolve use come from
// the current optimal-settings callback. Call from the update callback (main thread), where the
// game applies graphics options itself, and without holding locks the render thread takes.
bool RecomputeRenderScale();

// Forces renderer DLSS feature bits so game menus unlock the NVIDIA DLSS option on non-NVIDIA GPUs.
void ForceEnableDlssSupport();

// NVIDIA DLSS is on in the graphics options and the renderer runs a DLSS pass (not DLISP alone).
bool DlssEnabledInGame();
// NVIDIA DLSS is on in the graphics options, whichever mode the game picked for it.
bool DlssOptionOn();

// The game will run its scene resolve (DLISP) pass this frame (render scale below 1).
bool SceneResolveActive();

// Render resolution / output resolution as applied this frame (1 for NativeAA).
float RenderScale();

// Jitter recorded when the TAA pass that writes `output` was built.
std::optional<Jitter> JitterForOutput(const void* outputResource);

// Tells the jitter logic that FSR ran this frame (it only fills in skipped jitter while FSR runs).
void NoteDlssEvaluated();

}  // namespace mhwfsr::game
