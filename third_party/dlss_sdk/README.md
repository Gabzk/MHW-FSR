# NVIDIA DLSS SDK (not in the repository)

The NVIDIA RTX SDKs licence does not allow redistributing the SDK, so its files are not committed.
Get the DLSS SDK from https://github.com/NVIDIA/DLSS (built and tested with tag `v310.6.0`; later
310.x tags should work) and copy:

```
include/*.h                                     -> third_party/dlss_sdk/include/
lib/Windows_x86_64/x64/nvsdk_ngx_s.lib          -> third_party/dlss_sdk/lib/
lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib      -> third_party/dlss_sdk/lib/
LICENSE.txt                                     -> third_party/dlss_sdk/
```

`nvsdk_ngx_s.lib` is the static NGX library built against the static CRT, which the plugin also
uses (see `CMakeLists.txt`). `tools/package.ps1` copies `LICENSE.txt` into the release notices.
The plugin does not ship `nvngx_dlss.dll`; it uses the one the NVIDIA driver downloads.
