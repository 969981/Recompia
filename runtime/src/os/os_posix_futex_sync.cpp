// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_futex_sync.cpp: SRWLOCK and CONDITION_VARIABLE on Linux and Android, straight on the futex (os_posix.cpp keeps the pthread
// version for macOS). The guest kernel takes these on every sync SVC (kernel_sync.cpp g_k), every handle lookup (handles.cpp) and
// every write-watch fault, so their cost is paid tens of thousands of times a second. The pthread version paid a mutex round trip per
// call and a condition broadcast per release, and bionic's broadcast always enters the kernel (glibc's skips it when nobody waits):
// one futex system call per lock release on a phone, and every release woke every thread waiting for the lock.
//
// Both objects are one pointer of zeroed static storage (nx_os.h), used here as two 32-bit words.
// SRWLOCK: the lock word (0 free, kWriter one exclusive holder, n > 0 that many shared holders; bit 31 set by a thread that is about to
// sleep on it) and the count of threads in a sleep loop. The algorithm is musl's pthread_rwlock (MIT, adapted; notice in runtime/THIRD_PARTY_NOTICES.md): an uncontended acquire or release
// is one compare-and-swap; a release enters the kernel only when somebody sleeps (the last shared holder wakes one, the exclusive holder
// wakes all). Like Win32's SRW lock it is not fair; a waiter spins about 100 rounds before it sleeps.
// CONDITION_VARIABLE: a sequence word and the count of sleepers. A sleeper counts itself and reads the sequence while it still holds
// the lock, then releases it and waits for the sequence to change; a wake moves the sequence and wakes one (or all) only when somebody
// sleeps. A wake between the release and the wait changes the word first, so the futex returns at once and no wake is lost. Spurious
// returns are allowed by the Win32 contract (every caller re-checks its condition).
#include "nx_os_internal.h"

#if defined(__linux__)

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <climits>
#include <cstdint>

namespace {

constexpr uint32_t kWriter = 0x7fffffff;
constexpr uint32_t kSleeper = 0x80000000;

uint32_t* word0(void* const* p) { return reinterpret_cast<uint32_t*>(const_cast<void**>(p)); }
uint32_t* word1(void* const* p) { return word0(p) + 1; }

uint32_t ld(const uint32_t* w) { return __atomic_load_n(w, __ATOMIC_SEQ_CST); }
bool cas(uint32_t* w, uint32_t* expected, uint32_t want) {
    return __atomic_compare_exchange_n(w, expected, want, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
void add(uint32_t* w, int32_t d) { __atomic_fetch_add(w, static_cast<uint32_t>(d), __ATOMIC_SEQ_CST); }

long futex_wait(uint32_t* w, uint32_t expected, const timespec* rel) {
    return syscall(SYS_futex, w, FUTEX_WAIT_PRIVATE, expected, rel, nullptr, 0);
}
void futex_wake(uint32_t* w, int n) { syscall(SYS_futex, w, FUTEX_WAKE_PRIVATE, n, nullptr, nullptr, 0); }

void relax() {
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

bool try_shared(uint32_t* l) {
    uint32_t v = ld(l);
    for (;;) {
        const uint32_t c = v & kWriter;
        if (c == kWriter || c == kWriter - 1) return false;
        if (cas(l, &v, v + 1)) return true;
    }
}

bool try_exclusive(uint32_t* l) {
    uint32_t v = 0;
    return cas(l, &v, kWriter);
}

template <bool kShared>
void acquire(SRWLOCK* lock) {
    uint32_t* const l = word0(&lock->impl);
    uint32_t* const waiters = word1(&lock->impl);
    if (kShared ? try_shared(l) : try_exclusive(l)) return;
    for (int spins = 100; spins-- && ld(l) && !ld(waiters);) relax();
    while (!(kShared ? try_shared(l) : try_exclusive(l))) {
        uint32_t r = ld(l);
        if (kShared ? (r & kWriter) != kWriter : r == 0) continue;   // free (for this kind of holder) again: retry
        const uint32_t t = r | kSleeper;
        add(waiters, 1);
        cas(l, &r, t);
        futex_wait(l, t, nullptr);
        add(waiters, -1);
    }
}

void release(SRWLOCK* lock) {
    uint32_t* const l = word0(&lock->impl);
    uint32_t v, n, c;
    do {
        v = ld(l);
        c = v & kWriter;
        n = (c == kWriter || c == 1) ? 0 : v - 1;
    } while (!cas(l, &v, n));
    if (!n && (ld(word1(&lock->impl)) || (v & kSleeper))) futex_wake(l, c == kWriter ? INT_MAX : 1);
}

}  // namespace

void InitializeSRWLock(PSRWLOCK lock) {
    __atomic_store_n(word0(&lock->impl), 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(word1(&lock->impl), 0u, __ATOMIC_SEQ_CST);
}
void AcquireSRWLockExclusive(PSRWLOCK lock) { acquire<false>(lock); }
void ReleaseSRWLockExclusive(PSRWLOCK lock) { release(lock); }
void AcquireSRWLockShared(PSRWLOCK lock) { acquire<true>(lock); }
void ReleaseSRWLockShared(PSRWLOCK lock) { release(lock); }
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock) { return try_exclusive(word0(&lock->impl)) ? 1 : 0; }
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK lock) { return try_shared(word0(&lock->impl)) ? 1 : 0; }

void InitializeConditionVariable(PCONDITION_VARIABLE cv) {
    __atomic_store_n(word0(&cv->impl), 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(word1(&cv->impl), 0u, __ATOMIC_SEQ_CST);
}

BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD ms, ULONG) {
    if ((ld(word0(&lock->impl)) & kWriter) != kWriter) {   // not held exclusively: the Win32 contract is broken at the call site
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    uint32_t* const seq = word0(&cv->impl);
    uint32_t* const sleepers = word1(&cv->impl);
    add(sleepers, 1);
    const uint32_t s = ld(seq);
    release(lock);
    timespec rel{};
    if (ms != INFINITE) {
        rel.tv_sec = static_cast<time_t>(ms / 1000);
        rel.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    }
    const long r = futex_wait(seq, s, ms == INFINITE ? nullptr : &rel);
    const bool timed_out = r == -1 && errno == ETIMEDOUT;
    add(sleepers, -1);
    acquire<false>(lock);
    if (timed_out) {
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
    if (!ld(word1(&cv->impl))) return;
    add(word0(&cv->impl), 1);
    futex_wake(word0(&cv->impl), 1);
}

void WakeAllConditionVariable(PCONDITION_VARIABLE cv) {
    if (!ld(word1(&cv->impl))) return;
    add(word0(&cv->impl), 1);
    futex_wake(word0(&cv->impl), INT_MAX);
}

#endif  // __linux__
