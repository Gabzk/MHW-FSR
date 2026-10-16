#pragma once

#include <d3d12.h>
#include <array>
#include <cstdint>

namespace mhwfsr::telemetry {

struct Metrics {
    // Framerate & Frame Time
    float baseFps = 0.0f;               // Engine real frames per second
    float baseFrameTimeMs = 0.0f;       // Engine real frame time (ms)
    float presentedFps = 0.0f;          // Actual presented frames per second (doubles with FG)
    float presentedFrameTimeMs = 0.0f;  // Actual presented frame time (ms)

    // Upscaler Latency (FSR 3.1 / 4.1)
    float upscaleCpuMs = 0.0f;          // CPU command recording time for ffx::Dispatch(upscale)
    float upscaleGpuMs = 0.0f;          // GPU execution time via D3D12 Timestamp Query

    // Frame Generation Latency (FSR FG)
    float fgCpuMs = 0.0f;               // CPU command recording time for FG Prepare + Generate
    float fgGpuMs = 0.0f;               // GPU execution time via D3D12 Timestamp Query
    float fgPaceDelayMs = 0.0f;         // Pacing interval between Interpolated and Real Present

    // Anti-Lag 2 & End-to-End System Latency
    float antiLagWaitMs = 0.0f;         // CPU synchronization wait inside AntiLag2DX12::Update
    float inputToPresentMs = 0.0f;      // End-to-end latency from mouse input poll to Present
    float gpuQueueLatencyMs = 0.0f;     // In-flight GPU queue latency (CPU submit -> GPU finish)
    uint64_t generatedFramesTotal = 0;  // Total interpolated frames presented
};

void Init(ID3D12Device* device);
void Shutdown();

// Input & Anti-Lag 2 timing (called around AntiLag2::Update on main input thread)
void BeginAntiLagUpdate();
void EndAntiLagUpdate(bool active);

// Upscaler pass timing (called around FsrContext::Dispatch)
void BeginUpscalePass(ID3D12GraphicsCommandList* cl);
void EndUpscalePass(ID3D12GraphicsCommandList* cl);

// Frame Generation pass timing (called around framegen::GenerateInterpolatedFrame)
void BeginFrameGenPass(ID3D12GraphicsCommandList* cl);
void EndFrameGenPass(ID3D12GraphicsCommandList* cl);
void RecordFgPaceDelay(float delayMs);
void NoteInterpolatedFramePresented();

// Called once per real game frame in HookPresent, and once per actual swapchain Present
void OnRealFramePresent(ID3D12CommandQueue* queue);
void OnSwapChainPresent();

// Snapshot current smoothed metrics and frametime history for UI
Metrics GetMetrics();
const std::array<float, 120>& GetFrameTimeHistory(size_t& outOffset);

}  // namespace mhwfsr::telemetry
