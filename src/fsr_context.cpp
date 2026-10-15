#include "fsr_context.h"
#include "d3d_util.h"
#include "log.h"

#include <string_view>

namespace mhwfsr {

namespace {

void FsrMessageCallback(uint32_t type, const wchar_t* message) {
    if (!message) return;
    std::wstring_view msg(message);
    while (!msg.empty() && (msg.back() == L'\n' || msg.back() == L'\r')) {
        msg.remove_suffix(1);
    }
    // Convert wide string to narrow for logging
    int len = WideCharToMultiByte(CP_UTF8, 0, msg.data(), static_cast<int>(msg.size()), nullptr, 0, nullptr, nullptr);
    if (len > 0) {
        std::string narrow(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, msg.data(), static_cast<int>(msg.size()), narrow.data(), len, nullptr, nullptr);
        if (type == FFX_API_MESSAGE_TYPE_ERROR) {
            LOGE("[FSR] {}", narrow);
        } else if (type == FFX_API_MESSAGE_TYPE_WARNING) {
            LOGW("[FSR] {}", narrow);
        } else {
            LOGD("[FSR] {}", narrow);
        }
    }
}

}  // namespace

FsrContext::~FsrContext() {
    Shutdown();
}

void FsrContext::Retire() {
    if (m_ready) {
        ffx::Context ctx = m_context;
        d3d::RetireFeature([ctx]() mutable {
            ffx::ReturnCode rc = ffx::DestroyContext(ctx);
            if (!rc) {
                LOGW("ffxDestroyContext failed ({:#x})", static_cast<uint32_t>(rc));
            }
        });
        m_ready = false;
        m_context = {};
    }
}

void FsrContext::Shutdown() {
    if (m_ready) {
        ffx::ReturnCode rc = ffx::DestroyContext(m_context);
        if (!rc) {
            LOGW("ffxDestroyContext failed ({:#x})", static_cast<uint32_t>(rc));
        }
        m_ready = false;
        m_context = {};
    }
}

bool FsrContext::Ensure(ID3D12Device* device, const Settings& settings) {
    if (m_ready && m_settings == settings && m_device == device) {
        return true;
    }
    if (m_failed && m_settings == settings && m_device == device) {
        return false;
    }

    Retire();
    m_ready = false;
    m_failed = true;
    m_settings = settings;
    m_device = device;

    if (!device) {
        LOGE("FsrContext::Ensure: null device");
        return false;
    }

    ffx::CreateContextDescUpscale upscaleDesc{};
    upscaleDesc.flags = settings.flags;
    upscaleDesc.maxRenderSize = { settings.renderW, settings.renderH };
    upscaleDesc.maxUpscaleSize = { settings.upscaleW, settings.upscaleH };
    upscaleDesc.fpMessage = &FsrMessageCallback;

    ffx::CreateContextDescUpscaleVersion versionDesc{};
    versionDesc.version = FFX_UPSCALER_VERSION;

    ffx::CreateBackendDX12Desc backendDesc{};
    backendDesc.device = device;

    ffx::ReturnCode rc;
    if (settings.versionId != 0) {
        ffx::CreateContextDescOverrideVersion overrideDesc{};
        overrideDesc.versionId = settings.versionId;
        rc = ffx::CreateContext(m_context, nullptr, upscaleDesc, backendDesc, versionDesc, overrideDesc);
    } else {
        rc = ffx::CreateContext(m_context, nullptr, upscaleDesc, backendDesc, versionDesc);
    }

    if (!rc) {
        LOGE("FSR CreateContext failed (code {:#x}): render {}x{}, upscale {}x{}, flags {:#x}, version {:#x}",
             static_cast<uint32_t>(rc), settings.renderW, settings.renderH, settings.upscaleW, settings.upscaleH,
             settings.flags, settings.versionId);
        return false;
    }

    m_ready = true;
    m_failed = false;
    LOGI("FSR upscale context created: {}x{} -> {}x{}, flags {:#x}, version {:#x}",
         settings.renderW, settings.renderH, settings.upscaleW, settings.upscaleH,
         settings.flags, settings.versionId);
    return true;
}

bool FsrContext::Dispatch(const DispatchParams& params) {
    if (!m_ready) return false;
    if (!params.commandList || !params.color || !params.depth || !params.motion || !params.output) {
        LOGE("FsrContext::Dispatch: missing resource parameters");
        return false;
    }

    ffx::DispatchDescUpscale d{};
    d.commandList = params.commandList;
    d.color = ffxApiGetResourceDX12(params.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = ffxApiGetResourceDX12(params.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(params.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = ffxApiGetResourceDX12(params.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = { params.jitterX, params.jitterY };
    d.motionVectorScale = { params.motionScaleX, params.motionScaleY };
    d.renderSize = { params.renderW, params.renderH };
    d.upscaleSize = { params.upscaleW, params.upscaleH };
    d.enableSharpening = (params.sharpness > 0.0f);
    d.sharpness = std::clamp(params.sharpness, 0.0f, 1.0f);
    d.frameTimeDelta = params.frameTimeDeltaMs > 0.0f ? params.frameTimeDeltaMs : 16.666f;
    d.preExposure = params.preExposure > 0.0f ? params.preExposure : 1.0f;
    d.reset = params.reset;
    d.cameraNear = params.cameraNear > 0.0f ? params.cameraNear : 0.1f;
    d.cameraFar = params.cameraFar > d.cameraNear ? params.cameraFar : 10000.0f;
    d.cameraFovAngleVertical = params.cameraFovAngleVertical > 0.0f ? params.cameraFovAngleVertical : 0.785398f;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = 0;

    ffx::ReturnCode rc = ffx::Dispatch(m_context, d);
    if (!rc) {
        LOGE("FsrContext::Dispatch failed (code {:#x})", static_cast<uint32_t>(rc));
        return false;
    }
    return true;
}

}  // namespace mhwfsr
