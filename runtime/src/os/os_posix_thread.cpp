// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_thread.cpp: threads for src/os/nx_os.h -- the calling thread's identity (self_tid, self_thread), the process and
// thread pseudo-handles, CreateThread and its trampoline, priority and affinity, SuspendThread / ResumeThread and the
// register file of a stopped thread (GetThreadContext). Linux stops a running thread with a signal (os_posix_suspend.cpp).
#include "nx_os_internal.h"

#if !defined(_WIN32)

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread/qos.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

using namespace nxos;

namespace {

std::atomic<DWORD> g_next_tid{1};
thread_local DWORD t_tid = 0;
thread_local ThreadObj* t_self = nullptr;

}  // namespace

namespace nxos {

DWORD self_tid() {
    if (t_tid == 0) t_tid = g_next_tid.fetch_add(1, std::memory_order_relaxed);
    return t_tid;
}

ThreadObj* self_thread() {
    if (!t_self) {
        auto* t = new ThreadObj();
        t->th = pthread_self();
        t->tid = self_tid();
        t->started.store(true, std::memory_order_relaxed);
        t_self = t;
#if defined(__linux__)
        note_kernel_tid(t);
#endif
    }
    return t_self;
}

ThreadObj* thread_if_known() { return t_self; }

}  // namespace nxos

// ==================================================================================================================
// process and thread identity
// ==================================================================================================================
HANDLE GetCurrentProcess(void) { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoProcess)); }
DWORD GetCurrentProcessId(void) { return static_cast<DWORD>(getpid()); }
HANDLE GetCurrentThread(void) { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread)); }
DWORD GetCurrentThreadId(void) { return self_tid(); }

void Sleep(DWORD ms) {
    if (ms == 0) {
        sched_yield();
        return;
    }
    struct timespec ts{static_cast<time_t>(ms / 1000), static_cast<long>(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

DWORD SleepEx(DWORD ms, BOOL) {
    Sleep(ms);
    return 0;
}

BOOL SwitchToThread(void) { return sched_yield() == 0 ? TRUE : FALSE; }

HANDLE OpenProcess(DWORD, BOOL, DWORD pid) {
    if (pid == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    // A foreign pid can only be probed, not waited on: fs_volume.cpp uses this for "is the writer still alive".
    if (pid != static_cast<DWORD>(getpid()) && ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    return new ProcessObj(pid);
}

HANDLE OpenThread(DWORD, BOOL, DWORD tid) {
    if (tid == self_tid()) {
        ThreadObj* t = self_thread();
        retain(t);
        return t;
    }
    SetLastError(ERROR_INVALID_PARAMETER);   // no cross-thread pthread_t lookup without a mach thread port
    return nullptr;
}

BOOL TerminateProcess(HANDLE, UINT code) {
    std::fflush(nullptr);
    _exit(static_cast<int>(code));
}

void ExitProcess(UINT code) {
    std::fflush(nullptr);
    _exit(static_cast<int>(code));
}

BOOL GetExitCodeProcess(HANDLE h, PDWORD code) {
    if (!code) return FALSE;
    auto* p = as<ProcessObj>(h, Kind::Process);
    if (!p) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    *code = STILL_ACTIVE;
    return TRUE;
}

DWORD GetProcessHandleCount(HANDLE, PDWORD count) {
    if (count) *count = 0;
    return TRUE;
}

BOOL GetExitCodeThread(HANDLE h, PDWORD code) {
    if (!code) return FALSE;
    auto* t = as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    // The shim keeps no per-thread exit code (a pthread returns void*), so a thread that has ended reports 0 and a running one
    // STILL_ACTIVE: the caller asks whether it ended (diag/hle_census.cpp), not what it returned.
    *code = t->exited.load(std::memory_order_acquire) ? 0u : STILL_ACTIVE;
    return TRUE;
}

// ==================================================================================================================
// threads
// ==================================================================================================================
namespace {

#if defined(__linux__)
// The main thread's stack, the Linux side of pkp.exe's /STACK:134217728: the host main thread runs the guest's main thread and
// lifted code nests deeply. Linux grows the main stack up to RLIMIT_STACK (8 MiB on SteamOS and most desktops) and ignores the
// PT_GNU_STACK size the link writes; it keeps at least 128 MiB free below the stack for that growth, so raising the soft limit
// before main() is enough. glibc sized its default thread stack from the limit before this runs, so other threads keep theirs.
// A hard limit below 128 MiB caps it there. macOS takes the size from the executable's LC_MAIN (-stack_size).
// From Dittobyte/Recompia#546 (game-build/src/pkp_host_stack_linux.cpp there).
[[gnu::constructor]] void raise_main_stack() {
    constexpr rlim_t kMainStack = 134217728;
    rlimit rl{};
    if (getrlimit(RLIMIT_STACK, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= kMainStack) return;
    rl.rlim_cur = rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= kMainStack ? kMainStack : rl.rlim_max;
    setrlimit(RLIMIT_STACK, &rl);
}
#endif

struct StartBox {
    PTHREAD_START_ROUTINE fn = nullptr;
    LPVOID arg = nullptr;
    ThreadObj* self = nullptr;
};

void apply_priority(ThreadObj* t) {
#if defined(__APPLE__)
    if (t->th != pthread_self()) return;   // QoS is set from the thread itself
    const int p = t->priority.load(std::memory_order_relaxed);
    qos_class_t q = QOS_CLASS_DEFAULT;
    if (p >= 15) q = QOS_CLASS_USER_INTERACTIVE;
    else if (p >= 2) q = QOS_CLASS_USER_INITIATED;
    else if (p >= 0) q = QOS_CLASS_DEFAULT;
    else if (p >= -2) q = QOS_CLASS_UTILITY;
    else q = QOS_CLASS_BACKGROUND;
    pthread_set_qos_class_self_np(q, 0);
#else
    (void)t;
#endif
}

void* thread_trampoline(void* p) {
    StartBox box = *static_cast<StartBox*>(p);
    delete static_cast<StartBox*>(p);
    nxos_install_altstack();   // so a fault on this thread is reportable even when its stack is exhausted
    t_tid = box.self->tid;
    t_self = box.self;
#if defined(__linux__)
    note_kernel_tid(box.self);
#endif
    box.self->started.store(true, std::memory_order_release);

    // CREATE_SUSPENDED: the creator fills the guest Ctx before the thread may run (kernel/thread.cpp:146).
    if (box.self->suspend.load(std::memory_order_acquire) > 0) {
        pthread_mutex_lock(&box.self->mu);
        while (box.self->suspend.load(std::memory_order_relaxed) > 0)
            pthread_cond_wait(&box.self->cv, &box.self->mu);
        pthread_mutex_unlock(&box.self->mu);
    }
    apply_priority(box.self);

    struct timespec t0;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
    if (box.fn) box.fn(box.arg);
    struct timespec t1;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);

    box.self->cpu_ns = static_cast<uint64_t>(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                       static_cast<uint64_t>(t1.tv_nsec - t0.tv_nsec);
    box.self->exited.store(true, std::memory_order_release);
    wait_note();
    t_self = nullptr;
    release(box.self);
    return nullptr;
}

}  // namespace

HANDLE CreateThread(void*, SIZE_T stack, PTHREAD_START_ROUTINE start, LPVOID arg, DWORD flags, DWORD* tid) {
    auto* t = new ThreadObj();
    t->tid = g_next_tid.fetch_add(1, std::memory_order_relaxed);
    if (flags & CREATE_SUSPENDED) t->suspend.store(1, std::memory_order_relaxed);
    if (tid) *tid = t->tid;

    // Two references: the handle the caller gets, and the one the running thread holds until its trampoline returns.
    // Without the second, a CloseHandle right after CreateThread would free the object the thread is still writing to.
    retain(t);
    auto* box = new StartBox{start, arg, t};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (stack) pthread_attr_setstacksize(&attr, std::max<size_t>(stack, 64u << 10));
    const int rc = pthread_create(&t->th, &attr, thread_trampoline, box);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete box;
        release(t);   // the running-thread reference
        release(t);   // the handle reference
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        if (tid) *tid = 0;
        return nullptr;
    }
    return t;
}

BOOL SetThreadPriority(HANDLE h, int priority) {
    if (h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread))) {
        ThreadObj* t = self_thread();
        t->priority.store(priority, std::memory_order_relaxed);
        apply_priority(t);
        return TRUE;
    }
    auto* t = as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    t->priority.store(priority, std::memory_order_relaxed);
    apply_priority(t);
    return TRUE;
}

int GetThreadPriority(HANDLE h) {
    if (h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread))) return self_thread()->priority.load();
    auto* t = as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        SetLastError(ERROR_INVALID_HANDLE);
        return THREAD_PRIORITY_ERROR_RETURN_VALUE;
    }
    return t->priority.load();
}

BOOL SetThreadPriorityBoost(HANDLE, BOOL) { return TRUE; }   // no dynamic priority boost to turn off

DWORD_PTR SetThreadAffinityMask(HANDLE, DWORD_PTR) {
    // macOS has no affinity API: thread_policy_set(THREAD_AFFINITY_POLICY) is a hint at best and has been reported to
    // hurt on Apple Silicon, where the scheduler already migrates between P and E cores. Returning the mask the caller
    // "had" keeps sched_apply on its happy path; the guest's core pinning degrades to a preference the host ignores.
    return 1;
}

BOOL GetProcessAffinityMask(HANDLE, PDWORD_PTR proc_mask, PDWORD_PTR sys_mask) {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    const DWORD_PTR all = (n > 0 && n < 64) ? ((static_cast<DWORD_PTR>(1) << n) - 1) : ~static_cast<DWORD_PTR>(0);
    if (proc_mask) *proc_mask = all;
    if (sys_mask) *sys_mask = all;
    return TRUE;
}

namespace {
DWORD g_priority_class = NORMAL_PRIORITY_CLASS;
}

BOOL SetPriorityClass(HANDLE, DWORD cls) {
    g_priority_class = cls;
    return TRUE;
}

DWORD GetPriorityClass(HANDLE) { return g_priority_class; }

BOOL SetProcessInformation(HANDLE, int, LPVOID, DWORD) {
    // Windows 11's EcoQoS and its timer-resolution floor have no macOS counterpart: there is no per-process power state
    // to set, and the scheduler is not on a tick. sched.cpp logs the failure once and moves on, which is the truth.
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

#if defined(__APPLE__)
// The mach port of a thread the shim knows, for thread_suspend/thread_get_state. MACH_PORT_NULL when it is gone.
mach_port_t mach_thread_of(ThreadObj* t) {
    if (!t || !t->started.load(std::memory_order_acquire) || t->exited.load(std::memory_order_acquire)) return MACH_PORT_NULL;
    return pthread_mach_thread_np(t->th);
}
#endif

DWORD SuspendThread(HANDLE h) {
    auto* t = as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        SetLastError(ERROR_INVALID_HANDLE);
        return static_cast<DWORD>(-1);
    }
#if defined(__APPLE__)
    // thread_suspend on the mach port: the diag sampler and the freeze watchdog then read a stopped register set, which is
    // what GetThreadContext needs (thread_get_state fails on a running thread).
    const mach_port_t mt = mach_thread_of(t);
    if (mt == MACH_PORT_NULL || thread_suspend(mt) != KERN_SUCCESS) {
        SetLastError(ERROR_INVALID_HANDLE);
        return static_cast<DWORD>(-1);
    }
    return static_cast<DWORD>(t->suspend.fetch_add(1, std::memory_order_relaxed));
#elif defined(__linux__)
    // a running thread stops in the stop signal's handler (os_posix_suspend.cpp)
    DWORD prev = 0;
    if (!signal_suspend(t, &prev)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return static_cast<DWORD>(-1);
    }
    return prev;
#else
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return static_cast<DWORD>(-1);
#endif
}

DWORD ResumeThread(HANDLE h) {
    auto* t = as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        SetLastError(ERROR_INVALID_HANDLE);
        return static_cast<DWORD>(-1);
    }
#if defined(__APPLE__)
    if (const int n = t->suspend.load(std::memory_order_relaxed); n > 0) {
        if (const mach_port_t mt = mach_thread_of(t); mt != MACH_PORT_NULL) thread_resume(mt);
        return static_cast<DWORD>(t->suspend.fetch_sub(1, std::memory_order_relaxed) - 1);
    }
    return 0;
#else
#if defined(__linux__)
    if (DWORD stopped = 0; signal_resume(t, &stopped)) return stopped;   // stopped by SuspendThread (os_posix_suspend.cpp)
#endif
    const int prev = t->suspend.load(std::memory_order_relaxed);
    if (prev > 0) {
        pthread_mutex_lock(&t->mu);
        t->suspend.store(prev - 1, std::memory_order_relaxed);
        pthread_cond_broadcast(&t->cv);
        pthread_mutex_unlock(&t->mu);
    }
    return static_cast<DWORD>(prev);
#endif
}

void ExitThread(DWORD) {
    if (t_self) {
        t_self->exited.store(true, std::memory_order_release);
        wait_note();
    }
    pthread_exit(nullptr);
}

BOOL GetThreadContext(HANDLE h, LPCONTEXT ctx) {
    if (!ctx) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::memset(ctx, 0, sizeof *ctx);
    auto* t = as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
#if defined(__APPLE__) && defined(__aarch64__)
    // The caller suspended the thread (SuspendThread): thread_get_state fails on a running one.
    const mach_port_t mt = mach_thread_of(t);
    if (mt == MACH_PORT_NULL) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    arm_thread_state64_t st{};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(mt, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&st), &count) != KERN_SUCCESS) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    for (int i = 0; i < 29; i++) ctx->x[i] = st.__x[i];
    ctx->fp = st.__fp;
    ctx->lr = st.__lr;
    ctx->sp = st.__sp;
    ctx->pc = st.__pc;
    ctx->Cpsr = st.__cpsr;
    ctx->ContextFlags = 0x00100007u;   // CONTEXT_ARM64 | CONTROL | INTEGER | FLOATING_POINT: the whole file is there
    return TRUE;
#elif defined(__linux__)
    // the registers its stop signal's handler saved: the caller suspended it first (os_posix_suspend.cpp)
    if (!signal_context(t, ctx)) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return TRUE;
#else
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
#endif
}

BOOL SetThreadContext(HANDLE, const CONTEXT*) {
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

#endif  // !_WIN32
