// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to src/kernel: the poll wait (poll_wait.cpp, [kernel] poll_wait), as SleepThread and the SVC dispatcher use it.
//
// A guest loop that polls a value in guest memory and sleeps between polls has no waker: whoever changes the value only writes memory.
// The game layer lists such sleeps (nxrt/game.h PollWait: call site, and what the loop watches). A listed sleep registers its watch, and
// every guest thread that enters the kernel while one is registered looks at the watched values: an SVC is the one moment the host sees
// a guest thread, and a thread that has just finished the work the loop waits for soon makes one (it blocks for more work, signals,
// sleeps). When the value has changed the sleep ends there instead of on its time. The sleep never ends early for another reason, so the
// loop does not run more rounds than it did: the round after an early end is the one that finds its condition true.
#pragma once
#include <atomic>
#include <cstdint>

#include "nxrt/abi.h"

namespace nxrt::kernel {

constexpr int64_t kPollWaitMaxNs = 2000000;   // the longest SleepThread a listed call site may ask for and still be a poll wait

// SleepThread(ns > 0) of the calling guest thread: when the call site is listed and its watch applies, sleeps here (until the watched
// value changes, ns at most) and returns true; false: not a poll wait, the caller sleeps as usual.
bool poll_wait_sleep(const ::Ctx& c, int64_t ns);

namespace detail {
extern std::atomic<int32_t> g_poll_waiting;   // sleeps registered right now
void poll_wait_scan();
}  // namespace detail

// Every SVC of every guest thread, before its handler: one load while no poll wait is registered.
inline void poll_wait_check() {
    if (detail::g_poll_waiting.load(std::memory_order_relaxed) > 0) detail::poll_wait_scan();
}

}  // namespace nxrt::kernel
