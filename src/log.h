#pragma once

#include <format>
#include <string>
#include <string_view>

namespace mhwdlss {

enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void LogInit(const std::wstring& path, LogLevel minLevel);
void LogSetLevel(LogLevel minLevel);
bool LogEnabled(LogLevel level);
void LogWrite(LogLevel level, std::string_view text);

template <typename... Args>
void Log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    if (!LogEnabled(level)) return;
    LogWrite(level, std::format(fmt, std::forward<Args>(args)...));
}

#define LOGD(...) ::mhwdlss::Log(::mhwdlss::LogLevel::Debug, __VA_ARGS__)
#define LOGI(...) ::mhwdlss::Log(::mhwdlss::LogLevel::Info, __VA_ARGS__)
#define LOGW(...) ::mhwdlss::Log(::mhwdlss::LogLevel::Warn, __VA_ARGS__)
#define LOGE(...) ::mhwdlss::Log(::mhwdlss::LogLevel::Error, __VA_ARGS__)

}  // namespace mhwdlss
