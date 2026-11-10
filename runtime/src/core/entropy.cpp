// SPDX-License-Identifier: GPL-3.0-or-later
// Random bytes (include/nxrt/entropy.h): the OS generator, or an installed seeded source.
#include "nxrt/entropy.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

#pragma comment(lib, "bcrypt")

namespace nxrt::entropy {
namespace {
std::atomic<const Source*> g_source{nullptr};

uint64_t splitmix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
}  // namespace

bool fill(void* p, size_t n) {
    if (const Source* s = g_source.load(std::memory_order_acquire)) {
        s->fill(p, n);
        return true;
    }
    uint8_t* b = static_cast<uint8_t*>(p);
    for (size_t off = 0; off < n;) {
        const size_t m = std::min<size_t>(n - off, 0x10000000);
        const NTSTATUS st = BCryptGenRandom(nullptr, b + off, static_cast<ULONG>(m), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(st)) {
            NXLOG_E("entropy", "BCryptGenRandom failed: 0x%lX (%zu bytes from the host counter instead)", static_cast<unsigned long>(st), n - off);
            uint64_t x = qpc::ticks() ^ reinterpret_cast<uintptr_t>(p);
            for (; off < n; off += 8) {
                x = splitmix(x);
                std::memcpy(b + off, &x, std::min<size_t>(8, n - off));
            }
            return false;
        }
        off += m;
    }
    return true;
}

void set_source(const Source* s) { g_source.store(s, std::memory_order_release); }

bool seeded() { return g_source.load(std::memory_order_acquire) != nullptr; }

}  // namespace nxrt::entropy
