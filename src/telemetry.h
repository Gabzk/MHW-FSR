#pragma once

#include <d3d12.h>
#include <array>
#include <cstdint>

namespace mhwfsr::telemetry {

struct Metrics {
    // Framerate & Frame Time
    float baseFps = 0.0f;               // Engine real frames per second
    float baseFrameTimeMs = 0.0f;       // Engine real frame time (ms)
    float presentedFps = 0.0f;          // Actual presented frames per second
    float presentedFrameTimeMs = 0.0f;  // Actual presented frame time (ms)

    // Upscaler Latency (FSR 3.1 / 4.1)
    float upscaleCpuMs = 0.0f;          // CPU command recording time for ffx::Dispatch(upscale)
    float upscaleGpuMs = 0.0f;          // GPU execution time via D3D12 Timestamp Query

    // Anti-Lag 2 & End-to-End System Latency
    float antiLagWaitMs = 0.0f;         // CPU synchronization wait inside AntiLag2DX12::Update
    float inputToPresentMs = 0.0f;      // End-to-end latency from mouse input poll to Present
    float gpuQueueLatencyMs = 0.0f;     // In-flight GPU queue latency (CPU submit -> GPU finish)
};

void Init(ID3D12Device* device);
void Shutdown();

// Enable/disable active GPU timestamp query & fence profiling (active only when UI/OSD needs it)
void SetProfilerActive(bool active);
bool IsProfilerActive();

// Input & Anti-Lag 2 timing
void RecordInputSample();
void BeginAntiLagUpdate();
void EndAntiLagUpdate(bool active);

// Upscaler pass timing (called around FsrContext::Dispatch)
void BeginUpscalePass(ID3D12GraphicsCommandList* cl);
void EndUpscalePass(ID3D12GraphicsCommandList* cl);

// Called once per real game frame in HookPresent, and once per actual swapchain Present
void OnRealFramePresent(ID3D12CommandQueue* queue);
void OnSwapChainPresent();

// Snapshot current smoothed metrics and frametime history for UI
Metrics GetMetrics();
const std::array<float, 120>& GetFrameTimeHistory(size_t& outOffset);

}  // namespace mhwfsr::telemetry
