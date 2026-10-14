// Developer tool, not part of the plugin: loads RenderDoc into the game before its renderer exists,
// so frames can be captured without launching the game through RenderDoc (the exe's Steam DRM stub
// restarts the game through Steam, and the launcher loses it).
//
// Put MHW-Capture.dll into nativePC\plugins. The plugin folder has to be loaded before the game
// creates its D3D device, which SharpPluginLoader 2.0's early load does (see CLAUDE.md). Then:
//   echo 1 > nativePC\plugins\MHW-Capture.trigger    capture the next frame (or N frames)
// Captures go to nativePC\plugins\MHW-Capture\mhw_*.rdc, each one is logged to MHW-Capture.log.
// RENDERDOC_DLL overrides the renderdoc.dll path (default: Program Files\RenderDoc).
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "renderdoc_app.h"

namespace {

std::wstring g_dir;   // folder of this DLL
std::wstring g_stem;  // file name without extension
RENDERDOC_API_1_6_0* g_rdoc = nullptr;
FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Polls for the trigger file; its content is the number of frames to capture (default 1).
DWORD WINAPI Watch(void*) {
    const std::wstring trigger = g_dir + L"\\" + g_stem + L".trigger";
    uint32_t known = g_rdoc->GetNumCaptures();
    for (;;) {
        Sleep(100);
        uint32_t count = g_rdoc->GetNumCaptures();
        for (; known < count; ++known) {
            char path[1024] = {};
            uint32_t len = sizeof(path);
            uint64_t stamp = 0;
            if (g_rdoc->GetCapture(known, path, &len, &stamp)) Log("capture %u written: %s", known, path);
        }
        if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        int frames = 1;
        if (FILE* f = _wfopen(trigger.c_str(), L"rb")) {
            if (std::fscanf(f, "%d", &frames) != 1) frames = 1;
            std::fclose(f);
        }
        DeleteFileW(trigger.c_str());
        if (frames < 1) frames = 1;
        if (frames > 30) frames = 30;
        Log("trigger: capturing %d frame(s)", frames);
        g_rdoc->TriggerMultiFrameCapture(static_cast<uint32_t>(frames));
    }
}

void Start(HMODULE module) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(module, buf, MAX_PATH);
    std::wstring path(buf);
    size_t slash = path.find_last_of(L"\\/");
    g_dir = path.substr(0, slash);
    std::wstring file = path.substr(slash + 1);
    g_stem = file.substr(0, file.find_last_of(L'.'));
    g_log = _wfopen((g_dir + L"\\" + g_stem + L".log").c_str(), L"w");

    std::wstring dll = L"C:\\Program Files\\RenderDoc\\renderdoc.dll";
    if (GetEnvironmentVariableW(L"RENDERDOC_DLL", buf, MAX_PATH)) dll = buf;
    if (GetModuleHandleW(L"d3d12.dll") || GetModuleHandleW(L"d3d11.dll"))
        Log("note: d3d12/d3d11 already loaded; a device created before this point is not captured");

    HMODULE rdoc = LoadLibraryW(dll.c_str());
    if (!rdoc) {
        Log("LoadLibrary %s failed (%lu)", Narrow(dll).c_str(), GetLastError());
        return;
    }
    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(rdoc, "RENDERDOC_GetAPI"));
    if (!getApi || !getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&g_rdoc)) || !g_rdoc) {
        Log("RENDERDOC_GetAPI 1.6.0 failed");
        return;
    }
    int major = 0, minor = 0, patch = 0;
    g_rdoc->GetAPIVersion(&major, &minor, &patch);

    std::wstring out = g_dir + L"\\" + g_stem;
    CreateDirectoryW(out.c_str(), nullptr);
    g_rdoc->SetCaptureFilePathTemplate((Narrow(out) + "\\mhw").c_str());
    g_rdoc->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 0);
    g_rdoc->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation, 0);
    g_rdoc->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 0);
    g_rdoc->SetCaptureKeys(nullptr, 0);  // no F12/PrtScn: captures come from the trigger file
    g_rdoc->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    Log("RenderDoc %d.%d.%d loaded from %s; captures go to %s", major, minor, patch, Narrow(dll).c_str(),
        Narrow(out).c_str());

    if (HANDLE t = CreateThread(nullptr, 0, &Watch, nullptr, 0, nullptr)) CloseHandle(t);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Start(module);
    }
    return TRUE;
}
