// SPDX-License-Identifier: GPL-3.0-or-later
// std::atomic_ref for a standard library that does not have it yet: the NDK r28 libc++ (a trimmed LLVM 19 snapshot) leaves it out,
// although the compiler's builtins are there. The runtime uses atomic_ref on plain integers in guest-shared memory (HID rings, NVN
// semaphores, the co-op link queue). Force-included by CMake on Android only (runtime/CMakeLists.txt); a library that has it
// (__cpp_lib_atomic_ref) makes this file empty. Only the members the tree uses, on the __atomic builtins libc++ itself uses.
#pragma once
#if defined(__cplusplus)
#include <atomic>
#include <type_traits>

#if !defined(__cpp_lib_atomic_ref)
namespace std {

template <class T>
class atomic_ref {
    static_assert(is_integral_v<T> || is_pointer_v<T>, "this atomic_ref covers integers and pointers");
    T* p_;
    static constexpr int order(memory_order m) { return static_cast<int>(m); }

public:
    explicit atomic_ref(T& v) noexcept : p_(&v) {}
    T load(memory_order m = memory_order_seq_cst) const noexcept { return __atomic_load_n(p_, order(m)); }
    void store(T v, memory_order m = memory_order_seq_cst) const noexcept { __atomic_store_n(p_, v, order(m)); }
    T exchange(T v, memory_order m = memory_order_seq_cst) const noexcept { return __atomic_exchange_n(p_, v, order(m)); }
    T fetch_add(T v, memory_order m = memory_order_seq_cst) const noexcept { return __atomic_fetch_add(p_, v, order(m)); }
    T fetch_sub(T v, memory_order m = memory_order_seq_cst) const noexcept { return __atomic_fetch_sub(p_, v, order(m)); }
    bool compare_exchange_weak(T& expected, T desired, memory_order s, memory_order f) const noexcept {
        return __atomic_compare_exchange_n(p_, &expected, desired, true, order(s), order(f));
    }
    bool compare_exchange_weak(T& expected, T desired, memory_order m = memory_order_seq_cst) const noexcept {
        return compare_exchange_weak(expected, desired, m, fail_order(m));
    }
    bool compare_exchange_strong(T& expected, T desired, memory_order s, memory_order f) const noexcept {
        return __atomic_compare_exchange_n(p_, &expected, desired, false, order(s), order(f));
    }
    bool compare_exchange_strong(T& expected, T desired, memory_order m = memory_order_seq_cst) const noexcept {
        return compare_exchange_strong(expected, desired, m, fail_order(m));
    }

private:
    // The failure order a single-order compare_exchange uses ([atomics.ref.ops]): release -> relaxed, acq_rel -> acquire.
    static constexpr memory_order fail_order(memory_order m) {
        return m == memory_order_release ? memory_order_relaxed : m == memory_order_acq_rel ? memory_order_acquire : m;
    }
};

}  // namespace std
#endif
#endif
