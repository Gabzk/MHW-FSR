#include "ngx_shim.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "config.h"
#include "d3d_util.h"
#include "fsr_context.h"
#include "framegen.h"
#include "fsr_loader.h"
#include "game.h"
#include "log.h"
#include "memory.h"
#include "ngx_compat.h"
#include "ngx_params.h"
#include "overlay/overlay.h"
#include "probe.h"

namespace mhwfsr::ngx {

namespace {

using Microsoft::WRL::ComPtr;

constexpr unsigned kFirstHandleId = 0x4d480001;
constexpr uint64_t kDetailedEvaluations = 3;
constexpr unsigned kMaxStaleFrames = 8;

enum class Api { D3D11, D3D12 };
const char* ApiName(Api api) { return api == Api::D3D12 ? "D3D12" : "D3D11"; }

struct Table {
    void* init;
    void* shutdown;
    void* scratch;
    void* getParameters;
    void* create;
    void* evaluate;
    void* release;
};

using PfnShutdown = NVSDK_NGX_Result(NVSDK_CONV*)();
using PfnRelease = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

struct ApiState {
    Table* table = nullptr;  // live table in the game
    Table legacy{};          // NGX 1.x core entry points that were there before us
    Table shim{};
};

struct ShimHandle {
    unsigned int id = 0;
};

struct Feature {
    ShimHandle handle;
    NVSDK_NGX_Feature id = NVSDK_NGX_Feature_Reserved_Unknown;
    Api api = Api::D3D12;
    NVSDK_NGX_Handle* legacy = nullptr;

    FsrContext fsr;

    unsigned expectedWidth = 0, expectedHeight = 0;
    unsigned staleFrames = 0;
    bool resetNext = false;
    uint64_t evaluations = 0;
    uint64_t resets = 0;

    NVSDK_NGX_Handle* Handle() { return reinterpret_cast<NVSDK_NGX_Handle*>(&handle); }
    const char* Name() const { return id == NVSDK_NGX_Feature_SuperSampling ? "FSR(TAA)" : "FSR(DLISP)"; }
};

struct UpscaleInputs {
    bool valid = false;
    d3d::Mirror12 motion12, depth12;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* depth = nullptr;
    unsigned x = 0, y = 0, width = 0, height = 0;
    float jitterX = 0, jitterY = 0;
    float cameraNear = 0.1f, cameraFar = 10000.0f, cameraFov = 0.785398f;
    int reset = 0;
    uint64_t frame = 0;
};

std::recursive_mutex g_mutex;
ApiState g_api[2];
std::vector<std::unique_ptr<Feature>> g_features;
std::atomic<unsigned> g_nextHandleId{kFirstHandleId};

ShimParameters g_params;
NVSDK_NGX_Parameter* g_legacyParams = nullptr;
bool g_contextCreated = false;
bool g_fallbackChecked = false;

UpscaleInputs g_upscale;
std::wstring g_dataDir;
std::wstring g_gameDir;
bool g_probeRequested = false;
bool g_adoptedLate = false;

LARGE_INTEGER g_qpcFreq{};
LARGE_INTEGER g_lastDispatchQpc{};

ApiState& State(Api api) { return g_api[api == Api::D3D12 ? 1 : 0]; }

unsigned GetUI(const NVSDK_NGX_Parameter* p, const char* name, unsigned def = 0) {
    unsigned v = def;
    return NVSDK_NGX_SUCCEED(p->Get(name, &v)) ? v : def;
}

int GetI(const NVSDK_NGX_Parameter* p, const char* name, int def = 0) {
    int v = def;
    return NVSDK_NGX_SUCCEED(p->Get(name, &v)) ? v : def;
}

ID3D12Resource* GetResource12(const NVSDK_NGX_Parameter* p, const char* name) {
    ID3D12Resource* r = nullptr;
    return NVSDK_NGX_SUCCEED(p->Get(name, &r)) ? r : nullptr;
}

d3d::TextureInfo Info(ID3D12Resource* resource) {
    return d3d::Info(resource);
}

ID3D12Device* DeviceOf(void* cmd) {
    if (!cmd) return nullptr;
    ComPtr<ID3D12Device> device;
    if (FAILED(static_cast<ID3D12GraphicsCommandList*>(cmd)->GetDevice(IID_PPV_ARGS(&device)))) return nullptr;
    return device.Get();
}

Feature* FindFeature(const NVSDK_NGX_Handle* handle) {
    if (!handle) return nullptr;
    for (auto& f : g_features) {
        if (f->Handle() == handle || (f->legacy && f->legacy == handle)) return f.get();
    }
    return nullptr;
}

Feature* AddFeature(Api api, NVSDK_NGX_Feature id, NVSDK_NGX_Handle* legacy) {
    auto f = std::make_unique<Feature>();
    f->handle.id = g_nextHandleId++;
    f->id = id;
    f->api = api;
    f->legacy = legacy;
    g_features.push_back(std::move(f));
    return g_features.back().get();
}

struct Rect {
    unsigned x = 0, y = 0, width = 0, height = 0;
};

Rect RectFrom(const NVSDK_NGX_Parameter* p, const d3d::TextureInfo& texture) {
    Rect r{GetUI(p, "Rect.X"), GetUI(p, "Rect.Y"), GetUI(p, "Rect.W"), GetUI(p, "Rect.H")};
    if (!r.width || !r.height) {
        r.x = r.y = 0;
        r.width = texture.width;
        r.height = texture.height;
    }
    return r;
}

bool StaleFrame(Feature& f, unsigned width, unsigned height) {
    if (!f.expectedWidth || (width == f.expectedWidth && height == f.expectedHeight)) {
        f.staleFrames = 0;
        return false;
    }
    if (++f.staleFrames > kMaxStaleFrames) {
        LOGW("{}: the game renders {}x{} instead of expected {}x{}; updating expected size",
             f.Name(), width, height, f.expectedWidth, f.expectedHeight);
        f.expectedWidth = width;
        f.expectedHeight = height;
        f.staleFrames = 0;
        return false;
    }
    if (f.staleFrames == 1) {
        LOGD("{}: skipping {}x{} frame from before switch to {}x{}", f.Name(), width, height, f.expectedWidth, f.expectedHeight);
    }
    return true;
}

NVSDK_NGX_Result NVSDK_CONV OptimalSettingsCallback(NVSDK_NGX_Parameter* p) {
    if (!p) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::lock_guard lock(g_mutex);
    unsigned w = GetUI(p, NVSDK_NGX_Parameter_Width);
    unsigned h = GetUI(p, NVSDK_NGX_Parameter_Height);
    float scale = g_config.RenderScale();
    unsigned rw = w, rh = h;
    if (scale < 0.999f) {
        rw = std::max(1u, static_cast<unsigned>(std::lround(w * scale)));
        rh = std::max(1u, static_cast<unsigned>(std::lround(h * scale)));
    }
    p->Set(NVSDK_NGX_Parameter_OutWidth, rw);
    p->Set(NVSDK_NGX_Parameter_OutHeight, rh);
    p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, rw);
    p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, rh);
    p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, rw);
    p->Set(NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, rh);
    p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    p->Set("DLSSMode", 1);

    static unsigned lastW = 0, lastH = 0, lastRw = 0;
    if (w != lastW || h != lastH || rw != lastRw) {
        lastW = w;
        lastH = h;
        lastRw = rw;
        LOGI("FSR optimal settings: output {}x{} -> render {}x{} (scale {:.3f}, {})",
             w, h, rw, rh, scale, QualityModeName(g_config.mode));
    }
    return NVSDK_NGX_Result_Success;
}

float GetFrameDeltaTimeMs() {
    if (!g_qpcFreq.QuadPart) {
        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_lastDispatchQpc);
        return 16.666f;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = static_cast<double>(now.QuadPart - g_lastDispatchQpc.QuadPart) * 1000.0 / g_qpcFreq.QuadPart;
    g_lastDispatchQpc = now;
    return static_cast<float>(std::clamp(elapsed, 0.1, 100.0));
}

uint32_t BuildFsrFlags(bool hdr) {
    uint32_t flags = 0;
    if (g_config.hdr >= 0) hdr = (g_config.hdr != 0);
    if (hdr) flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (g_config.depthInverted) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (g_config.depthInfinite) flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (g_config.autoExposure) flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (g_config.mvJittered) flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (g_config.logLevel == LogLevel::Debug) flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    return flags;
}

void LogEvaluation(Feature& f, bool success, unsigned renderW, unsigned renderH,
                   unsigned outW, unsigned outH, float jx, float jy, uint64_t frame, int reset) {
    uint64_t n = f.evaluations++;
    if (reset) ++f.resets;
    if (n < kDetailedEvaluations || !success) {
        LOGD("{} evaluate #{}: status {}; render {}x{}, output {}x{}; jitter {:.4f},{:.4f} (frame {}); reset {}",
             f.Name(), n, success ? "OK" : "FAILED", renderW, renderH, outW, outH, jx, jy, frame, reset);
    } else if (g_config.verboseFrames && n < 40) {
        LOGD("{} evaluate #{}: jitter {:.4f},{:.4f} (frame {}) reset {}", f.Name(), n, jx, jy, frame, reset);
    } else if ((n & (n - 1)) == 0) {
        Log(n == 512 ? LogLevel::Info : LogLevel::Debug,
            "{} evaluate #{} ok ({} resets since #{}; {})",
            f.Name(), n, f.resets, n / 2, game::JitterStats());
        f.resets = 0;
    }
}

NVSDK_NGX_Result EvaluateAtTaa(Feature& f, void* cmd, const NVSDK_NGX_Parameter* p) {
    if (f.api != Api::D3D12) {
        LOGE("EvaluateAtTaa: DirectX 11 is not supported by MHW-FSR. Please switch to DirectX 12.");
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }

    auto* cl = static_cast<ID3D12GraphicsCommandList*>(cmd);
    ID3D12Resource* color = GetResource12(p, NVSDK_NGX_Parameter_Color);
    ID3D12Resource* output = GetResource12(p, NVSDK_NGX_Parameter_Output);
    ID3D12Resource* motion = GetResource12(p, NVSDK_NGX_Parameter_MotionVectors);
    ID3D12Resource* depth = GetResource12(p, NVSDK_NGX_Parameter_Depth);

    if (!color || !output || !motion || !depth) {
        if (f.evaluations++ < kDetailedEvaluations) {
            LOGE("FSR evaluate: missing input resource");
        }
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }

    d3d::TextureInfo colorInfo = Info(color);
    d3d::TextureInfo outputInfo = Info(output);
    Rect rect = RectFrom(p, colorInfo);

    auto jitter = game::JitterForOutput(output);
    float scale = game::RenderScale();
    float jx = (jitter ? jitter->x : 0.0f) * g_config.jitterScaleX * scale;
    float jy = (jitter ? jitter->y : 0.0f) * g_config.jitterScaleY * scale;
    int reset = GetI(p, NVSDK_NGX_Parameter_Reset) || (jitter && jitter->cut && g_config.cutReset);
    if (f.resetNext) {
        f.resetNext = false;
        reset = 1;
    }
    uint64_t frame = jitter ? jitter->frame : 0;

    probe::Poll();
    if (g_probeRequested && !game::DlispActive()) {
        g_probeRequested = false;
        probe::Schedule(cl, color, motion, depth, output);
    }
    probe::RecordFrame(cl, color, motion, depth, jitter ? jitter->x : 0.0f, jitter ? jitter->y : 0.0f);

    if (game::DlispActive()) {
        // Upscaling mode: pass jittered color on to output texture and mirror motion/depth
        if (rect.x + rect.width > outputInfo.width || rect.y + rect.height > outputInfo.height) {
            g_upscale.valid = false;
            return NVSDK_NGX_Result_Success;
        }
        d3d::CopyRect12(cl, output, color, rect.x, rect.y, rect.width, rect.height);
        g_upscale.motion = g_upscale.motion12.Update(cl, motion);
        g_upscale.depth = g_upscale.depth12.Update(cl, depth);
        g_upscale.valid = (g_upscale.motion && g_upscale.depth);
        g_upscale.x = rect.x;
        g_upscale.y = rect.y;
        g_upscale.width = rect.width;
        g_upscale.height = rect.height;
        g_upscale.jitterX = jx;
        g_upscale.jitterY = jy;
        g_upscale.reset = reset;
        g_upscale.frame = frame;
        if (jitter) {
            g_upscale.cameraNear = jitter->cameraNear;
            g_upscale.cameraFar = jitter->cameraFar;
            g_upscale.cameraFov = jitter->fovY;
        }

        if (f.evaluations++ < kDetailedEvaluations) {
            LOGD("FSR evaluate: upscaling mode, render {}x{} passed on for DLISP", rect.width, rect.height);
        }
        return NVSDK_NGX_Result_Success;
    }

    // NativeAA (DLAA equivalent) mode: FSR runs directly at TAA position
    g_upscale.valid = false;
    g_upscale.motion = g_upscale.depth = nullptr;
    g_upscale.motion12.Reset();
    g_upscale.depth12.Reset();

    if (g_config.bypass) return NVSDK_NGX_Result_Success;
    if (StaleFrame(f, rect.width, rect.height)) return NVSDK_NGX_Result_Success;

    ID3D12Device* device = DeviceOf(cl);
    if (!device) return NVSDK_NGX_Result_Fail;

    bool hdr = (GetI(p, "Hint.HDR", 1) != 0) || d3d::IsFloatFormat(colorInfo.format);
    uint32_t flags = BuildFsrFlags(hdr);
    uint64_t versionId = loader::ResolveVersionId(device, g_config.version);

    FsrContext::Settings s{};
    s.renderW = rect.width;
    s.renderH = rect.height;
    s.upscaleW = outputInfo.width;
    s.upscaleH = outputInfo.height;
    s.flags = flags;
    s.versionId = versionId;

    if (!f.fsr.Ensure(device, s)) return NVSDK_NGX_Result_Fail;

    game::NoteDlssEvaluated();

    FsrContext::DispatchParams dp{};
    dp.commandList = cl;
    dp.color = color;
    dp.depth = depth;
    dp.motion = motion;
    dp.output = output;
    dp.jitterX = jx;
    dp.jitterY = jy;
    dp.motionScaleX = g_config.mvScaleX;
    dp.motionScaleY = g_config.mvScaleY;
    dp.renderW = rect.width;
    dp.renderH = rect.height;
    dp.upscaleW = outputInfo.width;
    dp.upscaleH = outputInfo.height;
    dp.sharpness = g_config.Sharpness();
    dp.frameTimeDeltaMs = GetFrameDeltaTimeMs();
    dp.preExposure = g_config.preExposure;
    dp.cameraNear = jitter ? jitter->cameraNear : 0.1f;
    dp.cameraFar = jitter ? jitter->cameraFar : 10000.0f;
    dp.cameraFovAngleVertical = jitter ? jitter->fovY : 0.785398f;
    dp.reset = (reset != 0);

    bool ok = f.fsr.Dispatch(dp);
    if (ok) {
        framegen::CaptureHudLess(cl, output);
        framegen::FrameData fd{};
        fd.depth = depth;
        fd.motion = motion;
        fd.jitterX = jx;
        fd.jitterY = jy;
        fd.mvScaleX = g_config.mvScaleX;
        fd.mvScaleY = g_config.mvScaleY;
        fd.renderW = rect.width;
        fd.renderH = rect.height;
        fd.displayW = outputInfo.width;
        fd.displayH = outputInfo.height;
        fd.cameraNear = dp.cameraNear;
        fd.cameraFar = dp.cameraFar;
        fd.fovY = dp.cameraFovAngleVertical;
        fd.deltaMs = dp.frameTimeDeltaMs;
        fd.reset = dp.reset;
        fd.frameId = frame;
        framegen::RecordFrameData(fd);
    }
    LogEvaluation(f, ok, rect.width, rect.height, outputInfo.width, outputInfo.height, jx, jy, frame, reset);
    return ok ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

NVSDK_NGX_Result EvaluateAtDlisp(Feature& f, void* cmd, const NVSDK_NGX_Parameter* p) {
    if (f.api != Api::D3D12) {
        return NVSDK_NGX_Result_FAIL_PlatformError;
    }

    auto* cl = static_cast<ID3D12GraphicsCommandList*>(cmd);
    ID3D12Resource* color = GetResource12(p, NVSDK_NGX_Parameter_Color);
    ID3D12Resource* output = GetResource12(p, NVSDK_NGX_Parameter_Output);

    if (!color || !output) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (!g_upscale.valid) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGD("DLISP called without valid TAA inputs (menu or transition); frame left untouched");
        }
        return NVSDK_NGX_Result_Fail;
    }
    g_upscale.valid = false;

    d3d::TextureInfo colorInfo = Info(color);
    d3d::TextureInfo outputInfo = Info(output);
    Rect rect = RectFrom(p, colorInfo);

    unsigned inW = std::min(rect.width, g_upscale.width);
    unsigned inH = std::min(rect.height, g_upscale.height);
    float jx = g_upscale.jitterX;
    float jy = g_upscale.jitterY;
    int reset = g_upscale.reset;
    if (f.resetNext) {
        f.resetNext = false;
        reset = 1;
    }

    if (StaleFrame(f, inW, inH)) return NVSDK_NGX_Result_Success;

    if (g_probeRequested) {
        g_probeRequested = false;
        probe::Schedule(cl, color, nullptr, nullptr, output);
    }

    ID3D12Device* device = DeviceOf(cl);
    if (!device) return NVSDK_NGX_Result_Fail;

    bool hdr = d3d::IsFloatFormat(colorInfo.format);
    uint32_t flags = BuildFsrFlags(hdr);
    uint64_t versionId = loader::ResolveVersionId(device, g_config.version);

    FsrContext::Settings s{};
    s.renderW = inW;
    s.renderH = inH;
    s.upscaleW = outputInfo.width;
    s.upscaleH = outputInfo.height;
    s.flags = flags;
    s.versionId = versionId;

    if (!f.fsr.Ensure(device, s)) return NVSDK_NGX_Result_Fail;

    game::NoteDlssEvaluated();

    FsrContext::DispatchParams dp{};
    dp.commandList = cl;
    dp.color = color;
    dp.depth = g_upscale.depth;
    dp.motion = g_upscale.motion;
    dp.output = output;
    dp.jitterX = jx;
    dp.jitterY = jy;
    dp.motionScaleX = g_config.mvScaleX;
    dp.motionScaleY = g_config.mvScaleY;
    dp.renderW = inW;
    dp.renderH = inH;
    dp.upscaleW = outputInfo.width;
    dp.upscaleH = outputInfo.height;
    dp.sharpness = g_config.Sharpness();
    dp.frameTimeDeltaMs = GetFrameDeltaTimeMs();
    dp.preExposure = g_config.preExposure;
    dp.cameraNear = g_upscale.cameraNear;
    dp.cameraFar = g_upscale.cameraFar;
    dp.cameraFovAngleVertical = g_upscale.cameraFov;
    dp.reset = (reset != 0);

    bool ok = f.fsr.Dispatch(dp);
    if (ok) {
        framegen::CaptureHudLess(cl, output);
        framegen::FrameData fd{};
        fd.depth = g_upscale.depth;
        fd.motion = g_upscale.motion;
        fd.jitterX = jx;
        fd.jitterY = jy;
        fd.mvScaleX = g_config.mvScaleX;
        fd.mvScaleY = g_config.mvScaleY;
        fd.renderW = inW;
        fd.renderH = inH;
        fd.displayW = outputInfo.width;
        fd.displayH = outputInfo.height;
        fd.cameraNear = dp.cameraNear;
        fd.cameraFar = dp.cameraFar;
        fd.fovY = dp.cameraFovAngleVertical;
        fd.deltaMs = dp.frameTimeDeltaMs;
        fd.reset = dp.reset;
        fd.frameId = g_upscale.frame;
        framegen::RecordFrameData(fd);
    }
    LogEvaluation(f, ok, inW, inH, outputInfo.width, outputInfo.height, jx, jy, g_upscale.frame, reset);
    return ok ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
}

NVSDK_NGX_Result ShimShutdown(Api api) {
    std::lock_guard lock(g_mutex);
    LOGD("game called NGX {} Shutdown", ApiName(api));
    for (auto& f : g_features) {
        if (f->api == api) {
            f->fsr.Shutdown();
        }
    }
    g_upscale.valid = false;
    g_upscale.motion = g_upscale.depth = nullptr;
    if (api == Api::D3D12) {
        g_upscale.motion12.Reset();
        g_upscale.depth12.Reset();
        d3d::RetireFlushFeatures();
    }
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result ShimCreate(Api api, void*, NVSDK_NGX_Feature id, NVSDK_NGX_Parameter* params,
                            NVSDK_NGX_Handle** out) {
    std::lock_guard lock(g_mutex);
    if (!out || !params) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *out = nullptr;
    if (id != NVSDK_NGX_Feature_SuperSampling && id != NVSDK_NGX_Feature_ImageSignalProcessing) {
        LOGW("game created unsupported NGX feature {}", static_cast<int>(id));
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    Feature* f = AddFeature(api, id, nullptr);
    f->expectedWidth = GetUI(params, NVSDK_NGX_Parameter_Width);
    f->expectedHeight = GetUI(params, NVSDK_NGX_Parameter_Height);
    LOGD("game created {} ({}): {}x{} -> {}x{}", f->Name(), ApiName(api),
         GetUI(params, NVSDK_NGX_Parameter_Width), GetUI(params, NVSDK_NGX_Parameter_Height),
         GetUI(params, NVSDK_NGX_Parameter_OutWidth), GetUI(params, NVSDK_NGX_Parameter_OutHeight));
    *out = f->Handle();
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result ShimEvaluate(Api api, void* cmd, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* params,
                              void*) {
    std::lock_guard lock(g_mutex);
    if (!params) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (!handle) return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    Feature* f = FindFeature(handle);
    if (!f || f->api != api) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOGE("game evaluated unknown NGX handle {}", static_cast<const void*>(handle));
        }
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;
    }
    return f->id == NVSDK_NGX_Feature_SuperSampling ? EvaluateAtTaa(*f, cmd, params) : EvaluateAtDlisp(*f, cmd, params);
}

NVSDK_NGX_Result ShimRelease(Api /*api*/, NVSDK_NGX_Handle* handle) {
    std::lock_guard lock(g_mutex);
    auto it = std::find_if(g_features.begin(), g_features.end(), [&](const auto& f) {
        return f->Handle() == handle || (f->legacy && f->legacy == handle);
    });
    if (it == g_features.end()) return NVSDK_NGX_Result_Success;
    Feature& f = **it;
    LOGD("game released {} feature {:#x}", f.Name(), f.handle.id);
    f.fsr.Retire();
    g_features.erase(it);
    return NVSDK_NGX_Result_Success;
}

template <Api A>
struct Entry {
    static NVSDK_NGX_Result NVSDK_CONV Init(unsigned long long, const wchar_t*, void*, NVSDK_NGX_Version) {
        // Unconditionally succeed for DX12 so in-game UI unlocks regardless of GPU manufacturer
        if constexpr (A == Api::D3D12) {
            LOGI("Game initialized NGX D3D12 -> MHW-FSR ready");
            return NVSDK_NGX_Result_Success;
        } else {
            LOGW("Game initialized NGX D3D11 -> MHW-FSR requires DirectX 12");
            return NVSDK_NGX_Result_FAIL_PlatformError;
        }
    }
    static NVSDK_NGX_Result NVSDK_CONV Shutdown() { return ShimShutdown(A); }
    static NVSDK_NGX_Result NVSDK_CONV Scratch(NVSDK_NGX_Feature, const NVSDK_NGX_Parameter*, size_t* size) {
        if (size) *size = 0;
        return NVSDK_NGX_Result_Success;
    }
    static NVSDK_NGX_Result NVSDK_CONV GetParameters(NVSDK_NGX_Parameter** out) {
        if (!out) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *out = &g_params;
        return NVSDK_NGX_Result_Success;
    }
    static NVSDK_NGX_Result NVSDK_CONV Create(void* cmd, NVSDK_NGX_Feature id, NVSDK_NGX_Parameter* p,
                                              NVSDK_NGX_Handle** out) {
        return ShimCreate(A, cmd, id, p, out);
    }
    static NVSDK_NGX_Result NVSDK_CONV Evaluate(void* cmd, const NVSDK_NGX_Handle* h, const NVSDK_NGX_Parameter* p,
                                                void* cb) {
        return ShimEvaluate(A, cmd, h, p, cb);
    }
    static NVSDK_NGX_Result NVSDK_CONV Release(NVSDK_NGX_Handle* h) { return ShimRelease(A, h); }

    static Table Make() {
        return Table{reinterpret_cast<void*>(&Init),          reinterpret_cast<void*>(&Shutdown),
                     reinterpret_cast<void*>(&Scratch),       reinterpret_cast<void*>(&GetParameters),
                     reinterpret_cast<void*>(&Create),        reinterpret_cast<void*>(&Evaluate),
                     reinterpret_cast<void*>(&Release)};
    }
};

void** SlotOf(HMODULE exe, const char* exportName) {
    auto* code = reinterpret_cast<uint8_t*>(GetProcAddress(exe, exportName));
    if (!code) return nullptr;
    for (int i = 0; i < 0x40; ++i) {
        if ((code[i] == 0x48 || code[i] == 0x4c) && code[i + 1] == 0x8b && (code[i + 2] & 0xc7) == 0x05) {
            return reinterpret_cast<void**>(mem::RipTarget(code + i + 3, code + i + 7));
        }
    }
    return nullptr;
}

Table* FindTable(HMODULE exe, Api api) {
    std::string prefix = std::string("NVSDK_NGX_") + ApiName(api) + "_";
    void** create = SlotOf(exe, (prefix + "CreateFeature").c_str());
    if (!create) return nullptr;
    auto* table = reinterpret_cast<Table*>(create - 4);
    struct Check {
        const char* name;
        void** expected;
    } checks[] = {
        {"Shutdown", &table->shutdown},
        {"GetScratchBufferSize", &table->scratch},
        {"GetParameters", &table->getParameters},
        {"EvaluateFeature_C", &table->evaluate},
        {"ReleaseFeature", &table->release},
    };
    for (const auto& c : checks) {
        void** slot = SlotOf(exe, (prefix + c.name).c_str());
        if (slot != c.expected) {
            LOGE("NGX {} table layout mismatch at {} ({} vs {})", ApiName(api), c.name, static_cast<void*>(slot),
                 static_cast<void*>(c.expected));
            return nullptr;
        }
    }
    return table;
}

void PatchTable(ApiState& s, Api api, bool initial) {
    if (!s.table) return;
    Table current = *s.table;
    if (current.create == s.shim.create) return;
    if (current.create) s.legacy = current;
    void** slots = reinterpret_cast<void**>(s.table);
    const void* const* values = reinterpret_cast<const void* const*>(&s.shim);
    for (int i = 0; i < 7; ++i) mem::WritePointer(slots + i, const_cast<void*>(values[i]));
    if (initial) {
        LOGD("NGX {} table hooked", ApiName(api));
    } else {
        LOGW("NGX {} table was rewritten by the game, hooked again", ApiName(api));
    }
}

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_realGetProcAddress = nullptr;

FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name) {
    if (!IS_INTRESOURCE(name) && std::strncmp(name, "NVSDK_NGX_D3D1", 14) == 0) {
        static constexpr const char* kEntries[] = {"Init",          "Shutdown",        "GetScratchBufferSize",
                                                   "GetParameters", "CreateFeature",   "EvaluateFeature",
                                                   "ReleaseFeature"};
        for (Api api : {Api::D3D11, Api::D3D12}) {
            std::string prefix = std::string("NVSDK_NGX_") + ApiName(api) + "_";
            if (std::strncmp(name, prefix.c_str(), prefix.size()) != 0) continue;
            const void* const* shim = reinterpret_cast<const void* const*>(&State(api).shim);
            for (int i = 0; i < 7; ++i) {
                if (std::strcmp(name + prefix.size(), kEntries[i]) != 0) continue;
                if (i == 0) LOGI("Game initialised NGX {} through shim entries", ApiName(api));
                return reinterpret_cast<FARPROC>(const_cast<void*>(shim[i]));
            }
        }
    }
    return g_realGetProcAddress ? g_realGetProcAddress(module, name) : GetProcAddress(module, name);
}

void** FindGetProcAddressSlot(HMODULE exe) {
    void* real[3] = {reinterpret_cast<void*>(&GetProcAddress), nullptr, nullptr};
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) real[1] = reinterpret_cast<void*>(GetProcAddress(k32, "GetProcAddress"));
    if (HMODULE kb = GetModuleHandleW(L"kernelbase.dll")) real[2] = reinterpret_cast<void*>(GetProcAddress(kb, "GetProcAddress"));
    auto rdata = mem::FindSection(exe, ".rdata");
    for (const char* exportName : {"NVSDK_NGX_D3D12_Init", "NVSDK_NGX_D3D11_Init"}) {
        auto* code = reinterpret_cast<uint8_t*>(GetProcAddress(exe, exportName));
        if (!code) continue;
        for (int i = 0; i < 0x200; ++i) {
            if (code[i] != 0xff || code[i + 1] != 0x15) continue;
            auto** slot = reinterpret_cast<void**>(mem::RipTarget(code + i + 2, code + i + 6));
            if (rdata && (reinterpret_cast<uint8_t*>(slot) < rdata->begin || reinterpret_cast<uint8_t*>(slot) >= rdata->end)) continue;
            void* value = mem::SafeRead<void*>(slot).value_or(nullptr);
            if (value && (value == real[0] || value == real[1] || value == real[2] || rdata)) return slot;
        }
    }
    return nullptr;
}

void HookNgxLookups(HMODULE exe) {
    void** slot = FindGetProcAddressSlot(exe);
    if (!slot) {
        LOGW("Loaded before renderer, but GetProcAddress import not found");
        return;
    }
    g_realGetProcAddress = reinterpret_cast<GetProcAddressFn>(*slot);
    mem::WritePointer(slot, reinterpret_cast<void*>(&HookGetProcAddress));
    LOGD("Hooked GetProcAddress import for early NGX init");
}

void AdoptContext(bool /*mayUseDxgi*/) {
    game::DlssContext* ctx = game::Context();
    if (ctx) {
        game::ForceEnableDlssSupport();
        if (ctx->params == &g_params) return;
        auto* lock = reinterpret_cast<CRITICAL_SECTION*>(ctx->lock);
        EnterCriticalSection(lock);
        g_legacyParams = static_cast<NVSDK_NGX_Parameter*>(ctx->params);
        ctx->params = &g_params;
        Api api = game::IsDx12().value_or(true) ? Api::D3D12 : Api::D3D11;
        if (ctx->dlssHandle && !FindFeature(static_cast<NVSDK_NGX_Handle*>(ctx->dlssHandle))) {
            AddFeature(api, NVSDK_NGX_Feature_SuperSampling, static_cast<NVSDK_NGX_Handle*>(ctx->dlssHandle));
        }
        if (ctx->dlispHandle && !FindFeature(static_cast<NVSDK_NGX_Handle*>(ctx->dlispHandle))) {
            AddFeature(api, NVSDK_NGX_Feature_ImageSignalProcessing, static_cast<NVSDK_NGX_Handle*>(ctx->dlispHandle));
        }
        LeaveCriticalSection(lock);
        LOGD("Adopted game DLSS context");
        return;
    }

    if (g_contextCreated || g_fallbackChecked) return;
    if (!game::Device()) return;
    g_fallbackChecked = true;

    auto dx12 = game::IsDx12();
    if (!dx12 || !*dx12) {
        LOGW("Renderer is not running in DirectX 12 mode; FSR unavailable");
        return;
    }

    auto* mem = static_cast<game::DlssContext*>(game::GameAlloc(sizeof(game::DlssContext)));
    if (!mem) {
        LOGE("Cannot allocate DLSS context for FSR");
        return;
    }
    std::memset(mem, 0, sizeof(*mem));
    InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(mem->lock));
    mem->params = &g_params;
    game::SetContext(mem);
    game::ForceEnableDlssSupport();
    g_contextCreated = true;
    LOGI("Created game DLSS context and enabled feature bits for MHW-FSR");
}

void OnFrameStart() {
    std::lock_guard lock(g_mutex);
    PatchTable(g_api[0], Api::D3D11, false);
    PatchTable(g_api[1], Api::D3D12, false);
    if (!g_contextCreated) {
        AdoptContext(false);
    } else {
        game::ForceEnableDlssSupport();
    }
    overlay::TryInstall();
}

void OnFrame() {
    std::lock_guard lock(g_mutex);
    PatchTable(g_api[0], Api::D3D11, false);
    PatchTable(g_api[1], Api::D3D12, false);
    AdoptContext(true);
    overlay::TryInstall();

    if (game::DlispActive()) {
        game::DlssContext* ctx = game::Context();
        if (ctx && !ctx->dlispHandle) {
            Api api = game::IsDx12().value_or(true) ? Api::D3D12 : Api::D3D11;
            ctx->dlispHandle = AddFeature(api, NVSDK_NGX_Feature_ImageSignalProcessing, nullptr)->Handle();
            LOGD("Attached DLISP feature for upscaling mode");
        }
    }
}

void ReloadSettings(bool& recompute);

void OnUpdate() {
    bool recompute = false;
    {
        std::lock_guard lock(g_mutex);
        PatchTable(g_api[0], Api::D3D11, false);
        PatchTable(g_api[1], Api::D3D12, false);
        AdoptContext(true);
        d3d::RetireTick(game::Frame());

        static ULONGLONG nextReload = 0;
        ULONGLONG now = GetTickCount64();
        if (now >= nextReload) {
            nextReload = now + 500;
            ReloadSettings(recompute);
        }
    }
    if (recompute && game::RecomputeRenderScale()) {
        LOGD("Render scale refreshed after config reload");
    }
}

void ReloadSettings(bool& recompute) {
    if (ReloadConfigIfChanged()) {
        game::ApplyConfig(g_config);
        LogSetLevel(g_config.logLevel);
        LOGI("MHW-FSR ini reloaded: mode {}, sharpness {:.2f}, autoExposure {}, depthInverted {}, MV scale {},{}, jitter scale {},{}, bypass {}",
             QualityModeName(g_config.mode), g_config.Sharpness(), g_config.autoExposure,
             g_config.depthInverted, g_config.mvScaleX, g_config.mvScaleY,
             g_config.jitterScaleX, g_config.jitterScaleY, g_config.bypass);

        for (auto& f : g_features) {
            f->evaluations = 0;
            f->resetNext = true;
        }
        g_probeRequested = g_config.debugProbe;
        if (g_config.debugDumpFrames > 0) {
            probe::RequestFrameDump(g_dataDir.c_str(), g_config.debugDumpFrames,
                                    static_cast<unsigned>(g_config.debugDumpDelay * 1000.0f));
        }
        if (game::DlssOptionOn()) recompute = true;
    }
    ReloadGameOptionsIfChanged();
}

bool InstallTables(const std::wstring& dataDir, const std::wstring& gameDir) {
    std::lock_guard lock(g_mutex);
    g_dataDir = dataDir;
    g_gameDir = gameDir;

    g_params.Set(NVSDK_NGX_Parameter_SuperSampling_Available, 1);
    g_params.Set("ImageSignalProcessing.Available", 1);
    g_params.Set(NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, reinterpret_cast<void*>(&OptimalSettingsCallback));

    HMODULE exe = GetModuleHandleW(nullptr);
    g_api[0].table = FindTable(exe, Api::D3D11);
    g_api[1].table = FindTable(exe, Api::D3D12);
    if (!g_api[0].table && !g_api[1].table) {
        LOGE("NGX function tables not found in game exe");
        return false;
    }

    g_api[0].shim = Entry<Api::D3D11>::Make();
    g_api[1].shim = Entry<Api::D3D12>::Make();
    PatchTable(g_api[0], Api::D3D11, true);
    PatchTable(g_api[1], Api::D3D12, true);

    AdoptContext(false);
    if (!game::Device()) HookNgxLookups(exe);
    game::SetFrameStartCallback(&OnFrameStart);
    game::SetFrameCallback(&OnFrame);
    game::SetUpdateCallback(&OnUpdate);
    LOGD("Renderer at MHW-FSR install: {}", game::DescribeState());
    return true;
}

}  // namespace

bool Install(const std::wstring& dataDir, const std::wstring& gameDir) {
    if (!InstallTables(dataDir, gameDir)) return false;
    if (game::Context() && game::DlssEnabledInGame() && game::RecomputeRenderScale()) {
        LOGD("FSR was already on, render scale refreshed");
    }
    return true;
}

}  // namespace mhwfsr::ngx
