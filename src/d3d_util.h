#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace mhwdlss::d3d {

using Microsoft::WRL::ComPtr;

// D3D12 frees an object as soon as its last reference goes, even while command lists the GPU has
// not finished still use it, and the GPU then faults (device removed). The plugin has no fence on
// the game's queue, so what it replaces is retired here and released some frames later, long after
// the game (which runs at most a few frames ahead of the GPU) has finished with it.
void Retire(std::function<void()> release);
void Retire(ComPtr<ID3D12Resource> resource);
// An NGX feature: retired like the rest, but RetireFlushFeatures can release it early.
void RetireFeature(std::function<void()> release);
// With the game's renderer frame counter, as often as wanted: releases what has been retired for
// long enough, counted in distinct frames.
void RetireTick(uint64_t frame);
// Releases the retired NGX features now: they must be gone before NGX shuts down.
void RetireFlushFeatures();

struct TextureInfo {
    unsigned width = 0;
    unsigned height = 0;
    unsigned format = 0;
};

TextureInfo Info(ID3D12Resource* resource);
TextureInfo Info(ID3D11Resource* resource);

bool IsFloatFormat(unsigned dxgiFormat);

// A texture owned by the plugin that mirrors `source` (same size and format). D3D12 copies are
// kept in NON_PIXEL_SHADER_RESOURCE; they are recreated when the source changes size or format,
// and the old one is retired.
class Mirror12 {
public:
    // Copies `source` (currently in NON_PIXEL_SHADER_RESOURCE) into the mirror.
    ID3D12Resource* Update(ID3D12GraphicsCommandList* cl, ID3D12Resource* source);
    void Reset() { Retire(std::move(texture_)); }

private:
    ComPtr<ID3D12Resource> texture_;
};

class Mirror11 {
public:
    ID3D11Resource* Update(ID3D11DeviceContext* ctx, ID3D11Resource* source);
    void Reset() { texture_.Reset(); }

private:
    ComPtr<ID3D11Texture2D> texture_;
};

// Copies a rectangle of `src` (NON_PIXEL_SHADER_RESOURCE) to the same place in `dst`
// (UNORDERED_ACCESS); both states are restored afterwards.
void CopyRect12(ID3D12GraphicsCommandList* cl, ID3D12Resource* dst, ID3D12Resource* src, unsigned x, unsigned y,
                unsigned w, unsigned h);
void CopyRect11(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src, unsigned x, unsigned y, unsigned w,
                unsigned h);

// Debug: writes an R11G11B10 texture to `path` as uint32 width, height, float RGB.
bool DumpTexture11(ID3D11DeviceContext* ctx, ID3D11Resource* texture, const std::wstring& path);

}  // namespace mhwdlss::d3d
