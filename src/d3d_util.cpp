#include "d3d_util.h"

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include <algorithm>
#include <atomic>
#include <deque>
#include <iterator>
#include <mutex>
#include <utility>

#include "log.h"

namespace mhwfsr::d3d {

namespace {

// Game frames a retired object is kept. Generous: the game builds a frame on the CPU at most a
// few frames ahead of the GPU.
constexpr uint64_t kRetireFrames = 16;

struct Retired {
    uint64_t age;  // value of g_frames when retired
    bool feature;
    std::function<void()> release;
};
std::mutex g_retireMutex;
std::deque<Retired> g_retired;
// Distinct game frames seen by RetireTick: callers may tick several times per frame.
uint64_t g_frames = 0;
uint64_t g_lastGameFrame = 0;

void Push(std::function<void()> release, bool feature) {
    if (!release) return;
    std::lock_guard lock(g_retireMutex);
    g_retired.push_back({g_frames, feature, std::move(release)});
}

}  // namespace

void Retire(std::function<void()> release) { Push(std::move(release), false); }

void Retire(ComPtr<ID3D12Resource> resource) {
    if (!resource) return;
    Retire([r = std::move(resource)]() mutable { r.Reset(); });
}

void RetireFeature(std::function<void()> release) { Push(std::move(release), true); }

void RetireTick(uint64_t frame) {
    std::deque<Retired> due;
    {
        std::lock_guard lock(g_retireMutex);
        if (frame == g_lastGameFrame) return;
        g_lastGameFrame = frame;
        ++g_frames;
        while (!g_retired.empty() && g_frames >= g_retired.front().age + kRetireFrames) {
            due.push_back(std::move(g_retired.front()));
            g_retired.pop_front();
        }
    }
    for (auto& r : due) r.release();
}

void RetireFlushFeatures() {
    std::deque<Retired> features;
    {
        std::lock_guard lock(g_retireMutex);
        auto keep = std::stable_partition(g_retired.begin(), g_retired.end(), [](const Retired& r) { return !r.feature; });
        std::move(keep, g_retired.end(), std::back_inserter(features));
        g_retired.erase(keep, g_retired.end());
    }
    for (auto& r : features) r.release();
}

TextureInfo Info(ID3D12Resource* resource) {
    TextureInfo info;
    if (!resource) return info;
    D3D12_RESOURCE_DESC desc = resource->GetDesc();
    info.width = static_cast<unsigned>(desc.Width);
    info.height = desc.Height;
    info.format = desc.Format;
    return info;
}

bool IsFloatFormat(unsigned f) {
    switch (static_cast<DXGI_FORMAT>(f)) {
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
            return true;
        default:
            return false;
    }
}

namespace {

constexpr UINT kDescRingSlots = 64;
constexpr UINT kDescsPerSlot = 5;  // SRV0, SRV1, SRV2, UAV0, UAV1

ID3D12Device* g_bridgeDevice = nullptr;
ComPtr<ID3D12RootSignature> g_bridgeRootSig;
ComPtr<ID3D12PipelineState> g_downsamplePso;
ComPtr<ID3D12PipelineState> g_transferPso;
ComPtr<ID3D12DescriptorHeap> g_bridgeHeap;
UINT g_srvUavDescSize = 0;
std::atomic<uint32_t> g_descRingIdx{0};

ComPtr<ID3D12Resource> g_upscaledHdr;
ComPtr<ID3D12Resource> g_lowResHdr;
bool g_upscaledInUav = false;
bool g_taaUpscaleReady = false;

constexpr char kDownsampleHlsl[] = R"(
cbuffer Constants : register(b0) {
    uint2 srcSize;
    uint2 dstSize;
    uint useUpscaled;
};
Texture2D<float4> SrcTex : register(t0);
RWTexture2D<float4> DstTaa : register(u0);
RWTexture2D<float4> DstCopy : register(u1);
SamplerState LinearClamp : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= dstSize.x || dtid.y >= dstSize.y) return;
    float2 invDst = 1.0f / float2(dstSize);
    float2 invSrc = 1.0f / float2(srcSize);
    float2 uv = (float2(dtid.xy) + 0.5f) * invDst;
    float2 offset = 0.25f * invSrc;
    float3 c0 = SrcTex.SampleLevel(LinearClamp, uv + float2(-offset.x, -offset.y), 0).rgb;
    float3 c1 = SrcTex.SampleLevel(LinearClamp, uv + float2( offset.x, -offset.y), 0).rgb;
    float3 c2 = SrcTex.SampleLevel(LinearClamp, uv + float2(-offset.x,  offset.y), 0).rgb;
    float3 c3 = SrcTex.SampleLevel(LinearClamp, uv + float2( offset.x,  offset.y), 0).rgb;
    float3 avg = (c0 + c1 + c2 + c3) * 0.25f;
    if (any(isnan(avg)) || any(isinf(avg))) avg = 0.0f;
    avg = max(avg, 0.0f);
    DstTaa[dtid.xy] = float4(avg, 1.0f);
    DstCopy[dtid.xy] = float4(avg, 1.0f);
}
)";

constexpr char kTransferTonemapHlsl[] = R"(
cbuffer Constants : register(b0) {
    uint2 srcSize;
    uint2 dstSize;
    uint useUpscaled;
};
Texture2D<float4> UpscaledHdr : register(t0);
Texture2D<float4> LowResHdr : register(t1);
Texture2D<float4> TonemappedLow : register(t2);
RWTexture2D<float4> FinalOut : register(u0);
SamplerState LinearClamp : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= dstSize.x || dtid.y >= dstSize.y) return;
    float2 uv = (float2(dtid.xy) + 0.5f) / float2(dstSize);

    float3 T_lo = TonemappedLow.SampleLevel(LinearClamp, uv, 0).rgb;
    if (any(isnan(T_lo)) || any(isinf(T_lo))) T_lo = 0.0f;
    T_lo = max(T_lo, 0.0f);

    if (useUpscaled == 0) {
        FinalOut[dtid.xy] = float4(T_lo, 1.0f);
        return;
    }

    float3 H_hi = UpscaledHdr.Load(int3(dtid.xy, 0)).rgb;
    float3 H_lo = LowResHdr.SampleLevel(LinearClamp, uv, 0).rgb;

    if (any(isnan(H_hi)) || any(isinf(H_hi))) H_hi = H_lo;
    if (any(isnan(H_lo)) || any(isinf(H_lo))) H_lo = 0.0f;
    H_hi = max(H_hi, 0.0f);
    H_lo = max(H_lo, 0.0f);

    const float eps = 1e-3f;
    float3 oneMinusT = max(1.0f - T_lo, 0.05f);
    float3 num = T_lo * (H_hi + eps) * (oneMinusT + T_lo);
    float3 den = oneMinusT * (H_lo + eps) + T_lo * (H_hi + eps);
    float3 outColor = num / max(den, 1e-5f);

    FinalOut[dtid.xy] = float4(outColor, 1.0f);
}
)";

bool EnsureBridgePipeline(ID3D12Device* device) {
    if (!device) return false;
    if (g_bridgeDevice == device && g_bridgeRootSig && g_downsamplePso && g_transferPso && g_bridgeHeap) {
        return true;
    }

    ResetTaaUpscale12();
    g_bridgeDevice = device;

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 5;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob))) {
        LOGE("TaaUpscaleBridge: failed to serialize root signature: {}",
             errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "unknown");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                           IID_PPV_ARGS(&g_bridgeRootSig)))) {
        LOGE("TaaUpscaleBridge: failed to create root signature");
        return false;
    }

    auto compilePso = [&](const char* src, size_t len, const char* name, ComPtr<ID3D12PipelineState>& outPso) -> bool {
        ComPtr<ID3DBlob> csBlob, csErr;
        HRESULT hr = D3DCompile(src, len, name, nullptr, nullptr, "main", "cs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &csBlob, &csErr);
        if (FAILED(hr)) {
            LOGE("TaaUpscaleBridge: failed to compile {}: {}", name,
                 csErr ? static_cast<const char*>(csErr->GetBufferPointer()) : "unknown");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = g_bridgeRootSig.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPso)))) {
            LOGE("TaaUpscaleBridge: failed to create PSO for {}", name);
            return false;
        }
        return true;
    };

    if (!compilePso(kDownsampleHlsl, sizeof(kDownsampleHlsl) - 1, "DownsampleBoxCS", g_downsamplePso)) return false;
    if (!compilePso(kTransferTonemapHlsl, sizeof(kTransferTonemapHlsl) - 1, "TransferTonemapCS", g_transferPso)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kDescRingSlots * kDescsPerSlot;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_bridgeHeap)))) {
        LOGE("TaaUpscaleBridge: failed to create descriptor heap");
        return false;
    }
    g_srvUavDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    LOGI("TaaUpscaleBridge: initialized D3D12 compute shaders for TAA-pass upscaling");
    return true;
}

bool EnsureUavTexture(ID3D12Device* device, ComPtr<ID3D12Resource>& tex, unsigned w, unsigned h, DXGI_FORMAT fmt) {
    if (tex) {
        D3D12_RESOURCE_DESC have = tex->GetDesc();
        if (have.Width == w && have.Height == h && have.Format == fmt) return true;
        Retire(std::move(tex));
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 nullptr, IID_PPV_ARGS(&tex));
    if (FAILED(hr)) {
        LOGE("TaaUpscaleBridge: failed to create {}x{} fmt {} texture ({:#x})",
             w, h, static_cast<int>(fmt), static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

void WriteSlotDescriptors(ID3D12Device* device, UINT slot,
                          ID3D12Resource* srv0, ID3D12Resource* srv1, ID3D12Resource* srv2,
                          ID3D12Resource* uav0, ID3D12Resource* uav1) {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_bridgeHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot * kDescsPerSlot) * g_srvUavDescSize;

    ID3D12Resource* srvs[3] = {srv0, srv1, srv2};
    for (int i = 0; i < 3; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = srvs[i]->GetDesc().Format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(srvs[i], &sd, cpu);
        cpu.ptr += g_srvUavDescSize;
    }

    ID3D12Resource* uavs[2] = {uav0, uav1};
    for (int i = 0; i < 2; ++i) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = uavs[i]->GetDesc().Format;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(uavs[i], nullptr, &ud, cpu);
        cpu.ptr += g_srvUavDescSize;
    }
}

}  // namespace

ID3D12Resource* BeginTaaUpscale12(ID3D12GraphicsCommandList* cl, ID3D12Device* device,
                                  unsigned upW, unsigned upH, unsigned lowW, unsigned lowH,
                                  DXGI_FORMAT format) {
    if (!cl || !device || !upW || !upH || !lowW || !lowH) return nullptr;
    if (!EnsureBridgePipeline(device)) return nullptr;

    bool hadUpscaled = (g_upscaledHdr != nullptr);
    if (!EnsureUavTexture(device, g_upscaledHdr, upW, upH, format)) return nullptr;
    if (!EnsureUavTexture(device, g_lowResHdr, lowW, lowH, format)) return nullptr;
    if (!hadUpscaled) g_upscaledInUav = false;

    if (!g_upscaledInUav) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {g_upscaledHdr.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cl->ResourceBarrier(1, &b);
        g_upscaledInUav = true;
    }
    return g_upscaledHdr.Get();
}

bool FinishTaaUpscale12(ID3D12GraphicsCommandList* cl, ID3D12Device* device,
                        ID3D12Resource* taaOutput, unsigned upW, unsigned upH,
                        unsigned lowW, unsigned lowH) {
    if (!cl || !device || !taaOutput || !g_upscaledHdr || !g_lowResHdr) return false;

    D3D12_RESOURCE_BARRIER preBarriers[2]{};
    preBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preBarriers[0].Transition = {g_upscaledHdr.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    preBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preBarriers[1].Transition = {g_lowResHdr.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    cl->ResourceBarrier(2, preBarriers);
    g_upscaledInUav = false;

    UINT slot = (g_descRingIdx.fetch_add(1, std::memory_order_relaxed)) % kDescRingSlots;
    WriteSlotDescriptors(device, slot,
                         g_upscaledHdr.Get(), g_upscaledHdr.Get(), g_upscaledHdr.Get(),
                         taaOutput, g_lowResHdr.Get());

    ID3D12DescriptorHeap* heaps[] = {g_bridgeHeap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_bridgeRootSig.Get());
    cl->SetPipelineState(g_downsamplePso.Get());

    uint32_t constants[5] = {upW, upH, lowW, lowH, 1u};
    cl->SetComputeRoot32BitConstants(0, 5, constants, 0);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_bridgeHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(slot * kDescsPerSlot) * g_srvUavDescSize;
    cl->SetComputeRootDescriptorTable(1, gpu);
    gpu.ptr += static_cast<UINT64>(3) * g_srvUavDescSize;
    cl->SetComputeRootDescriptorTable(2, gpu);

    cl->Dispatch((lowW + 7) / 8, (lowH + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER postBarriers[2]{};
    postBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postBarriers[0].Transition = {g_lowResHdr.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    postBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    postBarriers[1].UAV.pResource = taaOutput;
    cl->ResourceBarrier(2, postBarriers);

    g_taaUpscaleReady = true;
    return true;
}

bool ResolveTaaTonemap12(ID3D12GraphicsCommandList* cl, ID3D12Device* device,
                         ID3D12Resource* tonemappedLowRes, ID3D12Resource* finalSceneOutput,
                         unsigned lowW, unsigned lowH, unsigned upW, unsigned upH) {
    if (!cl || !device || !tonemappedLowRes || !finalSceneOutput) return false;
    if (!EnsureBridgePipeline(device)) return false;

    bool useUpscaled = g_taaUpscaleReady && g_upscaledHdr && g_lowResHdr && !g_upscaledInUav;
    if (useUpscaled) {
        D3D12_RESOURCE_DESC upDesc = g_upscaledHdr->GetDesc();
        if (upDesc.Width != upW || upDesc.Height != upH) {
            useUpscaled = false;
        }
    }
    ID3D12Resource* srv0 = useUpscaled ? g_upscaledHdr.Get() : tonemappedLowRes;
    ID3D12Resource* srv1 = useUpscaled ? g_lowResHdr.Get() : tonemappedLowRes;
    ID3D12Resource* srv2 = tonemappedLowRes;

    UINT slot = (g_descRingIdx.fetch_add(1, std::memory_order_relaxed)) % kDescRingSlots;
    WriteSlotDescriptors(device, slot, srv0, srv1, srv2, finalSceneOutput, finalSceneOutput);

    ID3D12DescriptorHeap* heaps[] = {g_bridgeHeap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g_bridgeRootSig.Get());
    cl->SetPipelineState(g_transferPso.Get());

    uint32_t constants[5] = {lowW, lowH, upW, upH, useUpscaled ? 1u : 0u};
    cl->SetComputeRoot32BitConstants(0, 5, constants, 0);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_bridgeHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(slot * kDescsPerSlot) * g_srvUavDescSize;
    cl->SetComputeRootDescriptorTable(1, gpu);
    gpu.ptr += static_cast<UINT64>(3) * g_srvUavDescSize;
    cl->SetComputeRootDescriptorTable(2, gpu);

    cl->Dispatch((upW + 7) / 8, (upH + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = finalSceneOutput;
    cl->ResourceBarrier(1, &uavBarrier);
    return true;
}

void ResetTaaUpscale12() {
    g_taaUpscaleReady = false;
    g_upscaledInUav = false;
    Retire(std::move(g_upscaledHdr));
    Retire(std::move(g_lowResHdr));
    g_downsamplePso.Reset();
    g_transferPso.Reset();
    g_bridgeRootSig.Reset();
    g_bridgeHeap.Reset();
    g_bridgeDevice = nullptr;
}

}  // namespace mhwfsr::d3d
