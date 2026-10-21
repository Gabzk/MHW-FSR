#include "telemetry.h"
#include "log.h"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace mhwfsr::telemetry {

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kQueryRingSize = 8;
constexpr uint32_t kTotalQueries = kQueryRingSize * 4; // [0..15] upscale pairs, [16..31] fg pairs

std::mutex g_mutex;
ComPtr<ID3D12QueryHeap> g_queryHeap;
ComPtr<ID3D12Resource> g_readbackBuffer;
ComPtr<ID3D12Fence> g_gpuLatencyFence;
uint64_t g_gpuLatencyFenceVal = 0;
std::array<int64_t, 16> g_fenceSubmitQpc{};

LARGE_INTEGER g_qpcFreq{};

std::atomic<uint32_t> g_upscaleWriteSlot{0};
std::atomic<uint32_t> g_fgWriteSlot{0};
thread_local int64_t t_upscaleStartQpc = 0;
thread_local uint32_t t_upscaleSlot = 0;
thread_local bool t_upscaleQueried = false;
thread_local int64_t t_fgStartQpc = 0;
thread_local uint32_t t_fgSlot = 0;
thread_local bool t_fgQueried = false;

std::atomic<bool> g_profilerActive{false};
std::atomic<uint64_t> g_gpuTimestampFreq{0};

std::atomic<int64_t> g_al2StartQpc{0};
std::atomic<int64_t> g_lastInputPollQpc{0};
std::atomic<int64_t> g_lastRealPresentQpc{0};
std::atomic<int64_t> g_lastSwapPresentQpc{0};
std::atomic<uint64_t> g_generatedFramesTotal{0};

Metrics g_metrics{};
std::array<float, 120> g_frameTimeHistory{};
size_t g_historyOffset = 0;

double QpcToMs(int64_t deltaTicks) {
    if (!g_qpcFreq.QuadPart) {
        QueryPerformanceFrequency(&g_qpcFreq);
    }
    if (g_qpcFreq.QuadPart <= 0 || deltaTicks <= 0) return 0.0;
    return static_cast<double>(deltaTicks) * 1000.0 / static_cast<double>(g_qpcFreq.QuadPart);
}

int64_t NowQpc() {
    if (!g_qpcFreq.QuadPart) {
        QueryPerformanceFrequency(&g_qpcFreq);
    }
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

float SmoothEma(float current, float sample, float alpha = 0.12f) {
    if (!std::isfinite(sample) || sample < 0.0f) return current;
    if (current <= 0.0001f) return sample;
    return current + alpha * (sample - current);
}

}  // namespace

void Init(ID3D12Device* device) {
    if (!device) return;
    std::lock_guard lock(g_mutex);
    if (g_queryHeap && g_readbackBuffer) return;

    QueryPerformanceFrequency(&g_qpcFreq);

    D3D12_QUERY_HEAP_DESC qhDesc{};
    qhDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhDesc.Count = kTotalQueries;
    qhDesc.NodeMask = 0;
    if (FAILED(device->CreateQueryHeap(&qhDesc, IID_PPV_ARGS(&g_queryHeap)))) {
        LOGW("telemetry: failed to create D3D12 timestamp query heap");
        g_queryHeap.Reset();
        return;
    }

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kTotalQueries * sizeof(uint64_t);
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&g_readbackBuffer)))) {
        LOGW("telemetry: failed to create D3D12 timestamp readback buffer");
        g_queryHeap.Reset();
        g_readbackBuffer.Reset();
        return;
    }

    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_gpuLatencyFence));
    LOGI("telemetry: initialized D3D12 GPU timestamp & latency profiler");
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    g_queryHeap.Reset();
    g_readbackBuffer.Reset();
    g_gpuLatencyFence.Reset();
    g_gpuLatencyFenceVal = 0;
    g_gpuTimestampFreq.store(0, std::memory_order_relaxed);
}

void SetProfilerActive(bool active) {
    g_profilerActive.store(active, std::memory_order_relaxed);
}

bool IsProfilerActive() {
    return g_profilerActive.load(std::memory_order_relaxed);
}

void RecordInputSample() {
    g_lastInputPollQpc.store(NowQpc(), std::memory_order_relaxed);
}

void BeginAntiLagUpdate() {
    if (!IsProfilerActive()) return;
    g_al2StartQpc.store(NowQpc(), std::memory_order_relaxed);
}

void EndAntiLagUpdate(bool active) {
    if (!IsProfilerActive()) return;
    int64_t now = NowQpc();
    int64_t start = g_al2StartQpc.load(std::memory_order_relaxed);
    if (g_lastInputPollQpc.load(std::memory_order_relaxed) == 0) {
        g_lastInputPollQpc.store(now, std::memory_order_relaxed);
    }

    float waitMs = active && start > 0 ? static_cast<float>(QpcToMs(now - start)) : 0.0f;
    waitMs = std::clamp(waitMs, 0.0f, 50.0f);

    std::lock_guard lock(g_mutex);
    if (active) {
        g_metrics.antiLagWaitMs = SmoothEma(g_metrics.antiLagWaitMs, waitMs, 0.15f);
    } else {
        g_metrics.antiLagWaitMs = 0.0f;
    }
}

void BeginUpscalePass(ID3D12GraphicsCommandList* cl) {
    t_upscaleQueried = false;
    if (!IsProfilerActive()) return;
    t_upscaleStartQpc = NowQpc();
    if (!cl || !g_queryHeap) return;
    uint32_t slot = g_upscaleWriteSlot.fetch_add(1, std::memory_order_relaxed) % kQueryRingSize;
    t_upscaleSlot = slot;
    t_upscaleQueried = true;
    uint32_t queryIdx = slot * 2;
    cl->EndQuery(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIdx);
}

void EndUpscalePass(ID3D12GraphicsCommandList* cl) {
    if (!IsProfilerActive() && !t_upscaleQueried) return;
    int64_t endQpc = NowQpc();
    float cpuMs = static_cast<float>(QpcToMs(endQpc - t_upscaleStartQpc));

    if (t_upscaleQueried && cl && g_queryHeap && g_readbackBuffer) {
        uint32_t queryIdx = t_upscaleSlot * 2;
        cl->EndQuery(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIdx + 1);
        cl->ResolveQueryData(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIdx, 2,
                             g_readbackBuffer.Get(), static_cast<UINT64>(queryIdx) * sizeof(uint64_t));
    }
    t_upscaleQueried = false;

    std::lock_guard lock(g_mutex);
    g_metrics.upscaleCpuMs = SmoothEma(g_metrics.upscaleCpuMs, std::clamp(cpuMs, 0.0f, 50.0f));
}

void BeginFrameGenPass(ID3D12GraphicsCommandList* cl) {
    t_fgQueried = false;
    if (!IsProfilerActive()) return;
    t_fgStartQpc = NowQpc();
    if (!cl || !g_queryHeap) return;
    uint32_t slot = g_fgWriteSlot.fetch_add(1, std::memory_order_relaxed) % kQueryRingSize;
    t_fgSlot = slot;
    t_fgQueried = true;
    uint32_t queryIdx = (kQueryRingSize + slot) * 2;
    cl->EndQuery(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIdx);
}

void EndFrameGenPass(ID3D12GraphicsCommandList* cl) {
    if (!IsProfilerActive() && !t_fgQueried) return;
    int64_t endQpc = NowQpc();
    float cpuMs = static_cast<float>(QpcToMs(endQpc - t_fgStartQpc));

    if (t_fgQueried && cl && g_queryHeap && g_readbackBuffer) {
        uint32_t queryIdx = (kQueryRingSize + t_fgSlot) * 2;
        cl->EndQuery(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIdx + 1);
        cl->ResolveQueryData(g_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryIdx, 2,
                             g_readbackBuffer.Get(), static_cast<UINT64>(queryIdx) * sizeof(uint64_t));
    }
    t_fgQueried = false;

    std::lock_guard lock(g_mutex);
    g_metrics.fgCpuMs = SmoothEma(g_metrics.fgCpuMs, std::clamp(cpuMs, 0.0f, 50.0f));
}

void RecordFgPaceDelay(float delayMs) {
    if (!IsProfilerActive()) return;
    std::lock_guard lock(g_mutex);
    g_metrics.fgPaceDelayMs = SmoothEma(g_metrics.fgPaceDelayMs, std::clamp(delayMs, 0.0f, 50.0f));
}

void NoteInterpolatedFramePresented() {
    g_generatedFramesTotal.fetch_add(1, std::memory_order_relaxed);
}

void OnRealFramePresent(ID3D12CommandQueue* queue) {
    int64_t now = NowQpc();
    int64_t prevReal = g_lastRealPresentQpc.exchange(now, std::memory_order_relaxed);
    float baseDtMs = (prevReal > 0) ? static_cast<float>(QpcToMs(now - prevReal)) : 16.666f;
    baseDtMs = std::clamp(baseDtMs, 0.5f, 250.0f);

    if (!IsProfilerActive()) {
        std::lock_guard lock(g_mutex);
        g_metrics.baseFrameTimeMs = SmoothEma(g_metrics.baseFrameTimeMs, baseDtMs, 0.10f);
        g_metrics.baseFps = (g_metrics.baseFrameTimeMs > 0.1f) ? (1000.0f / g_metrics.baseFrameTimeMs) : 0.0f;
        return;
    }

    int64_t inputQpc = g_lastInputPollQpc.load(std::memory_order_relaxed);
    float cpuInputToPresentMs = (inputQpc > 0 && now > inputQpc)
                                    ? static_cast<float>(QpcToMs(now - inputQpc))
                                    : baseDtMs * 0.5f;
    cpuInputToPresentMs = std::clamp(cpuInputToPresentMs, 0.1f, 150.0f);

    float gpuQueueMs = 0.0f;
    float resolvedUpscaleGpuMs = -1.0f;

    if (queue) {
        // Measure GPU queue latency via fence completion
        if (g_gpuLatencyFence) {
            uint64_t completed = g_gpuLatencyFence->GetCompletedValue();
            if (completed > 0 && completed <= g_gpuLatencyFenceVal) {
                int64_t submitQpc = g_fenceSubmitQpc[completed % g_fenceSubmitQpc.size()];
                if (submitQpc > 0 && now > submitQpc) {
                    float ageMs = static_cast<float>(QpcToMs(now - submitQpc));
                    gpuQueueMs = std::clamp(ageMs, 0.0f, 100.0f);
                }
            }
            uint64_t nextFence = ++g_gpuLatencyFenceVal;
            g_fenceSubmitQpc[nextFence % g_fenceSubmitQpc.size()] = now;
            queue->Signal(g_gpuLatencyFence.Get(), nextFence);
        }

        // Cache GPU timestamp frequency once instead of querying kernel every frame
        uint64_t gpuFreq = g_gpuTimestampFreq.load(std::memory_order_relaxed);
        if (gpuFreq == 0) {
            UINT64 freq = 0;
            if (SUCCEEDED(queue->GetTimestampFrequency(&freq)) && freq > 0) {
                gpuFreq = freq;
                g_gpuTimestampFreq.store(gpuFreq, std::memory_order_relaxed);
            }
        }

        // Resolve GPU timestamps from 4 frames ago (guaranteed completed on GPU)
        uint32_t upWrite = g_upscaleWriteSlot.load(std::memory_order_relaxed);
        if (g_readbackBuffer && gpuFreq > 0 && upWrite >= 4) {
            uint32_t upReadSlot = (upWrite - 4) % kQueryRingSize;
            size_t byteOffset = static_cast<size_t>(upReadSlot * 2) * sizeof(uint64_t);
            D3D12_RANGE readRange{byteOffset, byteOffset + 2 * sizeof(uint64_t)};
            uint64_t* mapped = nullptr;
            if (SUCCEEDED(g_readbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped))) && mapped) {
                uint64_t t0 = mapped[upReadSlot * 2];
                uint64_t t1 = mapped[upReadSlot * 2 + 1];
                if (t0 > 0 && t1 > t0) {
                    double ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(gpuFreq);
                    if (ms > 0.001 && ms < 100.0) {
                        resolvedUpscaleGpuMs = static_cast<float>(ms);
                    }
                }
                D3D12_RANGE emptyRange{0, 0};
                g_readbackBuffer->Unmap(0, &emptyRange);
            }
        }
    }

    std::lock_guard lock(g_mutex);
    g_metrics.baseFrameTimeMs = SmoothEma(g_metrics.baseFrameTimeMs, baseDtMs, 0.10f);
    g_metrics.baseFps = (g_metrics.baseFrameTimeMs > 0.1f) ? (1000.0f / g_metrics.baseFrameTimeMs) : 0.0f;

    if (gpuQueueMs > 0.0f) {
        g_metrics.gpuQueueLatencyMs = SmoothEma(g_metrics.gpuQueueLatencyMs, gpuQueueMs, 0.12f);
    }

    float totalPcLatencyMs = cpuInputToPresentMs + g_metrics.gpuQueueLatencyMs;
    if (g_metrics.fgGpuMs > 0.01f) {
        totalPcLatencyMs += g_metrics.fgPaceDelayMs;
    }
    g_metrics.inputToPresentMs = SmoothEma(g_metrics.inputToPresentMs, totalPcLatencyMs, 0.12f);

    if (resolvedUpscaleGpuMs >= 0.0f) {
        g_metrics.upscaleGpuMs = SmoothEma(g_metrics.upscaleGpuMs, resolvedUpscaleGpuMs, 0.15f);
    }
    g_metrics.generatedFramesTotal = g_generatedFramesTotal.load(std::memory_order_relaxed);
}

void OnSwapChainPresent() {
    int64_t now = NowQpc();
    int64_t prevSwap = g_lastSwapPresentQpc.exchange(now, std::memory_order_relaxed);
    if (prevSwap <= 0) return;

    float dtMs = static_cast<float>(QpcToMs(now - prevSwap));
    dtMs = std::clamp(dtMs, 0.2f, 250.0f);

    std::lock_guard lock(g_mutex);
    g_metrics.presentedFrameTimeMs = SmoothEma(g_metrics.presentedFrameTimeMs, dtMs, 0.08f);
    g_metrics.presentedFps = (g_metrics.presentedFrameTimeMs > 0.1f) ? (1000.0f / g_metrics.presentedFrameTimeMs) : 0.0f;

    g_frameTimeHistory[g_historyOffset] = g_metrics.presentedFrameTimeMs;
    g_historyOffset = (g_historyOffset + 1) % g_frameTimeHistory.size();
}

Metrics GetMetrics() {
    std::lock_guard lock(g_mutex);
    return g_metrics;
}

const std::array<float, 120>& GetFrameTimeHistory(size_t& outOffset) {
    std::lock_guard lock(g_mutex);
    outOffset = g_historyOffset;
    return g_frameTimeHistory;
}

}  // namespace mhwfsr::telemetry
