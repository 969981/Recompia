// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_file.h: part of src/os/nx_os.h for the POSIX build -- files and folders (os_posix_file.cpp).
#pragma once

#if !defined(NX_OS_H_PARTS)
#error "nx_os_file.h is a part of nx_os.h: include nx_os.h"
#endif

// ---- files -------------------------------------------------------------------------------------------------------
#define GENERIC_READ 0x80000000L
#define GENERIC_WRITE 0x40000000L
#define GENERIC_EXECUTE 0x20000000L
#define GENERIC_ALL 0x10000000L

#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004

#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5

#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400

#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2

// DwFlagsAndAttributes bits. POSIX has no advisory hint to carry them, so the performance hints are accepted and
// ignored; a flag with real semantics (FILE_FLAG_DELETE_ON_CLOSE, FILE_FLAG_OVERLAPPED) is deliberately not defined,
// so a use fails to compile instead of silently behaving differently. Positioned I/O comes through lpOverlapped.
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_WRITE_THROUGH 0x80000000

#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_COPY_ALLOWED 0x00000002
#define MOVEFILE_WRITE_THROUGH 0x00000008

#define FindExInfoStandard 0
#define FindExInfoBasic 1
#define FindExInfoMaxInfoLevel 2
#define FindExSearchNameMatch 0
#define FindExSearchLimitToDirectories 1
#define FindExSearchLimitToDevices 2
#define FIND_FIRST_EX_CASE_SENSITIVE 0x00000001
#define FIND_FIRST_EX_LARGE_FETCH 0x00000002

typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    WCHAR cFileName[MAX_PATH];
    WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *PWIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *PWIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;

// The path/name parameters accept const wchar_t* (a Win32 LPCWSTR, UTF-32 on Darwin) and, in C++, const char*
// (std::filesystem::path::c_str() on POSIX, UTF-8). nxos_* take UTF-8.
HANDLE nxos_create_file(const char* path, DWORD access, DWORD share, void* sa, DWORD disposition, DWORD flags);
BOOL nxos_delete_file(const char* path);
BOOL nxos_move_file(const char* from, const char* to, DWORD flags);
BOOL nxos_copy_file(const char* from, const char* to, BOOL fail_if_exists);
DWORD nxos_get_file_attributes(const char* path);
BOOL nxos_set_file_attributes(const char* path, DWORD attrs);
HANDLE nxos_find_first(const char* pattern, WIN32_FIND_DATAW* data);
BOOL nxos_find_next(HANDLE find, WIN32_FIND_DATAW* data);
HANDLE nxos_create_directory(const char* path);
BOOL nxos_remove_directory(const char* path);
DWORD nxos_full_path(const char* path, char* out, DWORD cap);
DWORD nxos_private_profile_string(const char* section, const char* key, const char* def, char* out, DWORD cap,
                                  const char* file);
HMODULE nxos_load_library(const char* path, DWORD flags);
DWORD nxos_get_module_file_name(HMODULE mod, char* out, DWORD cap);
HANDLE nxos_create_mutex(const char* name, BOOL initial_owner);
HANDLE nxos_create_event(const char* name, BOOL manual_reset, BOOL initial);
HANDLE nxos_create_waitable_timer(const char* name, DWORD flags, DWORD access);
DWORD nxos_get_environment(const char* name, char* out, DWORD cap);

BOOL ReadFile(HANDLE f, LPVOID buf, DWORD want, LPDWORD got, LPOVERLAPPED ov);
BOOL WriteFile(HANDLE f, LPCVOID buf, DWORD want, LPDWORD wrote, LPOVERLAPPED ov);
BOOL FlushFileBuffers(HANDLE f);
BOOL GetFileSizeEx(HANDLE f, PLARGE_INTEGER size);
DWORD GetFileSize(HANDLE f, LPDWORD high);

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA, *LPWIN32_FILE_ATTRIBUTE_DATA;
#define GetFileExInfoStandard 0
#define GetFileExMaxInfoLevel 1

BOOL GetFileAttributesExW(LPCWSTR path, int level, WIN32_FILE_ATTRIBUTE_DATA* data);
BOOL nxos_get_file_attributes_ex(const char* path, WIN32_FILE_ATTRIBUTE_DATA* data);
void GetSystemTimePreciseAsFileTime(LPFILETIME ft);
BOOL SetFilePointerEx(HANDLE f, LARGE_INTEGER dist, PLARGE_INTEGER new_pos, DWORD method);
BOOL SetEndOfFile(HANDLE f);
BOOL ReadProcessMemory(HANDLE proc, LPCVOID addr, LPVOID out, SIZE_T size, PSIZE_T got);
BOOL WriteProcessMemory(HANDLE proc, LPVOID addr, LPCVOID src, SIZE_T size, PSIZE_T wrote);
