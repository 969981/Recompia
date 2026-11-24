// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_process.cpp: the process's surroundings for src/os/nx_os.h -- environment variables, process creation (none),
// the computer and user names, the console and message boxes, COM (none), the shell (shellapi.h: none) and the UI language.
#include "nx_os_internal.h"

// The SDK-shaped header whose entry points (ShellExecuteA, ShellExecuteExA) live in this file.
#include "shellapi.h"

#if !defined(_WIN32)

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

using namespace nxos;

// ==================================================================================================================
// environment, process creation, console
// ==================================================================================================================
DWORD nxos_get_environment(const char* name, char* out, DWORD cap) {
    const char* v = name ? getenv(name) : nullptr;
    if (!v) {
        SetLastError(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    const size_t n = std::strlen(v);
    if (!out || cap == 0) return static_cast<DWORD>(n + 1);
    if (n + 1 > cap) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return static_cast<DWORD>(n + 1);
    }
    std::memcpy(out, v, n + 1);
    return static_cast<DWORD>(n);
}

BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value) {
    if (!name) return FALSE;
    return (value ? setenv(name, value, 1) : unsetenv(name)) == 0 ? TRUE : FALSE;
}

DWORD ExpandEnvironmentStringsA(LPCSTR src, LPSTR dst, DWORD cap) {
    if (!src) return 0;
    std::string out;
    for (const char* p = src; *p;) {
        if (*p == '%') {
            const char* close = std::strchr(p + 1, '%');
            if (close) {
                const std::string name(p + 1, static_cast<size_t>(close - p - 1));
                if (const char* v = getenv(name.c_str())) out += v;
                p = close + 1;
                continue;
            }
        }
        out += *p++;
    }
    if (!dst || cap == 0) return static_cast<DWORD>(out.size() + 1);
    const size_t n = std::min<size_t>(out.size(), cap - 1);
    std::memcpy(dst, out.data(), n);
    dst[n] = 0;
    return static_cast<DWORD>(n);
}

BOOL CreateProcessW(LPCWSTR, LPWSTR, void*, void*, BOOL, DWORD, void*, LPCWSTR, STARTUPINFOW*, PROCESS_INFORMATION* pi) {
    // The one caller (host_device.cpp) probes a Steam Deck helper; there is none on macOS.
    if (pi) std::memset(pi, 0, sizeof *pi);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

BOOL GetComputerNameA(LPSTR out, LPDWORD cap) {
    char buf[256] = {0};
    if (gethostname(buf, sizeof buf - 1) != 0) return FALSE;
    const size_t n = std::strlen(buf);
    if (!out || !cap || *cap < n + 1) {
        if (cap) *cap = static_cast<DWORD>(n + 1);
        return FALSE;
    }
    std::memcpy(out, buf, n + 1);
    *cap = static_cast<DWORD>(n);
    return TRUE;
}

BOOL GetUserNameA(LPSTR out, LPDWORD cap) {
    const char* u = getenv("USER");
    if (!u) u = getenv("LOGNAME");
    if (!u) return FALSE;
    const size_t n = std::strlen(u);
    if (!out || !cap || *cap < n + 1) {
        if (cap) *cap = static_cast<DWORD>(n + 1);
        return FALSE;
    }
    std::memcpy(out, u, n + 1);
    *cap = static_cast<DWORD>(n);
    return TRUE;
}

HANDLE GetStdHandle(DWORD which) {
    int fd = STDIN_FILENO;
    if (which == STD_OUTPUT_HANDLE) fd = STDOUT_FILENO;
    else if (which == STD_ERROR_HANDLE) fd = STDERR_FILENO;
    const int dup_fd = ::dup(fd);
    return dup_fd >= 0 ? new FileObj(dup_fd) : nullptr;
}

BOOL GetConsoleMode(HANDLE, PDWORD mode) {
    if (mode) *mode = 0;
    return FALSE;
}

BOOL SetConsoleMode(HANDLE, DWORD) { return FALSE; }

int MessageBoxW(HWND, LPCWSTR text, LPCWSTR caption, UINT) {
    const std::string t = nxos_utf8(text), c = nxos_utf8(caption);
    std::fprintf(stderr, "[%s] %s\n", c.c_str(), t.c_str());
    std::fflush(stderr);
    return 1;   // IDOK
}

int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT) {
    std::fprintf(stderr, "[%s] %s\n", caption ? caption : "", text ? text : "");
    std::fflush(stderr);
    return 1;   // IDOK
}

// ==================================================================================================================
// COM: none. The launcher, the Media Foundation movie path and the XAudio2 sink are Windows-only and not built.
// ==================================================================================================================
HRESULT CoInitializeEx(void*, DWORD) { return S_OK; }
void CoUninitialize(void) {}
HRESULT CoCreateInstance(REFCLSID, void*, DWORD, REFIID, void** out) {
    if (out) *out = nullptr;
    return E_NOINTERFACE;
}

HINSTANCE ShellExecuteA(HWND, LPCSTR verb, LPCSTR file, LPCSTR params, LPCSTR dir, int) {
    (void)verb;
    (void)params;
    (void)dir;
    // src/host_device.cpp hands a URL to the Steam keyboard; there is no shell to hand it to here, and the caller
    // logs a warning and carries on. Returning 0 is Win32's "the call failed".
    if (file) SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return nullptr;
}

BOOL ShellExecuteExA(SHELLEXECUTEINFOA* info) {
    // Opening a URL or a folder through the shell. macOS has "open", but the only caller is the Steam Deck helper path
    // in src/host_device.cpp, which does not exist here: report failure so the caller logs and moves on.
    if (info) info->hProcess = nullptr;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

LANGID GetUserDefaultUILanguage(void) {
    // The UI language, from the locale the process was started with. services/common.cpp only asks which language the
    // system menu strings should be in, and the locale is what decides that on macOS.
    const char* lang = getenv("LC_ALL");
    if (!lang || !*lang) lang = getenv("LC_MESSAGES");
    if (!lang || !*lang) lang = getenv("LANG");
    if (!lang) return MAKELANGID(LANG_ENGLISH, 0);
    const auto starts = [&](const char* p) { return std::strncmp(lang, p, std::strlen(p)) == 0; };
    if (starts("ja")) return MAKELANGID(LANG_JAPANESE, 0);
    if (starts("fr")) return MAKELANGID(LANG_FRENCH, 0);
    if (starts("de")) return MAKELANGID(LANG_GERMAN, 0);
    if (starts("it")) return MAKELANGID(LANG_ITALIAN, 0);
    if (starts("es")) return MAKELANGID(LANG_SPANISH, 0);
    if (starts("ko")) return MAKELANGID(LANG_KOREAN, 0);
    if (starts("zh")) return MAKELANGID(LANG_CHINESE, 0);
    if (starts("pt")) return MAKELANGID(LANG_PORTUGUESE, 0);
    if (starts("ru")) return MAKELANGID(LANG_RUSSIAN, 0);
    if (starts("nl")) return MAKELANGID(LANG_DUTCH, 0);
    if (starts("pl")) return MAKELANGID(LANG_POLISH, 0);
    return MAKELANGID(LANG_ENGLISH, 0);
}

LANGID GetUserDefaultLangID(void) { return GetUserDefaultUILanguage(); }
LANGID GetSystemDefaultUILanguage(void) { return GetUserDefaultUILanguage(); }

#endif  // !_WIN32
