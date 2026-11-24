// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_time.h: part of src/os/nx_os.h for the POSIX build -- clocks, system and file times, timer resolution (os_posix_time.cpp).
#pragma once

#if !defined(NX_OS_H_PARTS)
#error "nx_os_time.h is a part of nx_os.h: include nx_os.h"
#endif

// ---- time --------------------------------------------------------------------------------------------------------
BOOL QueryPerformanceCounter(LARGE_INTEGER* out);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* out);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
void GetSystemTime(LPSYSTEMTIME st);
void GetLocalTime(LPSYSTEMTIME st);
void GetSystemTimeAsFileTime(LPFILETIME ft);
BOOL SystemTimeToFileTime(const SYSTEMTIME* st, LPFILETIME ft);
BOOL FileTimeToSystemTime(const FILETIME* ft, LPSYSTEMTIME st);
BOOL FileTimeToLocalFileTime(const FILETIME* ft, LPFILETIME local);
BOOL LocalFileTimeToFileTime(const FILETIME* ft, LPFILETIME utc);
ULONGLONG GetSystemTimeAsUnixTime(void);
DWORD timeBeginPeriod(UINT ms);
DWORD timeEndPeriod(UINT ms);
