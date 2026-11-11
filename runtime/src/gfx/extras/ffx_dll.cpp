// SPDX-License-Identifier: GPL-3.0-or-later
#include "extras/ffx_dll.h"

#include <windows.h>

#include "nxrt/extras.h"

namespace nxrt::extras {
namespace {

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

}  // namespace

std::wstring ffx_find_dll() {
    const std::string cfg = settings().upscale.fsr_dll;
    if (!cfg.empty()) return exists(widen(cfg)) ? widen(cfg) : std::wstring();
    const std::wstring exe = exe_dir();
    for (const std::wstring& dir : {exe + L"upscalers\\fsr3\\", exe})
        for (const wchar_t* n : {L"amd_fidelityfx_loader_vk.dll", L"amd_fidelityfx_vk.dll"})
            if (exists(dir + n)) return dir + n;
    return {};
}

bool ffx_effect_dll_exists(const std::wstring& loader, const wchar_t* effect_dll) {
    const size_t s = loader.find_last_of(L"\\/");
    return s != std::wstring::npos && exists(loader.substr(0, s + 1) + effect_dll);
}

}  // namespace nxrt::extras
