#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace mhwdlss {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
// Read without the lock by LogEnabled on every game thread.
std::atomic<LogLevel> g_minLevel{LogLevel::Info};

const char* LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DBG";
        case LogLevel::Info: return "INF";
        case LogLevel::Warn: return "WRN";
        case LogLevel::Error: return "ERR";
    }
    return "???";
}
}  // namespace

void LogInit(const std::wstring& path, LogLevel minLevel) {
    std::lock_guard lock(g_mutex);
    g_minLevel = minLevel;
    if (g_file) return;
    // Shared read so the log can be tailed while the game runs.
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

void LogSetLevel(LogLevel minLevel) {
    std::lock_guard lock(g_mutex);
    g_minLevel = minLevel;
}

bool LogEnabled(LogLevel level) { return static_cast<int>(level) >= static_cast<int>(g_minLevel.load()); }

void LogWrite(LogLevel level, std::string_view text) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::lock_guard lock(g_mutex);
    if (!g_file) return;
    std::fprintf(g_file, "%02u:%02u:%02u.%03u [%s] [%5lu] %.*s\n", st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds, LevelTag(level), GetCurrentThreadId(), static_cast<int>(text.size()),
                 text.data());
    std::fflush(g_file);
}

}  // namespace mhwdlss
