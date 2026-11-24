// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_diag.h: part of src/os/nx_os.h for the POSIX build -- diagnostics: times, memory counters, thread snapshots (os_posix_diag.cpp).
#pragma once

#if !defined(NX_OS_H_PARTS)
#error "nx_os_diag.h is a part of nx_os.h: include nx_os.h"
#endif

// ---- diagnostics -------------------------------------------------------------------------------------------------
BOOL IsDebuggerPresent(void);
void DebugBreak(void);
BOOL QueryThreadCycleTime(HANDLE thread, PULONGLONG cycles);
BOOL GetProcessTimes(HANDLE proc, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel_t, LPFILETIME user_t);
BOOL GetThreadTimes(HANDLE thread, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel_t, LPFILETIME user_t);

typedef struct _PROCESS_MEMORY_COUNTERS {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
} PROCESS_MEMORY_COUNTERS, *PPROCESS_MEMORY_COUNTERS;
typedef struct _PROCESS_MEMORY_COUNTERS_EX {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX, *PPROCESS_MEMORY_COUNTERS_EX;

BOOL GetProcessMemoryInfo(HANDLE proc, PROCESS_MEMORY_COUNTERS* pmc, DWORD cb);
DWORD GetGuiResources(HANDLE proc, DWORD flags);
#define GR_GDIOBJECTS 0
#define GR_USEROBJECTS 1

typedef struct _THREADENTRY32 {
    DWORD dwSize;
    DWORD cntUsage;
    DWORD th32ThreadID;
    DWORD th32OwnerProcessID;
    LONG tpBasePri;
    LONG tpDeltaPri;
    DWORD dwFlags;
} THREADENTRY32, *PTHREADENTRY32, *LPTHREADENTRY32;
#define TH32CS_SNAPTHREAD 0x00000004
#define TH32CS_SNAPPROCESS 0x00000002

HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid);
BOOL Thread32First(HANDLE snap, PTHREADENTRY32 te);
BOOL Thread32Next(HANDLE snap, PTHREADENTRY32 te);
