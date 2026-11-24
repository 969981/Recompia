// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix.cpp: the POSIX side of src/os/nx_os.h -- the handle plumbing every file of the shim shares (nx_os_internal.h),
// the last-error slot and the handle calls. The rest of the shim is split by job: os_posix_thread.cpp (threads and
// thread identity), os_posix_sync.cpp (locks, events, mutexes, semaphores and the waits), os_posix_time.cpp (clocks and
// waitable timers), os_posix_string.cpp, os_posix_module.cpp (modules, the registry stand-in, paths), os_posix_process.cpp
// (environment, process creation, console, shell), os_posix_diag.cpp (diagnostics and processor topology) and
// os_posix_seh.cpp (structured exceptions), next to the files, memory, fiber, network and named-object files.
//
// A HANDLE is the object pointer, so CloseHandle is a dereference rather than a table lookup and DuplicateHandle is
// a refcount bump (diag/registry.cpp and kernel/thread.cpp both duplicate the current thread handle).
// GetCurrentProcess()/GetCurrentThread() are the two Win32 pseudo-handles ((HANDLE)-1 / (HANDLE)-2) and are
// recognised before the tag check; (HANDLE)-1 is also INVALID_HANDLE_VALUE, exactly as on Windows.
//
// Waiting follows one rule: every waitable holds an atomic predicate, and every state change broadcasts a single
// process-wide condition variable that every waiter re-checks after each wakeup. One condition variable for the whole
// process costs a broadcast per signal, and the runtime signals tens of times per frame, not millions; a condition
// variable per object plus a hand-rolled multi-object wait buys nothing here and gets the lost-wakeup cases wrong.
// The fast path -- an already-signalled object -- takes no lock at all.
#include "nx_os_internal.h"

#if !defined(_WIN32)

#include <errno.h>
#include <unistd.h>

#include <atomic>
#include <string>

using namespace nxos;

namespace {

thread_local DWORD t_last_error = 0;

}  // namespace

namespace nxos {

pthread_mutex_t g_wait_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_wait_cv = PTHREAD_COND_INITIALIZER;

void wait_note() {
    pthread_mutex_lock(&g_wait_mu);
    pthread_cond_broadcast(&g_wait_cv);
    pthread_mutex_unlock(&g_wait_mu);
}

// A condition variable deadline. pthread_cond_timedwait reads the condition variable's clock, which is CLOCK_REALTIME
// unless pthread_condattr_setclock said otherwise -- and Darwin has no pthread_condattr_setclock at all, while several
// of the objects here are PTHREAD_COND_INITIALIZER statics that cannot take an attribute. One clock for every condvar in
// the process therefore: the realtime clock. The cost is that a wall-clock jump moves a pending timeout; the runtime
// never sets the clock, and Win32's own absolute-timeout path has the same property.
void make_deadline(struct timespec* ts, DWORD ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += static_cast<time_t>(ms / 1000);
    ts->tv_nsec += static_cast<long>(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

bool past(const struct timespec& ts) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec > ts.tv_sec || (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec);
}

Obj* as_obj(HANDLE h) {
    if (!h) return nullptr;
    if (h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoProcess)) ||
        h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread)))
        return nullptr;
    auto* o = static_cast<Obj*>(h);
    return o->magic == kObjMagic ? o : nullptr;
}

void retain(Obj* o) { o->refs.fetch_add(1, std::memory_order_relaxed); }

void release(Obj* o) {
    if (!o) return;
    if (o->kind == Kind::Mutex && !static_cast<MutexObj*>(o)->name.empty()) {
        release_named_mutex(static_cast<MutexObj*>(o));
        return;
    }
    if (o->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete o;
}

uint64_t filetime_of(const struct timespec& ts) {
    return 11644473600ull * 10000000ull + static_cast<uint64_t>(ts.tv_sec) * 10000000ull +
           static_cast<uint64_t>(ts.tv_nsec) / 100ull;
}

void set_filetime(FILETIME* ft, uint64_t v) {
    ft->dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFFu);
    ft->dwHighDateTime = static_cast<DWORD>(v >> 32);
}

std::string native_path(const char* p) {
    std::string s = p ? p : "";
    for (char& c : s)
        if (c == '\\') c = '/';
    return s;
}

bool signaled_of(Obj* o) {
    switch (o->kind) {
        case Kind::Thread: return static_cast<ThreadObj*>(o)->exited.load(std::memory_order_acquire);
        case Kind::Process: return false;   // a foreign pid cannot be waited on; see OpenProcess
        case Kind::Semaphore: {
            auto* s = static_cast<SemaphoreObj*>(o);
            pthread_mutex_lock(&s->mu);
            const bool free_slot = s->count > 0;
            pthread_mutex_unlock(&s->mu);
            return free_slot;
        }
        case Kind::Event:
        case Kind::Timer: return static_cast<Waitable*>(o)->signaled.load(std::memory_order_acquire);
        case Kind::Mutex: {
            // A mutex is available when nobody owns it, and a thread that already owns it re-acquires it (Win32 mutexes are
            // recursive). Without the owner test the shim's mutexes started out "not signaled" and no wait ever completed:
            // the fs save root lock blocked the main thread for the whole run.
            auto* m = static_cast<MutexObj*>(o);
            return m->owner == self_tid() || m->signaled.load(std::memory_order_acquire);
        }
        default: return false;
    }
}

void consume(Obj* o) {
    if (o->kind == Kind::Event) {
        auto* e = static_cast<EventObj*>(o);
        if (!e->manual) e->signaled.store(false, std::memory_order_release);
        e->epoch.fetch_add(1);
    } else if (o->kind == Kind::Semaphore) {
        auto* s = static_cast<SemaphoreObj*>(o);
        pthread_mutex_lock(&s->mu);
        if (s->count > 0) --s->count;
        pthread_mutex_unlock(&s->mu);
        s->epoch.fetch_add(1);
    } else if (o->kind == Kind::Mutex) {
        auto* m = static_cast<MutexObj*>(o);
        if (m->owner == self_tid()) {
            ++m->depth;   // recursive: the owner takes it again
            return;
        }
        if (pthread_mutex_trylock(&m->mu) != 0) pthread_mutex_lock(&m->mu);   // lost the race: wait for the owner
        m->owner = self_tid();
        m->depth = 1;
        m->signaled.store(false, std::memory_order_release);
        m->epoch.fetch_add(1);
    }
}

}  // namespace nxos

// ==================================================================================================================
// errors
// ==================================================================================================================
DWORD GetLastError(void) { return t_last_error; }
void SetLastError(DWORD err) { t_last_error = err; }

void nxos_set_error_from_errno(int err) {
    switch (err) {
        case 0: t_last_error = ERROR_SUCCESS; break;
        case ENOENT: t_last_error = ERROR_FILE_NOT_FOUND; break;
        case ENOTDIR: t_last_error = ERROR_PATH_NOT_FOUND; break;
        case EACCES:
        case EPERM: t_last_error = ERROR_ACCESS_DENIED; break;
        case EEXIST: t_last_error = ERROR_ALREADY_EXISTS; break;
        case EBADF: t_last_error = ERROR_INVALID_HANDLE; break;
        case EINVAL: t_last_error = ERROR_INVALID_PARAMETER; break;
        case ENOMEM: t_last_error = ERROR_NOT_ENOUGH_MEMORY; break;
        case ENOSPC: t_last_error = ERROR_DISK_FULL; break;
        case ENOTSUP: t_last_error = ERROR_NOT_SUPPORTED; break;
        case EINTR: t_last_error = ERROR_OPERATION_ABORTED; break;
        case ETIMEDOUT: t_last_error = ERROR_TIMEOUT; break;
        case EISDIR: t_last_error = ERROR_ACCESS_DENIED; break;
        case ENAMETOOLONG:
        case ELOOP: t_last_error = ERROR_INVALID_NAME; break;
        default: t_last_error = ERROR_GEN_FAILURE; break;
    }
}

// ==================================================================================================================
// handles
// ==================================================================================================================
BOOL CloseHandle(HANDLE h) {
    if (!h || h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoProcess)) ||
        h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread))) {
        return TRUE;   // pseudo handles are not ours to close (thread.cpp:184 duplicates before it keeps one)
    }
    Obj* o = as_obj(h);
    if (!o) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    release(o);
    return TRUE;
}

BOOL DuplicateHandle(HANDLE, HANDLE src, HANDLE, HANDLE* out, DWORD, BOOL, DWORD) {
    if (!out) {
        t_last_error = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    if (src == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread))) {
        ThreadObj* t = self_thread();
        retain(t);
        *out = t;
        return TRUE;
    }
    if (src == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoProcess))) {
        *out = new ProcessObj(static_cast<DWORD>(getpid()));
        return TRUE;
    }
    Obj* o = as_obj(src);
    if (!o) {
        t_last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    retain(o);
    *out = o;
    return TRUE;
}

#endif  // !_WIN32
