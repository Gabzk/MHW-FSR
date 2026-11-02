#include <windows.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "config.h"

namespace fs = std::filesystem;
using namespace mhwfsr;

namespace {

int g_failures = 0;

void Check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "[FAIL] %s\n", msg);
        ++g_failures;
    } else {
        std::printf("[PASS] %s\n", msg);
    }
}

bool Near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

}  // namespace

int main() {
    fs::path tempDir = fs::temp_directory_path() / L"mhw_fsr_config_selftest";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);

    // 1. Test default .ini creation and default NativeAA preset
    fs::path iniPath = tempDir / L"MHW-FSR.ini";
    LoadConfig(iniPath.wstring());
    Check(fs::exists(iniPath, ec), "Default MHW-FSR.ini created when missing");
    Check(g_config.mode == QualityMode::NativeAA, "Default preset is NativeAA");
    Check(Near(g_config.RenderScale(), 1.0f), "NativeAA render scale is 1.0");
    Check(Near(g_config.Sharpness(), 0.33f), "Default sharpness is 0.33");
    Check(g_config.antiLag2 == true, "Default AntiLag2 is true");
    Check(g_config.showOsd == true, "Default ShowOSD is true");
    Check(g_config.showFps == true, "Default ShowFPS is true");
    Check(g_config.showLatencyOsd == false, "Default ShowLatencyOSD is false");

    // 2. Test legacy .ini compatibility (UltraPerformance -> Performance, [Advanced] ignored)
    {
        std::ofstream out(iniPath, std::ios::binary | std::ios::trunc);
        out << "[General]\r\n"
            << "Enabled=1\r\n"
            << "LogLevel=Debug\r\n"
            << "[FSR]\r\n"
            << "QualityPreset=UltraPerformance\r\n"
            << "Sharpness=0.50\r\n"
            << "AutoExposure=1\r\n"
            << "HDR=-1\r\n"
            << "Bypass=1\r\n"
            << "[Advanced]\r\n"
            << "DepthInverted=1\r\n"
            << "SuppressPostProcess=1\r\n"
            << "MotionVectorScaleX=1.0\r\n";
    }
    LoadConfig(iniPath.wstring());
    Check(g_config.logLevel == LogLevel::Debug, "Legacy ini LogLevel=Debug parsed");
    Check(g_config.mode == QualityMode::Performance, "Legacy UltraPerformance mapped to Performance");
    Check(Near(g_config.RenderScale(), 0.50f), "Performance render scale is 0.50");
    Check(Near(g_config.Sharpness(), 0.50f), "Legacy ini Sharpness=0.50 parsed");

    // 3. Test SaveConfig + LoadConfig round-trip
    Config saved{};
    saved.enabled = true;
    saved.logLevel = LogLevel::Info;
    saved.mode = QualityMode::Custom;
    saved.customScale = 0.65f;
    saved.sharpness = 0.80f;
    saved.version = "FSR 4.1.2";
    saved.fsr4Model = Fsr4Model::Lite;
    saved.antiLag2 = false;
    saved.antiLagMaxFps = 144;
    saved.showOsd = false;
    saved.showFps = false;
    saved.showLatencyOsd = true;

    SaveConfig(saved);
    g_config = Config{};
    LoadConfig(iniPath.wstring());

    Check(g_config.mode == QualityMode::Custom, "Round-trip Custom preset preserved");
    Check(Near(g_config.RenderScale(), 0.65f), "Round-trip Custom RenderScale=0.65 preserved");
    Check(Near(g_config.Sharpness(), 0.80f), "Round-trip Sharpness=0.80 preserved");
    Check(g_config.version == "FSR 4.1.2", "Round-trip Version preserved");
    Check(g_config.fsr4Model == Fsr4Model::Lite, "Round-trip Fsr4Model::Lite preserved");
    Check(g_config.antiLag2 == false, "Round-trip AntiLag2=false preserved");
    Check(g_config.antiLagMaxFps == 144, "Round-trip AntiLagMaxFps=144 preserved");
    Check(g_config.showOsd == false, "Round-trip ShowOSD=false preserved");
    Check(g_config.showFps == false, "Round-trip ShowFPS=false preserved");
    Check(g_config.showLatencyOsd == true, "Round-trip ShowLatencyOSD=true preserved");

    fs::remove_all(tempDir, ec);
    return g_failures == 0 ? 0 : 1;
}
