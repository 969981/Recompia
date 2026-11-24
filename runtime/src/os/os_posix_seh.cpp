// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_seh.cpp: structured exceptions for src/os/nx_os.h -- the vectored handlers, the top-level filter, RaiseException,
// the guarded call (the __try/__except stand-in) and the signal handler they all run from, with its altstack and fault ring.
#include "nx_os_internal.h"

#if !defined(_WIN32)

#include <dlfcn.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <ucontext.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

using namespace nxos;

// ==================================================================================================================
// structured exceptions: BSD signals, not mach ports
//
// A BSD signal handler runs on the faulting thread and returning from it resumes the faulting instruction, which is
// what write_watch.cpp needs (un-protect the page, return, the store retries). A mach exception port would hand the
// fault to a helper thread and need thread_set_state to resume: more machinery for less. The CONTEXT is built from
// the ucontext_t register file, so a crash report carries the real pc/lr/sp/fp.
// ==================================================================================================================
namespace {

// The vectored handlers, in a fixed table. The signal handler runs on the faulting thread and MUST NOT allocate or take a
// lock: the first version copied a std::vector under a std::mutex, the allocation itself faulted (a guarded memcpy over a
// decommitted page), and the nested handler deadlocked on the mutex it already held. Registration is rare and takes the
// mutex; readers only load the published count (release) and the slots.
constexpr int kVehMax = 32;
std::atomic<PVECTORED_EXCEPTION_HANDLER> g_veh[kVehMax] = {};
std::atomic<int> g_veh_count{0};
std::atomic<uintptr_t> g_fault_pthread{0};   // pthread_self() of the thread that faulted, for the report's stack bounds
std::atomic<uintptr_t> g_image_base{0};      // the main image's load address, so a fault pc can be turned into an RVA
std::mutex g_veh_mu;   // registration only: never taken by the signal handler
LPTOP_LEVEL_EXCEPTION_FILTER g_top_filter = nullptr;
std::atomic<bool> g_signals_installed{false};

// The __try/__except stand-in (nx_os.h): a fault inside nxos_guarded_call jumps back to its sigsetjmp instead of
// reaching the top-level filter. One frame per nesting level, per thread.
struct GuardFrame {
    sigjmp_buf env;
    GuardFrame* prev;
};
thread_local GuardFrame* t_guard = nullptr;
// The name the runtime gave this thread (log_set_thread_name), read by the fault note and the report. A thread-local pointer,
// not pthread_getname_np: the handler may run on an exhausted stack, where a libc call can trap.
thread_local const char* t_thread_name = nullptr;

// The last faults, in a fixed ring written without locks or allocation from the signal handler. The report prints them: the
// fatal fault alone does not say whether it was the first, nor whether a guard was armed at the time.
struct FaultRec {
    uint64_t pc;
    uint64_t sp;
    uint64_t guard_ptr;   // t_guard at the time of the fault
    uint64_t slot;        // &t_guard, to tell a TLS mix-up from a plain null
    const char* name;     // the thread's name (t_thread_name)
    uint64_t tp_ro;       // tpidrro_el0 at the fault: the thread pointer the TLS access uses
    uint64_t tp;
    uint32_t sig;
    uint32_t guard;
};
FaultRec g_fault_ring[8] = {};
std::atomic<uint32_t> g_fault_seq{0};

void record_fault(int sig, uint64_t pc, uint64_t sp, bool guard) {
    const uint32_t i = g_fault_seq.fetch_add(1, std::memory_order_relaxed);
    uint64_t tp_ro = 0, tp = 0;
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp_ro));
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
#elif defined(__x86_64__)
    // x86-64 has a single thread pointer: %fs:0 is the TCB's own address, which is what a TLS access adds to. Both fields carry
    // it so the record keeps its shape (the Apple report reads them side by side).
    __asm__ volatile("movq %%fs:0, %0" : "=r"(tp));
    tp_ro = tp;
#endif
    g_fault_ring[i % 8] = FaultRec{pc, sp, reinterpret_cast<uint64_t>(t_guard), reinterpret_cast<uint64_t>(&t_guard),
                                   t_thread_name, tp_ro, tp, static_cast<uint32_t>(sig), guard ? 1u : 0u};
}

DWORD code_of_signal(int sig) {
    switch (sig) {
        case SIGSEGV: return EXCEPTION_ACCESS_VIOLATION;
        case SIGBUS: return EXCEPTION_IN_PAGE_ERROR;
        case SIGILL: return EXCEPTION_ILLEGAL_INSTRUCTION;
        case SIGFPE: return EXCEPTION_PRIV_INSTRUCTION;
        case SIGTRAP: return EXCEPTION_BREAKPOINT;
        case SIGABRT: return 0xE0000004u;   // "ABORT": the guest's abort()/std::terminate, reported like a fault
        default: return EXCEPTION_ACCESS_VIOLATION;
    }
}

void fill_context(CONTEXT* cx, void* uctx) {
    std::memset(cx, 0, sizeof *cx);
    auto* uc = static_cast<ucontext_t*>(uctx);
#if defined(__APPLE__) && defined(__aarch64__)
    const auto& ss = uc->uc_mcontext->__ss;
    for (int i = 0; i < 29; i++) cx->x[i] = ss.__x[i];
    cx->fp = ss.__fp;
    cx->lr = ss.__lr;
    cx->sp = ss.__sp;
    cx->pc = ss.__pc;
    cx->Cpsr = ss.__cpsr;
#elif defined(__x86_64__)
    const auto& g = uc->uc_mcontext.gregs;
    cx->pc = static_cast<ULONGLONG>(g[REG_RIP]);
    cx->sp = static_cast<ULONGLONG>(g[REG_RSP]);
    cx->fp = static_cast<ULONGLONG>(g[REG_RBP]);
#elif defined(__aarch64__)
    for (int i = 0; i < 29; i++) cx->x[i] = uc->uc_mcontext.regs[i];
    cx->sp = uc->uc_mcontext.sp;
    cx->pc = uc->uc_mcontext.pc;
#else
    (void)uc;
#endif
}

// Runs the registered handlers in order. No allocation and no lock: safe from the signal handler. True when a handler
// continued execution (the faulting instruction is resumed).
bool run_veh(EXCEPTION_POINTERS* ep) {
    const int n = std::min<int>(g_veh_count.load(std::memory_order_acquire), kVehMax);
    for (int i = 0; i < n; ++i) {
        PVECTORED_EXCEPTION_HANDLER h = g_veh[i].load(std::memory_order_acquire);
        if (h && h(ep) == EXCEPTION_CONTINUE_EXECUTION) return true;
    }
    return false;
}

// One line to fd 2 from the signal handler: allocation-free, no locks, no stdio (the crash report below allocates, and if it
// faults too the original fault would leave no trace).
void note_fault(int sig, void* addr, uint64_t pc, uint64_t sp) {
    char b[160];
    size_t n = 0;
    auto put = [&](const char* s) {
        while (*s && n < sizeof b - 1) b[n++] = *s++;
    };
    auto hex = [&](uint64_t v) {
        put("0x");
        char t[16];
        int k = 0;
        do {
            t[k++] = "0123456789abcdef"[v & 15u];
            v >>= 4;
        } while (v);
        while (k && n < sizeof b - 1) b[n++] = t[--k];
    };
    auto dec = [&](int v) {
        char t[12];
        int k = 0;
        if (v <= 0) {
            put("0");
            return;
        }
        while (v > 0) {
            t[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        }
        while (k && n < sizeof b - 1) b[n++] = t[--k];
    };
    put("nxrt: unhandled signal ");
    dec(sig);
    put(" at ");
    hex(reinterpret_cast<uintptr_t>(addr));
    put(", pc ");
    hex(pc);
    put(", sp ");
    hex(sp);
    put(" on ");
    put(t_thread_name && *t_thread_name ? t_thread_name : "(unnamed)");
    put(", base ");
    hex(g_image_base.load(std::memory_order_relaxed));
    put("\n");
    (void)::write(2, b, n);
}

void handler(int sig, siginfo_t* info, void* uctx) {
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = code_of_signal(sig);
    rec.ExceptionAddress = info ? info->si_addr : nullptr;
    // The Win32 access parameters the vectored handlers read (mem::ww::on_fault, the GPU guards): [0] 1 = write (a fault on a
    // page this process made read-only is a write; Darwin reports protection faults as SIGBUS and gives no direction), [1] the
    // address. Without them every handler saw NumberParameters 0 and let the fault through (write-watch, 2026-11-10).
    if (sig == SIGSEGV || sig == SIGBUS) {
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = 1;
        rec.ExceptionInformation[1] = reinterpret_cast<ULONG_PTR>(info ? info->si_addr : nullptr);
    }
    CONTEXT cx;
    fill_context(&cx, uctx);
    EXCEPTION_POINTERS ep{&rec, &cx};
    record_fault(sig, cx.pc, cx.sp, t_guard != nullptr);

    // SIGILL and SIGTRAP are never part of the guarded-call machinery (write watch, GPU page faults), so note them before
    // anything can swallow them: an illegal instruction that a vectored handler continues from would otherwise leave no trace.
    if (sig == SIGILL || sig == SIGTRAP) note_fault(sig, info ? info->si_addr : nullptr, cx.pc, cx.sp);
    if (run_veh(&ep)) return;   // resumes the faulting instruction
    // A guarded call is the SEH frame: on Windows the frame handler runs after the vectored handlers, so this does too.
    if (t_guard) siglongjmp(t_guard->env, 1);
    // Nothing handled it. Say where, with write(2) and hand-rolled hex: the crash report below allocates, and a fault inside
    // it (or a second one on another thread) would otherwise leave no trace of the original one.
    note_fault(sig, info ? info->si_addr : nullptr, cx.pc, cx.sp);
    if (g_top_filter && g_top_filter(&ep) != EXCEPTION_CONTINUE_SEARCH) return;
    // Nothing handled it: restore the default action and return, so the fault kills the process and the kernel writes
    // a core instead of the thread spinning on a re-faulting instruction.
    signal(sig, SIG_DFL);
}

void install_signals() {
    if (g_signals_installed.exchange(true)) return;
    nxos_install_altstack();   // the main thread; every other thread installs its own when it starts
    // Bind every libc stub the signal path calls, now, on a normal stack. The first call through an unbound PLT stub goes to
    // dyld's binder, and a binder call made from inside the handler (write/pthread_self in note_fault, siglongjmp for a
    // guarded copy) faults again and recurses through the handler until the altstack is gone: that was the nvn-pool-land
    // SIGILL with the handler/note_fault stack on 2026-11-10.
    {
        // Volatile sinks: the compiler drops calls whose result is unused (it did), and the stubs stayed unbound.
        static volatile intptr_t bind_sink = 0;
        // pthread_t is a pointer on Darwin, unsigned long on glibc and long on bionic: a C cast to uintptr_t takes all three.
        bind_sink = static_cast<intptr_t>((uintptr_t)::pthread_self());
        bind_sink = static_cast<intptr_t>(::write(-1, "x", 1));   // fd -1: fails with EBADF, binds the stub
        sigjmp_buf jb;
        const int jumped = sigsetjmp(jb, 1);
        bind_sink = jumped;
        if (!jumped) siglongjmp(jb, 1);
    }
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    // SIGABRT too: the guest's abort()/std::terminate is where a failed init ends, and without it the run just stops.
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT}) sigaction(sig, &sa, nullptr);
}

}  // namespace

extern "C" uintptr_t nxos_fault_pthread() { return g_fault_pthread.load(std::memory_order_relaxed); }

extern "C" int nxos_recent_faults(uint64_t* pcs, uint64_t* sps, uint32_t* sigs, uint32_t* guards, int max) {
    const uint32_t seq = g_fault_seq.load(std::memory_order_relaxed);
    const int n = static_cast<int>(seq < 8 ? seq : 8);
    const int k = n < max ? n : max;
    for (int i = 0; i < k; ++i) {
        const FaultRec& r = g_fault_ring[(seq - static_cast<uint32_t>(k) + static_cast<uint32_t>(i)) % 8];
        pcs[i] = r.pc;
        sps[i] = r.sp;
        sigs[i] = r.sig;
        guards[i] = r.guard;
    }
    return k;
}

extern "C" int nxos_recent_fault_details(uint64_t* guard_ptrs, uint64_t* slots, const char** names, uint64_t* tps, int max) {
    const uint32_t seq = g_fault_seq.load(std::memory_order_relaxed);
    const int n = static_cast<int>(seq < 8 ? seq : 8);
    const int k = n < max ? n : max;
    for (int i = 0; i < k; ++i) {
        const FaultRec& r = g_fault_ring[(seq - static_cast<uint32_t>(k) + static_cast<uint32_t>(i)) % 8];
        guard_ptrs[i] = r.guard_ptr;
        slots[i] = r.slot;
        names[i] = r.name;
        if (tps) { tps[2 * i] = r.tp_ro; tps[2 * i + 1] = r.tp; }
    }
    return k;
}

namespace {
// The main image's load address, once, before any thread exists: the note line can then print a fault pc as an RVA too.
struct ImageBaseInit {
    ImageBaseInit() {
        Dl_info dl{};
        if (dladdr(reinterpret_cast<void*>(&nxos_fault_pthread), &dl) && dl.dli_fbase)
            g_image_base.store(reinterpret_cast<uintptr_t>(dl.dli_fbase), std::memory_order_relaxed);
    }
} g_image_base_init;
}  // namespace

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler_fn) {
    if (!handler_fn) return nullptr;
    install_signals();
    std::lock_guard<std::mutex> g(g_veh_mu);
    const int n = g_veh_count.load(std::memory_order_relaxed);
    if (n >= kVehMax) {
        std::fprintf(stderr, "nxos: AddVectoredExceptionHandler: the table is full (%d handlers)\n", kVehMax);
        return nullptr;
    }
    if (first) {
        for (int i = n; i > 0; --i) g_veh[i].store(g_veh[i - 1].load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_veh[0].store(handler_fn, std::memory_order_relaxed);
    } else {
        g_veh[n].store(handler_fn, std::memory_order_relaxed);
    }
    g_veh_count.store(n + 1, std::memory_order_release);
    return reinterpret_cast<PVOID>(handler_fn);
}

ULONG RemoveVectoredExceptionHandler(PVOID handle) {
    std::lock_guard<std::mutex> g(g_veh_mu);
    const int n = g_veh_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        if (g_veh[i].load(std::memory_order_relaxed) == reinterpret_cast<PVECTORED_EXCEPTION_HANDLER>(handle)) {
            for (int k = i; k + 1 < n; ++k) g_veh[k].store(g_veh[k + 1].load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_veh[n - 1].store(nullptr, std::memory_order_relaxed);
            g_veh_count.store(n - 1, std::memory_order_release);
            return 1;
        }
    }
    return 0;
}

LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    install_signals();
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_top_filter;
    g_top_filter = filter;
    return prev;
}

void RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args) {
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags;
    rec.ExceptionAddress = __builtin_return_address(0);
    rec.NumberParameters = std::min<DWORD>(nargs, EXCEPTION_MAXIMUM_PARAMETERS);
    for (DWORD i = 0; i < rec.NumberParameters; i++) rec.ExceptionInformation[i] = args ? args[i] : 0;
    CONTEXT cx;
    std::memset(&cx, 0, sizeof cx);
    cx.pc = reinterpret_cast<ULONGLONG>(__builtin_return_address(0));
    cx.sp = reinterpret_cast<ULONGLONG>(__builtin_frame_address(0));
    EXCEPTION_POINTERS ep{&rec, &cx};

    if (run_veh(&ep)) return;
    if (t_guard) siglongjmp(t_guard->env, 1);
    if (g_top_filter && g_top_filter(&ep) != EXCEPTION_CONTINUE_SEARCH) return;
    // Every caller in the runtime raises one of its own trap codes and terminates on the next line; if control ever
    // comes back here the process state is unknown, so stop.
    std::fflush(nullptr);
    abort();
}

void nxos_install_altstack(void) {
    stack_t cur{};
#if defined(__ANDROID__)
    // bionic gives every thread a 16 KiB one of its own, too small for the handler (VEH chain, crash report): replaced below.
    if (sigaltstack(nullptr, &cur) == 0 && (cur.ss_flags & SS_DISABLE) == 0 && cur.ss_size >= 64 * 1024) return;
#else
    if (sigaltstack(nullptr, &cur) == 0 && (cur.ss_flags & SS_DISABLE) == 0) return;   // already installed
#endif
    // A separate mapping, NOT a thread_local buffer: macOS puts the TLS block at the bottom of the thread's stack, so a static
    // altstack sat exactly where a stack overflow lands and the handler then overwrote the thread's deepest live frames (the
    // land thread's overflow turned into handler->note_fault recursion on 2026-11-10). 64 KiB of stack over a 64 KiB guard.
    // Linux has the same requirement for the same reason: a guest thread that overflows must be reported, not lost.
    static thread_local char* alt = nullptr;
    if (!alt) {
        void* p = ::mmap(nullptr, 2 * 64 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) return;
        ::mprotect(p, 64 * 1024, PROT_NONE);
        alt = static_cast<char*>(p);
    }
    stack_t ss{};
    ss.ss_sp = alt + 64 * 1024;
    ss.ss_size = 64 * 1024;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) != 0)
        std::fprintf(stderr, "nxrt: sigaltstack failed: %s\n", std::strerror(errno));
}

void nxos_set_thread_name(const char* name) { t_thread_name = name; }

int nxos_guarded_call(void (*fn)(void*), void* arg) {
    if (!fn) return 1;
    install_signals();
    nxos_install_altstack();   // a guarded copy can fault on a thread that has not installed one yet
    GuardFrame frame;
    frame.prev = t_guard;
    t_guard = &frame;
    if (sigsetjmp(frame.env, 1) == 0) {
        fn(arg);
        t_guard = frame.prev;
        return 1;
    }
    t_guard = frame.prev;   // the signal handler's siglongjmp lands here
    return 0;
}

#endif  // !_WIN32
