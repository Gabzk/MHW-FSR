#include "probe.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "log.h"

namespace mhwfsr::probe {

namespace {

using Microsoft::WRL::ComPtr;

struct Capture {
    std::string name;
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

std::vector<Capture> g_captures;
int g_framesLeft = 0;
constexpr int kWaitFrames = 8;

float Half(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    float v;
    if (exp == 31) v = man ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    else if (exp == 0) v = std::ldexp(static_cast<float>(man), -24);
    else v = std::ldexp(1.0f + man / 1024.0f, static_cast<int>(exp) - 15);
    return sign ? -v : v;
}

// Unsigned small float with a 5-bit exponent (R11G11B10 channels).
float SmallFloat(uint32_t bits, int manBits) {
    uint32_t man = bits & ((1u << manBits) - 1), exp = bits >> manBits;
    if (exp == 31) return man ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    if (exp == 0) return std::ldexp(static_cast<float>(man), -14 - manBits);
    return std::ldexp(1.0f + static_cast<float>(man) / (1u << manBits), static_cast<int>(exp) - 15);
}

bool Record(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, const char* name) {
    if (!res) return false;
    D3D12_RESOURCE_DESC desc = res->GetDesc();
    D3D12_HEAP_PROPERTIES heapProps{};
    D3D12_HEAP_FLAGS heapFlags{};
    HRESULT placed = res->GetHeapProperties(&heapProps, &heapFlags);
    LOGI("probe {}: {} {}x{} fmt {} mips {} array {} samples {}/{} layout {} flags {:#x} align {:#x}, heap {} "
         "(type {}, flags {:#x})",
         name, static_cast<int>(desc.Dimension), desc.Width, desc.Height, static_cast<int>(desc.Format), desc.MipLevels,
         desc.DepthOrArraySize, desc.SampleDesc.Count, desc.SampleDesc.Quality, static_cast<int>(desc.Layout),
         static_cast<unsigned>(desc.Flags), desc.Alignment, SUCCEEDED(placed) ? "committed/placed" : "reserved?",
         static_cast<int>(heapProps.Type), static_cast<unsigned>(heapFlags));
    ComPtr<ID3D12Device> device;
    if (FAILED(res->GetDevice(IID_PPV_ARGS(&device)))) return false;
    Capture c;
    c.name = name;
    c.width = static_cast<UINT>(desc.Width);
    c.height = desc.Height;
    c.format = desc.Format;
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &c.footprint, &rows, &rowSize, &total);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = total;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&c.buffer))))
        return false;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_SOURCE};
    cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = c.buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = c.footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cl->ResourceBarrier(1, &b);
    g_captures.push_back(std::move(c));
    return true;
}

struct Stats {
    uint64_t count = 0, nan = 0, inf = 0, zero = 0;
    double sum = 0, sumAbs = 0;
    float min = std::numeric_limits<float>::infinity();
    float max = -std::numeric_limits<float>::infinity();
    int firstBadX = -1, firstBadY = -1;
    std::vector<float> samples;  // for percentiles of |v|

    void Add(float v, UINT x, UINT y) {
        ++count;
        if (std::isnan(v) || std::isinf(v)) {
            if (std::isnan(v)) ++nan; else ++inf;
            if (firstBadX < 0) {
                firstBadX = static_cast<int>(x);
                firstBadY = static_cast<int>(y);
            }
            return;
        }
        if (v == 0.0f) ++zero;
        sum += v;
        sumAbs += std::fabs(v);
        min = std::min(min, v);
        max = std::max(max, v);
        if ((count & 63) == 0) samples.push_back(std::fabs(v));
    }

    std::string Describe() {
        float p50 = 0, p99 = 0;
        if (!samples.empty()) {
            std::sort(samples.begin(), samples.end());
            p50 = samples[samples.size() / 2];
            p99 = samples[std::min(samples.size() - 1, samples.size() * 99 / 100)];
        }
        uint64_t finite = count - nan - inf;
        return std::format("n {} nan {} inf {} (first bad at {},{}) zero {} min {:.6g} max {:.6g} mean {:.6g} "
                           "mean|v| {:.6g} p50|v| {:.6g} p99|v| {:.6g}",
                           count, nan, inf, firstBadX, firstBadY, zero, min, max, finite ? sum / finite : 0.0,
                           finite ? sumAbs / finite : 0.0, p50, p99);
    }
};

void Analyze(Capture& c) {
    uint8_t* data = nullptr;
    // The last row is not padded to the pitch, so the buffer can be shorter than pitch * height.
    D3D12_RANGE range{0, static_cast<SIZE_T>(c.buffer->GetDesc().Width)};
    if (FAILED(c.buffer->Map(0, &range, reinterpret_cast<void**>(&data)))) {
        LOGE("probe {}: map failed", c.name);
        return;
    }
    const UINT pitch = c.footprint.Footprint.RowPitch;
    auto at = [&](UINT, UINT y) { return data + c.footprint.Offset + static_cast<size_t>(y) * pitch; };

    if (c.format == DXGI_FORMAT_R11G11B10_FLOAT) {
        Stats r, g, b;
        for (UINT y = 0; y < c.height; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(at(0, y));
            for (UINT x = 0; x < c.width; ++x) {
                uint32_t v = row[x];
                r.Add(SmallFloat(v & 0x7ff, 6), x, y);
                g.Add(SmallFloat((v >> 11) & 0x7ff, 6), x, y);
                b.Add(SmallFloat(v >> 22, 5), x, y);
            }
        }
        LOGI("probe {} R: {}", c.name, r.Describe());
        LOGI("probe {} G: {}", c.name, g.Describe());
        LOGI("probe {} B: {}", c.name, b.Describe());
    } else if (c.format == DXGI_FORMAT_R16G16_FLOAT || c.format == DXGI_FORMAT_R16G16_TYPELESS) {
        Stats sx, sy;
        for (UINT y = 0; y < c.height; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(at(0, y));
            for (UINT x = 0; x < c.width; ++x) {
                sx.Add(Half(row[2 * x]), x, y);
                sy.Add(Half(row[2 * x + 1]), x, y);
            }
        }
        LOGI("probe {} X: {}", c.name, sx.Describe());
        LOGI("probe {} Y: {}", c.name, sy.Describe());
        for (auto [px, py] : {std::pair{c.width / 2, c.height / 2}, std::pair{c.width / 4, c.height / 4},
                              std::pair{c.width * 3 / 4, c.height * 3 / 4}, std::pair{10u, 10u}}) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(at(0, py));
            LOGI("probe {} at {},{}: {:.6g}, {:.6g}", c.name, px, py, Half(row[2 * px]), Half(row[2 * px + 1]));
        }
    } else if (c.format == DXGI_FORMAT_R32G32_FLOAT || c.format == DXGI_FORMAT_R32G32_TYPELESS) {
        Stats sx, sy;
        for (UINT y = 0; y < c.height; ++y) {
            const float* row = reinterpret_cast<const float*>(at(0, y));
            for (UINT x = 0; x < c.width; ++x) {
                sx.Add(row[2 * x], x, y);
                sy.Add(row[2 * x + 1], x, y);
            }
        }
        LOGI("probe {} X: {}", c.name, sx.Describe());
        LOGI("probe {} Y: {}", c.name, sy.Describe());
        for (auto [px, py] : {std::pair{c.width / 2, c.height / 2}, std::pair{c.width / 4, c.height / 4},
                              std::pair{c.width * 3 / 4, c.height * 3 / 4}, std::pair{10u, 10u}}) {
            const float* row = reinterpret_cast<const float*>(at(0, py));
            LOGI("probe {} at {},{}: {:.6g}, {:.6g}", c.name, px, py, row[2 * px], row[2 * px + 1]);
        }
    } else if (c.format == DXGI_FORMAT_R32_FLOAT || c.format == DXGI_FORMAT_R32_TYPELESS) {
        Stats s;
        for (UINT y = 0; y < c.height; ++y) {
            const float* row = reinterpret_cast<const float*>(at(0, y));
            for (UINT x = 0; x < c.width; ++x) s.Add(row[x], x, y);
        }
        LOGI("probe {}: {}", c.name, s.Describe());
        for (auto [px, py] : {std::pair{c.width / 2, c.height / 2}, std::pair{c.width / 2, 5u},
                              std::pair{c.width / 2, c.height - 5}, std::pair{5u, c.height / 2}}) {
            const float* row = reinterpret_cast<const float*>(at(0, py));
            LOGI("probe {} at {},{}: {:.8g}", c.name, px, py, row[px]);
        }
    } else {
        LOGI("probe {}: format {} not decoded", c.name, static_cast<int>(c.format));
    }
    D3D12_RANGE none{0, 0};
    c.buffer->Unmap(0, &none);
}

}  // namespace

void Schedule(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* mv, ID3D12Resource* depth,
              ID3D12Resource* output) {
    if (g_framesLeft > 0) return;
    g_captures.clear();
    if (output) {
        D3D12_RESOURCE_DESC d = output->GetDesc();
        LOGI("probe output: {}x{} fmt {} mips {} samples {} layout {} flags {:#x}", d.Width, d.Height,
             static_cast<int>(d.Format), d.MipLevels, d.SampleDesc.Count, static_cast<int>(d.Layout),
             static_cast<unsigned>(d.Flags));
    }
    bool ok = Record(cl, color, "color");
    ok &= Record(cl, mv, "motion");
    ok &= Record(cl, depth, "depth");
    g_framesLeft = kWaitFrames;
    LOGI("probe scheduled (copies recorded: {})", ok);
}

void Poll() {
    if (g_framesLeft <= 0) return;
    if (--g_framesLeft > 0) return;
    for (auto& c : g_captures) Analyze(c);
    g_captures.clear();
}

bool Pending() { return g_framesLeft > 0; }

namespace {

struct DumpFrame {
    Capture capture;
    Capture motion;
    Capture depth;
    float jx = 0, jy = 0;
};
std::wstring g_dumpDir;
int g_dumpWanted = 0;
ULONGLONG g_dumpStart = 0;  // GetTickCount64 value the recording starts at
std::vector<DumpFrame> g_dumpFrames;
int g_dumpWait = 0;
constexpr UINT kDumpSize = 768;

// Writes the w x h crop at (x0, y0) of a float texture with `channels` 32-bit channels.
void WriteFloatCrop(Capture& c, UINT channels, const wchar_t* prefix, size_t index, UINT x0, UINT y0, UINT w, UINT h) {
    uint8_t* data = nullptr;
    if (!c.buffer || FAILED(c.buffer->Map(0, nullptr, reinterpret_cast<void**>(&data)))) return;
    std::vector<float> out(static_cast<size_t>(w) * h * channels);
    for (UINT y = 0; y < h; ++y) {
        const float* row = reinterpret_cast<const float*>(data + c.footprint.Offset +
                                                          static_cast<size_t>(y0 + y) * c.footprint.Footprint.RowPitch);
        std::memcpy(&out[static_cast<size_t>(y) * w * channels], row + channels * x0, sizeof(float) * channels * w);
    }
    D3D12_RANGE none{0, 0};
    c.buffer->Unmap(0, &none);
    std::wstring path = g_dumpDir + L"\\" + prefix + std::to_wstring(index) + L".bin";
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        fwrite(&w, 4, 1, f);
        fwrite(&h, 4, 1, f);
        fwrite(out.data(), sizeof(float), out.size(), f);
        fclose(f);
    }
}

void WriteDump() {
    std::wstring list = g_dumpDir + L"\\frames.txt";
    FILE* idx = _wfopen(list.c_str(), L"w");
    for (size_t i = 0; i < g_dumpFrames.size(); ++i) {
        Capture& c = g_dumpFrames[i].capture;
        uint8_t* data = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(c.buffer->GetDesc().Width)};
        if (FAILED(c.buffer->Map(0, &range, reinterpret_cast<void**>(&data)))) continue;
        UINT w = std::min(kDumpSize, c.width), h = std::min(kDumpSize, c.height);
        UINT x0 = (c.width - w) / 2, y0 = (c.height - h) / 2;
        std::vector<float> lum(static_cast<size_t>(w) * h);
        for (UINT y = 0; y < h; ++y) {
            const uint32_t* row =
                reinterpret_cast<const uint32_t*>(data + c.footprint.Offset + static_cast<size_t>(y0 + y) * c.footprint.Footprint.RowPitch);
            for (UINT x = 0; x < w; ++x) {
                uint32_t v = row[x0 + x];
                float r = SmallFloat(v & 0x7ff, 6), g = SmallFloat((v >> 11) & 0x7ff, 6), b = SmallFloat(v >> 22, 5);
                lum[static_cast<size_t>(y) * w + x] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            }
        }
        D3D12_RANGE none{0, 0};
        c.buffer->Unmap(0, &none);
        std::wstring path = g_dumpDir + L"\\frame_" + std::to_wstring(i) + L".bin";
        if (FILE* f = _wfopen(path.c_str(), L"wb")) {
            fwrite(&w, 4, 1, f);
            fwrite(&h, 4, 1, f);
            fwrite(lum.data(), sizeof(float), lum.size(), f);
            fclose(f);
        }
        DumpFrame& df = g_dumpFrames[i];
        if (df.motion.format == DXGI_FORMAT_R32G32_FLOAT) WriteFloatCrop(df.motion, 2, L"motion_", i, x0, y0, w, h);
        if (df.depth.format == DXGI_FORMAT_R32_FLOAT) WriteFloatCrop(df.depth, 1, L"depth_", i, x0, y0, w, h);
        if (idx) fprintf(idx, "%zu %.6f %.6f %u %u\n", i, df.jx, df.jy, x0, y0);
    }
    if (idx) fclose(idx);
    LOGI("frame dump written: {} frames", g_dumpFrames.size());
    g_dumpFrames.clear();
}

}  // namespace

void RequestFrameDump(const wchar_t* dir, int count, unsigned delayMs) {
    g_dumpDir = dir;
    g_dumpWanted = count;
    g_dumpStart = GetTickCount64() + delayMs;
    g_dumpFrames.clear();
    g_dumpWait = 0;
    LOGI("frame dump of {} frames starts in {:.1f} s", count, delayMs / 1000.0);
}

void RecordFrame(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth,
                 float jitterX, float jitterY) {
    if (g_dumpWait > 0) {
        if (--g_dumpWait == 0) WriteDump();
        return;
    }
    if (g_dumpWanted <= 0 || GetTickCount64() < g_dumpStart) return;
    size_t before = g_captures.size();
    if (!Record(cl, color, "dump")) return;
    DumpFrame f;
    f.capture = std::move(g_captures.back());
    g_captures.resize(before);
    if (motion && Record(cl, motion, "dump motion")) {
        f.motion = std::move(g_captures.back());
        g_captures.resize(before);
    }
    if (depth && Record(cl, depth, "dump depth")) {
        f.depth = std::move(g_captures.back());
        g_captures.resize(before);
    }
    f.jx = jitterX;
    f.jy = jitterY;
    g_dumpFrames.push_back(std::move(f));
    if (--g_dumpWanted == 0) g_dumpWait = kWaitFrames;
}

}  // namespace mhwfsr::probe
