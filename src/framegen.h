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
    float cameraPosition[3] = {0.0f, 0.0f, 0.0f};
    float cameraUp[3] = {0.0f, 1.0f, 0.0f};
    float cameraRight[3] = {1.0f, 0.0f, 0.0f};
    float cameraForward[3] = {0.0f, 0.0f, 1.0f};
    float deltaMs = 16.666f;
    bool reset = false;
    uint64_t frameId = 0;
};

// Initializes the Frame Generation context and internal backbuffer textures
bool Init(ID3D12Device* device, uint32_t displayW, uint32_t displayH, DXGI_FORMAT backBufferFormat);

// Shuts down Frame Generation
void Shutdown();

// Saves HUDLess color buffer from upscaler output (optional)
void CaptureHudLess(ID3D12GraphicsCommandList* cl, ID3D12Resource* upscaleOutput);

// Mirrors depth/motion on the game command list and records frame parameters for FG Prepare
void RecordFrameData(ID3D12GraphicsCommandList* cl, const FrameData& data);

// Returns true if FrameGen is active and a valid frame's depth/motion were recorded this frame
bool HasPendingFrame();

// Backs up currentBackBuffer (real frame), dispatches FSR FG Prepare + Generate, and writes the
// interpolated frame into currentBackBuffer (leaving currentBackBuffer in PRESENT state)
bool GenerateInterpolatedFrame(ID3D12GraphicsCommandList* cl, ID3D12Resource* currentBackBuffer);

// Copies the backed-up real frame into targetBackBuffer (leaving targetBackBuffer in PRESENT state)
bool CopyRealFrameToBackBuffer(ID3D12GraphicsCommandList* cl, ID3D12Resource* targetBackBuffer);

// Status queries and controls
bool CheckSupport(ID3D12Device* device);
bool IsSupported();
bool IsActive();
void SetEnabled(bool enabled);
bool IsEnabled();

}  // namespace mhwfsr::framegen
