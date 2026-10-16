#include "framegen.h"
#include "barrier_batcher.h"
#include "config.h"
#include "d3d_util.h"
#include "fsr_loader.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include <ffx_api.hpp>
#include <ffx_framegeneration.hpp>
#include <dx12/ffx_api_dx12.hpp>

namespace mhwfsr::framegen {

namespace {

std::recursive_mutex g_mutex;
ffx::Context g_fgContext{};
ID3D12Device* g_device = nullptr;
bool g_ready = false;
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_supported{false};
std::atomic<bool> g_hasFrameData{false};

uint32_t g_displayW = 0;
uint32_t g_displayH = 0;
DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
DXGI_FORMAT g_contextHudLessFormat = DXGI_FORMAT_UNKNOWN;
uint64_t g_fgFrameId = 0;

d3d::Mirror12 g_depthMirror;
d3d::Mirror12 g_motionMirror;
ComPtr<ID3D12Resource> g_realFrameBackup;
ComPtr<ID3D12Resource> g_hudLessBackupBgra;
ComPtr<ID3D12Resource> g_hudLessBackupRgba;
ComPtr<ID3D12Resource> g_interpOutput;
std::atomic<ID3D12Resource*> g_capturedHudLessRes{nullptr};
std::atomic<DXGI_FORMAT> g_capturedHudLessFormat{DXGI_FORMAT_UNKNOWN};
std::atomic<uint64_t> g_hudLessCapturedCount{0};
FrameData g_frameData{};

DXGI_FORMAT NormalizeFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return fmt;
    }
}

bool CreateInternalTextures(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    g_realFrameBackup.Reset();
    g_hudLessBackupBgra.Reset();
    g_hudLessBackupRgba.Reset();
    g_interpOutput.Reset();
    g_capturedHudLessRes.store(nullptr, std::memory_order_relaxed);
    g_capturedHudLessFormat.store(DXGI_FORMAT_UNKNOWN, std::memory_order_relaxed);

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                 IID_PPV_ARGS(&g_realFrameBackup));
    if (FAILED(hr)) {
        LOGE("FSR FG: failed to create g_realFrameBackup ({}x{}, fmt {}, hr {:#x})",
             width, height, static_cast<int>(format), static_cast<uint32_t>(hr));
        return false;
    }

    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                         IID_PPV_ARGS(&g_hudLessBackupBgra));
    if (FAILED(hr)) {
        LOGW("FSR FG: failed to create g_hudLessBackupBgra (hr {:#x})", static_cast<uint32_t>(hr));
    }

    DXGI_FORMAT altFormat = (format == DXGI_FORMAT_B8G8R8A8_UNORM) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        D3D12_RESOURCE_DESC altDesc = desc;
        altDesc.Format = altFormat;
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &altDesc,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                        IID_PPV_ARGS(&g_hudLessBackupRgba));
    }

    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                         IID_PPV_ARGS(&g_interpOutput));
    if (FAILED(hr)) {
        LOGE("FSR FG: failed to create g_interpOutput UAV ({}x{}, fmt {}, hr {:#x})",
             width, height, static_cast<int>(format), static_cast<uint32_t>(hr));
        g_realFrameBackup.Reset();
        g_hudLessBackupBgra.Reset();
        g_hudLessBackupRgba.Reset();
        return false;
    }

    return true;
}

bool CreateFgContextLocked(ID3D12Device* device, uint32_t displayW, uint32_t displayH,
                           DXGI_FORMAT backBufferFormat, DXGI_FORMAT hudLessFormat) {
    if (g_fgContext) {
        ffx::DestroyContext(g_fgContext);
        g_fgContext = {};
    }

    uint32_t flags = 0;
    if (backBufferFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || backBufferFormat == DXGI_FORMAT_R10G10B10A2_UNORM ||
        backBufferFormat == DXGI_FORMAT_R11G11B10_FLOAT) {
        flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    }
    if (g_config.depthInverted) flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    if (g_config.depthInfinite) flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    if (g_config.mvJittered) flags |= FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;

    ffx::CreateContextDescFrameGeneration fgDesc{};
    fgDesc.flags = flags;
    fgDesc.displaySize = { displayW, displayH };
    fgDesc.maxRenderSize = { displayW, displayH };
    fgDesc.backBufferFormat = ffxApiGetSurfaceFormatDX12(backBufferFormat);

    ffx::CreateContextDescFrameGenerationHudless hudlessDesc{};
    hudlessDesc.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(hudLessFormat);

    ffx::CreateContextDescFrameGenerationVersion versionDesc{};
    versionDesc.version = FFX_FRAMEGENERATION_VERSION;

    ffx::CreateBackendDX12Desc backendDesc{};
    backendDesc.device = device;

    ffx::ReturnCode rc = ffx::CreateContext(g_fgContext, nullptr, fgDesc, hudlessDesc, backendDesc, versionDesc);
    if (!rc) {
        LOGE("FSR Frame Generation CreateContext failed (code {:#x}, bbFmt={}, hudLessFmt={})",
             static_cast<uint32_t>(rc), static_cast<int>(backBufferFormat), static_cast<int>(hudLessFormat));
        return false;
    }

    g_contextHudLessFormat = hudLessFormat;
    g_fgFrameId = 0;
    return true;
}

void ShutdownLocked() {
    g_hasFrameData.store(false);
    g_capturedHudLessRes.store(nullptr, std::memory_order_relaxed);
    g_capturedHudLessFormat.store(DXGI_FORMAT_UNKNOWN, std::memory_order_relaxed);
    g_depthMirror.Reset();
    g_motionMirror.Reset();
    d3d::Retire(std::move(g_realFrameBackup));
    d3d::Retire(std::move(g_hudLessBackupBgra));
    d3d::Retire(std::move(g_hudLessBackupRgba));
    d3d::Retire(std::move(g_interpOutput));
    if (g_ready) {
        if (g_fgContext) {
            ffx::DestroyContext(g_fgContext);
            g_fgContext = {};
        }
        g_ready = false;
        LOGI("AMD FidelityFX Frame Generation shutdown");
    }
}

}  // namespace

bool CheckSupport(ID3D12Device* device) {
    if (!device) return false;
    bool supp = loader::HasFrameGenSupport(device);
    g_supported.store(supp);
    if (supp) {
        LOGI("AMD FidelityFX Frame Generation: supported on device (amd_fidelityfx_framegeneration_dx12.dll)");
    } else {
        LOGI("AMD FidelityFX Frame Generation: runtime provider not available on this device");
    }
    return supp;
}

bool Init(ID3D12Device* device, uint32_t displayW, uint32_t displayH, DXGI_FORMAT backBufferFormat) {
    if (!device || displayW == 0 || displayH == 0) return false;

    DXGI_FORMAT normFormat = NormalizeFormat(backBufferFormat);

    std::lock_guard lock(g_mutex);
    if (g_ready && g_displayW == displayW && g_displayH == displayH && g_format == normFormat &&
        g_realFrameBackup && g_interpOutput) {
        return true;
    }

    ShutdownLocked();

    if (!loader::HasFrameGenSupport(device)) {
        LOGI("FSR Frame Generation: runtime provider not available on this device");
        g_supported.store(false);
        return false;
    }
    g_supported.store(true);

    if (!CreateInternalTextures(device, displayW, displayH, normFormat)) {
        return false;
    }

    if (!CreateFgContextLocked(device, displayW, displayH, normFormat, normFormat)) {
        g_realFrameBackup.Reset();
        g_hudLessBackupBgra.Reset();
        g_hudLessBackupRgba.Reset();
        g_interpOutput.Reset();
        return false;
    }

    g_device = device;
    g_displayW = displayW;
    g_displayH = displayH;
    g_format = normFormat;
    g_hasFrameData.store(false);
    g_ready = true;

    LOGI("AMD FidelityFX Frame Generation initialized successfully ({}x{}, format {})",
         displayW, displayH, static_cast<int>(normFormat));
    return true;
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    ShutdownLocked();
}

void CaptureHudLess(ID3D12GraphicsCommandList* cl, ID3D12Resource* sourceTexture, D3D12_RESOURCE_STATES currentState) {
    if (!IsActive() || !cl || !sourceTexture) return;

    D3D12_RESOURCE_DESC srcDesc = sourceTexture->GetDesc();
    if (srcDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        srcDesc.Width != g_displayW || srcDesc.Height != g_displayH) {
        return;
    }

    DXGI_FORMAT normSrcFmt = NormalizeFormat(srcDesc.Format);
    ID3D12Resource* dst = nullptr;
    if (normSrcFmt == g_format && g_hudLessBackupBgra) {
        dst = g_hudLessBackupBgra.Get();
    } else if ((normSrcFmt == DXGI_FORMAT_R8G8B8A8_UNORM || normSrcFmt == DXGI_FORMAT_B8G8R8A8_UNORM) && g_hudLessBackupRgba) {
        dst = g_hudLessBackupRgba.Get();
    } else {
        return;
    }

    d3d::BarrierBatcher batcher(cl);
    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        batcher.Transition(sourceTexture, currentState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    batcher.Transition(dst, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    batcher.Flush();

    cl->CopyResource(dst, sourceTexture);

    batcher.Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (currentState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        batcher.Transition(sourceTexture, D3D12_RESOURCE_STATE_COPY_SOURCE, currentState);
    }
    batcher.Flush();

    g_capturedHudLessFormat.store(normSrcFmt, std::memory_order_release);
    g_capturedHudLessRes.store(dst, std::memory_order_release);

    uint64_t count = g_hudLessCapturedCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 5) {
        LOGI("FSR FG: captured HUDLessColor #{} from {} ({}x{}, srcFmt={}, normFmt={}, state={:#x})",
             count, static_cast<void*>(sourceTexture), srcDesc.Width, srcDesc.Height,
             static_cast<int>(srcDesc.Format), static_cast<int>(normSrcFmt), static_cast<uint32_t>(currentState));
    }
}

bool HasHudLessForCurrentFrame() {
    return g_capturedHudLessRes.load(std::memory_order_acquire) != nullptr;
}

void RecordFrameData(ID3D12GraphicsCommandList* cl, const FrameData& data) {
    if (!IsActive() || !cl || !data.depth || !data.motion) {
        g_hasFrameData.store(false);
        return;
    }

    std::lock_guard lock(g_mutex);
    ID3D12Resource* mirroredDepth = g_depthMirror.Update(cl, data.depth);
    ID3D12Resource* mirroredMotion = g_motionMirror.Update(cl, data.motion);
    if (!mirroredDepth || !mirroredMotion) {
        g_hasFrameData.store(false);
        return;
    }

    g_frameData = data;
    g_frameData.depth = mirroredDepth;
    g_frameData.motion = mirroredMotion;
    g_hasFrameData.store(true);
}

bool HasPendingFrame() {
    return IsActive() && g_hasFrameData.load(std::memory_order_acquire);
}

bool GenerateInterpolatedFrame(ID3D12GraphicsCommandList* cl, ID3D12Resource* currentBackBuffer) {
    if (!IsActive() || !cl || !currentBackBuffer) {
        return false;
    }

    std::lock_guard lock(g_mutex);
    ID3D12Resource* hudLessRes = g_capturedHudLessRes.exchange(nullptr, std::memory_order_acq_rel);
    DXGI_FORMAT hudLessFmt = g_capturedHudLessFormat.load(std::memory_order_acquire);

    if (!g_hasFrameData.exchange(false) || !g_frameData.depth || !g_frameData.motion ||
        !g_realFrameBackup || !g_interpOutput) {
        return false;
    }

    // Check if backbuffer dimensions match
    D3D12_RESOURCE_DESC bbDesc = currentBackBuffer->GetDesc();
    if (bbDesc.Width != g_displayW || bbDesc.Height != g_displayH) {
        return false;
    }

    // Ensure FG context hudlessBackBufferFormat matches the active interpolation source format
    DXGI_FORMAT targetHudLessFmt = (hudLessRes && hudLessFmt != DXGI_FORMAT_UNKNOWN) ? hudLessFmt : g_format;
    if (targetHudLessFmt != g_contextHudLessFormat && g_device) {
        LOGI("FSR FG: recreating context for HUDLess format {} (backbuffer format {})",
             static_cast<int>(targetHudLessFmt), static_cast<int>(g_format));
        if (!CreateFgContextLocked(g_device, g_displayW, g_displayH, g_format, targetHudLessFmt)) {
            return false;
        }
    }

    // 1. Copy currentBackBuffer (Real Frame N in PRESENT state) -> g_realFrameBackup
    {
        d3d::BarrierBatcher batcher(cl);
        batcher.Transition(currentBackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        batcher.Transition(g_realFrameBackup.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        batcher.Flush();

        cl->CopyResource(g_realFrameBackup.Get(), currentBackBuffer);

        batcher.Transition(g_realFrameBackup.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        batcher.Flush();
    }

    const uint64_t fgFrameId = ++g_fgFrameId;
    const bool reset = g_frameData.reset || (fgFrameId == 1);

    // 2. Configure Frame Generation with NO_SWAPCHAIN_CONTEXT_NOTIFY
    ffx::ConfigureDescFrameGeneration configDesc{};
    configDesc.swapChain = nullptr;
    configDesc.presentCallback = nullptr;
    configDesc.presentCallbackUserContext = nullptr;
    configDesc.frameGenerationCallback = nullptr;
    configDesc.frameGenerationCallbackUserContext = nullptr;
    configDesc.frameGenerationEnabled = true;
    configDesc.allowAsyncWorkloads = false;
    if (hudLessRes) {
        configDesc.HUDLessColor = ffxApiGetResourceDX12(hudLessRes, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    } else {
        configDesc.HUDLessColor = FfxApiResource({});
    }
    configDesc.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    if (g_config.fgDebugView) {
        configDesc.flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;
    }
    if (g_config.fgDebugTearLines) {
        configDesc.flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES |
                            FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS;
    }
    configDesc.onlyPresentGenerated = false;
    configDesc.generationRect = { 0, 0, static_cast<int32_t>(g_displayW), static_cast<int32_t>(g_displayH) };
    configDesc.frameID = fgFrameId;

    ffx::ReturnCode rc = ffx::Configure(g_fgContext, configDesc);
    if (!rc) {
        LOGW("FSR FG Configure failed (code {:#x})", static_cast<uint32_t>(rc));
        d3d::BarrierBatcher batcher(cl);
        batcher.Transition(currentBackBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        batcher.Flush();
        return false;
    }

    // 3. Prepare frame data (dilated depth & motion vectors, camera info)
    ffx::DispatchDescFrameGenerationPrepareV2 prepDesc{};
    prepDesc.commandList = cl;
    prepDesc.frameID = fgFrameId;
    prepDesc.flags = configDesc.flags;
    prepDesc.renderSize = { g_frameData.renderW, g_frameData.renderH };
    prepDesc.jitterOffset = { g_frameData.jitterX, g_frameData.jitterY };
    prepDesc.motionVectorScale = { g_frameData.mvScaleX, g_frameData.mvScaleY };
    prepDesc.frameTimeDelta = std::max(0.1f, g_frameData.deltaMs);
    prepDesc.cameraNear = g_frameData.cameraNear;
    prepDesc.cameraFar = g_frameData.cameraFar;
    prepDesc.cameraFovAngleVertical = g_frameData.fovY;
    prepDesc.viewSpaceToMetersFactor = 1.0f;
    prepDesc.depth = ffxApiGetResourceDX12(g_frameData.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prepDesc.motionVectors = ffxApiGetResourceDX12(g_frameData.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    std::memcpy(prepDesc.cameraPosition, g_frameData.cameraPosition, sizeof(prepDesc.cameraPosition));
    std::memcpy(prepDesc.cameraUp, g_frameData.cameraUp, sizeof(prepDesc.cameraUp));
    std::memcpy(prepDesc.cameraRight, g_frameData.cameraRight, sizeof(prepDesc.cameraRight));
    std::memcpy(prepDesc.cameraForward, g_frameData.cameraForward, sizeof(prepDesc.cameraForward));
    prepDesc.reset = reset;

    rc = ffx::Dispatch(g_fgContext, prepDesc);
    if (!rc) {
        LOGW("FSR FG PrepareV2 failed (code {:#x})", static_cast<uint32_t>(rc));
        d3d::BarrierBatcher batcher(cl);
        batcher.Transition(currentBackBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        batcher.Flush();
        return false;
    }

    // 4. Dispatch Optical Flow + Frame Interpolation into g_interpOutput
    ffx::DispatchDescFrameGeneration genDesc{};
    genDesc.commandList = cl;
    genDesc.presentColor = ffxApiGetResourceDX12(g_realFrameBackup.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    genDesc.outputs[0] = ffxApiGetResourceDX12(g_interpOutput.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    genDesc.numGeneratedFrames = 1;
    genDesc.reset = reset;
    if (g_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        genDesc.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
    } else if (g_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        genDesc.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
    } else {
        genDesc.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    }
    genDesc.minMaxLuminance[0] = 0.0f;
    genDesc.minMaxLuminance[1] = 1000.0f;
    genDesc.generationRect = { 0, 0, static_cast<int32_t>(g_displayW), static_cast<int32_t>(g_displayH) };
    genDesc.frameID = fgFrameId;

    rc = ffx::Dispatch(g_fgContext, genDesc);
    if (!rc) {
        LOGW("FSR FG Dispatch failed (code {:#x})", static_cast<uint32_t>(rc));
        d3d::BarrierBatcher batcher(cl);
        batcher.Transition(currentBackBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        batcher.Flush();
        return false;
    }

    // 5. Copy g_interpOutput (interpolated frame) -> currentBackBuffer and restore states
    {
        d3d::BarrierBatcher batcher(cl);
        batcher.Uav(g_interpOutput.Get());
        batcher.Transition(g_interpOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        batcher.Transition(currentBackBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        batcher.Flush();

        cl->CopyResource(currentBackBuffer, g_interpOutput.Get());

        batcher.Transition(g_interpOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        batcher.Transition(currentBackBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        batcher.Flush();
    }

    if (fgFrameId <= 3 || (fgFrameId & (fgFrameId - 1)) == 0) {
        LOGI("FSR FG generated frame #{} ({}x{} -> {}x{}, reset={})",
             fgFrameId, g_frameData.renderW, g_frameData.renderH, g_displayW, g_displayH, reset);
    }

    return true;
}

bool CopyRealFrameToBackBuffer(ID3D12GraphicsCommandList* cl, ID3D12Resource* targetBackBuffer) {
    if (!cl || !targetBackBuffer) return false;
    std::lock_guard lock(g_mutex);
    if (!g_realFrameBackup) return false;

    d3d::BarrierBatcher batcher(cl);
    batcher.Transition(g_realFrameBackup.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    batcher.Transition(targetBackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    batcher.Flush();

    cl->CopyResource(targetBackBuffer, g_realFrameBackup.Get());

    batcher.Transition(g_realFrameBackup.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    batcher.Transition(targetBackBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
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
    bool prev = g_enabled.exchange(enabled);
    if (enabled && !prev) {
        g_fgFrameId = 0;
        g_hasFrameData.store(false);
    } else if (!enabled) {
        g_hasFrameData.store(false);
    }
}

bool IsEnabled() {
    return g_enabled.load();
}

}  // namespace mhwfsr::framegen
