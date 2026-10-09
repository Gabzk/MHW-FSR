#include "sharpen.h"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#pragma warning(push)
#pragma warning(disable : 4324)  // NISConfig is padded to its 256-byte alignment on purpose
#include "NIS_Config.h"
#pragma warning(pop)
#include "d3d_util.h"
#include "log.h"
#include "nis_hdr_cs.h"
#include "nis_rs.h"
#include "nis_sdr_cs.h"
#include "rcas_cs.h"
#include "vanilla_sharpen_cs.h"

namespace mhwdlss::sharpen {

namespace {

using Microsoft::WRL::ComPtr;

// NIS's constant buffer is the first 28 values of NISConfig (the struct is padded to 256 bytes).
// All shaders share NIS's root signature, so this is also the most any of them can take.
constexpr UINT kConstantCount = 28;
static_assert(offsetof(NISConfig, reserved1) + sizeof(float) == kConstantCount * 4);

// The `strength` 0..1 of the API is scaled to each filter's own range:
// - NVSharpen's sharpness 0..0.3; above that it is harsh on the game's HDR buffers;
// - Vanilla 0..1.5, DLISP's range, so 1 is the strongest the original allowed;
// - RCAS 0..1 as in versions 0.2 and 0.3.
constexpr float kNvSharpenMax = 0.3f;
constexpr float kVanillaMax = 1.5f;

// The compute shaders, in the order the pipelines and shader objects are created.
enum Shader { kNisSdr, kNisHdr, kVanilla, kRcas, kShaderCount };
const D3D12_SHADER_BYTECODE kShaders[kShaderCount] = {
    {kNisSdrCs, sizeof(kNisSdrCs)},
    {kNisHdrCs, sizeof(kNisHdrCs)},
    {k_vanilla_sharpen_cs, sizeof(k_vanilla_sharpen_cs)},
    {k_rcas_cs, sizeof(k_rcas_cs)},
};

// One dispatch of a filter: its shader, constants and thread group footprint.
struct Pass {
    Shader shader = kNisSdr;
    uint32_t constants[kConstantCount] = {};
    UINT constantCount = 0;
    unsigned blockWidth = 8, blockHeight = 8;  // pixels covered by one thread group

    UINT GroupsX(unsigned width) const { return (width + blockWidth - 1) / blockWidth; }
    UINT GroupsY(unsigned height) const { return (height + blockHeight - 1) / blockHeight; }
    // The first `count` 32-bit values of `values`.
    void SetConstants(const void* values, UINT count) {
        count = std::min(count, kConstantCount);
        std::memcpy(constants, values, count * 4);
        constantCount = count;
    }
};

NISConfig MakeConfig(unsigned width, unsigned height, unsigned x, unsigned y, float strength, bool hdr) {
    NISConfig config{};
    NVSharpenUpdateConfig(config, strength, 0, 0, width, height, width, height, x, y,
                          hdr ? NISHDRMode::Linear : NISHDRMode::None);
    if (hdr) {
        // NIS means sharpness 0 to be no sharpening, and its SDR constants get there; the linear
        // HDR ones still sharpen noticeably at 0. Below 0.5 (NIS's default) the effect is faded
        // out by scaling strength and limit together, which scales the unsharp mask linearly.
        float fade = std::min(1.0f, strength / 0.5f);
        config.kSharpStrengthMin *= fade;
        config.kSharpStrengthScale *= fade;
        config.kSharpLimitMin *= fade;
        config.kSharpLimitScale *= fade;
    }
    // NVSharpen writes while dst <= kOutputViewportWidth/Height, one column and row past the
    // viewport; that would land outside the render rectangle in the game's texture.
    config.kOutputViewportWidth = width - 1;
    config.kOutputViewportHeight = height - 1;
    return config;
}

// `width` x `height` is the plugin's texture; the result lands at x, y in the game's texture.
Pass MakePass(Sharpener filter, unsigned width, unsigned height, unsigned x, unsigned y, float strength, bool hdr) {
    strength = std::clamp(strength, 0.0f, 1.0f);
    Pass pass;
    switch (filter) {
        case Sharpener::Vanilla: {
            // vanilla_sharpen.hlsl: origin, size, strength, encode.
            struct {
                uint32_t originX, originY, width, height;
                float strength;
                uint32_t encode;
            } c{x, y, width, height, strength * kVanillaMax, hdr ? 1u : 0u};
            pass.shader = kVanilla;
            pass.SetConstants(&c, sizeof(c) / 4);
            break;
        }
        case Sharpener::Rcas: {
            // rcas.hlsl: size, target offset, sharpness, HDR flag, padding.
            struct {
                uint32_t width, height, x, y;
                float sharpness;
                uint32_t hdr, pad[2];
            } c{width, height, x, y, strength, hdr ? 1u : 0u, {0, 0}};
            pass.shader = kRcas;
            pass.SetConstants(&c, sizeof(c) / 4);
            break;
        }
        case Sharpener::NVSharpen:
        default: {
            NISConfig config = MakeConfig(width, height, x, y, strength * kNvSharpenMax, hdr);
            pass.shader = hdr ? kNisHdr : kNisSdr;
            pass.SetConstants(&config, kConstantCount);
            pass.blockWidth = pass.blockHeight = 32;  // NIS_BLOCK_WIDTH/HEIGHT in nis_sharpen.hlsl
            break;
        }
    }
    return pass;
}

// UAVs and SRVs need a typed format.
DXGI_FORMAT Typed(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        // sRGB views cannot be UAVs; the data is the same.
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return f;
    }
}

// ---------------------------------------------------------------------------------------------
// D3D12

// Descriptor pairs (SRV + UAV) used round robin; a pass writes its pair while recording, so the
// ring must be longer than the number of frames in flight.
constexpr UINT kDescriptorPairs = 64;

struct State12 {
    ID3D12Device* device = nullptr;  // owned by the game
    bool failed = false;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipeline[kShaderCount];
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptorSize = 0;
    UINT nextPair = 0;

    ComPtr<ID3D12Resource> target;
    unsigned width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
State12 g12;

bool Init12(ID3D12Device* device) {
    if (g12.device == device) return !g12.failed;
    g12 = {};
    g12.device = device;
    g12.failed = true;

    HRESULT hr = device->CreateRootSignature(0, kNisRootSig, sizeof(kNisRootSig), IID_PPV_ARGS(&g12.rootSignature));
    if (FAILED(hr)) {
        LOGE("sharpening: root signature failed ({:#x}); sharpening is off", static_cast<unsigned>(hr));
        return false;
    }
    for (int i = 0; i < kShaderCount; ++i) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = g12.rootSignature.Get();
        pso.CS = kShaders[i];
        hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g12.pipeline[i]));
        if (FAILED(hr)) {
            LOGE("sharpening: pipeline failed ({:#x}); sharpening is off", static_cast<unsigned>(hr));
            return false;
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 2 * kDescriptorPairs;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g12.heap));
    if (FAILED(hr)) {
        LOGE("sharpening: descriptor heap failed ({:#x}); sharpening is off", static_cast<unsigned>(hr));
        return false;
    }
    g12.descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g12.failed = false;
    return true;
}

void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    cl->ResourceBarrier(1, &b);
}

// ---------------------------------------------------------------------------------------------
// D3D11

struct State11 {
    ID3D11Device* device = nullptr;  // owned by the game
    bool failed = false;
    ComPtr<ID3D11ComputeShader> shader[kShaderCount];
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;

    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11ShaderResourceView> targetView;
    unsigned width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
State11 g11;

bool Init11(ID3D11Device* device) {
    if (g11.device == device) return !g11.failed;
    g11 = {};
    g11.device = device;
    g11.failed = true;
    HRESULT hr = S_OK;
    for (int i = 0; i < kShaderCount && SUCCEEDED(hr); ++i)
        hr = device->CreateComputeShader(kShaders[i].pShaderBytecode, kShaders[i].BytecodeLength, nullptr,
                                         &g11.shader[i]);
    if (FAILED(hr)) {
        LOGE("sharpening: compute shader failed ({:#x}); sharpening is off", static_cast<unsigned>(hr));
        return false;
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = kConstantCount * 4;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device->CreateBuffer(&cb, nullptr, &g11.constants);
    if (FAILED(hr)) {
        LOGE("sharpening: constant buffer failed ({:#x}); sharpening is off", static_cast<unsigned>(hr));
        return false;
    }
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sd, &g11.sampler);
    if (FAILED(hr)) {
        LOGE("sharpening: sampler failed ({:#x}); sharpening is off", static_cast<unsigned>(hr));
        return false;
    }
    g11.failed = false;
    return true;
}

}  // namespace

ID3D12Resource* Target12(ID3D12Device* device, unsigned width, unsigned height, DXGI_FORMAT format) {
    if (!device || !Init12(device)) return nullptr;
    format = Typed(format);
    if (g12.target && g12.width == width && g12.height == height && g12.format == format) return g12.target.Get();
    // The GPU may still be using the old one (see d3d::Retire).
    d3d::Retire(std::move(g12.target));

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                 IID_PPV_ARGS(&g12.target));
    if (FAILED(hr)) {
        LOGE("sharpening: {}x{} texture (format {}) failed ({:#x})", width, height, static_cast<int>(format),
             static_cast<unsigned>(hr));
        g12.target.Reset();
        return nullptr;
    }
    g12.target->SetName(L"MHWDLSS sharpen input");
    g12.width = width;
    g12.height = height;
    g12.format = format;
    LOGI("sharpening: {}x{} texture created", width, height);
    return g12.target.Get();
}

bool Apply12(ID3D12GraphicsCommandList* cl, ID3D12Resource* output, unsigned x, unsigned y, Sharpener filter,
             float strength, bool hdr) {
    if (!g12.target || g12.failed || !output) return false;

    UINT first = (g12.nextPair++ % kDescriptorPairs) * 2;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g12.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(first) * g12.descriptorSize;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g12.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(first) * g12.descriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = g12.format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g12.device->CreateShaderResourceView(g12.target.Get(), &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = Typed(output->GetDesc().Format);
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuUav = cpu;
    cpuUav.ptr += g12.descriptorSize;
    g12.device->CreateUnorderedAccessView(output, nullptr, &uav, cpuUav);

    Barrier(cl, g12.target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[] = {g12.heap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(g12.rootSignature.Get());
    Pass pass = MakePass(filter, g12.width, g12.height, x, y, strength, hdr);
    cl->SetPipelineState(g12.pipeline[pass.shader].Get());
    cl->SetComputeRoot32BitConstants(0, pass.constantCount, pass.constants, 0);
    cl->SetComputeRootDescriptorTable(1, gpu);
    cl->Dispatch(pass.GroupsX(g12.width), pass.GroupsY(g12.height), 1);
    Barrier(cl, g12.target.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}

ID3D11Resource* Target11(ID3D11Device* device, unsigned width, unsigned height, DXGI_FORMAT format) {
    if (!device || !Init11(device)) return nullptr;
    format = Typed(format);
    if (g11.target && g11.width == width && g11.height == height && g11.format == format) return g11.target.Get();
    g11.target.Reset();
    g11.targetView.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g11.target);
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(g11.target.Get(), nullptr, &g11.targetView);
    if (FAILED(hr)) {
        LOGE("sharpening: {}x{} texture (format {}) failed ({:#x})", width, height, static_cast<int>(format),
             static_cast<unsigned>(hr));
        g11.target.Reset();
        g11.targetView.Reset();
        return nullptr;
    }
    g11.width = width;
    g11.height = height;
    g11.format = format;
    LOGI("sharpening: {}x{} texture created", width, height);
    return g11.target.Get();
}

bool Apply11(ID3D11DeviceContext* ctx, ID3D11Resource* output, unsigned x, unsigned y, Sharpener filter,
             float strength, bool hdr) {
    if (!g11.target || g11.failed || !output) return false;
    // DLSS has already written the plugin's texture; without the pass it is copied over as is.
    auto unsharpened = [&] {
        D3D11_BOX box{0, 0, 0, g11.width, g11.height, 1};
        ctx->CopySubresourceRegion(output, 0, x, y, 0, g11.target.Get(), 0, &box);
        return false;
    };

    ComPtr<ID3D11Texture2D> outputTexture;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&outputTexture)))) return unsharpened();
    D3D11_TEXTURE2D_DESC outDesc;
    outputTexture->GetDesc(&outDesc);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = Typed(outDesc.Format);
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11UnorderedAccessView> uav;
    HRESULT hr = g11.device->CreateUnorderedAccessView(output, &uavDesc, &uav);
    if (FAILED(hr)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGE("sharpening: no UAV for the output texture ({:#x})", static_cast<unsigned>(hr));
        }
        return unsharpened();
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(g11.constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return unsharpened();
    Pass pass = MakePass(filter, g11.width, g11.height, x, y, strength, hdr);
    std::memcpy(mapped.pData, pass.constants, sizeof(pass.constants));
    ctx->Unmap(g11.constants.Get(), 0);

    // The game's compute state for the slots the pass uses.
    ComPtr<ID3D11ComputeShader> oldShader;
    ID3D11ClassInstance* oldInstances[D3D11_SHADER_MAX_INTERFACES] = {};
    UINT oldInstanceCount = D3D11_SHADER_MAX_INTERFACES;
    ctx->CSGetShader(&oldShader, oldInstances, &oldInstanceCount);
    ComPtr<ID3D11ShaderResourceView> oldSrv;
    ctx->CSGetShaderResources(0, 1, &oldSrv);
    ComPtr<ID3D11UnorderedAccessView> oldUav;
    ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
    ComPtr<ID3D11Buffer> oldCb;
    ctx->CSGetConstantBuffers(0, 1, &oldCb);
    ComPtr<ID3D11SamplerState> oldSampler;
    ctx->CSGetSamplers(0, 1, &oldSampler);

    ctx->CSSetShader(g11.shader[pass.shader].Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = g11.targetView.Get();
    ctx->CSSetShaderResources(0, 1, &srv);
    ID3D11UnorderedAccessView* uavPtr = uav.Get();
    ctx->CSSetUnorderedAccessViews(0, 1, &uavPtr, nullptr);
    ID3D11Buffer* cb = g11.constants.Get();
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ID3D11SamplerState* sampler = g11.sampler.Get();
    ctx->CSSetSamplers(0, 1, &sampler);
    ctx->Dispatch(pass.GroupsX(g11.width), pass.GroupsY(g11.height), 1);

    ctx->CSSetShader(oldShader.Get(), oldInstances, oldInstanceCount);
    for (UINT i = 0; i < oldInstanceCount; ++i)
        if (oldInstances[i]) oldInstances[i]->Release();
    ID3D11ShaderResourceView* restoreSrv = oldSrv.Get();
    ctx->CSSetShaderResources(0, 1, &restoreSrv);
    ID3D11UnorderedAccessView* restoreUav = oldUav.Get();
    UINT keepCounter = static_cast<UINT>(-1);
    ctx->CSSetUnorderedAccessViews(0, 1, &restoreUav, &keepCounter);
    ID3D11Buffer* restoreCb = oldCb.Get();
    ctx->CSSetConstantBuffers(0, 1, &restoreCb);
    ID3D11SamplerState* restoreSampler = oldSampler.Get();
    ctx->CSSetSamplers(0, 1, &restoreSampler);
    return true;
}

void Release(bool d3d12) {
    if (!d3d12) {
        g11 = {};
        return;
    }
    // Queued frames may still use the texture, descriptors and pipelines (see d3d::Retire).
    auto old = std::make_shared<State12>(std::move(g12));
    g12 = {};
    d3d::Retire([old]() mutable { old.reset(); });
}

}  // namespace mhwdlss::sharpen
