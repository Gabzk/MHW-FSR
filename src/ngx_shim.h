#pragma once

#include <string>

namespace mhwfsr::ngx {

// Redirects the game's NGX 1.x calls to the shim and initializes FSR backend.
bool Install(const std::wstring& dataDir, const std::wstring& gameDir);

}  // namespace mhwfsr::ngx
