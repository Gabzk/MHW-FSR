#pragma once

#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

namespace mhwfsr::loader {

struct FsrVersionInfo {
    uint64_t id = 0;
    std::string name;
};

// Initializes the FSR loader DLL paths. Call during plugin startup.
bool Init(const std::wstring& pluginDir, const std::wstring& customDllPath);

// Queries available upscaler versions from the loaded backend on the given D3D12 device.
std::vector<FsrVersionInfo> QueryVersions(ID3D12Device* device);

// Checks if FSR 4 (DirectML/ML-based) is supported by the hardware and driver.
bool HasFsr4Support(ID3D12Device* device);

// Checks if AMD FidelityFX Frame Generation is supported by the hardware and runtime.
bool HasFrameGenSupport(ID3D12Device* device);

// Resolves a version name or config string (e.g., "Auto", "FSR3", "FSR4", or hex ID) to an override ID.
// Returns 0 for "Auto" (default behavior).
uint64_t ResolveVersionId(ID3D12Device* device, const std::string& versionStr);

}  // namespace mhwfsr::loader
