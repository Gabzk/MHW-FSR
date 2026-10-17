#include "fsr_loader.h"
#include "log.h"

#include <windows.h>
#include <filesystem>
#include <algorithm>

#include <ffx_api.hpp>
#include <ffx_upscale.hpp>
#include <ffx_framegeneration.hpp>
#include <dx12/ffx_api_dx12.hpp>

namespace fs = std::filesystem;

namespace mhwfsr::loader {

namespace {
HMODULE g_loaderDll = nullptr;
fs::path g_targetDir;
bool g_loadAttempted = false;
bool g_queried = false;
std::vector<FsrVersionInfo> g_cachedVersions;

void EnsureLoaded() {
    if (g_loadAttempted) return;
    g_loadAttempted = true;
    if (g_targetDir.empty()) return;

    SetDllDirectoryW(g_targetDir.wstring().c_str());

    std::error_code ec;
    for (const wchar_t* dllName : {
             L"amd_fidelityfx_upscaler_dx12.dll",
             L"amd_fidelityfx_loader_dx12.dll"
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

// Extracts the major semantic version digit from a provider name (e.g. "4.1.1" -> 4, "FSR 3.1.5" -> 3, "2.3.4" -> 2).
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
        LOGI("Available FSR upscaler [{}/{}]: {} (id {:#x})", i + 1, count, name, ids[i]);
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

bool HasFrameGenSupport(ID3D12Device* device) {
    if (!device) return false;
    EnsureLoaded();
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

    auto normalize = [](std::string str) {
        std::string out;
        for (char c : str) {
            if (isalnum(static_cast<unsigned char>(c))) {
                out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
            }
        }
        return out;
    };

    std::string s = versionStr.empty() ? "auto" : versionStr;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)::tolower(c); });
    if (s == "auto" || s == "default") {
        // 1. Prefer FSR 4.x (major version == 4) if supported by the GPU/driver
        for (const auto& v : versions) {
            if (GetMajorVersion(v.name) == 4) {
                LogOverride(v.name + " (Auto-detected)", v.id);
                return v.id;
            }
        }
        // 2. Otherwise explicitly pick FSR 3.x (major version == 3, e.g. 3.1.5) so we never fall into 2.3.4
        for (const auto& v : versions) {
            if (GetMajorVersion(v.name) == 3) {
                LogOverride(v.name + " (Auto-detected)", v.id);
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

    // Check explicit major version requests ("FSR 4", "4.1", "FSR 3.1", "3.1.5", etc.)
    int requestedMajor = GetMajorVersion(s);
    if (requestedMajor == 4 || requestedMajor == 3 || requestedMajor == 2) {
        for (const auto& v : versions) {
            if (GetMajorVersion(v.name) == requestedMajor) {
                LogOverride(v.name, v.id);
                return v.id;
            }
        }
        // If user forced FSR 4 on a non-FSR4 GPU, fall back cleanly to FSR 3.x (never 2.3.4)
        if (requestedMajor == 4) {
            for (const auto& v : versions) {
                if (GetMajorVersion(v.name) == 3) {
                    LogOverride(v.name + " (Fallback from FSR 4)", v.id);
                    return v.id;
                }
            }
        }
    }

    // Direct substring / normalized match
    for (const auto& v : versions) {
        std::string vNameLower = v.name;
        std::transform(vNameLower.begin(), vNameLower.end(), vNameLower.begin(),
                       [](unsigned char c) { return (char)::tolower(c); });
        if (vNameLower.find(s) != std::string::npos || normalize(v.name).find(sNorm) != std::string::npos) {
            LogOverride(v.name, v.id);
            return v.id;
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
