// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_types.h: part of src/os/nx_os.h for the POSIX build -- the calling conventions, the Win32 integer and character types, status codes, handles and error codes (os_posix.cpp).
#pragma once

#if !defined(NX_OS_H_PARTS)
#error "nx_os_types.h is a part of nx_os.h: include nx_os.h"
#endif

// ---- calling conventions, attributes, decorations (all empty) ------------------------------------------------------
#define WINAPI
#define APIENTRY
#define CALLBACK
#define NTAPI
#define WINBASEAPI
#define STDAPICALLTYPE
#define STDMETHODCALLTYPE
#define STDMETHODIMP
#define STDAPI
#define DECLSPEC_NORETURN
#define WINAPIV
#ifndef __cdecl
#define __cdecl
#endif
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __fastcall
#define __fastcall
#endif
#ifndef _cdecl
#define _cdecl
#endif
#ifndef _stdcall
#define _stdcall
#endif
#define WINAPI_FAMILY_PARTITION(x) 1

#if defined(__cplusplus)
#define NX_OS_INLINE inline
#else
#define NX_OS_INLINE static inline
#endif

// ---- integer and character types (Win32 widths, not the LP64 widths) ----------------------------------------------
typedef uint8_t BYTE;
typedef BYTE* PBYTE;
typedef BYTE* LPBYTE;
typedef uint16_t WORD;
typedef WORD* PWORD;
typedef WORD* LPWORD;
typedef uint32_t DWORD;
typedef DWORD* PDWORD;
typedef DWORD* LPDWORD;
typedef int32_t LONG;
typedef LONG* PLONG;
typedef LONG* LPLONG;
typedef uint32_t ULONG;
typedef ULONG* PULONG;
typedef uint64_t ULONGLONG;
typedef ULONGLONG* PULONGLONG;
typedef int64_t LONGLONG;
typedef uint64_t DWORD64;
typedef uint64_t ULONG64;
typedef int64_t LONG64;
typedef uint32_t DWORD32;
typedef int64_t* PLONGLONG;
typedef int32_t INT;
typedef uint32_t UINT;
typedef UINT* PUINT;
typedef int16_t SHORT;
typedef uint16_t USHORT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef uint16_t ATOM;
typedef uint32_t COLORREF;
typedef uint32_t LCID;
typedef uint16_t LANGID;
#define SUBLANGID(id) ((WORD)(id))
#define MAKELANGID(p, s) ((WORD)(((WORD)(s) << 10) | (WORD)(p)))
typedef int BOOL;
typedef BOOL* PBOOL;
typedef BOOL* LPBOOL;
typedef unsigned char BOOLEAN;
typedef BOOLEAN* PBOOLEAN;
typedef size_t SIZE_T;
typedef size_t* PSIZE_T;
typedef intptr_t SSIZE_T;
typedef intptr_t INT_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t UINT_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t DWORD_PTR;
typedef DWORD_PTR* PDWORD_PTR;
typedef UINT_PTR* PUINT_PTR;
typedef ULONG_PTR* PULONG_PTR;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef int32_t HRESULT;
typedef int32_t NTSTATUS;
typedef HRESULT* PHRESULT;
typedef NTSTATUS* PNTSTATUS;

typedef void* LPVOID;
typedef void* PVOID;
typedef const void* LPCVOID;
typedef const void* PCVOID;
typedef char* LPSTR;
typedef char* PSTR;
typedef char* PCH;
typedef const char* LPCSTR;
typedef const char* PCSTR;
typedef const char* PCCH;
typedef wchar_t* LPWSTR;
typedef wchar_t* PWSTR;
typedef wchar_t* PWCHAR;
typedef const wchar_t* LPCWSTR;
typedef const wchar_t* PCWSTR;
typedef const wchar_t* PCWCHAR;

typedef void* HANDLE;
typedef HANDLE* PHANDLE;
typedef HANDLE* LPHANDLE;
typedef void* HMODULE;
typedef void* HINSTANCE;
typedef void* HLOCAL;
typedef void* HGLOBAL;
typedef void* HWND;
typedef void* HDC;
typedef void* HBRUSH;
typedef void* HFONT;
typedef void* HICON;
typedef void* HCURSOR;
typedef void* HMENU;
typedef void* HKEY;
typedef void* HMONITOR;
typedef void* HCRYPTMSG;
typedef void* HCERTSTORE;
typedef void* HCRYPTPROV;
typedef void* LSA_HANDLE;
typedef void* PSID;
typedef void* PACL;
typedef int (*FARPROC)(void);
typedef int (*PROC)(void);
typedef DWORD (*PTHREAD_START_ROUTINE)(LPVOID);

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    struct {
        DWORD LowPart;
        LONG HighPart;
    } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER, *LPLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

typedef struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct {
            DWORD Offset;
            DWORD OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
    ULONG_PTR InternalLow;
} OVERLAPPED, *LPOVERLAPPED;

typedef struct _STARTUPINFOA {
    DWORD cb;
    LPSTR lpReserved;
    LPSTR lpDesktop;
    LPSTR lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;
typedef struct _STARTUPINFOW {
    DWORD cb;
    LPWSTR lpReserved;
    LPWSTR lpDesktop;
    LPWSTR lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW, *LPSTARTUPINFOW;
typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} PROCESS_INFORMATION, *LPPROCESS_INFORMATION;

typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} GUID, IID, CLSID;
typedef GUID* LPGUID;
typedef const GUID* REFGUID;
typedef const IID& REFIID;
typedef const CLSID& REFCLSID;

#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0

// ---- status codes ------------------------------------------------------------------------------------------------
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_POINTER ((HRESULT)0x80004003)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define E_UNEXPECTED ((HRESULT)0x8000FFFF)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define HRESULT_FROM_WIN32(e) ((HRESULT)(e) <= 0 ? ((HRESULT)(e)) : ((HRESULT)(((e) & 0x0000FFFF) | 0x80070000)))
#define MAKE_HRESULT(sev, fac, code) ((HRESULT)(((uint32_t)(sev) << 31) | ((uint32_t)(fac) << 16) | (uint32_t)(code)))
#define MAKE_SCODE(sev, fac, code) MAKE_HRESULT(sev, fac, code)

// ---- handles -----------------------------------------------------------------------------------------------------
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)0xFFFFFFFF)
#define MAX_PATH 260
#define MAX_PATHW 260

// ---- errors ------------------------------------------------------------------------------------------------------
#define ERROR_SUCCESS 0L
#define ERROR_INVALID_FUNCTION 1L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_TOO_MANY_OPEN_FILES 4L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_INVALID_BLOCK 9L
#define ERROR_BAD_ENVIRONMENT 10L
#define ERROR_INVALID_DRIVE 15L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_WRITE_PROTECT 19L
#define ERROR_NOT_READY 21L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_LOCK_VIOLATION 33L
#define ERROR_HANDLE_EOF 38L
#define ERROR_HANDLE_DISK_FULL 39L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_GEN_FAILURE 31L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_BROKEN_PIPE 109L
#define ERROR_BUFFER_OVERFLOW 111L
#define ERROR_DISK_FULL 112L
#define ERROR_CALL_NOT_IMPLEMENTED 120L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_INVALID_NAME 123L
#define ERROR_MOD_NOT_FOUND 126L
#define ERROR_PROC_NOT_FOUND 127L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_ENVVAR_NOT_FOUND 203L
#define ERROR_MORE_DATA 234L
#define ERROR_NO_DATA 232L
#define ERROR_OPERATION_ABORTED 995L
#define ERROR_IO_PENDING 997L
#define ERROR_TIMEOUT 1460L
#define ERROR_COMMITMENT_LIMIT 1455L
#define ERROR_NOT_FOUND 1168L
#define ERROR_INVALID_ADDRESS 487L
#define ERROR_CANCELLED 1223L

// The MSVC CRT's _wfopen: a wide path and a mode. On POSIX the path is converted to UTF-8 first.
// FILE comes from <stdio.h>: glibc's `typedef struct _IO_FILE FILE` happens to match a repeated typedef, Darwin's
// `struct __sFILE` does not, so the typedef is not repeated here.
FILE* nxos_wfopen(const wchar_t* path, const char* mode);
FILE* nxos_wfreopen(const wchar_t* path, const char* mode, FILE* stream);
#if defined(__cplusplus)
// std::filesystem::path::c_str() is const char* on POSIX and const wchar_t* on Windows, and the mode is written as
// L"wb" at the call sites, so every combination has to resolve.
FILE* nxos_wfopen(const char* path, const char* mode);
FILE* nxos_wfreopen(const char* path, const char* mode, FILE* stream);
FILE* nxos_wfopen(const char* path, const wchar_t* mode);
FILE* nxos_wfreopen(const char* path, const wchar_t* mode, FILE* stream);
FILE* nxos_wfopen(const wchar_t* path, const wchar_t* mode);
FILE* nxos_wfreopen(const wchar_t* path, const wchar_t* mode, FILE* stream);
#endif
#if defined(__cplusplus)
#define _wfopen(path, mode) nxos_wfopen((path), (mode))
#define _wfreopen(path, mode, stream) nxos_wfreopen((path), (mode), (stream))
#else
#define _wfopen(path, mode) nxos_wfopen((path), (mode))
#define _wfreopen(path, mode, stream) nxos_wfreopen((path), (mode), (stream))
#endif
DWORD GetLastError(void);

// The MSVC CRT's 64-bit integer helpers, which the runtime calls directly (kernel/sched.cpp parses the affinity config).
#if defined(__cplusplus)
#define _strtoui64(str, end, base) strtoull((str), (end), (base))
#define _strtoi64(str, end, base) strtoll((str), (end), (base))
#define _wcstoui64(str, end, base) wcstoull((str), (end), (base))
#else
#define _strtoui64(str, end, base) strtoull((str), (end), (base))
#define _strtoi64(str, end, base) strtoll((str), (end), (base))
#endif
void SetLastError(DWORD err);
void nxos_set_error_from_errno(int err);

// The MSVC CRT's localtime_s takes (tm*, const time_t*); POSIX's localtime_r takes (const time_t*, tm*). Same result,
// zero on success. Only the C++ call sites use it (games/pkp/pkp_save_uuid.cpp).
#if defined(__cplusplus)
inline int localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : -1; }
#endif
