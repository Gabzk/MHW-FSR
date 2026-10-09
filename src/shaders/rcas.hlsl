// Sharpening pass run after DLSS, Sharpener=RCAS: RCAS (robust contrast-adaptive sharpening) from
// AMD FidelityFX Super Resolution 1.0, Copyright (c) 2021 Advanced Micro Devices, Inc., MIT
// licence (third_party/amd_fsr/licence.txt). The pass of versions 0.2 and 0.3.
//
// RCAS works on values in 0..1. Linear HDR input goes through the same reversible tone map FSR 2
// uses around its RCAS pass, x / (1 + max(r, g, b)), and is mapped back afterwards.
// HDR results are not clamped to the range of the five taps: on DLAA frames fine texture is made
// of single-pixel extremes, and such a clamp removes nearly all of the effect.
//
// Uses the root signature of nis_sharpen.hlsl (constants at b0, SRV t0, UAV u0); the sampler
// there is not used.

Texture2D<float4> Source : register(t0);    // DLSS output, `Size` pixels
RWTexture2D<float4> Target : register(u0);  // the game's texture

cbuffer Constants : register(b0) {
    uint2 Size;
    uint2 TargetOffset;  // where Source (0, 0) lands in Target
    float Sharpness;     // exp2(-stops), 1 = strongest
    uint Hdr;            // 1 = linear HDR input
    uint2 Pad;
};

// Upper bound on the negative lobe; larger values give stronger sharpening but more artefacts.
static const float kLimit = 0.25 - 1.0 / 16.0;

float Max3(float3 c) { return max(c.r, max(c.g, c.b)); }

float3 Load(int2 p) { return max(Source.Load(int3(clamp(p, int2(0, 0), int2(Size) - 1), 0)).rgb, 0.0); }

// Into and out of the 0..1 space RCAS works in.
float3 ToRcas(float3 c) { return Hdr ? c / (1.0 + Max3(c)) : saturate(c); }
float3 FromRcas(float3 c) {
    c = max(c, 0.0);
    return Hdr ? c / max(1.0 - Max3(c), 1.0 / 32768.0) : c;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size)) return;
    int2 p = int2(id.xy);

    //    b
    //  d e f
    //    h
    float3 rawB = Load(p + int2(0, -1));
    float3 rawD = Load(p + int2(-1, 0));
    float3 rawE = Load(p);
    float3 rawF = Load(p + int2(1, 0));
    float3 rawH = Load(p + int2(0, 1));
    float3 b = ToRcas(rawB);
    float3 d = ToRcas(rawD);
    float3 e = ToRcas(rawE);
    float3 f = ToRcas(rawF);
    float3 h = ToRcas(rawH);

    // Luma times two, for the noise detection.
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
    float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
    float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hL = h.b * 0.5 + (h.r * 0.5 + h.g);
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    float range = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);
    nz = saturate(abs(nz) / max(range, 1.0 / 65536.0));
    nz = -0.5 * nz + 1.0;

    // The lobe is limited so the result stays within 0..1 around the ring's min and max.
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1.0 / 65536.0);
    float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1.0 / 65536.0);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-kLimit, min(Max3(lobeRGB), 0.0)) * Sharpness * nz;

    float3 c = FromRcas((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0));
    // RCAS only keeps the result below the tone-mapped peak, which maps back to arbitrarily
    // bright linear values (sparkles, then bloom). HDR results are kept below twice the
    // brightest of the five taps.
    if (Hdr) c = min(c, 2.0 * max(max(max(rawB, rawD), max(rawF, rawH)), rawE));
    Target[TargetOffset + id.xy] = float4(c, Source.Load(int3(p, 0)).a);
}
