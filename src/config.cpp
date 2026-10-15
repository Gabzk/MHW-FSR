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
    L"; MHW-FSR - AMD FidelityFX FSR 3.1 & FSR 4.1 para Monster Hunter: World (DX12)\n"
    L"; =============================================================================\n"
    L"; Como usar:\n"
    L"; 1. Ative a API DirectX 12 nas opcoes graficas do jogo.\n"
    L"; 2. Ative a opcao \"NVIDIA DLSS\" (o mod intercepta essa chamada e roda o FSR nativo).\n"
    L"; 3. Este arquivo e recarregado automaticamente em tempo real enquanto o jogo roda.\n"
    L"\n"
    L"[General]\n"
    L"; Enabled: 1 = Mod ativado, 0 = Desativa completamente o plugin (requer reiniciar o jogo).\n"
    L"Enabled=1\n"
    L"\n"
    L"; LogLevel: Nivel de detalhe gravado em nativePC\\plugins\\MHW-FSR.log\n"
    L"; Opcoes: Debug, Info, Warn, Error.\n"
    L"; (Use 'Debug' para ver resolucoes exatas de cada frame e diagnostico completo).\n"
    L"LogLevel=Info\n"
    L"\n"
    L"[FSR]\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; QualityPreset: Define a resolucao de renderizacao interna e modo de operacao.\n"
    L"; Opcoes:\n"
    L";   NativeAA         -> Renderiza em 100% da resolucao nativa da tela (sem upscale).\n"
    L";                       Substitui o TAA borrado do jogo pelo anti-aliasing temporal\n"
    L";                       ultra-nitido do FSR. Maxima qualidade de imagem possivel.\n"
    L";   Quality          -> Modo Qualidade (~1.5x de escala: ex. 1706x960 upscaled para 1440p).\n"
    L";                       Excelente ganho de FPS mantendo a imagem praticamente nativa.\n"
    L";   Balanced         -> Modo Equilibrado (~1.7x de escala: ex. 1506x847 upscaled para 1440p).\n"
    L";   Performance      -> Modo Desempenho (2.0x de escala: ex. 1280x720 upscaled para 1440p).\n"
    L";                       Maximo FPS possivel.\n"
    L";   UltraPerformance -> Roda a 0.5 de escala (a engine do MHW nao permite menos que 0.5).\n"
    L";   Custom           -> Permite definir uma escala personalizada no parametro RenderScale abaixo.\n"
    L"QualityPreset=Quality\n"
    L"\n"
    L"; RenderScale: Usado apenas quando QualityPreset=Custom (valores de 0.50 a 1.00).\n"
    L"RenderScale=1.0\n"
    L"\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; Version: Versao do algoritmo de upscaling da AMD FidelityFX API.\n"
    L"; Opcoes:\n"
    L";   Auto      -> Recomendado. Detecta a sua GPU automaticamente:\n"
    L";                - Em GPUs RDNA 3 (como a RX 7700 XT) e RDNA 4: roda FSR 4 (IA/ML).\n"
    L";                - Em GPUs anteriores (RDNA 1, RDNA 2, etc.): roda FSR 3.1.5 (Temporal).\n"
    L";   FSR 4     -> Forca o upscaler FSR 4 com Machine Learning (tambem aceita: 4.1, FSR4).\n"
    L";   FSR 3.1   -> Forca o upscaler temporal FSR 3.1 (tambem aceita: 3.1.5, FSR3).\n"
    L";   0x...     -> ID hexadecimal exato de um provedor listado no MHW-FSR.log.\n"
    L"Version=Auto\n"
    L"\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; Sharpness: Filtro de nitidez adaptativo integrado (AMD FidelityFX RCAS).\n"
    L";   0.00      -> Nitidez desligada.\n"
    L";   0.33      -> Toque suave de nitidez (recomendado para 1440p / 4K).\n"
    L";   0.66      -> Nitidez moderada.\n"
    L";   1.00      -> Nitidez maxima.\n"
    L";   Game      -> Segue o valor da barra \"Nitidez do DLSS\" do menu grafico do jogo.\n"
    L"Sharpness=0.33\n"
    L"\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; AutoExposure: Estimativa de exposicao automatica pelo FSR.\n"
    L";   0 = Desligado (usa os buffers de exposicao e iluminacao nativos da engine MT Framework).\n"
    L";   1 = Ligado (permite que o FSR calcule sua propria exposicao dinamica).\n"
    L"AutoExposure=0\n"
    L"\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; DLLPath: Pasta contendo amd_fidelityfx_loader_dx12.dll e amd_fidelityfx_upscaler_dx12.dll.\n"
    L"; Deixe em branco para usar o padrao seguro: nativePC\\plugins\\MHW-FSR\\\n"
    L"DLLPath=\n"
    L"\n"
    L"; -----------------------------------------------------------------------------\n"
    L"; Bypass: Modo de comparacao (util apenas quando QualityPreset=NativeAA).\n"
    L";   0 = FSR ativo.\n"
    L";   1 = Mostra o TAA antigo da Capcom para comparar a diferenca de nitidez lado a lado.\n"
    L"Bypass=0\n";

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

}  // namespace mhwfsr
