#pragma once

#include <string>

#include "log.h"

namespace mhwdlss {

// The filter of the sharpening pass after DLSS (see sharpen.h).
enum class Sharpener {
    NVSharpen,  // NVIDIA Image Scaling's NVSharpen
    Vanilla,    // the filter of the game's original DLSS sharpening (DLISP's "DLSS Sharpen")
    Rcas,       // AMD FidelityFX RCAS
};

// No Ultra Performance: the game clamps its render scale to 0.5 and above.
enum class QualityMode {
    DLAA,
    Quality,
    Balanced,
    Performance,
    Custom,
};

struct Config {
    bool enabled = true;
    LogLevel logLevel = LogLevel::Info;

    QualityMode mode = QualityMode::DLAA;
    float customScale = 1.0f;  // render scale used when mode == Custom
    // DLSS render preset (0 = default, otherwise NVSDK_NGX_DLSS_Hint_Render_Preset value, K = 11).
    unsigned int preset = 11;
    std::wstring dllPath;  // nvngx_dlss.dll to use instead of the driver's (folder or file)
    bool autoExposure = false;
    // Sharpening after DLSS, 0..1; negative = the game's DLSS Sharpness option.
    float sharpness = 0.33f;
    Sharpener sharpener = Sharpener::NVSharpen;

    // Input conventions, measured for this game (see game.cpp and ngx_shim.cpp).
    bool depthInverted = true;
    bool mvJittered = false;
    // The game's dense velocity is the clip-space difference times the render size, without the
    // 0.5 and the y flip of a pixel offset. Measured with tools/motion_check.py on frames dumped
    // while walking and turning.
    float mvScaleX = 0.5f;
    float mvScaleY = -0.5f;
    // The camera jitter value h moves the image by (h.x / 2, -h.y / 2) pixels.
    float jitterScaleX = 0.5f;
    float jitterScaleY = -0.5f;
    bool forceJitter = true;      // keep the TAA jitter on while DLSS is active
    bool fullPixelJitter = true;  // double the game's half-pixel jitter while DLSS is active
    int hdr = -1;                 // -1 = from the game / input format, 0 or 1 = force
    float exposureScale = 1.0f;
    float preExposure = 1.0f;

    // Diagnostics.
    bool ngxLogging = false;
    bool verboseFrames = false;
    bool bypass = false;          // DLAA mode: keep the game's TAA result (A/B comparison)
    bool debugProbe = false;      // log statistics of the DLSS inputs after a reload (D3D12)
    int debugDumpFrames = 0;      // dump raw input frames + motion + depth + jitter after a reload (D3D12)
    float debugDumpDelay = 5.0f;  // seconds between the reload and the dump
    bool debugDumpOutput = false; // write one DLSS output frame to output.bin after a reload (D3D11)
    bool debugFixedJitter = false; // replace the camera jitter with a constant (camera units)
    float debugJitterX = 0.0f;
    float debugJitterY = 0.0f;

    // Render scale implied by the mode (1.0 for DLAA).
    float RenderScale() const;
    // Sharpening strength in effect, 0..1 (from the ini or the game's option).
    float Sharpness() const;
};

extern Config g_config;

// Loads <iniPath>. Missing keys keep their defaults; a missing file is written out with defaults.
void LoadConfig(const std::wstring& iniPath);

// Reloads the ini loaded by LoadConfig when its timestamp changed. Returns true on reload.
bool ReloadConfigIfChanged();

// The game's graphics_option.ini, read for its DLSS_Sharpness value. The game rewrites the file
// when graphics options are applied; the reload returns true when the value changed.
void LoadGameOptions(const std::wstring& path);
bool ReloadGameOptionsIfChanged();
float GameSharpness();

const char* QualityModeName(QualityMode mode);
const char* SharpenerName(Sharpener sharpener);

}  // namespace mhwdlss
