#include "fsr_loader.h"
#include "log.h"

#include <windows.h>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cstring>

#include <ffx_api.hpp>
#include <ffx_upscale.hpp>
#include <ffx_framegeneration.hpp>
#include <dx12/ffx_api_dx12.hpp>

namespace fs = std::filesystem;

namespace mhwfsr::loader {

namespace {
HMODULE g_loaderDll = nullptr;
HMODULE g_driverFfxDll = nullptr;
fs::path g_targetDir;
bool g_loadAttempted = false;
bool g_driverLoadAttempted = false;
bool g_queried = false;
std::vector<FsrVersionInfo> g_cachedVersions;

// The 4 internal FSR 4.1.2 model singletons in amdxcffx64.dll:
// [0] = FSR4 (FP8), [1] = FSR4-i8 (Standard RDNA 3), [2] = FSR4-i8-2 (RDNA 2), [3] = FSR4L-i8 (Lite)
bool g_singletonsDiscovered = false;
std::array<void*, 4> g_fsr4Singletons{nullptr, nullptr, nullptr, nullptr};
std::array<void*, 4> g_origCanCreate{nullptr, nullptr, nullptr, nullptr};

bool __fastcall CanCreateFalse(void*, void*) { return false; }
bool __fastcall CanCreateTrue(void*, void*) { return true; }

bool DiscoverFsr4Singletons(HMODULE hFfx) {
    if (g_singletonsDiscovered) return true;
    if (!hFfx) return false;

    auto* base = reinterpret_cast<uint8_t*>(hFfx);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (std::strncmp(reinterpret_cast<const char*>(sec->Name), ".text", 5) != 0) continue;
        uint8_t* begin = base + sec->VirtualAddress;
        size_t size = sec->Misc.VirtualSize;
        if (size < 40) continue;

        for (size_t off = 0; off + 40 <= size; ++off) {
            uint8_t* p = begin + off;
            if (p[0]  == 0xE8 && p[5]  == 0x48 && p[6]  == 0x89 && p[7]  == 0x44 && p[8]  == 0x24 && p[9]  == 0x20 &&
                p[10] == 0xE8 && p[15] == 0x48 && p[16] == 0x89 && p[17] == 0x44 && p[18] == 0x24 && p[19] == 0x28 &&
                p[20] == 0xE8 && p[25] == 0x48 && p[26] == 0x89 && p[27] == 0x44 && p[28] == 0x24 && p[29] == 0x30 &&
                p[30] == 0xE8 && p[35] == 0x48 && p[36] == 0x89 && p[37] == 0x44 && p[38] == 0x24 && p[39] == 0x38) {
                for (int i = 0; i < 4; ++i) {
                    uint8_t* callSite = p + i * 10;
                    int32_t rel = 0;
                    std::memcpy(&rel, callSite + 1, sizeof(rel));
                    auto getSingleton = reinterpret_cast<void* (__fastcall*)()>(callSite + 5 + rel);
                    void* singleton = getSingleton();
                    if (!singleton) return false;
                    g_fsr4Singletons[i] = singleton;
                    void** vtbl = *reinterpret_cast<void***>(singleton);
                    g_origCanCreate[i] = vtbl[2];
                }
                g_singletonsDiscovered = true;
                LOGI("Discovered 4 internal FSR 4.1.2 neural models in amdxcffx64.dll (FP8, FSR4-i8, FSR4-i8-2, FSR4L-i8)");
                return true;
            }
        }
    }
    return false;
}

// Redirects UpdateFfxApiProviderEx -> UpdateFfxApiProvider in amdxcffx64.dll's in-memory EAT so the
// driver's FSR 4.1.2 provider (including RDNA 2 / RX 6000 INT8 models) is always unlocked regardless
// of the global Adrenalin profile toggle.
bool UnlockDriverFfxProvider(HMODULE hFfx) {
    if (!hFfx) return false;
    auto* base = reinterpret_cast<uint8_t*>(hFfx);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto& expEntry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!expEntry.VirtualAddress || !expEntry.Size) return false;

    auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + expEntry.VirtualAddress);
    auto* names = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
    auto* funcs = reinterpret_cast<DWORD*>(base + exp->AddressOfFunctions);
    auto* ords = reinterpret_cast<WORD*>(base + exp->AddressOfNameOrdinals);

    DWORD rvaProvider = 0;
    DWORD* pRvaProviderEx = nullptr;
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        WORD ord = ords[i];
        if (ord >= exp->NumberOfFunctions) continue;
        if (std::strcmp(name, "UpdateFfxApiProvider") == 0) {
            rvaProvider = funcs[ord];
        } else if (std::strcmp(name, "UpdateFfxApiProviderEx") == 0) {
            pRvaProviderEx = &funcs[ord];
        }
    }

    if (rvaProvider && pRvaProviderEx && *pRvaProviderEx != rvaProvider) {
        DWORD oldProt = 0;
        if (VirtualProtect(pRvaProviderEx, sizeof(DWORD), PAGE_READWRITE, &oldProt)) {
            *pRvaProviderEx = rvaProvider;
            VirtualProtect(pRvaProviderEx, sizeof(DWORD), oldProt, &oldProt);
            return true;
        }
    }
    return rvaProvider != 0 && pRvaProviderEx != nullptr;
}

void EnsureDriverProviderLoaded() {
    if (g_driverLoadAttempted) return;
    g_driverLoadAttempted = true;

    std::error_code ec;
    fs::path candidate;

    // 1. Check plugin's MHW-FSR folder first (allows manual drop-in override)
    if (!g_targetDir.empty()) {
        fs::path localPath = g_targetDir / L"amdxcffx64.dll";
        if (fs::exists(localPath, ec)) {
            candidate = localPath;
        }
    }

    // 2. Otherwise locate amdxcffx64.dll alongside the active AMD DX12 driver (amdxc64.dll) in DriverStore
    if (candidate.empty()) {
        if (HMODULE hAmdxc = GetModuleHandleW(L"amdxc64.dll")) {
            wchar_t amdxcPath[MAX_PATH]{};
            if (GetModuleFileNameW(hAmdxc, amdxcPath, MAX_PATH) > 0) {
                fs::path driverCandidate = fs::path(amdxcPath).parent_path() / L"amdxcffx64.dll";
                if (fs::exists(driverCandidate, ec)) {
                    candidate = driverCandidate;
                }
            }
        }
    }

    if (candidate.empty()) return;

    g_driverFfxDll = LoadLibraryW(candidate.wstring().c_str());
    if (g_driverFfxDll) {
        bool unlocked = UnlockDriverFfxProvider(g_driverFfxDll);
        DiscoverFsr4Singletons(g_driverFfxDll);
        LOGI("Pinned AMD driver FidelityFX provider: {} (provider unlock: {})",
             candidate.string(), unlocked ? "active" : "default");
    } else {
        LOGW("Failed to pin {} (error {})", candidate.string(), GetLastError());
    }
}

void EnsureLoaded() {
    if (g_loadAttempted) return;
    g_loadAttempted = true;
    if (g_targetDir.empty()) return;

    SetDllDirectoryW(g_targetDir.wstring().c_str());

    std::error_code ec;
    for (const wchar_t* dllName : {
             L"amd_fidelityfx_loader_dx12.dll",
             L"amd_fidelityfx_upscaler_dx12.dll"
         }) {
        fs::path dllPath = g_targetDir / dllName;
        if (fs::exists(dllPath, ec)) {
            HMODULE mod = LoadLibraryW(dllPath.wstring().c_str());
            if (mod) {
                if (wcscmp(dllName, L"amd_fidelityfx_loader_dx12.dll") == 0) {
                    g_loaderDll = mod;
                }
                LOGI("Loaded {}", dllPath.string());
            } else {
                LOGW("Failed to load {} (error {})", dllPath.string(), GetLastError());
            }
        }
    }
}

// Extracts the major semantic version digit from a provider name (e.g. "4.1.2 *" -> 4, "FSR 3.1.5" -> 3, "2.3.4" -> 2).
int GetMajorVersion(std::string_view name) {
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (std::isdigit(c)) {
            int val = 0;
            while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) {
                val = val * 10 + (name[i] - '0');
                ++i;
            }
            return val;
        }
    }
    return 0;
}
}  // namespace

std::string FormatVersionName(std::string_view rawName) {
    while (!rawName.empty() &&
           (rawName.back() == ' ' || rawName.back() == '*' || rawName.back() == '\t' || rawName.back() == '\r' || rawName.back() == '\n')) {
        rawName.remove_suffix(1);
    }
    while (!rawName.empty() && (rawName.front() == ' ' || rawName.front() == '\t')) {
        rawName.remove_prefix(1);
    }
    if (rawName.empty()) return "FSR";
    std::string cleaned(rawName);
    if (cleaned.find("FSR") == std::string::npos && cleaned.find("fsr") == std::string::npos) {
        return "FSR " + cleaned;
    }
    return cleaned;
}

bool Init(const std::wstring& pluginDir, const std::wstring& customDllPath) {
    fs::path targetDir;
    if (!customDllPath.empty()) {
        targetDir = customDllPath;
    } else {
        targetDir = fs::path(pluginDir) / L"MHW-FSR";
    }

    std::error_code ec;
    if (fs::exists(targetDir, ec)) {
        LOGI("FSR loader directory: {}", targetDir.string());
        g_targetDir = targetDir;
        EnsureLoaded();
    } else {
        LOGW("FSR loader directory not found: {}", targetDir.string());
    }
    return true;
}

std::vector<FsrVersionInfo> QueryVersions(ID3D12Device* device) {
    if (g_queried) return g_cachedVersions;
    if (!device) return {};

    EnsureLoaded();
    EnsureDriverProviderLoaded();

    uint64_t count = 0;
    ffx::QueryDescGetVersions q{};
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = device;
    q.outputCount = &count;

    ffx::ReturnCode rc = ffx::Query(q);
    if (!rc || count == 0) {
        LOGD("FSR version query returned {} versions (code {})", count, static_cast<uint32_t>(rc));
        return {};
    }

    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds = ids.data();
    q.versionNames = names.data();

    rc = ffx::Query(q);
    if (!rc) {
        LOGW("FSR version query failed on second call (code {})", static_cast<uint32_t>(rc));
        return {};
    }

    g_cachedVersions.clear();
    for (size_t i = 0; i < count; ++i) {
        std::string name = names[i] ? names[i] : "Unknown";
        g_cachedVersions.push_back({ ids[i], name });
        LOGI("Available FSR upscaler [{}/{}]: {} (id {:#x})", i + 1, count, FormatVersionName(name), ids[i]);
    }
    g_queried = true;
    return g_cachedVersions;
}

bool HasFsr4Support(ID3D12Device* device) {
    if (!device) return false;
    auto versions = QueryVersions(device);
    for (const auto& v : versions) {
        if (GetMajorVersion(v.name) == 4) return true;
    }
    return false;
}

bool HasFsr412DriverSupport(ID3D12Device* device) {
    if (!device) return false;
    auto versions = QueryVersions(device);
    for (const auto& v : versions) {
        if (v.name.find("4.1.2") != std::string::npos || v.name.find('*') != std::string::npos) {
            return true;
        }
    }
    return false;
}

void ApplyFsr4ModelOverride(Fsr4Model model) {
    EnsureDriverProviderLoaded();
    if (!g_singletonsDiscovered) return;

    for (int i = 0; i < 4; ++i) {
        if (!g_fsr4Singletons[i]) continue;
        void** vtbl = *reinterpret_cast<void***>(g_fsr4Singletons[i]);
        void* desired = g_origCanCreate[i];
        switch (model) {
            case Fsr4Model::Auto:
                desired = g_origCanCreate[i];
                break;
            case Fsr4Model::Standard: // Index 1: FSR4-i8 (WMMA INT8)
                if (i < 1) desired = reinterpret_cast<void*>(&CanCreateFalse);
                else if (i == 1) desired = reinterpret_cast<void*>(&CanCreateTrue);
                else desired = g_origCanCreate[i];
                break;
            case Fsr4Model::Rdna2:    // Index 2: FSR4-i8-2 (DP4A INT8)
                if (i < 2) desired = reinterpret_cast<void*>(&CanCreateFalse);
                else if (i == 2) desired = reinterpret_cast<void*>(&CanCreateTrue);
                else desired = g_origCanCreate[i];
                break;
            case Fsr4Model::Lite:     // Index 3: FSR4L-i8 (Lite INT8)
                if (i < 3) desired = reinterpret_cast<void*>(&CanCreateFalse);
                else desired = reinterpret_cast<void*>(&CanCreateTrue);
                break;
        }
        if (vtbl[2] != desired) {
            DWORD oldProt = 0;
            if (VirtualProtect(&vtbl[2], sizeof(void*), PAGE_READWRITE, &oldProt)) {
                vtbl[2] = desired;
                VirtualProtect(&vtbl[2], sizeof(void*), oldProt, &oldProt);
            }
        }
    }
}

std::string DescribeActiveFsr4Model(const void* ffxContext) {
    if (!ffxContext || !g_singletonsDiscovered) return {};
    static constexpr const char* kModelNames[4] = {"FP8", "INT8", "RDNA2", "Lite"};
    auto* const* words = reinterpret_cast<void* const*>(ffxContext);
    for (int i = 0; i < 6; ++i) {
        uintptr_t ptr = reinterpret_cast<uintptr_t>(words[i]);
        if (ptr > 0x10000 && !IsBadReadPtr(words[i], 24)) {
            auto* const* sub = reinterpret_cast<void* const*>(words[i]);
            for (int k = 0; k < 4; ++k) {
                if (g_fsr4Singletons[k] && sub[1] == g_fsr4Singletons[k]) {
                    return kModelNames[k];
                }
            }
        }
    }
    return {};
}

bool HasFrameGenSupport(ID3D12Device* device) {
    if (!device) return false;
    EnsureLoaded();
    EnsureDriverProviderLoaded();
    uint64_t count = 0;
    ffx::QueryDescGetVersions q{};
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    q.device = device;
    q.outputCount = &count;

    ffx::ReturnCode rc = ffx::Query(q);
    return (static_cast<uint32_t>(rc) == FFX_API_RETURN_OK && count > 0);
}

uint64_t ResolveVersionId(ID3D12Device* device, const std::string& versionStr) {
    if (!device) return 0;
    auto versions = QueryVersions(device);
    if (versions.empty()) return 0;

    static std::string s_lastLoggedVersionStr;
    static uint64_t s_lastLoggedId = 0;
    static bool s_hasLogged = false;

    auto LogOverride = [&](const std::string& name, uint64_t id) {
        if (!s_hasLogged || s_lastLoggedVersionStr != versionStr || s_lastLoggedId != id) {
            s_lastLoggedVersionStr = versionStr;
            s_lastLoggedId = id;
            s_hasLogged = true;
            LOGI("Selected FSR version override: {} ({:#x})", name, id);
        }
    };

    auto normalize = [](std::string_view str) {
        std::string out;
        for (char c : str) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        return out;
    };

    std::string s = versionStr.empty() ? "auto" : versionStr;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // FSR 4.1.2 (driver) supersedes the SDK FSR 4.1.1: when it is available, migrate saved
    // "4.1.1" / generic "FSR 4" selections to it so the 4.1.1 provider is only a fallback.
    bool has412 = false;
    for (const auto& v : versions) {
        if (v.name.find("4.1.2") != std::string::npos) { has412 = true; break; }
    }
    if (has412 && (s.find("4.1.1") != std::string::npos || normalize(s) == "fsr4" || normalize(s) == "4")) {
        s = "4.1.2";
    }

    if (s == "auto" || s == "default") {
        // 0. Prefer the driver FSR 4.1.2 provider explicitly
        for (const auto& v : versions) {
            if (v.name.find("4.1.2") != std::string::npos) {
                LogOverride(FormatVersionName(v.name) + " (Auto-detected)", v.id);
                return v.id;
            }
        }
        // 1. Prefer highest FSR 4.x (major version == 4, e.g. 4.1.2 driver or 4.1.1 SDK) if supported
        for (const auto& v : versions) {
            if (GetMajorVersion(v.name) == 4) {
                LogOverride(FormatVersionName(v.name) + " (Auto-detected)", v.id);
                return v.id;
            }
        }
        // 2. Otherwise explicitly pick FSR 3.x (major version == 3, e.g. 3.1.5) so we never fall into 2.3.4
        for (const auto& v : versions) {
            if (GetMajorVersion(v.name) == 3) {
                LogOverride(FormatVersionName(v.name) + " (Auto-detected)", v.id);
                return v.id;
            }
        }
        return versions[0].id;
    }

    if (s.rfind("0x", 0) == 0) {
        try {
            uint64_t id = std::stoull(s, nullptr, 16);
            LogOverride("Custom ID", id);
            return id;
        } catch (...) {}
    }

    std::string sNorm = normalize(s);

    // 1. First check exact / substring match against formatted and raw names
    // (allows distinguishing "4.1.2" vs "4.1.1" when both FSR 4 providers are available).
    if (!sNorm.empty()) {
        for (const auto& v : versions) {
            std::string formatted = FormatVersionName(v.name);
            std::string formattedLower = formatted;
            std::transform(formattedLower.begin(), formattedLower.end(), formattedLower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string vNorm = normalize(formatted);
            std::string rawNorm = normalize(v.name);
            if (formattedLower.find(s) != std::string::npos ||
                vNorm.find(sNorm) != std::string::npos ||
                rawNorm.find(sNorm) != std::string::npos) {
                LogOverride(formatted, v.id);
                return v.id;
            }
        }
    }

    // 2. Fallback to major version match ("FSR 4", "FSR 3", or fallback from 4.1.2 -> 4.1.1 -> 3.1.5)
    int requestedMajor = GetMajorVersion(s);
    if (requestedMajor == 4 || requestedMajor == 3 || requestedMajor == 2) {
        for (const auto& v : versions) {
            if (GetMajorVersion(v.name) == requestedMajor) {
                LogOverride(FormatVersionName(v.name), v.id);
                return v.id;
            }
        }
        // If user forced FSR 4 on a non-FSR4 GPU, fall back cleanly to FSR 3.x (never 2.3.4)
        if (requestedMajor == 4) {
            for (const auto& v : versions) {
                if (GetMajorVersion(v.name) == 3) {
                    LogOverride(FormatVersionName(v.name) + " (Fallback from FSR 4)", v.id);
                    return v.id;
                }
            }
        }
    }

    if (!s_hasLogged || s_lastLoggedVersionStr != versionStr) {
        s_lastLoggedVersionStr = versionStr;
        s_lastLoggedId = 0;
        s_hasLogged = true;
        LOGW("Could not match FSR version '{}', using FSR 3.x fallback", versionStr);
    }
    for (const auto& v : versions) {
        if (GetMajorVersion(v.name) == 3) return v.id;
    }
    return 0;
}

}  // namespace mhwfsr::loader
