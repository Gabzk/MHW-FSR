#include "antilag.h"
#include "../third_party/amd/antilag2/ffx_antilag2_dx12.h"
#include "log.h"
#include "config.h"

#include <mutex>
#include <atomic>

namespace mhwfsr::antilag {

namespace {

std::mutex g_mutex;
AMD::AntiLag2DX12::Context g_context{};
std::atomic<bool> g_supported{false};
std::atomic<bool> g_active{false};
std::atomic<bool> g_initialized{false};

}  // namespace

bool Init(ID3D12Device* device) {
    if (!device) return false;

    std::lock_guard lock(g_mutex);
    if (g_initialized.load()) return g_supported.load();

    HRESULT hr = AMD::AntiLag2DX12::Initialize(&g_context, device);
    if (SUCCEEDED(hr)) {
        g_supported.store(true);
        g_initialized.store(true);
        LOGI("AMD FidelityFX Anti-Lag 2: supported and initialized successfully (amdxc64.dll)");
        return true;
    } else {
        g_supported.store(false);
        g_initialized.store(true);
        if (hr == E_HANDLE) {
            LOGI("AMD FidelityFX Anti-Lag 2: not available (non-AMD GPU or driver without amdxc64.dll)");
        } else {
            LOGW("AMD FidelityFX Anti-Lag 2: initialization returned {:#x}", static_cast<uint32_t>(hr));
        }
        return false;
    }
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_initialized.load()) {
        if (g_supported.load()) {
            AMD::AntiLag2DX12::DeInitialize(&g_context);
            LOGI("AMD FidelityFX Anti-Lag 2: deinitialized");
        }
        g_supported.store(false);
        g_active.store(false);
        g_initialized.store(false);
    }
}

void Update(bool enabled, uint32_t maxFps) {
    if (!g_supported.load()) {
        g_active.store(false);
        return;
    }

    HRESULT hr = AMD::AntiLag2DX12::Update(&g_context, enabled, maxFps);
    bool active = enabled && SUCCEEDED(hr);
    if (active != g_active.load()) {
        LOGI("AMD FidelityFX Anti-Lag 2: status changed to {} (enabled={}, maxFps={})",
             active ? "ACTIVE" : "INACTIVE", enabled, maxFps);
    }
    g_active.store(active);
}

void MarkEndOfFrameRendering() {
    if (!g_supported.load() || !g_active.load()) return;
    AMD::AntiLag2DX12::MarkEndOfFrameRendering(&g_context);
}

void SetFrameGenFrameType(bool bInterpolatedFrame) {
    if (!g_supported.load() || !g_active.load()) return;
    AMD::AntiLag2DX12::SetFrameGenFrameType(&g_context, bInterpolatedFrame);
}

bool IsSupported() {
    return g_supported.load();
}

bool IsActive() {
    return g_active.load();
}

}  // namespace mhwfsr::antilag
