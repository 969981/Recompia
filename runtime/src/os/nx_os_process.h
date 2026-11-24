// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_process.h: part of src/os/nx_os.h for the POSIX build -- environment and process creation, the console, the UI language and COM (os_posix_process.cpp).
#pragma once

#if !defined(NX_OS_H_PARTS)
#error "nx_os_process.h is a part of nx_os.h: include nx_os.h"
#endif

// ---- environment and process creation ----------------------------------------------------------------------------
BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value);
DWORD ExpandEnvironmentStringsA(LPCSTR src, LPSTR dst, DWORD cap);
BOOL CreateProcessW(LPCWSTR app, LPWSTR cmdline, void* pa, void* ta, BOOL inherit, DWORD flags, void* env, LPCWSTR dir,
                    STARTUPINFOW* si, PROCESS_INFORMATION* pi);
BOOL GetComputerNameA(LPSTR out, LPDWORD cap);
BOOL GetUserNameA(LPSTR out, LPDWORD cap);


// ---- console and UI (minimal: the game opens a console for logs, shows message boxes) -----------------------------
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define MB_OK 0x00000000
#define MB_ICONERROR 0x00000010
#define MB_ICONWARNING 0x00000030
#define MB_ICONINFORMATION 0x00000040
#define MB_YESNO 0x00000004
#define IDYES 6
#define IDNO 7

HANDLE GetStdHandle(DWORD which);
BOOL GetConsoleMode(HANDLE h, PDWORD mode);
BOOL SetConsoleMode(HANDLE h, DWORD mode);
int MessageBoxA(HWND owner, LPCSTR text, LPCSTR caption, UINT type);
int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);

LANGID GetUserDefaultUILanguage(void);
LANGID GetUserDefaultLangID(void);
LANGID GetSystemDefaultUILanguage(void);

#define PRIMARYLANGID(id) ((WORD)((id) & 0x3FF))
#define SUBLANGID_OF(id) ((WORD)(((id) >> 10) & 0x3F))
#define LANG_NEUTRAL 0x00
#define LANG_ENGLISH 0x09
#define LANG_FRENCH 0x0C
#define LANG_GERMAN 0x07
#define LANG_ITALIAN 0x10
#define LANG_JAPANESE 0x11
#define LANG_KOREAN 0x12
#define LANG_SPANISH 0x0A
#define LANG_CHINESE 0x04
#define LANG_PORTUGUESE 0x16
#define LANG_RUSSIAN 0x19
#define LANG_DUTCH 0x13
#define LANG_POLISH 0x15
#define SUBLANG_NEUTRAL 0x00
#define SUBLANG_ENGLISH_US 0x01
#define SUBLANG_ENGLISH_UK 0x02
#define SUBLANG_ENGLISH_CAN 0x04
#define SUBLANG_FRENCH 0x01
#define SUBLANG_FRENCH_CANADIAN 0x03
#define SUBLANG_GERMAN 0x01
#define SUBLANG_ITALIAN 0x01
#define SUBLANG_SPANISH 0x01
#define SUBLANG_SPANISH_MEXICAN 0x02
#define SUBLANG_SPANISH_MODERN 0x03
#define SUBLANG_CHINESE_HONGKONG 0x03
#define SUBLANG_CHINESE_MACAU 0x05
#define SUBLANG_JAPANESE_JAPAN 0x01
#define SUBLANG_KOREAN 0x01
#define SUBLANG_CHINESE_TRADITIONAL 0x01
#define SUBLANG_CHINESE_SIMPLIFIED 0x02
#define SUBLANG_PORTUGUESE_BRAZILIAN 0x01
#define SUBLANG_RUSSIAN_RUSSIA 0x01
#define SUBLANG_DUTCH 0x01
#define SUBLANG_POLISH_POLAND 0x01

// ---- COM (no-op: the launcher and the media/audio backends are Windows-only) -------------------------------------
#define COINIT_APARTMENTTHREADED 0x2
#define COINIT_MULTITHREADED 0x0
#define COINIT_DISABLE_OLE1DDE 0x4
#define CLSCTX_INPROC_SERVER 0x1
#define CLSCTX_ALL 0x17
#define RPC_E_CHANGED_MODE ((HRESULT)0x80010106)
#define CO_E_NOTINITIALIZED ((HRESULT)0x800401F0)

HRESULT CoInitializeEx(void* reserved, DWORD flags);
void CoUninitialize(void);
HRESULT CoCreateInstance(REFCLSID clsid, void* outer, DWORD clsctx, REFIID iid, void** out);
