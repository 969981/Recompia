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
// deprecated, and where they are not implemented CreateFiberEx fails with ERROR_NOT_SUPPORTED.
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
#include <ucontext.h>
#include <unistd.h>

#include <cstdio>
#include <new>

namespace {

struct Fiber {
    ucontext_t uc;
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
    return f;
}

void SwitchToFiber(LPVOID fiber) {
    Fiber* to = static_cast<Fiber*>(fiber);
    Fiber* from = t_current;
    if (!to || !from || to == from) return;   // only a fiber can switch (Windows: undefined otherwise); to itself: nothing to do
    t_current = to;
    swapcontext(&from->uc, &to->uc);
    // Resumed, maybe on another thread: the switch there made this fiber its current one. Nothing here reads t_current again.
}

BOOL IsThreadAFiber(void) { return t_current != nullptr; }

LPVOID GetCurrentFiber(void) { return t_current; }

#endif  // !_WIN32
