#pragma once

#include <d3d12.h>
#include <cstdint>
#include <algorithm>
#include <string>

#include <ffx_api.hpp>
#include <ffx_upscale.hpp>
#include <dx12/ffx_api_dx12.hpp>

namespace mhwfsr {

// Returns the active FSR version name (e.g. "FSR 4.1.1" or "FSR 3.1.5")
std::string ActiveFsrVersion();
void SetActiveFsrVersion(const std::string& version);

class FsrContext {
public:
    struct Settings {
        uint32_t renderW = 0;
        uint32_t renderH = 0;
        uint32_t upscaleW = 0;
        uint32_t upscaleH = 0;
        uint32_t flags = 0;
        uint64_t versionId = 0;

        bool operator==(const Settings&) const = default;
    };

    struct DispatchParams {
        ID3D12GraphicsCommandList* commandList = nullptr;
        ID3D12Resource* color = nullptr;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* motion = nullptr;
        ID3D12Resource* output = nullptr;
        float jitterX = 0.0f;
        float jitterY = 0.0f;
        float motionScaleX = 0.5f;
        float motionScaleY = -0.5f;
        uint32_t renderW = 0;
        uint32_t renderH = 0;
        uint32_t upscaleW = 0;
        uint32_t upscaleH = 0;
        float sharpness = 0.0f;
        float frameTimeDeltaMs = 16.666f;
        float preExposure = 1.0f;
        float cameraNear = 0.1f;
        float cameraFar = 10000.0f;
        float cameraFovAngleVertical = 0.785398f;
        bool reset = false;
    };

    FsrContext() = default;
    ~FsrContext();

    FsrContext(const FsrContext&) = delete;
    FsrContext& operator=(const FsrContext&) = delete;

    // Creates or re-creates the FSR context if settings changed.
    bool Ensure(ID3D12Device* device, const Settings& settings);

    // Dispatches the upscaler on the command list.
    bool Dispatch(const DispatchParams& params);

    // Retires current context via the D3D12 retire queue (frees safely after GPU is done).
    void Retire();

    // Shuts down immediately (e.g. on plugin unload).
    void Shutdown();

    bool IsReady() const { return m_ready; }
    const Settings& CurrentSettings() const { return m_settings; }

private:
    bool m_ready = false;
    bool m_failed = false;
    Settings m_settings{};
    ID3D12Device* m_device = nullptr;
    ffx::Context m_context{};
};

}  // namespace mhwfsr
