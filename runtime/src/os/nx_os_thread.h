// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_thread.h: part of src/os/nx_os.h for the POSIX build -- the process and thread handles, threads and fibers (os_posix_thread.cpp, os_posix_fiber.cpp).
#pragma once

#if !defined(NX_OS_H_PARTS)
#error "nx_os_thread.h is a part of nx_os.h: include nx_os.h"
#endif

// ---- process and thread handles ----------------------------------------------------------------------------------
HANDLE GetCurrentProcess(void);
DWORD GetCurrentProcessId(void);
HANDLE GetCurrentThread(void);
DWORD GetCurrentThreadId(void);
HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid);
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid);
BOOL TerminateProcess(HANDLE proc, UINT code);
void ExitProcess(UINT code);
void ExitThread(DWORD code);
BOOL GetExitCodeProcess(HANDLE proc, PDWORD code);
BOOL GetExitCodeThread(HANDLE thread, PDWORD code);
BOOL CloseHandle(HANDLE h);
BOOL DuplicateHandle(HANDLE src_proc, HANDLE src, HANDLE dst_proc, HANDLE* out, DWORD access, BOOL inherit, DWORD options);
DWORD GetProcessHandleCount(HANDLE proc, PDWORD count);

#define DUPLICATE_CLOSE_SOURCE 0x00000001
#define DUPLICATE_SAME_ACCESS 0x00000002

#define SYNCHRONIZE 0x00100000L
#define PROCESS_TERMINATE 0x0001
#define PROCESS_QUERY_INFORMATION 0x0400
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#define PROCESS_VM_READ 0x0010
#define THREAD_TERMINATE 0x0001
#define THREAD_SUSPEND_RESUME 0x0002
#define THREAD_GET_CONTEXT 0x0008
#define THREAD_SET_CONTEXT 0x0010
#define THREAD_SET_INFORMATION 0x0020
#define THREAD_QUERY_INFORMATION 0x0040
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800
#define THREAD_ALL_ACCESS 0x1FFFFF
#define STILL_ACTIVE 259

// ---- threads -----------------------------------------------------------------------------------------------------
#define CREATE_SUSPENDED 0x00000004
#define CREATE_NEW_CONSOLE 0x00000010
#define CREATE_NO_WINDOW 0x08000000
#define DETACHED_PROCESS 0x00000008
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_MODE_BACKGROUND_BEGIN 0x00010000
#define THREAD_MODE_BACKGROUND_END 0x00020000
#define IDLE_PRIORITY_CLASS 0x00000040
#define BELOW_NORMAL_PRIORITY_CLASS 0x00004000
#define NORMAL_PRIORITY_CLASS 0x00000020
#define ABOVE_NORMAL_PRIORITY_CLASS 0x00008000
#define HIGH_PRIORITY_CLASS 0x00000080
#define REALTIME_PRIORITY_CLASS 0x00000100

HANDLE CreateThread(void* sa, SIZE_T stack, PTHREAD_START_ROUTINE start, LPVOID arg, DWORD flags, DWORD* tid);
BOOL SetThreadPriority(HANDLE thread, int priority);
int GetThreadPriority(HANDLE thread);
BOOL SetThreadPriorityBoost(HANDLE thread, BOOL disable);
DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask);
typedef struct _PROCESSOR_NUMBER {
    WORD Group;
    BYTE Number;
    BYTE Reserved;
} PROCESSOR_NUMBER, *PPROCESSOR_NUMBER;
BOOL GetThreadIdealProcessorEx(HANDLE thread, PPROCESSOR_NUMBER out);
DWORD SetThreadIdealProcessorEx(HANDLE thread, const PROCESSOR_NUMBER* ideal, PDWORD previous);
DWORD GetCurrentProcessorNumber(void);
DWORD GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER out);
BOOL SetThreadStackGuarantee(PULONG guarantee);
BOOL IsBadReadPtr(LPCVOID addr, UINT_PTR size);
BOOL IsBadWritePtr(LPVOID addr, UINT_PTR size);

// Processor topology (kernel/sched.cpp maps the guest's three cores onto host processors). Windows answers through
// GetLogicalProcessorInformationEx; the shim answers from sysctl, in the same structure.
typedef struct _GROUP_AFFINITY {
    DWORD_PTR Mask;
    WORD Group;
    WORD Reserved[3];
} GROUP_AFFINITY, *PGROUP_AFFINITY;

typedef struct _PROCESSOR_RELATIONSHIP {
    BYTE Flags;
    BYTE EfficiencyClass;
    BYTE Reserved[20];
    WORD GroupCount;
    GROUP_AFFINITY GroupMask[1];
} PROCESSOR_RELATIONSHIP, *PPROCESSOR_RELATIONSHIP;

typedef struct _SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX {
    int Relationship;
    DWORD Size;
    union {
        PROCESSOR_RELATIONSHIP Processor;
        BYTE Reserved[64];
    };
} SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, *PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX;

#define RelationProcessorCore 0
#define RelationNumaNode 1
#define RelationCache 2
#define RelationProcessorPackage 3
#define RelationGroup 4

BOOL GetLogicalProcessorInformationEx(int relationship, SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* buffer, PDWORD len);
BOOL GetProcessAffinityMask(HANDLE proc, PDWORD_PTR proc_mask, PDWORD_PTR sys_mask);
BOOL SetPriorityClass(HANDLE proc, DWORD cls);

// Power throttling (kernel/sched.cpp turns Windows 11's EcoQoS off for the whole process). macOS has no such state: the
// scheduler decides, and there is no timer-resolution floor to opt out of, so SetProcessInformation reports failure.
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
typedef struct _PROCESS_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} PROCESS_POWER_THROTTLING_STATE, *PPROCESS_POWER_THROTTLING_STATE;
#define ProcessPowerThrottling 4
BOOL SetProcessInformation(HANDLE proc, int cls, LPVOID info, DWORD size);
DWORD GetPriorityClass(HANDLE proc);
DWORD SuspendThread(HANDLE thread);
DWORD ResumeThread(HANDLE thread);
BOOL SwitchToThread(void);
void Sleep(DWORD ms);
DWORD SleepEx(DWORD ms, BOOL alertable);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL all, DWORD ms);
DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL all, DWORD ms, BOOL alertable);

#define MAXIMUM_WAIT_OBJECTS 64
#define THREAD_PRIORITY_ERROR_RETURN 0x7FFFFFFF
#define THREAD_PRIORITY_ERROR_RETURN_VALUE THREAD_PRIORITY_ERROR_RETURN
#define INFINITE 0xFFFFFFFF
#define WAIT_OBJECT_0 0x00000000
#define WAIT_ABANDONED 0x00000080
#define WAIT_ABANDONED_0 0x00000080
#define WAIT_TIMEOUT 0x00000102
#define WAIT_FAILED 0xFFFFFFFF

// ---- fibers (os_posix_fiber.cpp, over ucontext) ------------------------------------------------------------------
// The calls services/network_cloud.cpp makes. A fiber runs on whichever thread switches to it, and thread-local
// storage stays that thread's, as with Windows fibers.
#define ERROR_ALREADY_FIBER 1280L
typedef void (*LPFIBER_START_ROUTINE)(LPVOID param);
LPVOID ConvertThreadToFiber(LPVOID param);
LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE start, LPVOID param);
void SwitchToFiber(LPVOID fiber);
BOOL IsThreadAFiber(void);
LPVOID GetCurrentFiber(void);
