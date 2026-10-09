#include "d3d_util.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

#include "log.h"

namespace mhwdlss::d3d {

namespace {

// Frames a retired object is kept. Generous: a frame can count more than once (several TAA
// filters), and the game queues at most about three frames ahead of the GPU.
constexpr uint64_t kRetireFrames = 16;

struct Retired {
    uint64_t frame;
    std::function<void()> release;
};
std::mutex g_retireMutex;
std::deque<Retired> g_retired;
uint64_t g_frame = 0;

}  // namespace

void Retire(std::function<void()> release) {
    if (!release) return;
    std::lock_guard lock(g_retireMutex);
    g_retired.push_back({g_frame, std::move(release)});
}

void Retire(ComPtr<ID3D12Resource> resource) {
    if (!resource) return;
    Retire([r = std::move(resource)]() mutable { r.Reset(); });
}

void RetireTick(uint64_t frame) {
    std::deque<Retired> due;
    {
        std::lock_guard lock(g_retireMutex);
        g_frame = frame;
        while (!g_retired.empty() && frame >= g_retired.front().frame + kRetireFrames) {
            due.push_back(std::move(g_retired.front()));
            g_retired.pop_front();
        }
    }
    for (auto& r : due) r.release();
}

void RetireFlush() {
    std::deque<Retired> all;
    {
        std::lock_guard lock(g_retireMutex);
        all.swap(g_retired);
    }
    for (auto& r : all) r.release();
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

TextureInfo Info(ID3D11Resource* resource) {
    TextureInfo info;
    ComPtr<ID3D11Texture2D> tex;
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&tex)))) return info;
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    info.width = desc.Width;
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

void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* a, D3D12_RESOURCE_STATES aBefore,
                D3D12_RESOURCE_STATES aAfter, ID3D12Resource* b, D3D12_RESOURCE_STATES bBefore,
                D3D12_RESOURCE_STATES bAfter) {
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Transition = {a, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, aBefore, aAfter};
    barriers[1].Transition = {b, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, bBefore, bAfter};
    cl->ResourceBarrier(2, barriers);
}

}  // namespace

ID3D12Resource* Mirror12::Update(ID3D12GraphicsCommandList* cl, ID3D12Resource* source) {
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    if (texture_) {
        D3D12_RESOURCE_DESC have = texture_->GetDesc();
        if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) Reset();
    }
    if (!texture_) {
        ComPtr<ID3D12Device> device;
        if (FAILED(source->GetDevice(IID_PPV_ARGS(&device)))) return nullptr;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        desc.Alignment = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&texture_));
        if (FAILED(hr)) {
            LOGE("mirror texture {}x{} fmt {} could not be created ({:#x})", desc.Width, desc.Height,
                 static_cast<int>(desc.Format), static_cast<unsigned>(hr));
            return nullptr;
        }
    }
    Transition(cl, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE,
               texture_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(texture_.Get(), source);
    Transition(cl, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               texture_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return texture_.Get();
}

ID3D11Resource* Mirror11::Update(ID3D11DeviceContext* ctx, ID3D11Resource* source) {
    ComPtr<ID3D11Texture2D> src;
    if (FAILED(source->QueryInterface(IID_PPV_ARGS(&src)))) return nullptr;
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);
    if (texture_) {
        D3D11_TEXTURE2D_DESC have;
        texture_->GetDesc(&have);
        if (have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format) texture_.Reset();
    }
    if (!texture_) {
        ComPtr<ID3D11Device> device;
        ctx->GetDevice(&device);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        desc.MipLevels = 1;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture_))) {
            LOGE("mirror texture {}x{} fmt {} could not be created", desc.Width, desc.Height,
                 static_cast<int>(desc.Format));
            return nullptr;
        }
    }
    ctx->CopySubresourceRegion(texture_.Get(), 0, 0, 0, 0, source, 0, nullptr);
    return texture_.Get();
}

void CopyRect12(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src, unsigned x, unsigned y,
                unsigned w, unsigned h) {
    Transition(cl, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE, dst,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = dst;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = src;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box{x, y, 0, x + w, y + h, 1};
    cl->CopyTextureRegion(&to, x, y, 0, &from, &box);
    Transition(cl, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, dst,
               D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void CopyRect11(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src, unsigned x, unsigned y, unsigned w,
                unsigned h) {
    D3D11_BOX box{x, y, 0, x + w, y + h, 1};
    ctx->CopySubresourceRegion(dst, 0, x, y, 0, src, 0, &box);
}

namespace {

float SmallFloat(uint32_t bits, int manBits) {
    uint32_t man = bits & ((1u << manBits) - 1), exp = bits >> manBits;
    if (exp == 31) return 0.0f;
    if (exp == 0) return std::ldexp(static_cast<float>(man), -14 - manBits);
    return std::ldexp(1.0f + static_cast<float>(man) / (1u << manBits), static_cast<int>(exp) - 15);
}

}  // namespace

bool DumpTexture11(ID3D11DeviceContext* ctx, ID3D11Resource* texture, const std::wstring& path) {
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&tex)))) return false;
    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R11G11B10_FLOAT) return false;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
    ctx->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    std::vector<float> rgb(static_cast<size_t>(desc.Width) * desc.Height * 3);
    for (UINT y = 0; y < desc.Height; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(m.pData) + y * m.RowPitch);
        for (UINT x = 0; x < desc.Width; ++x) {
            float* px = &rgb[(static_cast<size_t>(y) * desc.Width + x) * 3];
            px[0] = SmallFloat(row[x] & 0x7ff, 6);
            px[1] = SmallFloat((row[x] >> 11) & 0x7ff, 6);
            px[2] = SmallFloat(row[x] >> 22, 5);
        }
    }
    ctx->Unmap(staging.Get(), 0);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fwrite(&desc.Width, 4, 1, f);
    fwrite(&desc.Height, 4, 1, f);
    fwrite(rgb.data(), sizeof(float), rgb.size(), f);
    fclose(f);
    return true;
}

}  // namespace mhwdlss::d3d
