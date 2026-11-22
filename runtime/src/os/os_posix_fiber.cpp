// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_fiber.cpp: Win32 fibers over ucontext, for services/network_cloud.cpp (it runs the SDK's nn::os fibers on
// host fibers).
//
// A fiber is a ucontext_t and the stack it runs on. ConvertThreadToFiber gives the calling thread a fiber with no stack
// of its own (its registers are saved there by the first switch away); CreateFiberEx maps the stack, its reservation
// size with an inaccessible guard page below, as Windows reserves one; SwitchToFiber saves the running fiber and
// resumes another on the calling thread. As with Windows fibers, a fiber may be resumed on a different thread than the
// one that suspended it, and thread-local storage is always the running thread's. The current fiber is one of those
// thread-locals: a switch sets it before swapping and does not read it afterwards, when it may be on another thread.
//
// Windows ends the thread when a fiber's start routine returns; so does this. On Darwin the ucontext calls are
// deprecated, and where they are not implemented CreateFiberEx fails with ERROR_NOT_SUPPORTED. Android's bionic has no
// ucontext calls at all: there a fiber is the AArch64 callee-saved register set (AAPCS64: x19-x28, fp, lr, sp, d8-d15) and
// fiber_swap below saves one and loads the other, which is everything a call boundary has to preserve. A thread per fiber
// would not do: the lifted code on a fiber finds its guest thread through the running thread's thread-locals.
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700   // <ucontext.h> refuses to compile without it on Darwin
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1  // keep the Darwin extensions _XOPEN_SOURCE would hide
#endif

#include "nx_os.h"

#if !defined(_WIN32)

#include <pthread.h>
#include <sys/mman.h>
#if defined(__ANDROID__)
#include <cstdint>
#else
#include <ucontext.h>
#endif
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <new>

namespace {

#if defined(__ANDROID__)
struct Context {
    uint64_t x19_x28[10];
    uint64_t fp, lr, sp;
    uint64_t d8_d15[8];   // the low 64 bits of v8-v15, all AAPCS64 keeps across a call
};
static_assert(sizeof(Context) == 168, "fiber_swap's offsets");
#endif

struct Fiber {
#if defined(__ANDROID__)
    Context ctx{};
#else
    ucontext_t uc;
#endif
    LPFIBER_START_ROUTINE start = nullptr;   // null: a thread converted to a fiber (it runs on the thread's own stack)
    LPVOID param = nullptr;
    void* map = nullptr;                     // the stack mapping, guard page included
    size_t map_size = 0;
};

thread_local Fiber* t_current = nullptr;

// makecontext's entry: the switch that started this fiber made it the current one of the thread it runs on.
void fiber_entry() {
    Fiber* f = t_current;
    f->start(f->param);
    std::fprintf(stderr, "nxrt: a fiber's start routine returned: ending its thread, as Windows does\n");
    pthread_exit(nullptr);
}

}  // namespace

#if defined(__ANDROID__)
// Saves the callee-saved registers into *save and returns into the context in *load: where it last called fiber_swap, or, for a new
// fiber, fiber_entry on the fiber's own stack (CreateFiberEx).
extern "C" void nxos_fiber_swap(Context* save, const Context* load);
asm(R"(
    .text
    .p2align 2
    .globl nxos_fiber_swap
    .hidden nxos_fiber_swap
    .type nxos_fiber_swap, %function
nxos_fiber_swap:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    ldp x19, x20, [x1, #0]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldp x29, x30, [x1, #80]
    ldr x9, [x1, #96]
    mov sp, x9
    ldp d8, d9, [x1, #104]
    ldp d10, d11, [x1, #120]
    ldp d12, d13, [x1, #136]
    ldp d14, d15, [x1, #152]
    ret
    .size nxos_fiber_swap, .-nxos_fiber_swap
)");
#endif

LPVOID ConvertThreadToFiber(LPVOID param) {
    if (t_current) {
        SetLastError(ERROR_ALREADY_FIBER);
        return nullptr;
    }
    Fiber* f = new (std::nothrow) Fiber{};
    if (!f) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    f->param = param;
    t_current = f;
    return f;
}

LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE start, LPVOID param) {
    (void)flags;   // FIBER_FLAG_FLOAT_SWITCH: a ucontext always holds the floating-point registers
    if (!start) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    size_t size = reserve > commit ? reserve : commit;
    if (size == 0) size = 1u << 20;   // Windows: the image's default reservation, 1 MiB
    size = (size + page - 1) & ~(page - 1);
    void* m = mmap(nullptr, size + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    mprotect(m, page, PROT_NONE);   // the guard page: an overflow faults instead of running into the next mapping
    Fiber* f = new (std::nothrow) Fiber{};
    if (!f) {
        munmap(m, size + page);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    f->start = start;
    f->param = param;
    f->map = m;
    f->map_size = size + page;
#if defined(__ANDROID__)
    // The first switch "returns" into fiber_entry at the top of the stack (16-byte aligned, a zero frame pointer ends a stack
    // walk). fiber_entry never returns, so nothing has to sit above it.
    f->ctx.sp = reinterpret_cast<uint64_t>(static_cast<char*>(m) + page + size) & ~uint64_t{15};
    f->ctx.lr = reinterpret_cast<uint64_t>(&fiber_entry);
    f->ctx.fp = 0;
#else
    if (getcontext(&f->uc) != 0) {
        munmap(m, size + page);
        delete f;
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    f->uc.uc_stack.ss_sp = static_cast<char*>(m) + page;
    f->uc.uc_stack.ss_size = size;
    f->uc.uc_link = nullptr;
    makecontext(&f->uc, fiber_entry, 0);
#endif
    return f;
}

void SwitchToFiber(LPVOID fiber) {
    Fiber* to = static_cast<Fiber*>(fiber);
    Fiber* from = t_current;
    if (!to || !from || to == from) return;   // only a fiber can switch (Windows: undefined otherwise); to itself: nothing to do
    t_current = to;
#if defined(__ANDROID__)
    nxos_fiber_swap(&from->ctx, &to->ctx);
#else
    swapcontext(&from->uc, &to->uc);
#endif
    // Resumed, maybe on another thread: the switch there made this fiber its current one. Nothing here reads t_current again.
}

BOOL IsThreadAFiber(void) { return t_current != nullptr; }

LPVOID GetCurrentFiber(void) { return t_current; }

#endif  // !_WIN32
