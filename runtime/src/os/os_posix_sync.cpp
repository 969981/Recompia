// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_sync.cpp: synchronisation for src/os/nx_os.h -- SRWLOCK, CONDITION_VARIABLE, CRITICAL_SECTION, and the waitable
// objects: WaitForSingleObject / WaitForMultipleObjects, events, (named) mutexes and semaphores. Every wait follows the one
// rule at the top of os_posix.cpp: an atomic predicate per object, one process-wide condition variable. Waitable timers are
// in os_posix_time.cpp.
#include "nx_os_internal.h"

#if !defined(_WIN32)

#include <errno.h>
#include <pthread.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

using namespace nxos;

// ==================================================================================================================
// SRWLOCK: a real reader/writer lock over one pthread mutex plus one condition variable
//
// `readers` is the state: > 0 = that many shared holders, 0 = free, -1 = one exclusive holder. The exclusive holder
// does NOT keep the mutex: SleepConditionVariableSRW needs it held to hand to pthread_cond_wait, and it releases the
// write lock atomically by clearing `readers` and broadcasting just before the wait. A thread that wants the exclusive
// lock always takes the mutex first, sees `readers == 0`, sets -1 and lets go; a waker that needs the exclusive lock
// to signal cannot slip into the window between the clear and the cond_wait, because it blocks on the mutex that the
// waiter still holds there. That is exactly the Win32 contract (a condition variable wait on an SRWLOCK must hold it
// exclusively), and it is why no wakeup can be lost.
//
// SRWLOCK and CONDITION_VARIABLE are one pointer each because static storage zeroed by the loader must be a valid
// unlocked lock (os/sync.cpp:110 says so, and KThread::cv_storage is one void*), while a Darwin pthread_mutex_t is 64
// bytes whose zeroed state is not initialised. The pthread objects are allocated on first use instead.
// ==================================================================================================================
namespace {

#if !defined(__linux__)   // Linux and Android: os_posix_futex_sync.cpp
struct SrwImpl {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    int readers = 0;
};

pthread_mutex_t g_srw_init_mu = PTHREAD_MUTEX_INITIALIZER;

SrwImpl* srw_get(SRWLOCK* lock) {
    SrwImpl* impl = static_cast<SrwImpl*>(__atomic_load_n(&lock->impl, __ATOMIC_ACQUIRE));
    if (impl) return impl;
    pthread_mutex_lock(&g_srw_init_mu);
    impl = static_cast<SrwImpl*>(lock->impl);
    if (!impl) {
        impl = new SrwImpl();
        __atomic_store_n(&lock->impl, impl, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_srw_init_mu);
    return impl;
}

pthread_mutex_t g_cv_init_mu = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t* cv_get(CONDITION_VARIABLE* cv) {
    pthread_cond_t* impl = static_cast<pthread_cond_t*>(__atomic_load_n(&cv->impl, __ATOMIC_ACQUIRE));
    if (impl) return impl;
    pthread_mutex_lock(&g_cv_init_mu);
    impl = static_cast<pthread_cond_t*>(cv->impl);
    if (!impl) {
        impl = new pthread_cond_t();
        pthread_cond_init(impl, nullptr);
        __atomic_store_n(&cv->impl, impl, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_cv_init_mu);
    return impl;
}

#endif  // !__linux__

struct CsImpl {
    pthread_mutex_t mu;
};

pthread_mutex_t g_cs_init_mu = PTHREAD_MUTEX_INITIALIZER;

CsImpl* cs_get(CRITICAL_SECTION* cs) {
    CsImpl* impl = static_cast<CsImpl*>(__atomic_load_n(&cs->impl, __ATOMIC_ACQUIRE));
    if (impl) return impl;
    pthread_mutex_lock(&g_cs_init_mu);
    impl = static_cast<CsImpl*>(cs->impl);
    if (!impl) {
        impl = new CsImpl();
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&impl->mu, &attr);
        pthread_mutexattr_destroy(&attr);
        __atomic_store_n(&cs->impl, impl, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_cs_init_mu);
    return impl;
}

}  // namespace

#if !defined(__linux__)   // Linux and Android: os_posix_futex_sync.cpp
void InitializeSRWLock(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    impl->readers = 0;
    pthread_cond_broadcast(&impl->cv);
    pthread_mutex_unlock(&impl->mu);
}

void AcquireSRWLockExclusive(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    while (impl->readers != 0) pthread_cond_wait(&impl->cv, &impl->mu);
    impl->readers = -1;
    pthread_mutex_unlock(&impl->mu);
}

void ReleaseSRWLockExclusive(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    impl->readers = 0;
    pthread_cond_broadcast(&impl->cv);
    pthread_mutex_unlock(&impl->mu);
}

void AcquireSRWLockShared(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    while (impl->readers < 0) pthread_cond_wait(&impl->cv, &impl->mu);
    impl->readers++;
    pthread_mutex_unlock(&impl->mu);
}

void ReleaseSRWLockShared(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    if (--impl->readers == 0) pthread_cond_broadcast(&impl->cv);
    pthread_mutex_unlock(&impl->mu);
}

BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    if (impl->readers != 0) {
        pthread_mutex_unlock(&impl->mu);
        return 0;
    }
    impl->readers = -1;
    pthread_mutex_unlock(&impl->mu);
    return 1;
}

BOOLEAN TryAcquireSRWLockShared(PSRWLOCK lock) {
    SrwImpl* impl = srw_get(lock);
    pthread_mutex_lock(&impl->mu);
    if (impl->readers < 0) {
        pthread_mutex_unlock(&impl->mu);
        return 0;
    }
    impl->readers++;
    pthread_mutex_unlock(&impl->mu);
    return 1;
}

void InitializeConditionVariable(PCONDITION_VARIABLE cv) { (void)cv_get(cv); }

BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG) {
    pthread_cond_t* cond = cv_get(cv);
    SrwImpl* impl = srw_get(lock);

    pthread_mutex_lock(&impl->mu);
    if (impl->readers != -1) {
        // Not held exclusively: the Win32 contract is broken at the call site. Releasing `readers` would corrupt the
        // lock, so report failure instead of pretending.
        pthread_mutex_unlock(&impl->mu);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    impl->readers = 0;   // release the write lock, still holding mu: atomic with the wait below
    pthread_cond_broadcast(&impl->cv);

    int rc;
    if (ms == INFINITE) {
        rc = pthread_cond_wait(cond, &impl->mu);
    } else {
        struct timespec dl;
        make_deadline(&dl, ms);
        rc = pthread_cond_timedwait(cond, &impl->mu, &dl);
    }
    // Reacquire the write lock while still holding mu.
    while (impl->readers != 0) pthread_cond_wait(&impl->cv, &impl->mu);
    impl->readers = -1;
    pthread_mutex_unlock(&impl->mu);

    if (rc == ETIMEDOUT) {
        SetLastError(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

BOOL SleepConditionVariableCS(PCONDITION_VARIABLE, PCRITICAL_SECTION, DWORD) {
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

void WakeConditionVariable(PCONDITION_VARIABLE cv) {
    // A never-used condition variable has no waiter: signalling it is a no-op, and every caller re-checks a flag
    // under the lock before waiting (kernel_sync.cpp block_on_wake, os/sync.cpp wait_step), so nothing is lost.
    pthread_cond_t* impl = static_cast<pthread_cond_t*>(__atomic_load_n(&cv->impl, __ATOMIC_ACQUIRE));
    if (impl) pthread_cond_signal(impl);
}

void WakeAllConditionVariable(PCONDITION_VARIABLE cv) {
    pthread_cond_t* impl = static_cast<pthread_cond_t*>(__atomic_load_n(&cv->impl, __ATOMIC_ACQUIRE));
    if (impl) pthread_cond_broadcast(impl);
}

#endif  // !__linux__

// ==================================================================================================================
// CRITICAL_SECTION (the vendored Link Play module only)
// ==================================================================================================================
void InitializeCriticalSection(PCRITICAL_SECTION cs) { (void)cs_get(cs); }
void EnterCriticalSection(PCRITICAL_SECTION cs) { pthread_mutex_lock(&cs_get(cs)->mu); }
void LeaveCriticalSection(PCRITICAL_SECTION cs) { pthread_mutex_unlock(&cs_get(cs)->mu); }
BOOL TryEnterCriticalSection(PCRITICAL_SECTION cs) { return pthread_mutex_trylock(&cs_get(cs)->mu) == 0 ? TRUE : FALSE; }
void DeleteCriticalSection(PCRITICAL_SECTION) {}

// ==================================================================================================================
// waitable objects
// ==================================================================================================================
namespace {

DWORD wait_any(const HANDLE* handles, DWORD count, BOOL all, DWORD ms) {
    if (!handles || count == 0 || count > MAXIMUM_WAIT_OBJECTS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    Obj* objs[MAXIMUM_WAIT_OBJECTS];
    for (DWORD i = 0; i < count; i++) {
        objs[i] = as_obj(handles[i]);
        if (!objs[i]) {
            SetLastError(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
    }

    struct timespec dl{};
    const bool timed = ms != INFINITE;
    if (timed) make_deadline(&dl, ms);

    for (;;) {
        bool ready = all != FALSE;
        DWORD hit = 0;
        bool any = false;
        for (DWORD i = 0; i < count; i++) {
            const bool s = signaled_of(objs[i]);
            if (s) {
                any = true;
                hit = i;
            }
            if (all) ready = ready && s;
        }
        if (all ? ready : any) {
            if (all) {
                for (DWORD i = 0; i < count; i++) consume(objs[i]);
                return WAIT_OBJECT_0;
            }
            consume(objs[hit]);
            return WAIT_OBJECT_0 + hit;
        }
        if (timed && past(dl)) return WAIT_TIMEOUT;

        pthread_mutex_lock(&g_wait_mu);
        // Re-check under the mutex: a signal that landed between the check above and this lock broadcast before we
        // took it, so the predicate read here is the one that broadcast belongs to.
        ready = all != FALSE;
        any = false;
        for (DWORD i = 0; i < count; i++) {
            const bool s = signaled_of(objs[i]);
            if (all) ready = ready && s;
            else if (s) any = true;
        }
        if (!(all ? ready : any)) {
            if (timed) pthread_cond_timedwait(&g_wait_cv, &g_wait_mu, &dl);
            else pthread_cond_wait(&g_wait_cv, &g_wait_mu);
        }
        pthread_mutex_unlock(&g_wait_mu);
    }
}

}  // namespace

DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return wait_any(&h, 1, FALSE, ms); }
DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL) { return wait_any(&h, 1, FALSE, ms); }
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL all, DWORD ms) {
    return wait_any(handles, count, all, ms);
}
DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL all, DWORD ms, BOOL) {
    return wait_any(handles, count, all, ms);
}

HANDLE nxos_create_event(const char* name, BOOL manual_reset, BOOL initial) {
    (void)name;   // the two named uses are single-process slot mutexes; see nxos_create_mutex
    auto* e = new EventObj(manual_reset != FALSE);
    e->signaled.store(initial != FALSE, std::memory_order_relaxed);
    return e;
}

HANDLE CreateEventA(void*, BOOL manual_reset, BOOL initial, LPCSTR name) {
    return nxos_create_event(name, manual_reset, initial);
}

BOOL SetEvent(HANDLE h) {
    auto* w = as<Waitable>(h, Kind::Event);
    if (!w) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    w->signaled.store(true, std::memory_order_release);
    w->epoch.fetch_add(1);
    wait_note();
    return TRUE;
}

BOOL ResetEvent(HANDLE h) {
    auto* w = as<Waitable>(h, Kind::Event);
    if (!w) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    w->signaled.store(false, std::memory_order_release);
    w->epoch.fetch_add(1);
    return TRUE;
}

// Named mutexes: kernel/sched.cpp picks a scheduler slot through one and tests for ERROR_ALREADY_EXISTS; fs_volume.cpp
// opens and closes one around every save commit. A name stays in the list while a handle to it is open: the last
// CloseHandle takes it out under the list's lock, so a later create never finds a deleted object (it used to, and
// retained and returned freed memory from the second save commit on). Each name in the list also holds its claim in
// os_posix_named.cpp, which is what other processes see; it is dropped (the fd closed) under the same lock, so a create
// right after the last close claims afresh instead of meeting this process's old lock. Waiting and owning stay inside
// one process: a second process gets ERROR_ALREADY_EXISTS but its own mutex object.
namespace {
pthread_mutex_t g_named_mu = PTHREAD_MUTEX_INITIALIZER;
std::vector<std::pair<std::string, MutexObj*>> g_named_mutexes;
}  // namespace

namespace nxos {

void release_named_mutex(MutexObj* m) {
    pthread_mutex_lock(&g_named_mu);
    const bool last = m->refs.fetch_sub(1, std::memory_order_acq_rel) == 1;
    if (last) {
        for (auto it = g_named_mutexes.begin(); it != g_named_mutexes.end(); ++it)
            if (it->second == m) {
                g_named_mutexes.erase(it);
                break;
            }
        delete m;   // under the lock: its destructor closes the claim (see above)
    }
    pthread_mutex_unlock(&g_named_mu);
}

}  // namespace nxos

HANDLE nxos_create_mutex(const char* name, BOOL initial_owner) {
    auto* m = new MutexObj();
    if (name && *name) {
        m->name = name;
        pthread_mutex_lock(&g_named_mu);
        for (auto& kv : g_named_mutexes) {
            if (kv.first == m->name) {
                retain(kv.second);
                delete m;
                pthread_mutex_unlock(&g_named_mu);
                SetLastError(ERROR_ALREADY_EXISTS);
                return kv.second;
            }
        }
        bool others = false;
        m->claim_fd = named_claim(m->name, &others);
        g_named_mutexes.emplace_back(m->name, m);
        pthread_mutex_unlock(&g_named_mu);
        if (others) {   // as Win32: the mutex existed, so initial ownership is not given
            m->signaled.store(true, std::memory_order_release);
            SetLastError(ERROR_ALREADY_EXISTS);
            return m;
        }
        SetLastError(ERROR_SUCCESS);   // as Win32: a caller testing for ERROR_ALREADY_EXISTS must not see an older error
    }
    if (initial_owner) {
        pthread_mutex_lock(&m->mu);
        m->owner = self_tid();
        m->depth = 1;
        m->signaled.store(false, std::memory_order_release);
    } else {
        m->signaled.store(true, std::memory_order_release);   // unowned: the first wait takes it
    }
    return m;
}

BOOL ReleaseMutex(HANDLE h) {
    auto* m = as<MutexObj>(h, Kind::Mutex);
    if (!m) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (m->owner == self_tid()) {
        if (m->depth > 1) {
            --m->depth;   // one of the recursive acquisitions
            return TRUE;
        }
        m->owner = 0;
        m->depth = 0;
        pthread_mutex_unlock(&m->mu);
        m->signaled.store(true, std::memory_order_release);
        m->epoch.fetch_add(1);
        wait_note();   // wake the threads blocked on it: the cv is the only wake-up path
    }
    return TRUE;
}

HANDLE CreateSemaphoreW(void*, LONG initial, LONG max, LPCWSTR) {
    auto* s = new SemaphoreObj();
    s->count = initial;
    s->max = max;
    return s;
}

BOOL ReleaseSemaphore(HANDLE h, LONG release, PLONG previous) {
    auto* s = as<SemaphoreObj>(h, Kind::Semaphore);
    if (!s) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    pthread_mutex_lock(&s->mu);
    if (previous) *previous = s->count;
    s->count = std::min<long>(s->count + release, s->max);
    pthread_mutex_unlock(&s->mu);
    wait_note();
    return TRUE;
}

#endif  // !_WIN32
