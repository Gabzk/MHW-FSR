#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

namespace mhwfsr::framegen {

using Microsoft::WRL::ComPtr;

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
    float deltaMs = 16.666f;
    bool reset = false;
    uint64_t frameId = 0;
};

// Initializes the Frame Generation context and resources
bool Init(ID3D12Device* device, uint32_t displayW, uint32_t displayH, DXGI_FORMAT backBufferFormat);

// Shuts down Frame Generation
void Shutdown();

// Saves HUDLess color buffer from upscaler output
void CaptureHudLess(ID3D12GraphicsCommandList* cl, ID3D12Resource* upscaleOutput);

// Records frame parameters for prepare dispatch
void RecordFrameData(const FrameData& data);

// Prepares and dispatches frame generation on command list
bool Dispatch(ID3D12GraphicsCommandList* cl, ID3D12Resource* currentBackBuffer, ID3D12Resource* targetBuffer);

// Status queries and controls
bool CheckSupport(ID3D12Device* device);
bool IsSupported();
bool IsActive();
void SetEnabled(bool enabled);
bool IsEnabled();

}  // namespace mhwfsr::framegen
