#include <windows.h>

#include <filesystem>

#include "config.h"
#include "crash.h"
#include "fsr_loader.h"
#include "game.h"
#include "log.h"
#include "ngx_shim.h"

namespace fs = std::filesystem;
using namespace mhwfsr;

namespace {

std::string Narrow(const fs::path& p) { return p.string(); }

void Start(HMODULE module) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(module, buf, MAX_PATH);
    fs::path dll(buf);
    fs::path dir = dll.parent_path();
    std::wstring stem = dll.stem().wstring();

    LoadConfig((dir / (stem + L".ini")).wstring());
    LogInit((dir / (stem + L".log")).wstring(), g_config.logLevel);

    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path exe(buf);
    LoadGameOptions((exe.parent_path() / L"graphics_option.ini").wstring());

    LOGI("MHW-FSR {} loaded from {}", MHW_FSR_VERSION, Narrow(dll));
    LOGI("Game: {}", Narrow(exe));
    LOGI("Config: mode {} (render scale {:.3f}), sharpness {:.2f}{}, autoExposure {}, depthInverted {}, MV scale {},{}, "
         "jitter scale {},{}, reflection fix {}",
         QualityModeName(g_config.mode), g_config.RenderScale(),
         g_config.Sharpness(),
         g_config.sharpness < 0.0f ? " (game option)" : "",
         g_config.autoExposure,
         g_config.depthInverted, g_config.mvScaleX, g_config.mvScaleY,
         g_config.jitterScaleX, g_config.jitterScaleY, g_config.reflectionFix);

    std::error_code ec;
    // Check if an older DLSS version is still present
    for (const wchar_t* oldDll : {L"MHWDLSS.dll", L"MHW-DLSS4.dll"}) {
        if (_wcsicmp(dll.filename().c_str(), oldDll) != 0 && fs::exists(dir / oldDll, ec)) {
            LOGW("An older DLSS mod ({}) is also installed in {}: please remove it to avoid conflicts",
                 Narrow(oldDll), Narrow(dir));
        }
    }

    if (!g_config.enabled) {
        LOGI("Plugin disabled in ini");
        return;
    }
    InstallCrashLogger();

    // Data directory for FSR runtime files and DLLs (nativePC\plugins\MHW-FSR\)
    fs::path dataDir = dir / stem;
    fs::create_directories(dataDir, ec);

    // Initialize FSR loader paths
    loader::Init(dir.wstring(), g_config.dllPath);

    if (!game::Init()) {
        LOGE("Game hooks failed, plugin inactive");
        return;
    }
    if (!ngx::Install(dataDir.wstring(), exe.parent_path().wstring())) {
        LOGE("Engine hooks failed, plugin inactive");
        return;
    }

    // Activate hooks now that everything is installed
    game::Activate(g_config);
    LOGI("Ready: turn on NVIDIA DLSS in the game's graphics options to activate AMD FidelityFX FSR");
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        try {
            Start(module);
        } catch (const std::exception& e) {
            LOGE("Startup failed: {}", e.what());
        } catch (...) {
            LOGE("Startup failed with unknown exception");
        }
    }
    return TRUE;
}
