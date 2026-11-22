// SPDX-License-Identifier: GPL-3.0-or-later
// Android build of the lifted code only: force-included (-include) before every lifted unit by the Android flag table, never part
// of recomp/include (no desktop stamp sees this folder). Lifter headers and the lifter's output are unchanged.
//
// A compare-and-swap whose expected and desired values are equal never changes memory: it returns the current value and succeeds
// when that value is the expected one. Lifted guest code uses that form a lot as an atomic read (CASAL with the same register or
// zero on both sides), and as a read-modify-write it takes the cache line exclusively on every call, which turns a poll of a word
// other threads write into cache-line ping-pong (a guest worker spent 45 % of its time in such CASes on a Tensor G2). Here that case
// is a full fence and a sequentially consistent load (DMB ISH; LDAR): the same value and success flag as the CAS, and ordering at least
// as strong as its acquire + release (every earlier access completes before the load, every later one waits for it), no write. Any other compare-and-swap is the compiler's builtin unchanged.
// Exclusive-monitor emulation (ctx.h stx*) is value based, so a store of the same value was never observable to it either.
#pragma once
#ifdef __cplusplus
#include <cstdint>

template <typename T, typename D>
static inline __attribute__((always_inline)) bool nx_cas_n(T *p, T *expected, D desired, bool weak, int success, int failure) {
    if constexpr (sizeof(T) <= 8) {
        if (*expected == T(desired)) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            T v = __atomic_load_n(p, __ATOMIC_SEQ_CST);
            bool ok = v == *expected;
            *expected = v;
            return ok;
        }
    }
    return __atomic_compare_exchange_n(p, expected, T(desired), weak, success, failure);
}
// Every __atomic_compare_exchange_n written after this point (lifter headers, lifted units) goes through nx_cas_n. The function above
// was parsed before the macro exists, so its own call is the builtin.
#define __atomic_compare_exchange_n(p, e, d, w, s, f) nx_cas_n((p), (e), (d), (w), (s), (f))
#endif
