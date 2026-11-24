// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_sync.h: part of src/os/nx_os.h for the POSIX build -- SRWLOCK, CONDITION_VARIABLE, CRITICAL_SECTION, events, mutexes, semaphores, waits and waitable timers (os_posix_sync.cpp, os_posix_time.cpp).
#pragma once

#if !defined(NX_OS_H_PARTS)
#error "nx_os_sync.h is a part of nx_os.h: include nx_os.h"
#endif

// ---- synchronisation primitives ----------------------------------------------------------------------------------
// SRWLOCK and CONDITION_VARIABLE are one pointer each: static storage zeroed by the loader must be a valid unlocked
// lock (os/sync.cpp relies on that, and KThread::cv_storage is one void*), and a Darwin pthread_mutex_t is 64 bytes
// whose zeroed state is not initialised. The pthread objects are allocated on first use instead.
typedef struct {
    void* impl;
} SRWLOCK, *PSRWLOCK, *LPSRWLOCK;
typedef struct {
    void* impl;
} CONDITION_VARIABLE, *PCONDITION_VARIABLE, *LPCONDITION_VARIABLE;
typedef struct {
    void* impl;
} CRITICAL_SECTION, *PCRITICAL_SECTION, *LPCRITICAL_SECTION;

#define SRWLOCK_INIT {0}
#define CONDITION_VARIABLE_INIT {0}

void InitializeSRWLock(PSRWLOCK lock);
void AcquireSRWLockExclusive(PSRWLOCK lock);
void ReleaseSRWLockExclusive(PSRWLOCK lock);
void AcquireSRWLockShared(PSRWLOCK lock);
void ReleaseSRWLockShared(PSRWLOCK lock);
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock);
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK lock);
void InitializeConditionVariable(PCONDITION_VARIABLE cv);
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG flags);
BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms);
void WakeConditionVariable(PCONDITION_VARIABLE cv);
void WakeAllConditionVariable(PCONDITION_VARIABLE cv);

#define CONDITION_VARIABLE_LOCKMODE_SHARED 0x1

void InitializeCriticalSection(PCRITICAL_SECTION cs);
void EnterCriticalSection(PCRITICAL_SECTION cs);
void LeaveCriticalSection(PCRITICAL_SECTION cs);
BOOL TryEnterCriticalSection(PCRITICAL_SECTION cs);
void DeleteCriticalSection(PCRITICAL_SECTION cs);

HANDLE CreateEventA(void* sa, BOOL manual_reset, BOOL initial, LPCSTR name);
BOOL SetEvent(HANDLE ev);
BOOL ResetEvent(HANDLE ev);
HANDLE CreateMutexA(void* sa, BOOL initial_owner, LPCSTR name);
BOOL ReleaseMutex(HANDLE mutex);
HANDLE CreateSemaphoreW(void* sa, LONG initial, LONG max, LPCWSTR name);
BOOL ReleaseSemaphore(HANDLE sem, LONG release, PLONG previous);
BOOL SetWaitableTimer(HANDLE timer, const LARGE_INTEGER* due, LONG period, void* routine, void* arg, BOOL resume);
// The _Ex form adds a wake context and flags (tolerable delay, resume). The shim has neither: it forwards and ignores them.
BOOL SetWaitableTimerEx(HANDLE timer, const LARGE_INTEGER* due, LONG period, void* routine, void* arg,
                        void* wake_context, ULONG flags);
#define TOLERABLE_DELAY_NONE 0
#define TOLERABLE_DELAY_SHORT 1
#define TOLERABLE_DELAY_LONG 2
#define TIMER_RESUME 0x00000001
#define TIMER_NO_WAKE 0x00000002
BOOL CancelWaitableTimer(HANDLE timer);

#define CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#define TIMER_ALL_ACCESS 0x1F0003
#define TIMER_MODIFY_STATE 0x0002
#define TIMER_QUERY_STATE 0x0001
