#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct ID3D12Resource;
struct ID3D11Resource;

namespace mhwdlss {
struct Config;
}

namespace mhwdlss::game {

// Layout of the game's DLSS context object (0x48 bytes), the one stored at
// device + 0x42700 and created by the game's NGX init wrapper.
struct DlssContext {
    void* params;          // NVSDK_NGX_Parameter*
    void* d3d11Context;    // ID3D11DeviceContext* (DX11 only)
    uint8_t lock[0x28];    // CRITICAL_SECTION
    void* dlssHandle;      // NVSDK_NGX_Handle* for SuperSampling
    void* dlispHandle;     // NVSDK_NGX_Handle* for ImageSignalProcessing
};
static_assert(sizeof(DlssContext) == 0x48);

struct Jitter {
    float x = 0.0f;
    float y = 0.0f;
    uint64_t frame = 0;
};

// Finds the game globals and hooks the TAA filter. Safe to call under the loader lock. The hooks
// pass straight through to the game until Activate.
bool Init();
// Makes the hooks act, once the NGX side is installed too, with the settings from `config`.
void Activate(const Config& config);
// Publishes the settings the hooks read (call after every ini load).
void ApplyConfig(const Config& config);

// The renderer's device wrapper and the DLSS context living in it.
uint8_t* Device();
DlssContext* Context();
void SetContext(DlssContext* ctx);

// The game's renderer API; nullopt when the flag could not be located.
std::optional<bool> IsDx12();
// ID3D11DeviceContext* the game hands to NGX in DX11 mode.
void* ImmediateContext11();

// Allocates through the game's operator new, so the game may free the block itself.
void* GameAlloc(size_t size);

// Called at every TAA pass build, before the pass is built. The game builds the pass on worker
// threads, possibly more than once per frame and at the same time.
void SetFrameCallback(void (*callback)());
// Called once per frame on the game's main thread, from the TAA filter's update (which also runs
// while the filter is switched off).
void SetUpdateCallback(void (*callback)());

// The renderer's frame counter (0 before the renderer exists).
uint64_t Frame();

// Re-runs the game's "DLSS settings changed" path so render scale and DLISP use come from
// the current optimal-settings callback. Call from the update callback (main thread), where the
// game applies graphics options itself, and without holding locks the render thread takes.
bool RecomputeRenderScale();

// NGX could not be initialised: DLSS cannot run this session, so the game's own anti-aliasing
// setting is given back.
void SetDlssUnavailable(bool unavailable);

bool DlssEnabledInGame();

// The game will run its DLISP pass this frame (render scale below 1).
bool DlispActive();

// Render resolution / output resolution as applied this frame (1 for DLAA).
float RenderScale();

// One-line dump of the renderer's DLSS fields, for the log.
std::string DescribeState();

// Jitter recorded when the TAA pass that writes `output` was built.
std::optional<Jitter> JitterForOutput(const void* outputResource);

// Tells the jitter logic that DLSS ran this frame (it only fills in skipped jitter while DLSS runs).
void NoteDlssEvaluated();

// Jitter and TAA counters since the previous call, for the log.
std::string JitterStats();

}  // namespace mhwdlss::game
