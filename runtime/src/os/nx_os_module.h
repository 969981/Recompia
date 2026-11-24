// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_module.h: part of src/os/nx_os.h for the POSIX build -- modules and symbols, the registry stand-in (os_posix_module.cpp).
#pragma once

#if !defined(NX_OS_H_PARTS)
#error "nx_os_module.h is a part of nx_os.h: include nx_os.h"
#endif

// ---- modules and symbols -----------------------------------------------------------------------------------------
HMODULE GetModuleHandleA(LPCSTR name);
FARPROC GetProcAddress(HMODULE mod, LPCSTR name);
BOOL FreeLibrary(HMODULE mod);
DWORD GetModuleFileNameA(HMODULE mod, LPSTR out, DWORD cap);
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out);

#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x00000001
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x00000008

// ---- registry (none on macOS: the callers fall back) -------------------------------------------------------------
#define HKEY_CLASSES_ROOT ((HKEY)(uintptr_t)0x80000000)
#define HKEY_CURRENT_USER ((HKEY)(uintptr_t)0x80000001)
#define HKEY_LOCAL_MACHINE ((HKEY)(uintptr_t)0x80000002)
#define HKEY_USERS ((HKEY)(uintptr_t)0x80000003)
#define HKEY_CURRENT_CONFIG ((HKEY)(uintptr_t)0x80000005)

#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#define KEY_ALL_ACCESS 0xF003F
#define KEY_WOW64_64KEY 0x0100
#define KEY_WOW64_32KEY 0x0200
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_MULTI_SZ 7
#define REG_QWORD 11
#define RRF_RT_REG_SZ 0x00000002
#define RRF_RT_REG_DWORD 0x00000010
#define RRF_RT_REG_QWORD 0x00000040
#define RRF_RT_ANY 0x0000FFFF
#define RRF_SUBKEY_WOW6464KEY 0x00010000
#define RRF_SUBKEY_WOW6432KEY 0x00020000

LONG RegGetValueA(HKEY key, LPCSTR subkey, LPCSTR value, DWORD flags, LPDWORD type, LPVOID data, LPDWORD cap);
LONG RegOpenKeyExA(HKEY key, LPCSTR subkey, DWORD options, DWORD access, PHANDLE out);
LONG RegQueryValueExA(HKEY key, LPCSTR value, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD cap);
LONG RegCloseKey(HKEY key);
LONG RegSetValueExA(HKEY key, LPCSTR value, DWORD reserved, DWORD type, const BYTE* data, DWORD cap);
LONG RegCreateKeyExA(HKEY key, LPCSTR subkey, DWORD reserved, LPSTR cls, DWORD options, DWORD access, void* sa, PHANDLE out, LPDWORD disp);
