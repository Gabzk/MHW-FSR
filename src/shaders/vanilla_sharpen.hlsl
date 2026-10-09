// Sharpening pass run after DLSS, Sharpener=Vanilla: the filter of the game's original DLSS
// sharpening. NVIDIA shipped it as "DLSS Sharpen" (cuda_sharpen_kernel) with DLISP, the NGX
// feature the game's DLSS sharpness option went to. Reimplemented from what that kernel computes;
// no NVIDIA code:
// - a high-pass of luma: the pixel minus the mean of its four neighbours;
// - weakened where the 3x3 neighbourhood has high contrast (soft minimum and maximum of the cross
//   and the full 3x3, as in AMD's CAS);
// - applied to each colour channel with an overlay blend, the high-pass centred on 0.5.
//
// The overlay blend and the clamps expect display-referred values in 0..1. The game's buffers
// hold linear HDR colour, so with kEncode set the filter works on sqrt(x / (1 + x)) and decodes
// afterwards. With strength up to 1.5 the blend factor stays within 0.5 +- 0.19, so the result
// stays inside 0..1 and the decode cannot blow up.
//
// Differences to the original: border pixels repeat the edge (the original takes the left and top
// neighbours of the first column and row from the opposite edge), and the extra, per-pixel
// strength DLISP reads from the alpha of another input texture is not part of this pass.
//
// Uses the root signature of nis_sharpen.hlsl (constants at b0, SRV t0, UAV u0); the sampler
// there is not used.

cbuffer Constants : register(b0) {
    uint2 kOrigin;    // render rectangle origin in the output texture
    uint2 kSize;      // render rectangle size, also the input texture size
    float kStrength;  // 0 to 1.5, DLISP's range
    uint kEncode;     // 1: linear HDR input
};

Texture2D<float4> gInput : register(t0);
RWTexture2D<float4> gOutput : register(u0);

static const float3 kLumaWeights = float3(0.2126, 0.7152, 0.0722);

float3 Encode(float3 c) {
    c = max(c, 0.0);
    return kEncode ? sqrt(c / (1.0 + c)) : c;
}

float3 Decode(float3 e) {
    if (!kEncode) return e;
    float3 t = min(e * e, 0.999);
    return t / (1.0 - t);
}

float LumaAt(int2 p) {
    p = clamp(p, int2(0, 0), int2(kSize) - 1);
    return dot(Encode(gInput.Load(int3(p, 0)).rgb), kLumaWeights);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kSize.x || id.y >= kSize.y) return;
    int2 p = int2(id.xy);
    float4 input = gInput.Load(int3(p, 0));
    float3 c = Encode(input.rgb);
    float l = dot(c, kLumaWeights);

    float n = LumaAt(p + int2(0, -1));
    float s = LumaAt(p + int2(0, 1));
    float w = LumaAt(p + int2(-1, 0));
    float e = LumaAt(p + int2(1, 0));
    float nw = LumaAt(p + int2(-1, -1));
    float ne = LumaAt(p + int2(1, -1));
    float sw = LumaAt(p + int2(-1, 1));
    float se = LumaAt(p + int2(1, 1));

    float min5 = min(l, min(min(n, s), min(w, e)));
    float max5 = max(l, max(max(n, s), max(w, e)));
    float min9 = min(min5, min(min(nw, ne), min(sw, se)));
    float max9 = max(max5, max(max(nw, ne), max(sw, se)));
    float contrast = 0.5 * (max9 + max5) - 0.5 * (min9 + min5);
    float k = max(kStrength * (1.0 - saturate(contrast)), 0.0);

    float blend = 0.5 + k * 0.5 * (l - saturate(0.25 * (n + s + w + e)));
    float3 lo = 2.0 * c * blend;
    float3 hi = 1.0 - 2.0 * (1.0 - c) * (1.0 - blend);
    float3 o = float3(c.r < 0.5 ? lo.r : hi.r, c.g < 0.5 ? lo.g : hi.g, c.b < 0.5 ? lo.b : hi.b);

    // Added as a difference, so that pixels the filter leaves alone keep their exact input value
    // (decoding the encoded input again is not exact, and a store into R11G11B10 can round the
    // tiny error down by a whole step).
    float3 result = input.rgb + (Decode(max(o, 0.0)) - Decode(c));
    gOutput[kOrigin + id.xy] = float4(max(result, 0.0), input.a);
}
