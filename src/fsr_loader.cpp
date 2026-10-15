#include "fsr_loader.h"
#include "log.h"

#include <windows.h>
#include <filesystem>
#include <algorithm>

#include <ffx_api.hpp>
#include <ffx_upscale.hpp>
#include <dx12/ffx_api_dx12.hpp>

namespace fs = std::filesystem;

namespace mhwfsr::loader {

namespace {
HMODULE g_loaderDll = nullptr;
bool g_queried = false;
std::vector<FsrVersionInfo> g_cachedVersions;
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
        SetDllDirectoryW(targetDir.wstring().c_str());
        fs::path loaderDllPath = targetDir / L"amd_fidelityfx_loader_dx12.dll";
        if (fs::exists(loaderDllPath, ec)) {
            g_loaderDll = LoadLibraryW(loaderDllPath.wstring().c_str());
            if (g_loaderDll) {
                LOGI("Loaded {}", loaderDllPath.string());
            } else {
                LOGW("Failed to pre-load {} (error {})", loaderDllPath.string(), GetLastError());
            }
        }
    } else {
        LOGW("FSR loader directory not found: {}", targetDir.string());
    }
    return true;
}

std::vector<FsrVersionInfo> QueryVersions(ID3D12Device* device) {
    if (g_queried) return g_cachedVersions;
    if (!device) return {};

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

uint64_t ResolveVersionId(ID3D12Device* device, const std::string& versionStr) {
    if (!device) return 0;
    auto versions = QueryVersions(device);

    if (versionStr.empty()) return 0;

    std::string s = versionStr;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)::tolower(c); });
    if (s == "auto" || s == "default") return 0;

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

    if (s.rfind("0x", 0) == 0) {
        try {
            uint64_t id = std::stoull(s, nullptr, 16);
            LogOverride("Custom ID", id);
            return id;
        } catch (...) {}
    }

    auto normalize = [](std::string str) {
        std::string out;
        for (char c : str) {
            if (isalnum(static_cast<unsigned char>(c))) {
                out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
            }
        }
        return out;
    };

    std::string sNorm = normalize(s);

    // 1. Direct substring match
    for (const auto& v : versions) {
        std::string vNameLower = v.name;
        std::transform(vNameLower.begin(), vNameLower.end(), vNameLower.begin(),
                       [](unsigned char c) { return (char)::tolower(c); });
        if (vNameLower.find(s) != std::string::npos) {
            LogOverride(v.name, v.id);
            return v.id;
        }
    }

    // 2. Normalized alphanumeric match (handles "fsr4", "fsr 4", "fsr3", etc.)
    for (const auto& v : versions) {
        std::string vNorm = normalize(v.name);
        if (vNorm.find(sNorm) != std::string::npos || sNorm.find(vNorm) != std::string::npos) {
            LogOverride(v.name, v.id);
            return v.id;
        }
        // Also check if user specified "4" and name contains "4", or user specified "3" and name contains "3"
        if ((sNorm == "4" || sNorm == "fsr4" || sNorm == "41" || sNorm == "fsr41") &&
            (vNorm.find("4") != std::string::npos || vNorm.find("fsr4") != std::string::npos)) {
            LogOverride(v.name, v.id);
            return v.id;
        }
        if ((sNorm == "3" || sNorm == "fsr3" || sNorm == "31" || sNorm == "fsr31") &&
            (vNorm.find("3") != std::string::npos || vNorm.find("fsr3") != std::string::npos)) {
            LogOverride(v.name, v.id);
            return v.id;
        }
    }

    if (!s_hasLogged || s_lastLoggedVersionStr != versionStr) {
        s_lastLoggedVersionStr = versionStr;
        s_lastLoggedId = 0;
        s_hasLogged = true;
        LOGW("Could not match FSR version '{}', using default", versionStr);
    }
    return 0;
}

}  // namespace mhwfsr::loader
