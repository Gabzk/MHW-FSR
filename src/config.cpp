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
    L"; MHW-FSR - AMD FidelityFX FSR 3.1 & FSR 4 for Monster Hunter: World (DX12)\n"
    L"; =============================================================================\n"
    L"; Quick Start:\n"
    L"; 1. In game Display Settings, enable DirectX 12 API.\n"
    L"; 2. Turn ON \"NVIDIA DLSS\" (this plugin intercepts it and runs AMD FSR instead).\n"
    L"; 3. Press F10 (or Insert) in game to change these settings live.\n"
    L"; 4. This file is hot-reloaded while the game is running.\n"
    L"\n"
    L"[General]\n"
    L"; Enabled: 1 = Plugin enabled, 0 = Disabled completely (requires game restart).\n"
    L"Enabled=1\n"
    L"\n"
    L"; ShowOSD: 1 = Show the top-left corner overlay (OSD), 0 = Hidden.\n"
    L"ShowOSD=1\n"
    L"\n"
    L"; ShowFPS: 1 = Show the FPS & frametime counter inside the OSD, 0 = Hide it.\n"
    L"ShowFPS=1\n"
    L"\n"
    L"; ShowLatencyOSD: 1 = Show the GPU/CPU latency breakdown in the OSD, 0 = Compact OSD.\n"
    L"ShowLatencyOSD=0\n"
    L"\n"
    L"; LogLevel: Detail written to nativePC\\plugins\\MHW-FSR.log (Debug, Info, Warn, Error).\n"
    L"LogLevel=Info\n"
    L"\n"
    L"[FSR]\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; QualityPreset: Internal rendering resolution.\n"
    L";   NativeAA    -> 100% native resolution, FSR replaces the game's blurry TAA.\n"
    L";                  Best image quality, same performance as the stock game.\n"
    L";   Quality     -> ~1.5x upscale (e.g. 1707x960 -> 1440p). For handhelds / older PCs.\n"
    L";   Balanced    -> ~1.7x upscale (e.g. 1506x847 -> 1440p).\n"
    L";   Performance -> 2.0x upscale (e.g. 1280x720 -> 1440p). The game never renders below 50%.\n"
    L";   Custom      -> Uses RenderScale below.\n"
    L"QualityPreset=NativeAA\n"
    L"\n"
    L"; RenderScale: Used only when QualityPreset=Custom (0.50 to 1.00).\n"
    L"RenderScale=0.75\n"
    L"\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; Version: Upscaler version from the AMD FidelityFX API.\n"
    L";   Auto      -> Recommended. Detects your GPU & driver:\n"
    L";                - RDNA 2 (RX 6000), RDNA 3 (RX 7000) & RDNA 4 (RX 9000): FSR 4.1.2 (driver ML).\n"
    L";                - Other GPUs (RX 5000, Intel, Nvidia): FSR 3.1.5 (temporal).\n"
    L";   FSR 4.1.2 -> Forces the AMD driver FSR 4.1.2 machine learning upscaler (RDNA 2/3/4).\n"
    L";   FSR 3.1.5 -> Forces the FSR 3.1.5 temporal upscaler (all DX12 GPUs).\n"
    L";   0x...     -> Exact hexadecimal provider ID listed in MHW-FSR.log.\n"
    L"Version=Auto\n"
    L"\n"
    L"; Fsr4Model: Neural network variant when running FSR 4.1.2.\n"
    L";   Auto (GPU default), Standard (FSR4-i8, best quality), RDNA2 (FSR4-i8-2, balanced),\n"
    L";   Lite (FSR4L-i8, fastest).\n"
    L"Fsr4Model=Auto\n"
    L"\n"
    L"; Sharpness: Post-upscale sharpening (AMD FidelityFX RCAS), 0.00 (off) to 1.00 (max).\n"
    L"; 0.33 is a subtle touch. \"Game\" follows the in-game \"DLSS Sharpness\" slider.\n"
    L"Sharpness=0.33\n"
    L"\n"
    L"; DLLPath: Folder with amd_fidelityfx_loader_dx12.dll & amd_fidelityfx_upscaler_dx12.dll.\n"
    L"; Leave empty to use the default: nativePC\\plugins\\MHW-FSR\\\n"
    L"DLLPath=\n"
    L"\n"
    L"[AntiLag]\n"
    L"; AntiLag2: AMD FidelityFX Anti-Lag 2 driver-level latency reduction (Radeon only).\n"
    L"AntiLag2=1\n"
    L"\n"
    L"; AntiLagMaxFPS: Framerate limiter via driver pacing (0 = Unlimited).\n"
    L"AntiLagMaxFPS=0\n";

std::wstring g_iniPath;
unsigned long long g_iniTime = 0;

std::wstring g_gameOptionsPath;
unsigned long long g_gameOptionsTime = 0;
float g_gameSharpness = 0.0f;

unsigned long long LastWrite(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
           data.ftLastWriteTime.dwLowDateTime;
}

std::wstring ReadString(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    constexpr wchar_t kMissing[] = L"\x01";
    wchar_t buf[1024];
    GetPrivateProfileStringW(section, key, kMissing, buf, static_cast<DWORD>(std::size(buf)), ini.c_str());
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
        case QualityMode::Custom: return "Custom";
    }
    return "?";
}

const char* Fsr4ModelName(Fsr4Model model) {
    switch (model) {
        case Fsr4Model::Auto: return "Auto";
        case Fsr4Model::Standard: return "Standard";
        case Fsr4Model::Rdna2: return "RDNA2";
        case Fsr4Model::Lite: return "Lite";
    }
    return "Auto";
}

void LoadConfig(const std::wstring& ini) {
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::ofstream out(ini, std::ios::binary);
        for (const wchar_t* p = kDefaultIni; *p; ++p) out.put(static_cast<char>(*p));
    }

    Config c;
    c.enabled = ReadBool(ini, L"General", L"Enabled", c.enabled);
    c.showOsd = ReadBool(ini, L"General", L"ShowOSD", c.showOsd);
    c.showFps = ReadBool(ini, L"General", L"ShowFPS", c.showFps);
    c.showLatencyOsd = ReadBool(ini, L"General", L"ShowLatencyOSD", c.showLatencyOsd);

    auto level = Lower(ReadString(ini, L"General", L"LogLevel", L"info"));
    if (level == L"debug") c.logLevel = LogLevel::Debug;
    else if (level == L"warn") c.logLevel = LogLevel::Warn;
    else if (level == L"error") c.logLevel = LogLevel::Error;
    else c.logLevel = LogLevel::Info;

    // "UltraPerformance" (removed preset) rendered at the same 50% as Performance.
    auto mode = Lower(ReadString(ini, L"FSR", L"QualityPreset", L"nativeaa"));
    if (mode == L"quality") c.mode = QualityMode::Quality;
    else if (mode == L"balanced") c.mode = QualityMode::Balanced;
    else if (mode == L"performance" || mode == L"ultraperformance") c.mode = QualityMode::Performance;
    else if (mode == L"custom") c.mode = QualityMode::Custom;
    else c.mode = QualityMode::NativeAA;

    c.customScale = std::clamp(ReadFloat(ini, L"FSR", L"RenderScale", c.customScale), 0.5f, 1.0f);

    auto sharpness = Lower(ReadString(ini, L"FSR", L"Sharpness", L""));
    if (sharpness == L"game")
        c.sharpness = -1.0f;
    else if (!sharpness.empty())
        c.sharpness = std::clamp(ReadFloat(ini, L"FSR", L"Sharpness", c.sharpness), 0.0f, 1.0f);

    c.version = Narrow(ReadString(ini, L"FSR", L"Version", L"Auto"));

    auto modelStr = Lower(ReadString(ini, L"FSR", L"Fsr4Model", L"auto"));
    if (modelStr == L"standard" || modelStr == L"fsr4-i8" || modelStr == L"int8" || modelStr == L"rdna3")
        c.fsr4Model = Fsr4Model::Standard;
    else if (modelStr == L"rdna2" || modelStr == L"fsr4-i8-2" || modelStr == L"int8-2" || modelStr == L"balanced" || modelStr == L"fast")
        c.fsr4Model = Fsr4Model::Rdna2;
    else if (modelStr == L"lite" || modelStr == L"fsr4l-i8" || modelStr == L"fsr4l")
        c.fsr4Model = Fsr4Model::Lite;
    else
        c.fsr4Model = Fsr4Model::Auto;

    c.dllPath = ReadString(ini, L"FSR", L"DLLPath", L"");

    c.antiLag2 = ReadBool(ini, L"AntiLag", L"AntiLag2", c.antiLag2);
    c.antiLagMaxFps = std::clamp(static_cast<int>(ReadFloat(ini, L"AntiLag", L"AntiLagMaxFPS", static_cast<float>(c.antiLagMaxFps))), 0, 1000);

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
}

bool ReloadGameOptionsIfChanged() {
    if (g_gameOptionsPath.empty() || LastWrite(g_gameOptionsPath) == g_gameOptionsTime) return false;
    float before = g_gameSharpness;
    LoadGameOptions(g_gameOptionsPath);
    return g_gameSharpness != before;
}

float GameSharpness() { return g_gameSharpness; }

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

    writeBool(L"General", L"ShowOSD", c.showOsd);
    writeBool(L"General", L"ShowFPS", c.showFps);
    writeBool(L"General", L"ShowLatencyOSD", c.showLatencyOsd);
    writeStr(L"FSR", L"QualityPreset", Widen(QualityModeName(c.mode)));
    writeFloat(L"FSR", L"RenderScale", c.customScale);
    if (c.sharpness < 0.0f) writeStr(L"FSR", L"Sharpness", L"Game");
    else writeFloat(L"FSR", L"Sharpness", c.sharpness);
    writeStr(L"FSR", L"Version", Widen(c.version));
    writeStr(L"FSR", L"Fsr4Model", Widen(Fsr4ModelName(c.fsr4Model)));
    writeBool(L"AntiLag", L"AntiLag2", c.antiLag2);
    wchar_t fps[16];
    swprintf_s(fps, L"%d", c.antiLagMaxFps);
    writeStr(L"AntiLag", L"AntiLagMaxFPS", fps);
    g_iniTime = LastWrite(g_iniPath);
}

}  // namespace mhwfsr
