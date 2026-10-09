#include "game.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "config.h"
#include "log.h"
#include "memory.h"

namespace mhwdlss::game {

namespace {

// Offsets inside game objects (MHW:I 15.2x, DX11 and DX12 builds share them).
constexpr ptrdiff_t kRendererDevice = 0x78;
constexpr ptrdiff_t kRendererDlssOn = 0xa5;
constexpr ptrdiff_t kRendererDlssBits = 0xb4;     // 1 = DLSS, 2 = DLISP
constexpr ptrdiff_t kRendererDlispOn = 0xa6;
constexpr ptrdiff_t kRendererScaleApplied = 0x204;
constexpr ptrdiff_t kRendererRenderFrame = 0x1f208;
constexpr ptrdiff_t kRendererFrame = 0x1f210;
constexpr ptrdiff_t kRendererView = 0x1f450;
constexpr ptrdiff_t kViewFlags = 0x58;
// View flag bits for which the TAA update and draw do nothing.
constexpr uint32_t kViewSkipsTaa = (1u << 0x12) | (1u << 0x17);
constexpr ptrdiff_t kDeviceDlssContext = 0x42700;
constexpr ptrdiff_t kCameraJitter = 0x38;
constexpr ptrdiff_t kCameraJitterFrame = 0x40;  // render frame the jitter was set in
constexpr ptrdiff_t kTaaHistory = 0x168;   // three texture objects, indexed by frame % 3
constexpr ptrdiff_t kTaaSharpen = 0x187;   // mSharpen: TAA sharpen pass; the history slot shifts by one
constexpr ptrdiff_t kTextureNative = 0x60;
constexpr ptrdiff_t kTextureDepthNative = 0xf0;
// Natives are reference counted; the game releases them at +0x2554f90 (lock xadd -1, destroyed
// when the count was 1).
constexpr ptrdiff_t kNativeRefCount = 0x8;
constexpr ptrdiff_t kNativeResource11 = 0x18;
constexpr ptrdiff_t kNativeResource12 = 0x20;
constexpr ptrdiff_t kDrawContextView = 0x1d0;
constexpr ptrdiff_t kViewTargets = 0x628;
constexpr ptrdiff_t kTargetsDepthCopy = 0xa0;

// uTemporalAAFilter2 per-frame update: picks the Halton jitter for the frame and hands it to
// the camera. It references the renderer, scene and camera singletons.
constexpr std::string_view kTaaUpdatePattern =
    "48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 8B 90 50 F4 01 00 8B 42 58 0F BA E0 12 72 ?? "
    "0F BA E0 17 72 ?? 0F 57 C0 0F 2E 81 80 01 00 00 74 ?? 83 79 0C 08 75 ?? 8B 41 14 "
    "83 E0 12 3C 12 75 ?? 80 B9 84 01 00 00 00 74 ?? 48 8B 0D ?? ?? ?? ?? 48 89 5C 24 20 "
    "48 8B 1D ?? ?? ?? ?? E8";
// uTemporalAAFilter2 draw: builds the TAA pass and, with DLSS on, the NGX evaluate command.
constexpr std::string_view kTaaDrawPrologue = "48 89 54 24 10 55 53 48 8D 6C 24 88 48 81 EC 78 01 00 00";
// Renderer method that re-queries DLSS optimal settings and updates the render scale.
constexpr std::string_view kRecomputePattern =
    "40 53 48 83 EC 40 48 8B 41 78 48 8B D9 48 85 C0 0F 84 ?? ?? ?? ?? 4C 8B 90 00 27 04 00 "
    "4D 85 D2 0F 84 ?? ?? ?? ?? 48 8B 81 48 F4 01 00 8B 89 40 B4 07 00";
// Slot distance in the vtable between update (+0x48) and draw (+0x60).
constexpr ptrdiff_t kDrawSlotFromUpdate = 3;
// Scene setter for the anti-aliasing switches (scene, mode 0..3): writes a word, TAA in its low
// byte and FXAA in its high byte. Only searched for the word's offset.
constexpr std::string_view kSceneAaSetterPattern =
    "44 0F B6 C2 84 D2 74 ?? 41 83 E8 01 74 ?? 41 83 E8 01 74 ?? 41 83 F8 01 75 ?? 66 C7 81 ?? ?? ?? ?? 01 01 C3";
constexpr ptrdiff_t kSceneAaSetterDisp = 29;
// Paired getters "is DX11" / "is DX12" reading the same global byte.
constexpr std::string_view kApiFlagPattern =
    "80 3D ?? ?? ?? ?? 00 0F 94 C0 C3 CC CC CC CC CC 0F B6 05 ?? ?? ?? ?? C3";
// operator new: forwards to the MT Framework allocator (vtable +0x48, 16-byte alignment).
constexpr std::string_view kOperatorNewPattern =
    "4C 8B 0D ?? ?? ?? ?? 4D 85 C9 74 ?? 49 8B 01 48 8B D1 41 B8 10 00 00 00 49 8B C9 48 FF 60 48";
constexpr ptrdiff_t kDeviceImmediateContext11 = 0x1400;

uint8_t** g_renderer = nullptr;
uint8_t** g_camera = nullptr;
uint8_t** g_scene = nullptr;
const uint8_t* g_apiFlag = nullptr;
// Halton(2,3) table the TAA update picks the jitter from (20 entries, float2).
constexpr ptrdiff_t kSceneHalton = 0xe6a0;
constexpr uint64_t kHaltonCount = 20;

using OperatorNewFn = void*(__fastcall*)(size_t size);
OperatorNewFn g_operatorNew = nullptr;

using TaaDrawFn = void*(__fastcall*)(void* self, void* drawContext);
TaaDrawFn g_origTaaDraw = nullptr;
using TaaUpdateFn = void*(__fastcall*)(void* self);
TaaUpdateFn g_origTaaUpdate = nullptr;
constexpr ptrdiff_t kTaaJitterEnable = 0x184;

using RecomputeFn = void(__fastcall*)(void* renderer);
RecomputeFn g_recompute = nullptr;

// Renderer frame of the last DLSS evaluation, plus one (0 = none yet).
std::atomic<uint64_t> g_lastDlssFrame{0};
// Since the last JitterStats() call.
std::atomic<uint64_t> g_jitterFrames{0};
std::atomic<uint64_t> g_jitterSetByPlugin{0};
std::atomic<uint64_t> g_sharpenSuppressed{0};

// The hooks run on several game threads at once (the TAA pass is built on worker threads, the
// update on the main thread), so they read their settings from here, published by ApplyConfig,
// instead of from g_config while an ini reload replaces it.
struct HookSettings {
    std::atomic<bool> bypass{false};
    std::atomic<bool> forceJitter{true};
    std::atomic<bool> fullPixelJitter{true};
    std::atomic<bool> verboseFrames{false};
    std::atomic<bool> debugFixedJitter{false};
    std::atomic<float> debugJitterX{0.0f};
    std::atomic<float> debugJitterY{0.0f};
};
HookSettings g_settings;
// Until the NGX side is installed the hooks only call the game's functions.
std::atomic<bool> g_active{false};
std::atomic<bool> g_dlssUnavailable{false};
void (*g_updateCallback)() = nullptr;

struct JitterRecord {
    const void* resource11 = nullptr;
    const void* resource12 = nullptr;
    Jitter jitter;
};
std::mutex g_jitterMutex;
std::array<JitterRecord, 8> g_jitterRecords;
size_t g_jitterNext = 0;

void CaptureJitter(uint8_t* taa) {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    uint8_t* camera = g_camera ? *g_camera : nullptr;
    if (!renderer || !camera) return;

    auto frame = mem::SafeRead<uint64_t>(renderer + kRendererFrame);
    auto jitter = mem::SafeRead<std::array<float, 2>>(camera + kCameraJitter);
    auto sharpen = mem::SafeRead<uint8_t>(taa + kTaaSharpen);
    if (!frame || !jitter || !sharpen) return;

    // Same slot choice as the game's DLSS evaluate: history[frame % 3], shifted by one
    // when the TAA sharpen pass is active.
    uint64_t slot = (*sharpen ? *frame + 1 : *frame) % 3;
    auto texture = mem::SafeRead<uint8_t*>(taa + kTaaHistory + slot * sizeof(void*));
    if (!texture || !*texture) return;
    auto native = mem::SafeRead<uint8_t*>(*texture + kTextureNative);
    if (!native || !*native) return;

    JitterRecord rec;
    rec.resource11 = mem::SafeRead<void*>(*native + kNativeResource11).value_or(nullptr);
    rec.resource12 = mem::SafeRead<void*>(*native + kNativeResource12).value_or(nullptr);
    rec.jitter = {(*jitter)[0], (*jitter)[1], *frame};

    static std::atomic<uint64_t> logged{0};
    uint64_t n = logged.load();
    if (n < 4 || (g_settings.verboseFrames && n < 40)) {
        ++logged;
        // Inputs of the TAA update's jitter decision, to explain a zero jitter.
        LOGI("jitter capture: frame {}, slot {}, jitter {:.4f},{:.4f}, camera frame {}, render frame {}, blend {:.3f}, "
             "type {}, flags {:#x}, enabled {}, view flags {:#x}",
             *frame, slot, (*jitter)[0], (*jitter)[1],
             mem::SafeRead<uint64_t>(camera + kCameraJitter + 8).value_or(0),
             mem::SafeRead<uint64_t>(renderer + 0x1f208).value_or(0),
             mem::SafeRead<float>(taa + 0x180).value_or(-1.0f), mem::SafeRead<uint32_t>(taa + 0xc).value_or(0),
             mem::SafeRead<uint32_t>(taa + 0x14).value_or(0), mem::SafeRead<uint8_t>(taa + 0x184).value_or(0),
             mem::SafeRead<uint32_t>(mem::SafeRead<uint8_t*>(renderer + 0x1f450).value_or(nullptr) + 0x58)
                 .value_or(0));
    }

    std::lock_guard lock(g_jitterMutex);
    g_jitterRecords[g_jitterNext] = rec;
    g_jitterNext = (g_jitterNext + 1) % g_jitterRecords.size();
}

void (*g_frameCallback)() = nullptr;

// The DLSS evaluate command takes the view's depth from its R32F depth copy, a plain texture.
// On DX12 the command's barrier setup reads the texture's depth-view native (+0xf0), which only
// depth formats have, and crashes on null. The game's own copy command falls back to the colour
// native (+0x60) in that case; give the depth copy the same fallback before the pass is built.
//
// When the texture is rebuilt (ini reloads, graphics option changes, area changes) the game
// releases +0x60 and +0xf0 separately, so the alias takes a reference of its own. Without it the
// first release destroyed the native and the second decremented freed memory, or the count of
// whatever object had been allocated there since, destroying that one while still in use (seen
// as exploded geometry until the game restarted).
void PatchViewDepth(uint8_t* drawContext) {
    auto view = mem::SafeRead<uint8_t*>(drawContext + kDrawContextView);
    if (!view || !*view) return;
    auto targets = mem::SafeRead<uint8_t*>(*view + kViewTargets);
    if (!targets || !*targets) return;
    auto depth = mem::SafeRead<uint8_t*>(*targets + kTargetsDepthCopy);
    if (!depth || !*depth) return;
    auto** depthNative = reinterpret_cast<void**>(*depth + kTextureDepthNative);
    auto colorNative = mem::SafeRead<void*>(*depth + kTextureNative);
    auto current = mem::SafeRead<void*>(depthNative);
    if (!colorNative || !*colorNative || !current || *current) return;
    auto* native = static_cast<uint8_t*>(*colorNative);
    auto count = mem::SafeRead<LONG>(native + kNativeRefCount);
    if (!count || *count <= 0) return;  // not a live native
    // The pass can be built on two threads at once; only the thread that installs the alias keeps
    // the reference it took.
    auto* refs = reinterpret_cast<volatile LONG*>(native + kNativeRefCount);
    LONG references = InterlockedIncrement(refs);
    if (InterlockedCompareExchangePointer(depthNative, *colorNative, nullptr) != nullptr) {
        InterlockedDecrement(refs);
        return;
    }
    LOGI("game: depth copy texture {} given its native {} as depth view (references {})", static_cast<void*>(*depth),
         *colorNative, references);
}

// The Halton entry the TAA update gives the camera this frame (scene + 0xe6a0, frame % 20).
const float* HaltonForFrame(uint8_t* renderer, uint8_t* scene) {
    uint64_t frame = *reinterpret_cast<uint64_t*>(renderer + kRendererFrame);
    return reinterpret_cast<const float*>(scene + kSceneHalton + (frame % kHaltonCount) * 8);
}

// Besides the enable switch, the TAA update only jitters when the scene's TAA settings say so
// (blend rate, filter type and flags). DLSS needs the jitter whenever it runs, so when the
// update left the camera alone and DLSS ran in the last frames, the jitter is set here the same
// way the update does it. Returns false when the camera keeps its value.
bool EnsureJitter(uint8_t* taa) {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    uint8_t* camera = g_camera ? *g_camera : nullptr;
    uint8_t* scene = g_scene ? *g_scene : nullptr;
    if (!renderer || !camera || !scene) return false;
    const float* halton = HaltonForFrame(renderer, scene);
    auto* jitter = reinterpret_cast<float*>(camera + kCameraJitter);
    if (jitter[0] == halton[0] && jitter[1] == halton[1]) return true;  // set by the update

    uint64_t frame = *reinterpret_cast<uint64_t*>(renderer + kRendererFrame);
    uint64_t last = g_lastDlssFrame.load();
    if (!last || frame + 1 > last + 4) return false;  // DLSS is not running
    auto view = mem::SafeRead<uint8_t*>(renderer + kRendererView);
    if (!view || !*view) return false;
    auto flags = mem::SafeRead<uint32_t>(*view + kViewFlags);
    if (!flags || (*flags & kViewSkipsTaa)) return false;

    jitter[0] = halton[0];
    jitter[1] = halton[1];
    *reinterpret_cast<uint32_t*>(camera + kCameraJitterFrame) =
        *reinterpret_cast<uint32_t*>(renderer + kRendererRenderFrame);
    ++g_jitterSetByPlugin;
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOGI("jitter: the TAA update did not jitter the camera (blend {:.3f}, type {}, flags {:#x}, enabled {}); "
             "the plugin does while DLSS runs",
             mem::SafeRead<float>(taa + 0x180).value_or(-1.0f), mem::SafeRead<uint32_t>(taa + 0xc).value_or(0),
             mem::SafeRead<uint32_t>(taa + 0x14).value_or(0), mem::SafeRead<uint8_t>(taa + kTaaJitterEnable).value_or(0));
    }
    return true;
}

// The game turns the Halton value h into a clip-space offset of h / output size, which moves the
// image by h / 2 * render scale render pixels: its jitter covers less than half a pixel. With
// DLSS the camera value is scaled by 2 / render scale right after the TAA update sets it, so the
// samples spread over a whole render pixel.
void WidenJitter() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    uint8_t* camera = g_camera ? *g_camera : nullptr;
    uint8_t* scene = g_scene ? *g_scene : nullptr;
    if (!renderer || !camera || !scene) return;
    const float* halton = HaltonForFrame(renderer, scene);
    auto* jitter = reinterpret_cast<float*>(camera + kCameraJitter);
    // Only a value the update has just written; a second filter instance may not rewrite it.
    if (jitter[0] != halton[0] || jitter[1] != halton[1]) return;
    float factor = 2.0f / RenderScale();
    jitter[0] *= factor;
    jitter[1] *= factor;
}

// The scene's anti-aliasing switches, TAA in the low byte and FXAA in the high byte of a word at
// scene + g_sceneAa. The game sets them from the Anti-Aliasing option when it applies graphics
// options (+0x43dcb0), and the TAA and FXAA filters follow them every frame. With DLSS on, that
// apply sets TAA only for render scales below 1, but at 1 (DLAA, which the stock game never had)
// it keeps the option, which the game greys out while DLSS is on: FXAA then ran over the DLSS
// output, and Off or FXAA turned the TAA filter off, and with it DLSS, which runs inside it. So
// while DLSS runs the switches are held at TAA only.
constexpr uint16_t kAaTaaOnly = 0x0001;
ptrdiff_t g_sceneAa = 0;
std::mutex g_aaMutex;
uint8_t* g_aaScene = nullptr;
bool g_aaHeld = false;
uint16_t g_aaGame = 0;  // the game's own setting, last seen
bool g_aaOurs = false;  // the word still holds what the plugin wrote (the game has not set it since)

// Bypass shows the game's TAA instead of DLSS, in DLAA only: the upscaling modes need the TAA
// pass and the jitter for DLSS at the DLISP position either way.
bool Bypassed() { return g_settings.bypass && !DlispActive(); }

std::string AaName(uint16_t value) {
    switch (value) {
        case 0x0000: return "Off";
        case 0x0001: return "TAA";
        case 0x0100: return "FXAA";
        case 0x0101: return "TAA+FXAA";
        default: return std::format("{:#06x}", value);
    }
}

void HoldTaaOnly() {
    uint8_t* scene = g_scene ? *g_scene : nullptr;
    if (!scene || !g_sceneAa) return;
    std::lock_guard lock(g_aaMutex);
    if (scene != g_aaScene) {
        g_aaScene = scene;
        g_aaHeld = false;
        g_aaOurs = false;
    }
    auto* word = reinterpret_cast<volatile uint16_t*>(scene + g_sceneAa);
    uint16_t current = *word;
    if (current != kAaTaaOnly) g_aaOurs = false;  // the game has set it since
    bool dlss = DlssEnabledInGame();
    bool bypassed = Bypassed();
    bool unavailable = g_dlssUnavailable;
    if (dlss && !bypassed && !unavailable) {
        // Any other value was set by the game (at start, or when it applied the options again).
        if (current != kAaTaaOnly) {
            if (!g_aaHeld || current != g_aaGame)
                LOGI("anti-aliasing: the game set {}; TAA only while DLSS runs", AaName(current));
            g_aaGame = current;
            *word = kAaTaaOnly;
            g_aaOurs = true;
        } else if (!g_aaHeld && !g_aaOurs) {
            g_aaGame = current;  // TAA only is the game's own setting
        }
        g_aaHeld = true;
    } else if (g_aaHeld) {
        g_aaHeld = false;
        // Turning DLSS off makes the game apply the option again (after clearing the DLSS state
        // checked above), so only a hold the plugin ends itself has a setting to give back.
        if (dlss && g_aaOurs && g_aaGame != kAaTaaOnly) {
            *word = g_aaGame;
            g_aaOurs = false;
            LOGI("anti-aliasing: back to the game's {} ({})", AaName(g_aaGame),
                 unavailable ? "DLSS is not available" : "Bypass");
        }
    }
}

// The pass can be built on two threads at once for the same filter, so the TAA sharpen flag is
// cleared by the first build that starts and restored by the last one that ends.
struct SharpenHold {
    int builds = 0;
    uint8_t saved = 0;
    bool cleared = false;
};
std::mutex g_sharpenMutex;
std::unordered_map<uint8_t*, SharpenHold> g_sharpenHolds;

class SharpenGuard {
public:
    SharpenGuard(uint8_t* taa, bool suppress) : taa_(taa) {
        std::lock_guard lock(g_sharpenMutex);
        SharpenHold& h = g_sharpenHolds[taa];
        if (h.builds++ > 0) return;
        h.saved = taa[kTaaSharpen];
        h.cleared = suppress && h.saved;
        if (!h.cleared) return;
        taa[kTaaSharpen] = 0;
        ++g_sharpenSuppressed;
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGI("game: TAA sharpen pass (amount {:.3f}) turned off while DLSS runs",
                 mem::SafeRead<float>(taa + 0x198).value_or(-1.0f));
        }
    }
    ~SharpenGuard() {
        std::lock_guard lock(g_sharpenMutex);
        auto it = g_sharpenHolds.find(taa_);
        if (it == g_sharpenHolds.end() || --it->second.builds > 0) return;
        if (it->second.cleared) taa_[kTaaSharpen] = it->second.saved;
        g_sharpenHolds.erase(it);
    }
    SharpenGuard(const SharpenGuard&) = delete;
    SharpenGuard& operator=(const SharpenGuard&) = delete;

private:
    uint8_t* taa_;
};

// Scene parameters may turn the TAA jitter off (the title screen does). DLSS needs the
// sub-pixel jitter, so it is switched on for the duration of the update that applies it.
// The update runs every frame, also while the TAA filter is switched off.
void* __fastcall HookTaaUpdate(void* self) {
    if (!g_active) return g_origTaaUpdate(self);
    if (g_updateCallback) g_updateCallback();
    HoldTaaOnly();
    if (!DlssEnabledInGame() || Bypassed()) return g_origTaaUpdate(self);
    auto* taa = static_cast<uint8_t*>(self);
    auto* enable = taa + kTaaJitterEnable;
    uint8_t saved = *enable;
    bool forceJitter = g_settings.forceJitter;
    if (forceJitter) *enable = 1;
    void* result = g_origTaaUpdate(self);
    *enable = saved;
    ++g_jitterFrames;
    if (g_settings.debugFixedJitter) {
        // Constant jitter for checking the sign convention (DLSS guide, section 8.4.2).
        if (uint8_t* camera = g_camera ? *g_camera : nullptr) {
            auto* jitter = reinterpret_cast<float*>(camera + kCameraJitter);
            jitter[0] = g_settings.debugJitterX;
            jitter[1] = g_settings.debugJitterY;
        }
        return result;
    }
    if (forceJitter) EnsureJitter(taa);
    if (g_settings.fullPixelJitter) WidenJitter();
    return result;
}

void* __fastcall HookTaaDraw(void* self, void* drawContext) {
    if (!g_active) return g_origTaaDraw(self, drawContext);
    static std::atomic<uint64_t> calls{0};
    if (calls.fetch_add(1) == 0) LOGI("game: first TAA draw (filter {})", self);
    if (g_frameCallback) g_frameCallback();
    auto* taa = static_cast<uint8_t*>(self);
    bool dlss = DlssEnabledInGame();
    // With the TAA sharpen pass on, the game points DLSS at the next history slot and then has the
    // sharpen pass write the sharpened TAA result over it. The pass stays off while DLSS runs.
    SharpenGuard sharpen(taa, dlss && !Bypassed());
    if (dlss) PatchViewDepth(static_cast<uint8_t*>(drawContext));
    CaptureJitter(taa);
    return g_origTaaDraw(self, drawContext);
}

uint8_t* ExeBase() { return reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)); }

uintptr_t Rva(const void* p) { return static_cast<uintptr_t>(static_cast<const uint8_t*>(p) - ExeBase()); }

}  // namespace

bool Init() {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto text = mem::FindSection(exe, ".text");
    auto rdata = mem::FindSection(exe, ".rdata");
    if (!text || !rdata) {
        LOGE("game: executable sections not found");
        return false;
    }

    auto updates = mem::FindPattern(*text, kTaaUpdatePattern);
    if (updates.size() != 1) {
        LOGE("game: TAA update function matched {} times (expected 1); unsupported game version?",
             updates.size());
        return false;
    }
    uint8_t* update = updates[0];
    g_renderer = reinterpret_cast<uint8_t**>(mem::RipTarget(update + 0x07, update + 0x0b));
    g_scene = reinterpret_cast<uint8_t**>(mem::RipTarget(update + 0x49, update + 0x4d));
    g_camera = reinterpret_cast<uint8_t**>(mem::RipTarget(update + 0x55, update + 0x59));
    LOGI("game: TAA update at +{:#x}, renderer global +{:#x}, scene global +{:#x}, camera global +{:#x}",
         Rva(update), Rva(g_renderer), Rva(g_scene), Rva(g_camera));

    // The TAA vtable is the only place in .rdata that points at the update method.
    void** updateSlot = nullptr;
    for (auto* p = reinterpret_cast<void**>(rdata->begin); p + 1 <= reinterpret_cast<void**>(rdata->end); ++p) {
        if (*p == update) {
            if (updateSlot) {
                LOGE("game: TAA update method referenced from more than one vtable");
                return false;
            }
            updateSlot = p;
        }
    }
    if (!updateSlot) {
        LOGE("game: TAA vtable not found");
        return false;
    }
    void** drawSlot = updateSlot + kDrawSlotFromUpdate;
    auto drawCheck = mem::FindPattern(
        mem::Section{static_cast<uint8_t*>(*drawSlot), static_cast<uint8_t*>(*drawSlot) + 0x20}, kTaaDrawPrologue);
    if (drawCheck.size() != 1 || drawCheck[0] != *drawSlot) {
        LOGE("game: TAA draw slot +{:#x} does not point at the expected function", Rva(drawSlot));
        return false;
    }
    g_origTaaDraw = reinterpret_cast<TaaDrawFn>(*drawSlot);
    mem::WritePointer(drawSlot, reinterpret_cast<void*>(&HookTaaDraw));
    LOGI("game: TAA draw +{:#x} hooked through vtable slot +{:#x}", Rva(g_origTaaDraw), Rva(drawSlot));
    g_origTaaUpdate = reinterpret_cast<TaaUpdateFn>(*updateSlot);
    mem::WritePointer(updateSlot, reinterpret_cast<void*>(&HookTaaUpdate));

    auto recompute = mem::FindPattern(*text, kRecomputePattern);
    if (recompute.size() == 1) {
        g_recompute = reinterpret_cast<RecomputeFn>(recompute[0]);
        LOGI("game: render scale update at +{:#x}", Rva(recompute[0]));
    } else {
        LOGW("game: render scale update matched {} times; scale changes need a settings toggle",
             recompute.size());
    }

    auto aaSetter = mem::FindPattern(*text, kSceneAaSetterPattern);
    if (aaSetter.size() == 1) {
        int32_t offset;
        std::memcpy(&offset, aaSetter[0] + kSceneAaSetterDisp, sizeof(offset));
        g_sceneAa = offset;
        LOGI("game: anti-aliasing switches at scene +{:#x} (setter +{:#x})", offset, Rva(aaSetter[0]));
    } else {
        LOGW("game: anti-aliasing setter matched {} times; the Anti-Aliasing option stays in effect with DLSS",
             aaSetter.size());
    }

    auto apiFlag = mem::FindPattern(*text, kApiFlagPattern);
    if (apiFlag.size() == 1) {
        g_apiFlag = mem::RipTarget(apiFlag[0] + 0x13, apiFlag[0] + 0x17);
        // Set when the renderer starts, which may be after the plugin is loaded.
        if (Device())
            LOGI("game: renderer API flag at +{:#x} (DX12 = {})", Rva(g_apiFlag), *g_apiFlag != 0);
        else
            LOGI("game: renderer API flag at +{:#x} (renderer not started yet)", Rva(g_apiFlag));
    } else {
        LOGW("game: renderer API flag matched {} times", apiFlag.size());
    }

    auto opNew = mem::FindPattern(*text, kOperatorNewPattern);
    if (opNew.size() == 1) {
        g_operatorNew = reinterpret_cast<OperatorNewFn>(opNew[0]);
    } else {
        LOGW("game: operator new matched {} times", opNew.size());
    }
    return true;
}

std::optional<bool> IsDx12() {
    if (!g_apiFlag) return std::nullopt;
    return *g_apiFlag != 0;
}

void* ImmediateContext11() {
    uint8_t* device = Device();
    if (!device) return nullptr;
    return mem::SafeRead<void*>(device + kDeviceImmediateContext11).value_or(nullptr);
}

void* GameAlloc(size_t size) { return g_operatorNew ? g_operatorNew(size) : nullptr; }

void SetFrameCallback(void (*callback)()) { g_frameCallback = callback; }

void SetUpdateCallback(void (*callback)()) { g_updateCallback = callback; }

void ApplyConfig(const Config& config) {
    g_settings.bypass = config.bypass;
    g_settings.forceJitter = config.forceJitter;
    g_settings.fullPixelJitter = config.fullPixelJitter;
    g_settings.verboseFrames = config.verboseFrames;
    g_settings.debugFixedJitter = config.debugFixedJitter;
    g_settings.debugJitterX = config.debugJitterX;
    g_settings.debugJitterY = config.debugJitterY;
}

void Activate(const Config& config) {
    ApplyConfig(config);
    g_active = true;
}

uint64_t Frame() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    return renderer ? mem::SafeRead<uint64_t>(renderer + kRendererFrame).value_or(0) : 0;
}

void SetDlssUnavailable(bool unavailable) { g_dlssUnavailable = unavailable; }

uint8_t* Device() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer) return nullptr;
    return mem::SafeRead<uint8_t*>(renderer + kRendererDevice).value_or(nullptr);
}

DlssContext* Context() {
    uint8_t* device = Device();
    if (!device) return nullptr;
    return mem::SafeRead<DlssContext*>(device + kDeviceDlssContext).value_or(nullptr);
}

void SetContext(DlssContext* ctx) {
    if (uint8_t* device = Device()) *reinterpret_cast<DlssContext**>(device + kDeviceDlssContext) = ctx;
}

bool RecomputeRenderScale() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer || !g_recompute) return false;
    g_recompute(renderer);
    return true;
}

std::string DescribeState() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer) return "renderer not created";
    auto u8 = [&](ptrdiff_t off) { return mem::SafeRead<uint8_t>(renderer + off).value_or(0); };
    auto u32 = [&](ptrdiff_t off) { return mem::SafeRead<uint32_t>(renderer + off).value_or(0); };
    auto f32 = [&](ptrdiff_t off) { return mem::SafeRead<float>(renderer + off).value_or(0.0f); };
    DlssContext* ctx = Context();
    return std::format(
        "DLSS on {}, DLISP on {}, mode {}, DLSS scale {:.3f}, feature bits {}, render scale {:.3f}/{:.3f}, "
        "context {}",
        u8(kRendererDlssOn), u8(0xa6), u32(0xa8), f32(0xb0), u32(kRendererDlssBits), f32(0x200), f32(0x204),
        static_cast<void*>(ctx));
}

float RenderScale() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    float scale = renderer ? mem::SafeRead<float>(renderer + kRendererScaleApplied).value_or(1.0f) : 1.0f;
    return std::clamp(scale, 0.25f, 1.0f);
}

bool DlispActive() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer || !DlssEnabledInGame()) return false;
    auto on = mem::SafeRead<uint8_t>(renderer + kRendererDlispOn).value_or(0);
    auto bits = mem::SafeRead<uint32_t>(renderer + kRendererDlssBits).value_or(0);
    auto scale = mem::SafeRead<float>(renderer + kRendererScaleApplied).value_or(1.0f);
    // Same test as the game's DLISP pass.
    return on && (bits & 2) && scale != 1.0f;
}

bool DlssOptionOn() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    return renderer && mem::SafeRead<uint8_t>(renderer + kRendererDlssOn).value_or(0) != 0;
}

bool DlssEnabledInGame() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer) return false;
    auto on = mem::SafeRead<uint8_t>(renderer + kRendererDlssOn).value_or(0);
    auto bits = mem::SafeRead<uint32_t>(renderer + kRendererDlssBits).value_or(0);
    return on && (bits & 1);
}

void NoteDlssEvaluated() {
    uint8_t* renderer = g_renderer ? *g_renderer : nullptr;
    if (!renderer) return;
    g_lastDlssFrame = mem::SafeRead<uint64_t>(renderer + kRendererFrame).value_or(0) + 1;
}

std::string JitterStats() {
    uint64_t frames = g_jitterFrames.exchange(0);
    uint64_t byPlugin = g_jitterSetByPlugin.exchange(0);
    uint64_t sharpen = g_sharpenSuppressed.exchange(0);
    return std::format("camera jittered by the plugin in {} of {} frames, TAA sharpen suppressed in {}", byPlugin,
                       frames, sharpen);
}

std::optional<Jitter> JitterForOutput(const void* outputResource) {
    std::lock_guard lock(g_jitterMutex);
    const JitterRecord* best = nullptr;
    for (const auto& rec : g_jitterRecords) {
        if (rec.resource11 != outputResource && rec.resource12 != outputResource) continue;
        if (!best || rec.jitter.frame > best->jitter.frame) best = &rec;
    }
    if (!best) return std::nullopt;
    return best->jitter;
}

}  // namespace mhwdlss::game
