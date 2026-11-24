// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_module.cpp: modules and paths for src/os/nx_os.h -- LoadLibrary over dlopen, GetProcAddress, the module file
// name, the registry stand-in (no registry here: every key is "not found") and the wide-path fopen / freopen.
#include "nx_os_internal.h"

#if !defined(_WIN32)

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

using namespace nxos;

namespace {

pthread_mutex_t g_mod_mu = PTHREAD_MUTEX_INITIALIZER;
ModuleObj* g_main_module = nullptr;

ModuleObj* main_module() {
    pthread_mutex_lock(&g_mod_mu);
    if (!g_main_module) {
        g_main_module = new ModuleObj();
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(&main_module), &info) && info.dli_fname) g_main_module->path = info.dli_fname;
    }
    pthread_mutex_unlock(&g_mod_mu);
    return g_main_module;
}

}  // namespace

// ==================================================================================================================
// modules
// ==================================================================================================================
HMODULE nxos_load_library(const char* path, DWORD) {
    if (!path || !*path) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    std::string p = native_path(path);
    void* dl = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        // Win32 module names carry a .dll suffix; a POSIX build's plugins are .dylib/.so. Try the obvious rewrite
        // before giving up, so one configured name works on both platforms (temporal_ffx.cpp:90 loads "ffx_fsr3").
        std::string alt = p;
        const size_t dot = alt.find_last_of('.');
        const size_t slash = alt.find_last_of('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) alt.erase(dot);
#if defined(__APPLE__)
        alt += ".dylib";
#else
        alt += ".so";
#endif
        dl = dlopen(alt.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (dl) p = alt;
    }
    if (!dl) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    auto* m = new ModuleObj();
    m->dl = dl;
    m->path = p;
    return m;
}

FARPROC GetProcAddress(HMODULE mod, LPCSTR name) {
    if (!mod || !name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    auto* m = as<ModuleObj>(mod, Kind::Module);
    if (!m || !m->dl) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    void* sym = dlsym(m->dl, name);
    if (!sym) SetLastError(ERROR_PROC_NOT_FOUND);
    return reinterpret_cast<FARPROC>(sym);
}

BOOL FreeLibrary(HMODULE mod) {
    auto* m = as<ModuleObj>(mod, Kind::Module);
    if (!m) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (m->dl && m != g_main_module) dlclose(m->dl);
    m->dl = nullptr;
    release(m);
    return TRUE;
}

HMODULE GetModuleHandleA(LPCSTR name) {
    if (!name || !*name) return main_module();
    // "ntdll.dll" and friends are probed for Wine detection (host_probe.cpp, nvn_gpu.cpp, sched.cpp): report absence.
    SetLastError(ERROR_MOD_NOT_FOUND);
    return nullptr;
}

BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out) {
    if (!out) return FALSE;
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(name)), &info) && info.dli_fname) {
            *out = main_module();
            return TRUE;
        }
        SetLastError(ERROR_MOD_NOT_FOUND);
        return FALSE;
    }
    HMODULE m = GetModuleHandleA(name);
    if (!m) return FALSE;
    *out = m;
    return TRUE;
}

DWORD nxos_get_module_file_name(HMODULE mod, char* out, DWORD cap) {
    if (!out || cap == 0) return 0;
    std::string path;
    if (!mod) {
        path = main_module()->path;
#if defined(__ANDROID__)
        // An app has no program folder of its own: the runtime library sits in the read-only nativeLibraryDir. Files the runtime
        // keeps "next to the exe" (crash and freeze reports, asset folders) belong in the game folder, the working directory
        // after --game, so the program answers as if it lived there.
        char cwd[4096];
        const size_t slash = path.find_last_of('/');
        if (getcwd(cwd, sizeof cwd)) path = std::string(cwd) + "/" + (slash == std::string::npos ? path : path.substr(slash + 1));
#endif
    } else {
        auto* m = as<ModuleObj>(mod, Kind::Module);
        if (!m) {
            SetLastError(ERROR_INVALID_HANDLE);
            return 0;
        }
        path = m->path;
    }
    if (path.empty()) {
#if defined(__APPLE__)
        char buf[4096];
        uint32_t n = static_cast<uint32_t>(sizeof buf);
        if (_NSGetExecutablePath(buf, &n) == 0) path = buf;
#else
        char buf[4096];
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            path = buf;
        }
#endif
    }
    const size_t n = std::min<size_t>(path.size(), cap - 1);
    std::memcpy(out, path.data(), n);
    out[n] = 0;
    return static_cast<DWORD>(n);
}

DWORD GetModuleFileNameA(HMODULE mod, LPSTR out, DWORD cap) { return nxos_get_module_file_name(mod, out, cap); }

// ==================================================================================================================
// registry: macOS has none. Every key reports "not found" so the callers take their fallback path.
// ==================================================================================================================
LONG RegOpenKeyExA(HKEY, LPCSTR, DWORD, DWORD, PHANDLE out) {
    if (out) *out = nullptr;
    return ERROR_FILE_NOT_FOUND;
}
LONG RegQueryValueExA(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) { return ERROR_FILE_NOT_FOUND; }
LONG RegCloseKey(HKEY) { return ERROR_SUCCESS; }
LONG RegSetValueExA(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD) { return ERROR_SUCCESS; }
LONG RegCreateKeyExA(HKEY, LPCSTR, DWORD, LPSTR, DWORD, DWORD, void*, PHANDLE, LPDWORD) { return ERROR_SUCCESS; }

FILE* nxos_wfopen(const wchar_t* path, const char* mode) {
    if (!path || !mode) return nullptr;
    const std::string p = nxos_utf8(path);
    return std::fopen(p.c_str(), mode);
}

FILE* nxos_wfopen(const char* path, const char* mode) { return path && mode ? std::fopen(path, mode) : nullptr; }
FILE* nxos_wfreopen(const char* path, const char* mode, FILE* stream) {
    return path && mode ? std::freopen(path, mode, stream) : nullptr;
}
FILE* nxos_wfopen(const char* path, const wchar_t* mode) {
    const std::string m = nxos_utf8(mode);
    return nxos_wfopen(path, m.c_str());
}
FILE* nxos_wfreopen(const char* path, const wchar_t* mode, FILE* stream) {
    const std::string m = nxos_utf8(mode);
    return nxos_wfreopen(path, m.c_str(), stream);
}
FILE* nxos_wfopen(const wchar_t* path, const wchar_t* mode) {
    const std::string m = nxos_utf8(mode);
    return nxos_wfopen(path, m.c_str());
}
FILE* nxos_wfreopen(const wchar_t* path, const wchar_t* mode, FILE* stream) {
    const std::string m = nxos_utf8(mode);
    return nxos_wfreopen(path, m.c_str(), stream);
}

FILE* nxos_wfreopen(const wchar_t* path, const char* mode, FILE* stream) {
    if (!path || !mode) return nullptr;
    const std::string p = nxos_utf8(path);
    return std::freopen(p.c_str(), mode, stream);
}

#endif  // !_WIN32
