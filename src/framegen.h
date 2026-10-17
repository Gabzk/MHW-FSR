#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

namespace mhwfsr::framegen {

struct FrameData {
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float mvScaleX = 0.5f;
    float mvScaleY = -0.5f;
    uint32_t renderW = 0;
    uint32_t renderH = 0;
    uint32_t displayW = 0;
    uint32_t displayH = 0;
    float cameraNear = 0.1f;
    float cameraFar = 10000.0f;
    float fovY = 0.785398f;
    float cameraPosition[3] = {0.0f, 0.0f, 0.0f};
    float cameraUp[3] = {0.0f, 1.0f, 0.0f};
    float cameraRight[3] = {1.0f, 0.0f, 0.0f};
    float cameraForward[3] = {0.0f, 0.0f, 1.0f};
    float deltaMs = 16.666f;
    bool reset = false;
    uint64_t frameId = 0;
};

inline bool Init(ID3D12Device*, uint32_t, uint32_t, DXGI_FORMAT) { return false; }
inline void Shutdown() {}
inline void CaptureHudLess(ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES) {}
inline bool HasHudLessForCurrentFrame() { return false; }
inline void RecordFrameData(ID3D12GraphicsCommandList*, const FrameData&) {}
inline bool HasPendingFrame() { return false; }
inline bool GenerateInterpolatedFrame(ID3D12GraphicsCommandList*, ID3D12Resource*) { return false; }
inline bool CopyRealFrameToBackBuffer(ID3D12GraphicsCommandList*, ID3D12Resource*) { return false; }
inline bool CheckSupport(ID3D12Device*) { return false; }
inline bool IsSupported() { return false; }
inline bool IsActive() { return false; }
inline void SetEnabled(bool) {}
inline bool IsEnabled() { return false; }

}  // namespace mhwfsr::framegen
