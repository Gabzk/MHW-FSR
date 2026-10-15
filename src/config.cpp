#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>

namespace mhwfsr {

Config g_config;

namespace {

constexpr wchar_t kDefaultIni[] =
    L"; =============================================================================\n"
    L"; MHW-FSR - AMD FidelityFX FSR 3.1 & FSR 4.1 for Monster Hunter: World (DX12)\n"
    L"; =============================================================================\n"
    L"; Quick Start:\n"
    L"; 1. In game Display Settings, enable DirectX 12 API.\n"
    L"; 2. Turn ON \"NVIDIA DLSS\" (this plugin intercepts and runs native AMD FSR instead).\n"
    L"; 3. This file is automatically hot-reloaded in real-time while the game is running.\n"
    L"\n"
    L"[General]\n"
    L"; Enabled: 1 = Plugin enabled, 0 = Disabled completely (requires game restart).\n"
    L"Enabled=1\n"
    L"\n"
    L"; LogLevel: Detail level written to nativePC\\plugins\\MHW-FSR.log\n"
    L"; Options: Debug, Info, Warn, Error.\n"
    L"; (Use 'Debug' to log per-frame render resolutions and full diagnostics).\n"
    L"LogLevel=Info\n"
    L"\n"
    L"[FSR]\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; QualityPreset: Internal rendering resolution and upscaling mode.\n"
    L"; Options:\n"
    L";   NativeAA         -> Renders at 100% native screen resolution (no upscaling).\n"
    L";                       Replaces the game's blurry TAA with FSR's ultra-crisp temporal\n"
    L";                       anti-aliasing pass. Highest possible image clarity.\n"
    L";   Quality          -> Quality mode (~1.5x scale: e.g., 1706x960 upscaled to 1440p).\n"
    L";                       Substantial FPS boost with near-native visual fidelity.\n"
    L";   Balanced         -> Balanced mode (~1.7x scale: e.g., 1506x847 upscaled to 1440p).\n"
    L";   Performance      -> Performance mode (2.0x scale: e.g., 1280x720 upscaled to 1440p).\n"
    L";                       Maximum performance / framerate.\n"
    L";   UltraPerformance -> Runs at 0.5 scale (the game engine clamps render scale to >= 0.5).\n"
    L";   Custom           -> Uses the custom scale specified in RenderScale below.\n"
    L"QualityPreset=Quality\n"
    L"\n"
    L"; RenderScale: Used only when QualityPreset=Custom (values from 0.50 to 1.00).\n"
    L"RenderScale=1.0\n"
    L"\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; Version: Upscaler version from the AMD FidelityFX API.\n"
    L"; Options:\n"
    L";   Auto      -> Recommended. Automatically detects your GPU:\n"
    L";                - RDNA 3 (e.g. RX 7000 series) & RDNA 4: runs FSR 4 (ML/DirectML).\n"
    L";                - Pre-RDNA 3 (RX 6000, 5000, Intel, Nvidia): runs FSR 3.1.5 (Temporal).\n"
    L";   FSR 4     -> Forces the FSR 4 Machine Learning upscaler (also accepts: 4.1, FSR4).\n"
    L";   FSR 3.1   -> Forces the FSR 3.1 temporal upscaler (also accepts: 3.1.5, FSR3).\n"
    L";   0x...     -> Exact hexadecimal provider ID listed in MHW-FSR.log.\n"
    L"Version=Auto\n"
    L"\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; Sharpness: Integrated post-upscale sharpening (AMD FidelityFX RCAS).\n"
    L";   0.00      -> Sharpening off.\n"
    L";   0.33      -> Subtle touch of sharpness (recommended for 1440p / 4K).\n"
    L";   0.66      -> Moderate sharpness.\n"
    L";   1.00      -> Maximum sharpness.\n"
    L";   Game      -> Follows the in-game \"DLSS Sharpness\" slider value.\n"
    L"Sharpness=0.33\n"
    L"\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; AutoExposure: Automatic exposure estimation by FSR.\n"
    L";   0 = Off (uses MT Framework engine's internal exposure/luminance buffer).\n"
    L";   1 = On  (allows FSR to compute its own exposure dynamically).\n"
    L"AutoExposure=0\n"
    L"\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; DLLPath: Directory containing amd_fidelityfx_loader_dx12.dll & amd_fidelityfx_upscaler_dx12.dll.\n"
    L"; Leave empty to use default: nativePC\\plugins\\MHW-FSR\\\n"
    L"DLLPath=\n"
    L"\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; Bypass: Comparison mode (only active when QualityPreset=NativeAA).\n"
    L";   0 = FSR active.\n"
    L";   1 = Displays vanilla MT Framework TAA to compare sharpness side by side.\n"
    L"Bypass=0\n"
    L"\n"
    L"[AntiLag]\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; AntiLag2: AMD FidelityFX Anti-Lag 2 driver-level latency reduction (Radeon).\n"
    L";   1 = Enabled (synchronizes CPU and GPU pacing, reducing click-to-photon latency).\n"
    L";   0 = Disabled.\n"
    L"AntiLag2=1\n"
    L"\n"
    L"; AntiLagMaxFPS: Framerate limiter via driver pacing (0 = Unlimited).\n"
    L"AntiLagMaxFPS=0\n"
    L"\n"
    L"[FrameGeneration]\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; FrameGen: AMD FidelityFX Frame Generation (interpolates frames to double FPS).\n"
    L";   1 = Enabled, 0 = Disabled.\n"
    L"FrameGen=0\n";

std::wstring g_iniPath;
unsigned long long g_iniTime = 0;

std::wstring g_gameOptionsPath;
unsigned long long g_gameOptionsTime = 0;
float g_gameSharpness = 0.0f;
int g_gameFrameRate = 0;

unsigned long long LastWrite(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
           data.ftLastWriteTime.dwLowDateTime;
}

constexpr const wchar_t* kSections[] = {L"General", L"FSR", L"DLSS", L"Advanced"};

std::wstring ReadString(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    constexpr wchar_t kMissing[] = L"\x01";
    wchar_t buf[1024];
    GetPrivateProfileStringW(section, key, kMissing, buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
    for (const wchar_t* other : kSections) {
        if (wcscmp(buf, kMissing) != 0) break;
        if (_wcsicmp(other, section) != 0)
            GetPrivateProfileStringW(other, key, kMissing, buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
    }
    std::wstring s = wcscmp(buf, kMissing) == 0 ? def : buf;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((s[i] == L';' || s[i] == L'#') && (i == 0 || std::iswspace(s[i - 1]))) {
            s.resize(i);
            break;
        }
    }
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::iswspace(s[start])) ++start;
    s = s.substr(start);
    if (s.size() >= 2 && (s.front() == L'"' || s.front() == L'\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    return s;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool ReadBool(const std::wstring& ini, const wchar_t* section, const wchar_t* key, bool def) {
    auto s = Lower(ReadString(ini, section, key, def ? L"1" : L"0"));
    if (s == L"1" || s == L"true" || s == L"on" || s == L"yes") return true;
    if (s == L"0" || s == L"false" || s == L"off" || s == L"no") return false;
    return def;
}

float ReadFloat(const std::wstring& ini, const wchar_t* section, const wchar_t* key, float def) {
    auto s = ReadString(ini, section, key, L"");
    if (s.empty()) return def;
    try {
        return std::stof(s);
    } catch (...) {
        return def;
    }
}

std::string Narrow(const std::wstring& ws) {
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), ws.data(), len);
    return ws;
}

}  // namespace

float Config::RenderScale() const {
    switch (mode) {
        case QualityMode::NativeAA: return 1.0f;
        case QualityMode::Quality: return 2.0f / 3.0f;
        case QualityMode::Balanced: return 1.0f / 1.7f;
        case QualityMode::Performance: return 0.5f;
        case QualityMode::UltraPerformance: return 0.5f;
        case QualityMode::Custom: return std::clamp(customScale, 0.5f, 1.0f);
    }
    return 1.0f;
}

float Config::Sharpness() const { return std::clamp(sharpness < 0.0f ? g_gameSharpness : sharpness, 0.0f, 1.0f); }

const char* QualityModeName(QualityMode mode) {
    switch (mode) {
        case QualityMode::NativeAA: return "NativeAA";
        case QualityMode::Quality: return "Quality";
        case QualityMode::Balanced: return "Balanced";
        case QualityMode::Performance: return "Performance";
        case QualityMode::UltraPerformance: return "UltraPerformance";
        case QualityMode::Custom: return "Custom";
    }
    return "?";
}

void LoadConfig(const std::wstring& ini) {
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::ofstream out(ini, std::ios::binary);
        for (const wchar_t* p = kDefaultIni; *p; ++p) out.put(static_cast<char>(*p));
    }

    Config c;
    c.enabled = ReadBool(ini, L"General", L"Enabled", c.enabled);

    auto level = Lower(ReadString(ini, L"General", L"LogLevel", L"info"));
    if (level == L"debug") c.logLevel = LogLevel::Debug;
    else if (level == L"warn") c.logLevel = LogLevel::Warn;
    else if (level == L"error") c.logLevel = LogLevel::Error;
    else c.logLevel = LogLevel::Info;

    auto mode = Lower(ReadString(ini, L"FSR", L"QualityPreset", L""));
    if (mode.empty()) {
        mode = Lower(ReadString(ini, L"FSR", L"Mode", L"nativeaa"));
    }
    if (mode == L"quality") c.mode = QualityMode::Quality;
    else if (mode == L"balanced") c.mode = QualityMode::Balanced;
    else if (mode == L"performance") c.mode = QualityMode::Performance;
    else if (mode == L"ultraperformance") c.mode = QualityMode::UltraPerformance;
    else if (mode == L"custom") c.mode = QualityMode::Custom;
    else c.mode = QualityMode::NativeAA;

    c.customScale = ReadFloat(ini, L"FSR", L"RenderScale", c.customScale);

    auto sharpness = Lower(ReadString(ini, L"FSR", L"Sharpness", L""));
    if (sharpness == L"game")
        c.sharpness = -1.0f;
    else if (!sharpness.empty())
        c.sharpness = std::clamp(ReadFloat(ini, L"FSR", L"Sharpness", c.sharpness), 0.0f, 1.0f);

    c.autoExposure = ReadBool(ini, L"FSR", L"AutoExposure", c.autoExposure);
    c.version = Narrow(ReadString(ini, L"FSR", L"Version", L"Auto"));
    c.dllPath = ReadString(ini, L"FSR", L"DLLPath", L"");
    c.bypass = ReadBool(ini, L"FSR", L"Bypass", c.bypass);

    // [AntiLag]
    c.antiLag2 = ReadBool(ini, L"AntiLag", L"AntiLag2", c.antiLag2);
    c.antiLagMaxFps = std::max(0, static_cast<int>(ReadFloat(ini, L"AntiLag", L"AntiLagMaxFPS", static_cast<float>(c.antiLagMaxFps))));

    // [FrameGeneration]
    c.frameGen = ReadBool(ini, L"FrameGeneration", L"FrameGen", c.frameGen);

    // [Advanced]
    c.depthInverted = ReadBool(ini, L"Advanced", L"DepthInverted", c.depthInverted);
    c.depthInfinite = ReadBool(ini, L"Advanced", L"DepthInfinite", c.depthInfinite);
    c.mvJittered = ReadBool(ini, L"Advanced", L"MVJittered", c.mvJittered);
    c.mvScaleX = ReadFloat(ini, L"Advanced", L"MotionScaleX", c.mvScaleX);
    c.mvScaleY = ReadFloat(ini, L"Advanced", L"MotionScaleY", c.mvScaleY);
    c.jitterScaleX = ReadFloat(ini, L"Advanced", L"JitterScaleX", c.jitterScaleX);
    c.jitterScaleY = ReadFloat(ini, L"Advanced", L"JitterScaleY", c.jitterScaleY);
    c.forceJitter = ReadBool(ini, L"Advanced", L"ForceJitter", c.forceJitter);
    c.fullPixelJitter = ReadBool(ini, L"Advanced", L"FullPixelJitter", c.fullPixelJitter);
    c.reflectionFix = ReadBool(ini, L"Advanced", L"ReflectionFix", c.reflectionFix);
    c.cutReset = ReadBool(ini, L"Advanced", L"CutReset", c.cutReset);
    c.jitterPhases = std::clamp(static_cast<int>(ReadFloat(ini, L"Advanced", L"JitterPhases", -1.0f)), -1, 256);
    auto mipBias = Lower(ReadString(ini, L"Advanced", L"MipBias", L"auto"));
    if (mipBias == L"off") c.mipBias = 0.0f;
    else if (mipBias != L"auto") c.mipBias = std::clamp(ReadFloat(ini, L"Advanced", L"MipBias", 0.0f), -3.0f, 0.0f);
    c.hdr = static_cast<int>(ReadFloat(ini, L"Advanced", L"HDR", static_cast<float>(c.hdr)));
    c.exposureScale = ReadFloat(ini, L"Advanced", L"ExposureScale", c.exposureScale);
    c.preExposure = ReadFloat(ini, L"Advanced", L"PreExposure", c.preExposure);

    c.verboseFrames = ReadBool(ini, L"Advanced", L"VerboseFrames", c.verboseFrames);
    c.debugProbe = ReadBool(ini, L"Advanced", L"DebugProbe", c.debugProbe);
    c.debugDumpFrames = static_cast<int>(ReadFloat(ini, L"Advanced", L"DebugDumpFrames", 0.0f));
    c.debugDumpDelay = std::clamp(ReadFloat(ini, L"Advanced", L"DebugDumpDelay", c.debugDumpDelay), 0.0f, 60.0f);
    c.debugFixedJitter = ReadBool(ini, L"Advanced", L"DebugFixedJitter", c.debugFixedJitter);
    c.debugJitterX = ReadFloat(ini, L"Advanced", L"DebugJitterX", c.debugJitterX);
    c.debugJitterY = ReadFloat(ini, L"Advanced", L"DebugJitterY", c.debugJitterY);

    g_config = c;
    g_iniPath = ini;
    g_iniTime = LastWrite(ini);
}

bool ReloadConfigIfChanged() {
    if (g_iniPath.empty()) return false;
    if (LastWrite(g_iniPath) == g_iniTime) return false;
    LoadConfig(g_iniPath);
    return true;
}

void LoadGameOptions(const std::wstring& path) {
    g_gameOptionsPath = path;
    g_gameOptionsTime = LastWrite(path);
    g_gameSharpness = std::clamp(ReadFloat(path, L"GraphicsOption", L"DLSS_Sharpness", 0.0f), 0.0f, 1.0f);
    g_gameFrameRate = std::max(0, static_cast<int>(ReadFloat(path, L"GraphicsOption", L"FrameRate", 0.0f)));
}

bool ReloadGameOptionsIfChanged() {
    if (g_gameOptionsPath.empty() || LastWrite(g_gameOptionsPath) == g_gameOptionsTime) return false;
    float before = g_gameSharpness;
    LoadGameOptions(g_gameOptionsPath);
    return g_gameSharpness != before;
}

float GameSharpness() { return g_gameSharpness; }

int GameFrameRate() { return g_gameFrameRate; }

void SaveConfig(const Config& c) {
    if (g_iniPath.empty()) return;
    auto writeStr = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& val) {
        WritePrivateProfileStringW(sec, key, val.c_str(), g_iniPath.c_str());
    };
    auto writeBool = [&](const wchar_t* sec, const wchar_t* key, bool val) {
        writeStr(sec, key, val ? L"1" : L"0");
    };
    auto writeFloat = [&](const wchar_t* sec, const wchar_t* key, float val) {
        wchar_t buf[32];
        swprintf_s(buf, L"%.2f", val);
        writeStr(sec, key, buf);
    };

    writeStr(L"FSR", L"QualityPreset", Widen(QualityModeName(c.mode)));
    writeFloat(L"FSR", L"RenderScale", c.customScale);
    writeFloat(L"FSR", L"Sharpness", c.sharpness);
    writeBool(L"FSR", L"AutoExposure", c.autoExposure);
    writeStr(L"FSR", L"Version", Widen(c.version));
    writeBool(L"FSR", L"Bypass", c.bypass);
    writeBool(L"AntiLag", L"AntiLag2", c.antiLag2);
    writeFloat(L"AntiLag", L"AntiLagMaxFPS", static_cast<float>(c.antiLagMaxFps));
    writeBool(L"FrameGeneration", L"FrameGen", c.frameGen);
    g_iniTime = LastWrite(g_iniPath);
}

}  // namespace mhwfsr
