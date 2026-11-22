// SPDX-License-Identifier: GPL-3.0-or-later
// fiber_selftest.cpp: a standalone check of the Android fiber switch (runtime/src/os/os_posix_fiber.cpp, nxos_fiber_swap). It is
// not part of any build and links no game code: it compiles the real os_posix_fiber.cpp next to this file and checks, with
// synthetic values only, what a switch has to keep.
//   1. Every AAPCS64 callee-saved register (x19-x28, d8-d15) holds a pattern across SwitchToFiber, on both sides of the switch.
//   2. Two fibers and the thread's own fiber ping-pong many times, each keeping its own state.
//   3. A fiber suspended on one thread resumes on another, and sees that thread's thread-locals.
//
// Build (NDK r28, from the repository root) and run on an arm64 device, or under qemu-aarch64 user mode:
//   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++ --target=aarch64-linux-android30 -std=c++20 -O2 -static \
//       -Iruntime/src/os -Iruntime/include tools/android/fiber-selftest/fiber_selftest.cpp runtime/src/os/os_posix_fiber.cpp \
//       -o fiber_selftest
//   ./fiber_selftest        (prints "fiber selftest: ok" and exits 0, or names the first failed check and exits 1)
#include "nx_os.h"

#include <pthread.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if !defined(__ANDROID__) || !defined(__aarch64__)
#error "the Android fiber switch is AArch64 Android only"
#endif

// The one shim call os_posix_fiber.cpp makes besides the fiber API (the real one lives in os_posix.cpp).
void SetLastError(DWORD) {}

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "fiber selftest: FAILED: %s\n", what);
        ++g_failures;
    }
}

// Puts a pattern in every callee-saved register, switches to `to`, and checks the pattern is still there when this side resumes.
// The register variables are pinned with asm constraints on both sides of the call, so the compiler keeps them in exactly those
// registers across it instead of spilling them.
void switch_keeping_registers(LPVOID to, uint64_t seed) {
    register uint64_t x19 asm("x19") = seed ^ 19, x20 asm("x20") = seed ^ 20, x21 asm("x21") = seed ^ 21, x22 asm("x22") = seed ^ 22;
    register uint64_t x23 asm("x23") = seed ^ 23, x24 asm("x24") = seed ^ 24, x25 asm("x25") = seed ^ 25, x26 asm("x26") = seed ^ 26;
    register uint64_t x27 asm("x27") = seed ^ 27, x28 asm("x28") = seed ^ 28;
    register double d8 asm("d8") = double(seed % 1000) + 0.8, d9 asm("d9") = double(seed % 1000) + 0.9;
    register double d10 asm("d10") = double(seed % 1000) + 0.10, d11 asm("d11") = double(seed % 1000) + 0.11;
    register double d12 asm("d12") = double(seed % 1000) + 0.12, d13 asm("d13") = double(seed % 1000) + 0.13;
    register double d14 asm("d14") = double(seed % 1000) + 0.14, d15 asm("d15") = double(seed % 1000) + 0.15;
    asm volatile("" : "+r"(x19), "+r"(x20), "+r"(x21), "+r"(x22), "+r"(x23), "+r"(x24), "+r"(x25), "+r"(x26), "+r"(x27), "+r"(x28));
    asm volatile("" : "+w"(d8), "+w"(d9), "+w"(d10), "+w"(d11), "+w"(d12), "+w"(d13), "+w"(d14), "+w"(d15));
    SwitchToFiber(to);
    asm volatile("" : "+r"(x19), "+r"(x20), "+r"(x21), "+r"(x22), "+r"(x23), "+r"(x24), "+r"(x25), "+r"(x26), "+r"(x27), "+r"(x28));
    asm volatile("" : "+w"(d8), "+w"(d9), "+w"(d10), "+w"(d11), "+w"(d12), "+w"(d13), "+w"(d14), "+w"(d15));
    const bool x_ok = x19 == (seed ^ 19) && x20 == (seed ^ 20) && x21 == (seed ^ 21) && x22 == (seed ^ 22) && x23 == (seed ^ 23) &&
                      x24 == (seed ^ 24) && x25 == (seed ^ 25) && x26 == (seed ^ 26) && x27 == (seed ^ 27) && x28 == (seed ^ 28);
    const double b = double(seed % 1000);
    const bool d_ok = d8 == b + 0.8 && d9 == b + 0.9 && d10 == b + 0.10 && d11 == b + 0.11 && d12 == b + 0.12 && d13 == b + 0.13 &&
                      d14 == b + 0.14 && d15 == b + 0.15;
    check(x_ok, "x19-x28 kept across a switch");
    check(d_ok, "d8-d15 kept across a switch");
}

constexpr int kRounds = 10000;
LPVOID g_main = nullptr, g_a = nullptr, g_b = nullptr;
int g_a_count = 0, g_b_count = 0;

// A and B: each round A switches to B, B back to main; both keep their own counter and registers.
void WINAPI fiber_a(LPVOID) {
    for (int i = 0; i < kRounds; i++) {
        ++g_a_count;
        switch_keeping_registers(g_b, 0xa000000000000000ull + uint64_t(i));
    }
    check(false, "fiber A ran past its last round");
    for (;;) SwitchToFiber(g_main);
}

void WINAPI fiber_b(LPVOID) {
    for (int i = 0; i < kRounds; i++) {
        ++g_b_count;
        switch_keeping_registers(g_main, 0xb000000000000000ull + uint64_t(i));
    }
    check(false, "fiber B ran past its last round");
    for (;;) SwitchToFiber(g_main);
}

// Cross-thread: fiber C records the thread it runs on before and after one switch; main suspends it, a second thread resumes it.
// The tag is read in a function of its own: within one function the compiler may keep a thread-local's address across a call,
// which a fiber resumed on another thread would then read through (the contract of every fiber implementation, ucontext and
// Windows alike: nothing holds a thread-local's address across a switch, SwitchToFiber's comment).
thread_local int t_thread_tag = 0;
__attribute__((noinline)) int thread_tag() { return t_thread_tag; }
LPVOID g_c = nullptr, g_c_back = nullptr;
int g_c_tag_before = 0, g_c_tag_after = 0;

void WINAPI fiber_c(LPVOID) {
    g_c_tag_before = thread_tag();
    switch_keeping_registers(g_c_back, 0xc0c0c0c0c0c0c0c0ull);
    g_c_tag_after = thread_tag();
    switch_keeping_registers(g_c_back, 0xc1c1c1c1c1c1c1c1ull);
    for (;;) SwitchToFiber(g_c_back);
}

void* second_thread(void*) {
    t_thread_tag = 2;
    g_c_back = ConvertThreadToFiber(nullptr);
    switch_keeping_registers(g_c, 0x2222222222222222ull);   // resumes C where main's thread left it
    return nullptr;
}

}  // namespace

int main() {
    t_thread_tag = 1;
    g_main = ConvertThreadToFiber(nullptr);
    g_a = CreateFiberEx(0, 256 * 1024, 0, fiber_a, nullptr);
    g_b = CreateFiberEx(0, 256 * 1024, 0, fiber_b, nullptr);
    check(g_main && g_a && g_b, "fibers created");
    if (g_failures) return 1;
    for (int i = 0; i < kRounds; i++) switch_keeping_registers(g_a, 0x1000000000000000ull + uint64_t(i));
    check(g_a_count == kRounds && g_b_count == kRounds, "every round ran once in each fiber");

    g_c = CreateFiberEx(0, 256 * 1024, 0, fiber_c, nullptr);
    g_c_back = g_main;
    switch_keeping_registers(g_c, 0x1111111111111111ull);   // C starts on this thread and switches back
    pthread_t t;
    check(pthread_create(&t, nullptr, second_thread, nullptr) == 0, "second thread started");
    pthread_join(t, nullptr);
    check(g_c_tag_before == 1 && g_c_tag_after == 2, "a fiber resumed on another thread sees that thread's thread-locals");

    if (g_failures) return 1;
    std::printf("fiber selftest: ok (%d rounds, cross-thread resume)\n", kRounds);
    return 0;
}
