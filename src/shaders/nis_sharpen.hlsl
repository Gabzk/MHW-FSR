// Sharpening pass run after DLSS: NVSharpen from the NVIDIA Image Scaling SDK v1.0.3
// (third_party/nis, MIT licence), unmodified, configured here:
// - sharpen only (no NIS scaling);
// - viewport support, so the result lands at the render rectangle's offset in the game's texture;
// - NIS_HDR_MODE is set by the build: 1 (linear HDR) for the game's float buffers, 0 for SDR.
//
// Compiled by the build as cs_5_0 once per HDR mode, and as rootsig_1_0 for the D3D12 root
// signature below. The 28 root constants are NIS's constant buffer (NISConfig without padding).

#define RootSig                                                                                    \
    "RootConstants(num32BitConstants = 28, b0),"                                                   \
    "DescriptorTable(SRV(t0), UAV(u0)),"                                                           \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_CLAMP,"      \
    " addressV = TEXTURE_ADDRESS_CLAMP, addressW = TEXTURE_ADDRESS_CLAMP)"

#define NIS_SCALER 0
#define NIS_VIEWPORT_SUPPORT 1
#define NIS_BLOCK_WIDTH 32
#define NIS_BLOCK_HEIGHT 32
#define NIS_THREAD_GROUP_SIZE 128

#ifndef NIS_HDR_MODE
#define NIS_HDR_MODE 1
#endif

#include "NIS_Main.hlsl"
