#include "dlss_dll.h"

#include <windows.h>

#include <filesystem>
#include <format>
#include <vector>

#include "config.h"
#include "log.h"

namespace fs = std::filesystem;

namespace mhwdlss {
namespace {

constexpr wchar_t kDllName[] = L"nvngx_dlss.dll";

// The driver's store also holds the DLSS 1 and 2 builds it downloaded for other games. DLSS 3 is
// the oldest taken from there.
constexpr uint64_t kMinDriverVersion = 3ull << 48;

struct Dll {
    fs::path path;
    uint64_t version = 0;
};

// NVIDIA's model store in ProgramData\NVIDIA\NGX\models. The driver downloads DLSS there for its
// per-game updates and for the NVIDIA App's DLSS override, as dlss\versions\<v>\files\<id>.bin.
Dll NewestDriverDlss() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    fs::path root = n && n < MAX_PATH ? fs::path(buf) : fs::path(L"C:\\ProgramData");
    root /= L"NVIDIA\\NGX\\models\\dlss\\versions";

    Dll best;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || _wcsicmp(it->path().extension().c_str(), L".bin") != 0) continue;
        uint64_t version = DllVersion(it->path().wstring());
        LOGD("driver store: {} {}", it->path().string(), VersionString(version));
        if (version >= kMinDriverVersion && version > best.version) best = {it->path(), version};
    }
    return best;
}

bool SameFile(const WIN32_FILE_ATTRIBUTE_DATA& a, const WIN32_FILE_ATTRIBUTE_DATA& b) {
    return a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
           CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
}

// Copies `source` to `dir`\nvngx_dlss.dll unless the copy there is the same file already (size and
// write time; copying keeps the time). Returns false when `dir` ends up without a copy.
bool Stage(const fs::path& source, const fs::path& dir) {
    fs::path target = dir / kDllName;
    WIN32_FILE_ATTRIBUTE_DATA src{}, dst{};
    if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &src)) {
        LOGW("cannot read {} (error {})", source.string(), GetLastError());
        return false;
    }
    bool present = GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &dst) != 0;
    if (present && SameFile(src, dst)) return true;

    std::error_code ec;
    fs::create_directories(dir, ec);
    // Through a temporary name, so that an interrupted copy never leaves a broken DLL behind.
    fs::path temp = dir / L"nvngx_dlss.dll.tmp";
    DWORD error = ERROR_SUCCESS;
    if (!CopyFileW(source.c_str(), temp.c_str(), FALSE))
        error = GetLastError();
    else if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
        error = GetLastError();
    if (error == ERROR_SUCCESS) {
        LOGD("copied {} to {}", source.string(), target.string());
        return true;
    }
    DeleteFileW(temp.c_str());
    LOGW("cannot copy {} to {} (error {}){}", source.string(), target.string(), error,
         present ? ", using the older copy there" : "");
    return present;
}

}  // namespace

uint64_t DllVersion(const std::wstring& path) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return 0;
    std::vector<uint8_t> data(size);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &len) || !fixed)
        return 0;
    return (static_cast<uint64_t>(fixed->dwFileVersionMS) << 32) | fixed->dwFileVersionLS;
}

std::string VersionString(uint64_t version) {
    if (!version) return "(no version)";
    return std::format("{}.{}.{}.{}", (version >> 48) & 0xffff, (version >> 32) & 0xffff, (version >> 16) & 0xffff,
                       version & 0xffff);
}

std::wstring SelectDlssFolder(const std::wstring& dataDirName, const std::wstring& gameDir) {
    fs::path dataDir(dataDirName);
    std::error_code ec;

    if (!g_config.dllPath.empty()) {
        fs::path p(g_config.dllPath);
        if (p.is_relative()) p = fs::path(gameDir) / p;
        p = p.lexically_normal();
        if (fs::is_directory(p, ec)) {
            fs::path dll = p / kDllName;
            if (fs::is_regular_file(dll, ec)) {
                LOGI("nvngx_dlss.dll: {} {} (DLLPath)", dll.string(), VersionString(DllVersion(dll.wstring())));
                return p.wstring();
            }
            LOGW("DLLPath {} holds no nvngx_dlss.dll", p.string());
        } else if (fs::is_regular_file(p, ec)) {
            // NGX only loads files called nvngx_dlss.dll; others are copied under that name.
            bool named = _wcsicmp(p.filename().c_str(), kDllName) == 0;
            fs::path folder = named ? p.parent_path() : dataDir / L"custom";
            if (named || Stage(p, folder)) {
                LOGI("nvngx_dlss.dll: {} {} (DLLPath)", p.string(), VersionString(DllVersion(p.wstring())));
                return folder.wstring();
            }
        } else {
            LOGW("DLLPath {} does not exist", p.string());
        }
        LOGW("DLLPath is not usable, using the driver's DLSS instead");
    }

    Dll driver = NewestDriverDlss();
    fs::path local = dataDir / kDllName;
    bool hasLocal = fs::is_regular_file(local, ec);
    uint64_t localVersion = hasLocal ? DllVersion(local.wstring()) : 0;

    if (driver.version && driver.version >= localVersion) {
        fs::path folder = dataDir / L"driver";
        if (Stage(driver.path, folder)) {
            LOGI("nvngx_dlss.dll: {} from the driver ({})", VersionString(driver.version), driver.path.string());
            if (hasLocal)
                LOGD("{} is {} and stays unused", local.string(), VersionString(localVersion));
            return folder.wstring();
        }
    }
    if (hasLocal) {
        LOGI("nvngx_dlss.dll: {} {} ({})", local.string(), VersionString(localVersion),
             driver.version ? "newer than the driver's " + VersionString(driver.version)
                            : std::string("the driver has no DLSS 3 or newer"));
        return dataDir.wstring();
    }
    LOGE("no nvngx_dlss.dll: the driver has not downloaded DLSS 3 or newer (ProgramData\\NVIDIA\\NGX\\models\\dlss) "
         "and {} does not exist. Set DLLPath in the ini to a nvngx_dlss.dll.",
         local.string());
    return dataDir.wstring();
}

}  // namespace mhwdlss
