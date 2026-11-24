// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: the FidelityFX API from the SDK's signed Vulkan DLLs, loaded at run time: the FSR SDK v2.4.0 pair
// amd_fidelityfx_loader_vk.dll + amd_fidelityfx_upscaler_vk.dll (MIT, signed), or the v1.1.4 amd_fidelityfx_vk.dll; [upscale] fsr_dll names
// one explicitly, otherwise upscalers\fsr3\ (the release package) or the executable's folder is searched.
#include <windows.h>

#include <string>

#include "extras/temporal_ffx_api.h"
#include "nxrt/extras.h"
#include "nxrt/log.h"

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "fsr3";

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s + 1);
}
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The candidates, in order: [upscale] fsr_dll, then the release package's upscalers\fsr3\ folder next to the executable, then the executable's own
// folder; in each the v2.4.0 loader and the v1.1.4 single DLL.
std::wstring find_dll() {
    const std::string cfg = settings().upscale.fsr_dll;
    if (!cfg.empty()) return exists(widen(cfg)) ? widen(cfg) : std::wstring();
    const std::wstring exe = exe_dir();
    for (const std::wstring& dir : {exe + L"upscalers\\fsr3\\", exe})
        for (const wchar_t* n : {L"amd_fidelityfx_loader_vk.dll", L"amd_fidelityfx_vk.dll"})
            if (exists(dir + n)) return dir + n;
    return {};
}

}  // namespace

bool ffx_api_present() { return !find_dll().empty(); }

bool ffx_api_load(FfxApi* api) {
    const std::wstring path = find_dll();
    if (path.empty()) {
        NXLOG_W(kTag, "no FidelityFX DLL: put amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll (FSR SDK v2.4.0, signedbin) in "
                      "upscalers\\fsr3 next to the executable, or name one in [upscale] fsr_dll");
        return false;
    }
    const HMODULE mod = LoadLibraryW(path.c_str());
    if (!mod) {
        NXLOG_E(kTag, "LoadLibrary failed (error %lu)", GetLastError());
        return false;
    }
    api->create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(mod, "ffxCreateContext"));
    api->destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(mod, "ffxDestroyContext"));
    api->query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(mod, "ffxQuery"));
    api->dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(mod, "ffxDispatch"));
    if (!api->create || !api->destroy || !api->query || !api->dispatch) {
        NXLOG_E(kTag, "the DLL does not export the FidelityFX API");
        return false;
    }
    char n[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, n, sizeof n, nullptr, nullptr);
    NXLOG_I(kTag, "FidelityFX API loaded from %s", n);
    return true;
}

}  // namespace nxrt::extras
