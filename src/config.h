#pragma once

#include <limits>
#include <string>

#include "log.h"

namespace mhwfsr {

// FSR Quality Presets
enum class QualityMode {
    NativeAA,
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
    Custom,
};

struct Config {
    bool enabled = true;
    LogLevel logLevel = LogLevel::Info;

    QualityMode mode = QualityMode::NativeAA;
    float customScale = 1.0f;  // render scale used when mode == Custom
    std::string version = "Auto"; // "Auto" (FSR 4 on RDNA3/4, FSR 3.1 otherwise) or specific version ID/name
    std::wstring dllPath;      // Folder containing amd_fidelityfx_loader_dx12.dll (empty: nativePC\plugins\MHW-FSR\)
    bool autoExposure = false;
    // Sharpening after FSR, 0..1; negative = the game's DLSS Sharpness option.
    float sharpness = 0.33f;

    // Input conventions, measured for this game (see game.cpp).
    bool depthInverted = true;
    bool depthInfinite = false;
    bool mvJittered = false;
    float mvScaleX = 0.5f;
    float mvScaleY = -0.5f;
    float jitterScaleX = 0.5f;
    float jitterScaleY = -0.5f;
    bool forceJitter = true;      // keep the TAA jitter on while FSR is active
    bool fullPixelJitter = true;  // double the game's half-pixel jitter while FSR is active
    bool reflectionFix = true;    // hold game's 2x2 reflection history swap off while FSR runs
    bool cutReset = true;         // reset history on camera cuts
    int jitterPhases = -1;
    float mipBias = std::numeric_limits<float>::quiet_NaN();
    int hdr = -1;                 // -1 = auto from game / float format, 0 or 1 = force
    float exposureScale = 1.0f;
    float preExposure = 1.0f;

    // Diagnostics
    bool verboseFrames = false;
    bool bypass = false;          // NativeAA mode: keep the game's TAA result (A/B comparison)
    bool debugProbe = false;      // log statistics of FSR inputs after a reload (D3D12)
    int debugDumpFrames = 0;      // dump raw input frames + motion + depth + jitter after reload (D3D12)
    float debugDumpDelay = 5.0f;  // seconds between reload and dump
    bool debugFixedJitter = false;
    float debugJitterX = 0.0f;
    float debugJitterY = 0.0f;

    // Render scale implied by the mode (1.0 for NativeAA).
    float RenderScale() const;
    // Sharpening strength in effect, 0..1 (from ini or game's option).
    float Sharpness() const;
};

extern Config g_config;

// Loads <iniPath>. Missing keys keep defaults; missing file is written out with defaults.
void LoadConfig(const std::wstring& iniPath);

// Reloads ini when timestamp changed. Returns true on reload.
bool ReloadConfigIfChanged();

// The game's graphics_option.ini, read for DLSS_Sharpness.
void LoadGameOptions(const std::wstring& path);
bool ReloadGameOptionsIfChanged();
float GameSharpness();
int GameFrameRate();

const char* QualityModeName(QualityMode mode);

}  // namespace mhwfsr
