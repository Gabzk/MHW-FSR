#pragma once

namespace mhwfsr {

// Logs access violations (registers and an unwound call stack) to the plugin log. The
// exception is passed on untouched.
void InstallCrashLogger();

}  // namespace mhwfsr
