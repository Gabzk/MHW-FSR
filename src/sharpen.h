#pragma once

#include <d3d11.h>
#include <d3d12.h>

#include "config.h"

namespace mhwdlss::sharpen {

// Sharpening after DLSS. DLSS 2.5.1 and later ignore the Sharpness parameter the game sends, so
// the plugin sharpens the DLSS output itself: DLSS writes into the plugin's texture from
// Target12/Target11 instead of the game's output, and Apply12/Apply11 sharpen that texture into
// the game's output texture with one of three filters:
// - NVSharpen, from NVIDIA Image Scaling (shaders/nis_sharpen.hlsl);
// - Vanilla, the filter of the game's original DLSS sharpening (shaders/vanilla_sharpen.hlsl);
// - RCAS, from AMD FidelityFX Super Resolution 1.0 (shaders/rcas.hlsl).
//
// `strength` is 0..1, scaled to each filter's own range in sharpen.cpp. `hdr` means linear HDR
// input (the game's float buffers); otherwise the input is taken as display-referred 0..1.

// D3D12: the texture is in UNORDERED_ACCESS, the state DLSS wants for its output. Returns nullptr
// when the pass cannot run (DLSS should then write the game's output directly).
ID3D12Resource* Target12(ID3D12Device* device, unsigned width, unsigned height, DXGI_FORMAT format);
// Records the pass on the DLSS command list. `output` is in UNORDERED_ACCESS and stays there.
bool Apply12(ID3D12GraphicsCommandList* cl, ID3D12Resource* output, unsigned x, unsigned y, Sharpener filter,
             float strength, bool hdr);

ID3D11Resource* Target11(ID3D11Device* device, unsigned width, unsigned height, DXGI_FORMAT format);
// Runs the pass on `ctx`; the compute state it touches is restored afterwards.
bool Apply11(ID3D11DeviceContext* ctx, ID3D11Resource* output, unsigned x, unsigned y, Sharpener filter,
             float strength, bool hdr);

// Drops all GPU objects (NGX shutdown / device change).
void Release();

}  // namespace mhwdlss::sharpen
