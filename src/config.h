#pragma once

#include <string>

#include "log.h"

namespace mhwfsr {

// FSR Quality Presets. The game engine never renders below 50% of the output size, so Performance
// (2.0x) is the lowest preset.
enum class QualityMode {
    NativeAA,
    Quality,
    Balanced,
    Performance,
    Custom,
};

// Internal neural model variants inside AMD FSR 4.1.2 (amdxcffx64.dll)
enum class Fsr4Model {
    Auto = 0,      // Hardware default (FP8 on RDNA4, FSR4-i8 on RDNA3, FSR4-i8-2 on RDNA2)
    Standard = 1,  // FSR4-i8 (WMMA INT8 - RDNA 3/4 High Quality)
    Rdna2 = 2,     // FSR4-i8-2 (DP4A INT8 - RDNA 2/3/4 Balanced)
    Lite = 3,      // FSR4L-i8 (DP4A INT8 Lite - RDNA 2/3/4 Max FPS)
};

struct Config {
    bool enabled = true;
    LogLevel logLevel = LogLevel::Info;

    QualityMode mode = QualityMode::NativeAA;
    float customScale = 0.75f;  // render scale used when mode == Custom
    std::string version = "Auto"; // "Auto" (FSR 4.1.2 on RDNA 2/3/4, FSR 3.1.5 otherwise) or specific version ID/name
    Fsr4Model fsr4Model = Fsr4Model::Auto;
    std::wstring dllPath;      // Folder containing amd_fidelityfx_loader_dx12.dll (empty: nativePC\plugins\MHW-FSR\)
    // Sharpening after FSR, 0..1; negative = the game's DLSS Sharpness option.
    float sharpness = 0.33f;

    // AMD FidelityFX Anti-Lag 2
    bool antiLag2 = true;
    int antiLagMaxFps = 0;  // 0 = unlimited

    // On-screen display
    bool showOsd = true;
    bool showFps = true;
    bool showLatencyOsd = false;

    // Render scale implied by the mode (1.0 for NativeAA).
    float RenderScale() const;
    // Sharpening strength in effect, 0..1 (from ini or game's option).
    float Sharpness() const;
};

extern Config g_config;

// Loads <iniPath>. Missing keys keep defaults; missing file is written out with defaults.
// Keys that no longer exist (the old [Advanced] section, Bypass, ...) are ignored.
void LoadConfig(const std::wstring& iniPath);

// Reloads ini when timestamp changed. Returns true on reload.
bool ReloadConfigIfChanged();

// The game's graphics_option.ini, read for DLSS_Sharpness.
void LoadGameOptions(const std::wstring& path);
bool ReloadGameOptionsIfChanged();
float GameSharpness();

const char* QualityModeName(QualityMode mode);
const char* Fsr4ModelName(Fsr4Model model);

// Writes current config values back to MHW-FSR.ini
void SaveConfig(const Config& c);

}  // namespace mhwfsr
