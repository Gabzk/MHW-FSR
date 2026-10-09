#pragma once

#include <cstdint>
#include <string>

namespace mhwdlss {

// Picks the nvngx_dlss.dll that NGX loads and returns the folder that holds it under that name.
// DLLPath from the ini wins when set (a folder holding nvngx_dlss.dll, or a DLL file of any name).
// Otherwise the newest DLSS the NVIDIA driver has downloaded is used, unless nvngx_dlss.dll in
// `dataDir` is newer. Files that NGX cannot load where they are (the driver stores DLSS as
// <id>.bin) are copied to a subfolder of `dataDir` first, and again only when they change.
std::wstring SelectDlssFolder(const std::wstring& dataDir, const std::wstring& gameDir);

// File version of a DLL packed as major.minor.build.revision in 16 bits each; 0 when it has none.
uint64_t DllVersion(const std::wstring& path);
std::string VersionString(uint64_t version);

}  // namespace mhwdlss
