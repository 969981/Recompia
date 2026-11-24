// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_suspend.cpp: SuspendThread / ResumeThread / GetThreadContext for a running thread on Linux, which the
// diagnostics that read another thread's registers rely on (diag/sampler.cpp, diag/watchdog.cpp). Linux has no
// thread_suspend (macOS uses the mach call, os_posix_thread.cpp), so the suspender sends the thread a real-time signal with
// tgkill; the handler copies the registers out of its ucontext, publishes kStopped and waits on a futex until
// ResumeThread sets kRunning. The handler only touches atomics, the futex syscall and errno (async-signal-safe), and
// reads the thread's object through thread_if_known(), which never allocates. tgkill rather than pthread_kill: a thread
// that exits between the check and the signal makes tgkill fail with ESRCH, where pthread_kill on a finished detached
// thread is undefined. SA_RESTART restarts the interrupted system calls that can be restarted; the rest see EINTR,
// which their callers already retry (Sleep, the wait plumbing).
#include "nx_os_internal.h"

#if defined(__linux__)

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

namespace nxos {
namespace {

enum : int { kRunning = 0, kAsked = 1, kStopped = 2 };

// How long SuspendThread waits for the handler. A thread that has the signal blocked, or is stopped in a debugger, is
// reported as not suspendable instead of stalling the sampler.
constexpr long kStopWaitNs = 100'000'000;

static_assert(sizeof(std::atomic<int>) == sizeof(int) && std::atomic<int>::is_always_lock_free,
              "the futex word is the atomic's storage");

int stop_signal() { return SIGRTMIN + 6; }   // SIGRTMIN already skips the ones glibc keeps for itself

long futex(std::atomic<int>* word, int op, int val, const struct timespec* timeout) {
    return syscall(SYS_futex, reinterpret_cast<int*>(word), op, val, timeout, nullptr, 0);
}

void save_registers(ThreadObj* t, const ucontext_t* uc) {
#if defined(__x86_64__)
    // CONTEXT is the arm64 register file on every POSIX target (nx_os.h); on x86-64, pc/sp/fp carry rip/rsp/rbp (the
    // stack walker follows the rbp chain, whose two-word records match arm64's) and x[] the other integer registers.
    const greg_t* g = uc->uc_mcontext.gregs;
    static constexpr int kGeneral[] = {REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RSI, REG_RDI, REG_R8,  REG_R9,
                                       REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
    for (size_t i = 0; i < sizeof kGeneral / sizeof kGeneral[0]; i++) t->sig_x[i] = static_cast<uint64_t>(g[kGeneral[i]]);
    t->sig_pc = static_cast<uint64_t>(g[REG_RIP]);
    t->sig_sp = static_cast<uint64_t>(g[REG_RSP]);
    t->sig_fp = static_cast<uint64_t>(g[REG_RBP]);
    t->sig_lr = 0;
#elif defined(__aarch64__)
    const auto& m = uc->uc_mcontext;
    for (int i = 0; i < 29; i++) t->sig_x[i] = m.regs[i];
    t->sig_fp = m.regs[29];
    t->sig_lr = m.regs[30];
    t->sig_sp = m.sp;
    t->sig_pc = m.pc;
#else
    (void)t;
    (void)uc;
#endif
}

void on_stop_signal(int, siginfo_t*, void* uc) {
    const int saved_errno = errno;
    ThreadObj* t = thread_if_known();
    if (t && t->sig_state.load(std::memory_order_acquire) == kAsked) {
        save_registers(t, static_cast<const ucontext_t*>(uc));
        int asked = kAsked;
        if (t->sig_state.compare_exchange_strong(asked, kStopped, std::memory_order_acq_rel)) {
            futex(&t->sig_state, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr);
            while (t->sig_state.load(std::memory_order_acquire) == kStopped)
                futex(&t->sig_state, FUTEX_WAIT_PRIVATE, kStopped, nullptr);
        }
    }
    errno = saved_errno;
}

void install_handler() {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, [] {
        struct sigaction sa {};
        sa.sa_sigaction = on_stop_signal;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigfillset(&sa.sa_mask);
        sigaction(stop_signal(), &sa, nullptr);
    });
}

long remaining_ns(const struct timespec& end) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (end.tv_sec - now.tv_sec) * 1'000'000'000L + (end.tv_nsec - now.tv_nsec);
}

// Signals t and waits for its handler to report kStopped. False (and t left running) when it does not stop in time.
bool stop(ThreadObj* t, pid_t tid) {
    t->sig_state.store(kAsked, std::memory_order_release);
    if (syscall(SYS_tgkill, getpid(), tid, stop_signal()) != 0) {
        t->sig_state.store(kRunning, std::memory_order_release);
        return false;
    }
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_nsec += kStopWaitNs;
    end.tv_sec += end.tv_nsec / 1'000'000'000L;
    end.tv_nsec %= 1'000'000'000L;
    while (t->sig_state.load(std::memory_order_acquire) == kAsked) {
        const long left = remaining_ns(end);
        if (left <= 0) break;
        const struct timespec wait{0, left};
        futex(&t->sig_state, FUTEX_WAIT_PRIVATE, kAsked, &wait);
    }
    int asked = kAsked;
    // the handler may stop the thread between the last check and this exchange: then it counts as stopped
    return !t->sig_state.compare_exchange_strong(asked, kRunning, std::memory_order_acq_rel);
}

}  // namespace

void note_kernel_tid(ThreadObj* t) { t->sig_tid.store(static_cast<pid_t>(syscall(SYS_gettid)), std::memory_order_release); }

bool signal_suspend(ThreadObj* t, DWORD* prev) {
    const pid_t tid = t->sig_tid.load(std::memory_order_acquire);
    if (tid == 0 || t->exited.load(std::memory_order_acquire) || t == thread_if_known()) return false;
    install_handler();
    pthread_mutex_lock(&t->sig_mu);
    const bool ok = t->sig_depth > 0 || stop(t, tid);
    if (ok) *prev = static_cast<DWORD>(t->sig_depth++);
    pthread_mutex_unlock(&t->sig_mu);
    return ok;
}

bool signal_resume(ThreadObj* t, DWORD* prev) {
    pthread_mutex_lock(&t->sig_mu);
    const bool mine = t->sig_depth > 0;
    if (mine) {
        *prev = static_cast<DWORD>(t->sig_depth--);
        if (t->sig_depth == 0) {
            t->sig_state.store(kRunning, std::memory_order_release);
            futex(&t->sig_state, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr);
        }
    }
    pthread_mutex_unlock(&t->sig_mu);
    return mine;
}

bool signal_context(ThreadObj* t, CONTEXT* ctx) {
    if (t->sig_state.load(std::memory_order_acquire) != kStopped) return false;
    for (int i = 0; i < 29; i++) ctx->x[i] = t->sig_x[i];
    ctx->fp = t->sig_fp;
    ctx->lr = t->sig_lr;
    ctx->sp = t->sig_sp;
    ctx->pc = t->sig_pc;
    ctx->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    return true;
}

}  // namespace nxos

#endif  // __linux__
