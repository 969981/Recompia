// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_string.h: part of src/os/nx_os.h for the POSIX build -- code pages and string calls (os_posix_string.cpp).
#pragma once

#if !defined(NX_OS_H_PARTS)
#error "nx_os_string.h is a part of nx_os.h: include nx_os.h"
#endif

// ---- strings -----------------------------------------------------------------------------------------------------
#define CP_ACP 0
#define CP_OEMCP 1
#define CP_UTF8 65001
#define CP_UTF16 1200
#define MB_ERR_INVALID_CHARS 0x00000008
#define WC_ERR_INVALID_CHARS 0x00000080
#define WC_NO_BEST_FIT_CHARS 0x00000400

int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int src_len, LPWSTR dst, int dst_len);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int src_len, LPSTR dst, int dst_len, LPCSTR def, LPBOOL used);
int lstrlenW(LPCWSTR s);
int lstrlenA(LPCSTR s);
LPWSTR lstrcpyW(LPWSTR dst, LPCWSTR src);
int lstrcmpiW(LPCWSTR a, LPCWSTR b);
int lstrcmpiA(LPCSTR a, LPCSTR b);
LPWSTR CharUpperW(LPWSTR s);
DWORD FormatMessageW(DWORD flags, LPCVOID src, DWORD msg_id, DWORD lang, LPWSTR out, DWORD cap, void* args);
void OutputDebugStringA(LPCSTR s);
void OutputDebugStringW(LPCWSTR s);
BOOL CompareStringOrdinal(LPCWSTR a, int a_len, LPCWSTR b, int b_len, BOOL ignore_case);
