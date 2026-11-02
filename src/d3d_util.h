#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>

namespace mhwfsr::d3d {

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
bool IsFloatFormat(unsigned dxgiFormat);

// TAA-slot upscaling bridge:
// 1. BeginTaaUpscale12 allocates/returns the full-display-resolution (upW x upH) UAV texture
//    in D3D12_RESOURCE_STATE_UNORDERED_ACCESS so FSR can upscale directly inside EvaluateAtTaa.
// 2. FinishTaaUpscale12 transitions the upscaled HDR texture to SRV and downsamples it (de-jittered)
//    into both `taaOutput` (lowW x lowH, left in UNORDERED_ACCESS for uToneMapFilter) and an
//    internal `lowResHdr` reference texture (kept in NON_PIXEL_SHADER_RESOURCE).
// 3. ResolveTaaTonemap12 runs at the end of the Scene Pass, transferring the exact exposure,
//    filmic tonemapping, and 3D LUT color grading from `tonemappedLowRes` onto the full-resolution
//    `upscaledHdr` texture produced at the TAA pass, writing into `finalSceneOutput` (upW x upH).
ID3D12Resource* BeginTaaUpscale12(ID3D12GraphicsCommandList* cl, ID3D12Device* device,
                                  unsigned upW, unsigned upH, unsigned lowW, unsigned lowH,
                                  DXGI_FORMAT format);
bool FinishTaaUpscale12(ID3D12GraphicsCommandList* cl, ID3D12Device* device,
                        ID3D12Resource* taaOutput, unsigned upW, unsigned upH,
                        unsigned lowW, unsigned lowH);
bool ResolveTaaTonemap12(ID3D12GraphicsCommandList* cl, ID3D12Device* device,
                         ID3D12Resource* tonemappedLowRes, ID3D12Resource* finalSceneOutput,
                         unsigned lowW, unsigned lowH, unsigned upW, unsigned upH);
void ResetTaaUpscale12();

}  // namespace mhwfsr::d3d
