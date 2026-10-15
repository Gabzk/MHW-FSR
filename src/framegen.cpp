#include "framegen.h"
#include "barrier_batcher.h"
#include "config.h"
#include "d3d_util.h"
#include "fsr_loader.h"
#include "log.h"

#include <mutex>
#include <atomic>
#include <algorithm>

#include <ffx_api.hpp>
#include <ffx_framegeneration.hpp>
#include <dx12/ffx_api_dx12.hpp>

namespace mhwfsr::framegen {

namespace {

std::mutex g_mutex;
ffx::Context g_fgContext{};
bool g_ready = false;
std::atomic<bool> g_enabled = false;
std::atomic<bool> g_supported = false;

uint32_t g_displayW = 0;
uint32_t g_displayH = 0;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;

d3d::Mirror12 g_hudLessMirror;
ID3D12Resource* g_hudLessResource = nullptr;
FrameData g_frameData{};

void FgMessageCallback(uint32_t type, const wchar_t* message) {
    if (!message) return;
    std::wstring_view msg(message);
    while (!msg.empty() && (msg.back() == L'\n' || msg.back() == L'\r')) {
        msg.remove_suffix(1);
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, msg.data(), static_cast<int>(msg.size()), nullptr, 0, nullptr, nullptr);
    if (len > 0) {
        std::string narrow(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, msg.data(), static_cast<int>(msg.size()), narrow.data(), len, nullptr, nullptr);
        if (type == FFX_API_MESSAGE_TYPE_ERROR) {
            LOGE("[FSR FG] {}", narrow);
        } else if (type == FFX_API_MESSAGE_TYPE_WARNING) {
            LOGW("[FSR FG] {}", narrow);
        } else {
            LOGD("[FSR FG] {}", narrow);
        }
    }
}

}  // namespace

bool Init(ID3D12Device* device, uint32_t displayW, uint32_t displayH, DXGI_FORMAT backBufferFormat) {
    if (!device || displayW == 0 || displayH == 0) return false;

    std::lock_guard lock(g_mutex);
    if (g_ready && g_displayW == displayW && g_displayH == displayH && g_format == backBufferFormat) {
        return true;
    }

    Shutdown();

    if (!loader::HasFrameGenSupport(device)) {
        LOGI("FSR Frame Generation: runtime provider not available on this device");
        g_supported.store(false);
        return false;
    }
    g_supported.store(true);

    uint32_t flags = FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    if (g_config.depthInverted) flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    if (g_config.depthInfinite) flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;

    ffx::CreateContextDescFrameGeneration fgDesc{};
    fgDesc.flags = flags;
    fgDesc.displaySize = { displayW, displayH };
    fgDesc.maxRenderSize = { displayW, displayH };
    fgDesc.backBufferFormat = ffxApiGetSurfaceFormatDX12(backBufferFormat);

    ffx::CreateContextDescFrameGenerationVersion versionDesc{};
    versionDesc.version = FFX_FRAMEGENERATION_VERSION;

    ffx::CreateBackendDX12Desc backendDesc{};
    backendDesc.device = device;

    ffx::ReturnCode rc = ffx::CreateContext(g_fgContext, nullptr, fgDesc, backendDesc, versionDesc);
    if (!rc) {
        LOGE("FSR Frame Generation CreateContext failed (code {:#x})", static_cast<uint32_t>(rc));
        return false;
    }

    g_displayW = displayW;
    g_displayH = displayH;
    g_format = backBufferFormat;
    g_ready = true;

    LOGI("AMD FidelityFX Frame Generation initialized successfully ({}x{}, format {})",
         displayW, displayH, static_cast<int>(backBufferFormat));
    return true;
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_ready) {
        ffx::DestroyContext(g_fgContext);
        g_fgContext = {};
        g_ready = false;
        g_hudLessMirror.Reset();
        g_hudLessResource = nullptr;
        LOGI("AMD FidelityFX Frame Generation shutdown");
    }
}

void CaptureHudLess(ID3D12GraphicsCommandList* cl, ID3D12Resource* upscaleOutput) {
    if (!g_enabled.load() || !g_ready || !cl || !upscaleOutput) {
        g_hudLessResource = nullptr;
        return;
    }
    g_hudLessResource = g_hudLessMirror.UpdateFromUav(cl, upscaleOutput);
}

void RecordFrameData(const FrameData& data) {
    std::lock_guard lock(g_mutex);
    g_frameData = data;
}

bool Dispatch(ID3D12GraphicsCommandList* cl, ID3D12Resource* currentBackBuffer, ID3D12Resource* targetBuffer) {
    if (!g_enabled.load() || !g_ready || !cl || !currentBackBuffer || !targetBuffer) {
        return false;
    }

    std::lock_guard lock(g_mutex);
    if (!g_frameData.depth || !g_frameData.motion) {
        return false;
    }

    // 1. Configure Frame Generation with HUDLess texture if captured
    ffx::ConfigureDescFrameGeneration configDesc{};
    configDesc.frameGenerationEnabled = true;
    configDesc.allowAsyncWorkloads = false;
    if (g_hudLessResource) {
        configDesc.HUDLessColor = ffxApiGetResourceDX12(g_hudLessResource, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    }
    configDesc.generationRect = { 0, 0, static_cast<int32_t>(g_displayW), static_cast<int32_t>(g_displayH) };
    configDesc.frameID = g_frameData.frameId;
    ffx::Configure(g_fgContext, configDesc);

    // 2. Prepare frame data (dilated depth & motion vectors, camera info)
    ffx::DispatchDescFrameGenerationPrepareV2 prepDesc{};
    prepDesc.commandList = cl;
    prepDesc.renderSize = { g_frameData.renderW, g_frameData.renderH };
    prepDesc.jitterOffset = { g_frameData.jitterX, g_frameData.jitterY };
    prepDesc.motionVectorScale = { g_frameData.mvScaleX, g_frameData.mvScaleY };
    prepDesc.frameTimeDelta = g_frameData.deltaMs;
    prepDesc.reset = g_frameData.reset;
    prepDesc.cameraNear = g_frameData.cameraNear;
    prepDesc.cameraFar = g_frameData.cameraFar;
    prepDesc.cameraFovAngleVertical = g_frameData.fovY;
    prepDesc.viewSpaceToMetersFactor = 1.0f;
    prepDesc.depth = ffxApiGetResourceDX12(g_frameData.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prepDesc.motionVectors = ffxApiGetResourceDX12(g_frameData.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prepDesc.frameID = g_frameData.frameId;

    ffx::ReturnCode rc = ffx::Dispatch(g_fgContext, prepDesc);
    if (!rc) {
        LOGW("FSR FG Prepare failed (code {:#x})", static_cast<uint32_t>(rc));
        return false;
    }

    // 3. Dispatch interpolated frame generation
    ffx::DispatchDescFrameGeneration genDesc{};
    genDesc.commandList = cl;
    genDesc.presentColor = ffxApiGetResourceDX12(currentBackBuffer, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    genDesc.outputs[0] = ffxApiGetResourceDX12(targetBuffer, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    genDesc.numGeneratedFrames = 1;
    genDesc.reset = g_frameData.reset;
    genDesc.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    genDesc.generationRect = { 0, 0, static_cast<int32_t>(g_displayW), static_cast<int32_t>(g_displayH) };
    genDesc.frameID = g_frameData.frameId;

    rc = ffx::Dispatch(g_fgContext, genDesc);
    if (!rc) {
        LOGW("FSR FG Dispatch failed (code {:#x})", static_cast<uint32_t>(rc));
        return false;
    }

    // 4. Synchronize GPU UAV writes on output
    d3d::BarrierBatcher batcher(cl);
    batcher.Uav(targetBuffer);
    batcher.Flush();

    return true;
}

bool IsSupported() {
    return g_supported.load();
}

bool IsActive() {
    return g_ready && g_enabled.load();
}

void SetEnabled(bool enabled) {
    g_enabled.store(enabled);
}

bool IsEnabled() {
    return g_enabled.load();
}

}  // namespace mhwfsr::framegen
