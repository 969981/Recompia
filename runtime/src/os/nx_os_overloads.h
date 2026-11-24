// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_overloads.h: part of src/os/nx_os.h for the POSIX build -- the C++ overloads for the path and name parameters, and the inline helpers over the parts before it.
#pragma once

#if !defined(NX_OS_H_PARTS)
#error "nx_os_overloads.h is a part of nx_os.h: include nx_os.h"
#endif

#if defined(__cplusplus)

// ---- C++ overloads: the path/name parameters ----------------------------------------------------------------
#include <cstring>   // std::strchr: libstdc++'s <string.h> leaves the C functions in the global namespace only
#include <string>

std::string nxos_utf8(const char* s);
std::string nxos_utf8(const wchar_t* s);

// The folder a program keeps its per-user data in, without a trailing separator: $HOME/Library/Application Support on macOS and
// Linux (".", not "", when HOME is unset); on Android the working directory, which is the game folder the runtime was started in
// (--game), because an app's private storage is out of the player's reach. The services' save folder default builds on it.
std::string nxos_app_data_dir();

NX_OS_INLINE HANDLE CreateFileW(LPCWSTR p, DWORD a, DWORD sh, void* sa, DWORD d, DWORD f, HANDLE t) {
    (void)t;
    return nxos_create_file(nxos_utf8(p).c_str(), a, sh, sa, d, f);
}
NX_OS_INLINE HANDLE CreateFileW(LPCSTR p, DWORD a, DWORD sh, void* sa, DWORD d, DWORD f, HANDLE t) {
    (void)t;
    return nxos_create_file(p, a, sh, sa, d, f);
}
NX_OS_INLINE HANDLE CreateFileA(LPCSTR p, DWORD a, DWORD sh, void* sa, DWORD d, DWORD f, HANDLE t) {
    (void)t;
    return nxos_create_file(p, a, sh, sa, d, f);
}
NX_OS_INLINE HANDLE CreateFileA(LPCWSTR p, DWORD a, DWORD sh, void* sa, DWORD d, DWORD f, HANDLE t) {
    (void)t;
    return nxos_create_file(nxos_utf8(p).c_str(), a, sh, sa, d, f);
}
NX_OS_INLINE BOOL DeleteFileW(LPCWSTR p) { return nxos_delete_file(nxos_utf8(p).c_str()); }
NX_OS_INLINE BOOL DeleteFileW(LPCSTR p) { return nxos_delete_file(p); }
NX_OS_INLINE BOOL DeleteFileA(LPCSTR p) { return nxos_delete_file(p); }
NX_OS_INLINE BOOL MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD f) {
    const std::string x = nxos_utf8(a), y = nxos_utf8(b);
    return nxos_move_file(x.c_str(), y.c_str(), f);
}
NX_OS_INLINE BOOL MoveFileExW(LPCSTR a, LPCSTR b, DWORD f) { return nxos_move_file(a, b, f); }
NX_OS_INLINE BOOL MoveFileW(LPCWSTR a, LPCWSTR b) { return MoveFileExW(a, b, 0); }
NX_OS_INLINE BOOL MoveFileExA(LPCSTR a, LPCSTR b, DWORD f) { return nxos_move_file(a, b, f); }
NX_OS_INLINE BOOL MoveFileA(LPCSTR a, LPCSTR b) { return nxos_move_file(a, b, 0); }
NX_OS_INLINE BOOL CopyFileA(LPCSTR a, LPCSTR b, BOOL fail_if_exists) { return nxos_copy_file(a, b, fail_if_exists); }
NX_OS_INLINE BOOL CreateDirectoryA(LPCSTR p, void* sa) { (void)sa; return nxos_create_directory(p) != INVALID_HANDLE_VALUE; }
NX_OS_INLINE BOOL RemoveDirectoryA(LPCSTR p) { return nxos_remove_directory(p); }
NX_OS_INLINE DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD cap, LPCSTR file) {
    return nxos_private_profile_string(section, key, def, out, cap, file);
}
NX_OS_INLINE DWORD GetFullPathNameA(LPCSTR p, DWORD cap, LPSTR out, LPSTR* file_part) {
    if (file_part) *file_part = nullptr;
    return nxos_full_path(p, out, cap);
}
NX_OS_INLINE DWORD GetFullPathNameW(LPCWSTR p, DWORD cap, LPWSTR out, LPWSTR* file_part) {
    const std::string narrow = nxos_utf8(p);
    std::string buf(cap ? cap : 1, '\0');
    const DWORD n = nxos_full_path(narrow.c_str(), buf.data(), cap);
    if (file_part) *file_part = nullptr;
    if (n == 0 || n >= cap) return n;
    return static_cast<DWORD>(MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), static_cast<int>(n), out, static_cast<int>(cap)));
}
NX_OS_INLINE BOOL CopyFileW(LPCWSTR a, LPCWSTR b, BOOL fail_if_exists) {
    const std::string x = nxos_utf8(a), y = nxos_utf8(b);
    return nxos_copy_file(x.c_str(), y.c_str(), fail_if_exists);
}
NX_OS_INLINE BOOL CopyFileW(LPCSTR a, LPCSTR b, BOOL fail_if_exists) { return nxos_copy_file(a, b, fail_if_exists); }
NX_OS_INLINE DWORD GetFileAttributesW(LPCWSTR p) { return nxos_get_file_attributes(nxos_utf8(p).c_str()); }
NX_OS_INLINE DWORD GetFileAttributesW(LPCSTR p) { return nxos_get_file_attributes(p); }
NX_OS_INLINE DWORD GetFileAttributesA(LPCSTR p) { return nxos_get_file_attributes(p); }
NX_OS_INLINE BOOL GetFileAttributesExW(LPCSTR path, int level, WIN32_FILE_ATTRIBUTE_DATA* data) {
    return nxos_get_file_attributes_ex(path, data);
}
NX_OS_INLINE BOOL SetFileAttributesW(LPCWSTR p, DWORD a) { return nxos_set_file_attributes(nxos_utf8(p).c_str(), a); }
NX_OS_INLINE BOOL SetFileAttributesW(LPCSTR p, DWORD a) { return nxos_set_file_attributes(p, a); }
NX_OS_INLINE HANDLE FindFirstFileExW(LPCWSTR pattern, int level, WIN32_FIND_DATAW* data, int search, void* filter, DWORD flags) {
    (void)level; (void)search; (void)filter; (void)flags;
    return nxos_find_first(nxos_utf8(pattern).c_str(), data);
}
NX_OS_INLINE HANDLE FindFirstFileExW(LPCSTR pattern, int level, WIN32_FIND_DATAW* data, int search, void* filter, DWORD flags) {
    (void)level; (void)search; (void)filter; (void)flags;
    return nxos_find_first(pattern, data);
}
NX_OS_INLINE HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data) {
    return nxos_find_first(nxos_utf8(pattern).c_str(), data);
}
NX_OS_INLINE HANDLE CreateDirectoryW(LPCWSTR p, void* sa) { (void)sa; return nxos_create_directory(nxos_utf8(p).c_str()); }
NX_OS_INLINE HANDLE CreateDirectoryW(LPCSTR p, void* sa) { (void)sa; return nxos_create_directory(p); }
NX_OS_INLINE BOOL RemoveDirectoryW(LPCWSTR p) { return nxos_remove_directory(nxos_utf8(p).c_str()); }
NX_OS_INLINE BOOL RemoveDirectoryW(LPCSTR p) { return nxos_remove_directory(p); }
// The process working directory: chdir on the UTF-8 path, with the Win32 separators the callers build converted.
NX_OS_INLINE BOOL SetCurrentDirectoryW(LPCWSTR p) {
    std::string path = nxos_utf8(p);
    for (char& c : path)
        if (c == '\\') c = '/';
    return ::chdir(path.c_str()) == 0;
}
NX_OS_INLINE BOOL SetCurrentDirectoryW(LPCSTR p) {
    std::string path = p ? p : "";
    for (char& c : path)
        if (c == '\\') c = '/';
    return ::chdir(path.c_str()) == 0;
}
// The MSVC CRT's _putenv("NAME=value"): setenv, and unsetenv when the value is empty.
NX_OS_INLINE int _putenv(const char* s) {
    if (!s) return -1;
    const char* eq = std::strchr(s, '=');
    if (!eq) return -1;
    const std::string name(s, static_cast<size_t>(eq - s));
    return eq[1] ? ::setenv(name.c_str(), eq + 1, 1) : ::unsetenv(name.c_str());
}
NX_OS_INLINE HMODULE LoadLibraryW(LPCWSTR p) { return nxos_load_library(nxos_utf8(p).c_str(), 0); }
NX_OS_INLINE HMODULE LoadLibraryW(LPCSTR p) { return nxos_load_library(p, 0); }
NX_OS_INLINE HMODULE LoadLibraryA(LPCSTR p) { return nxos_load_library(p, 0); }
NX_OS_INLINE HMODULE LoadLibraryExW(LPCWSTR p, HANDLE f, DWORD flags) { (void)f; return nxos_load_library(nxos_utf8(p).c_str(), flags); }
NX_OS_INLINE HMODULE LoadLibraryExW(LPCSTR p, HANDLE f, DWORD flags) { (void)f; return nxos_load_library(p, flags); }
NX_OS_INLINE DWORD GetModuleFileNameW(HMODULE mod, LPWSTR out, DWORD cap) {
    std::string buf(cap, '\0');
    const DWORD n = nxos_get_module_file_name(mod, buf.data(), cap);
    if (n == 0) return 0;
    const int w = MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), static_cast<int>(n), out, static_cast<int>(cap));
    return w > 0 ? static_cast<DWORD>(w) : 0;
}
NX_OS_INLINE HMODULE GetModuleHandleW(LPCWSTR name) {
    if (!name || !*name) return GetModuleHandleA(nullptr);
    (void)name;   // there is no ntdll here: the callers that ask for it want its functions (NtDelayExecution, NtQuerySystemInformation, RtlWaitOnAddress), so report absence
    SetLastError(ERROR_MOD_NOT_FOUND);
    return nullptr;
}
NX_OS_INLINE BOOL FindClose(HANDLE h) { return CloseHandle(h); }
NX_OS_INLINE BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* data) { return nxos_find_next(h, data); }
NX_OS_INLINE HANDLE CreateMutexW(void* sa, BOOL owner, LPCWSTR name) {
    return nxos_create_mutex(name ? nxos_utf8(name).c_str() : nullptr, owner);
}
NX_OS_INLINE HANDLE CreateMutexW(void* sa, BOOL owner, LPCSTR name) { (void)sa; return nxos_create_mutex(name, owner); }
NX_OS_INLINE HANDLE CreateMutexA(void* sa, BOOL owner, LPCSTR name) { (void)sa; return nxos_create_mutex(name, owner); }
NX_OS_INLINE HANDLE CreateEventW(void* sa, BOOL manual, BOOL initial, LPCWSTR name) {
    (void)sa;
    return nxos_create_event(name ? nxos_utf8(name).c_str() : nullptr, manual, initial);
}
NX_OS_INLINE HANDLE CreateWaitableTimerExW(void* sa, LPCWSTR name, DWORD flags, DWORD access) {
    (void)sa;
    return nxos_create_waitable_timer(name ? nxos_utf8(name).c_str() : nullptr, flags, access);
}
NX_OS_INLINE DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR out, DWORD cap) {
    return nxos_get_environment(name, out, cap);
}
NX_OS_INLINE LONG RegGetValueA(HKEY key, LPCSTR subkey, LPCSTR value, DWORD flags, LPDWORD type, LPVOID data, LPDWORD cap) {
    (void)key; (void)subkey; (void)value; (void)flags; (void)type; (void)data;
    if (cap) *cap = 0;
    return ERROR_FILE_NOT_FOUND;
}

#endif  // __cplusplus
