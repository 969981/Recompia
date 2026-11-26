// SPDX-License-Identifier: GPL-3.0-or-later
// The submission slots' upload rings: their size ([nvn] upload_slot_kb), buffer usage, and the use and overflow statistics in the log.
// The slots and their rings live in nvn_gpu.cpp.
#include <algorithm>
#include <atomic>
#include <chrono>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/platform_facts.h"
#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "nvn.gpu";

std::atomic<uint64_t> g_up_peak{0}, g_up_resets{0}, g_up_over{0}, g_up_over_ns{0};
std::atomic<uint64_t> g_up_hist[33];   // ring bytes a submission used, in 256 KiB buckets (32 = 8 MiB or more)

// Every 5000 submissions: the distribution of ring use per submission (p50/p90/p99/max, and how many would not fit 2/3/4/6 MiB), and the
// overflow cost since the last line (uploads that took a staging buffer, and the CPU ms spent making them).
void upload_ring_report() {
    static uint64_t last_over = 0, last_ns = 0;
    static auto last_t = std::chrono::steady_clock::now();
    uint64_t h[33], n = 0;
    for (int i = 0; i < 33; ++i) n += (h[i] = g_up_hist[i].load(std::memory_order_relaxed));
    if (!n) return;
    auto pct = [&](double q) { uint64_t a = 0; for (int i = 0; i < 33; ++i) { a += h[i]; if (a >= q * n) return (i + 1) * 256; } return 33 * 256; };
    auto over = [&](int kib) { uint64_t a = 0; for (int i = kib / 256; i < 33; ++i) a += h[i]; return 100.0 * a / n; };
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::max(1e-3, std::chrono::duration<double>(now - last_t).count());
    const uint64_t ov = g_up_over.load(), ns = g_up_over_ns.load();
    NXLOG_I(kTag, "upload rings: per submission p50 <%d KiB, p90 <%d, p99 <%d, max %llu KiB; over 2/3/4/6 MiB: %.2f/%.2f/%.2f/%.3f %% of %llu; "
                  "ring %llu KiB: %.1f staging uploads/s, %.2f ms/s making them",
            pct(.5), pct(.9), pct(.99), static_cast<unsigned long long>(g_up_peak.load() >> 10), over(2048), over(3072), over(4096), over(6144),
            static_cast<unsigned long long>(n), static_cast<unsigned long long>(upload_ring_bytes() >> 10), (ov - last_over) / secs, (ns - last_ns) / 1e6 / secs);
    last_over = ov; last_ns = ns; last_t = now;
}

}  // namespace

// Per slot; larger uploads get their own staging buffer. [nvn] upload_slot_kb (64..8192, default 8192): smaller rings hold less memory
// (36 slots x 8 MiB = 288 MiB of GPU memory); an upload that does not fit takes a staging buffer as before. The peak use is logged.
VkDeviceSize upload_ring_bytes() {
    static const VkDeviceSize b = static_cast<VkDeviceSize>(std::clamp<int64_t>(nxrt::config_get_int("nvn", "upload_slot_kb", 8192), 64, 8192)) << 10;
    return b;
}

VkBufferUsageFlags upload_ring_usage() {
    return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |   // quad_args_gpu: indexed indirect arguments a dispatch writes
           (facts::kPhone ? VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT : 0u);           // phones: copies of texel buffers at an offset the GPU cannot view
}

void upload_ring_note(VkDeviceSize used) {
    if (used > g_up_peak.load(std::memory_order_relaxed)) g_up_peak.store(used, std::memory_order_relaxed);
    g_up_hist[std::min<uint64_t>(32, used >> 18)].fetch_add(1, std::memory_order_relaxed);
    if ((g_up_resets.fetch_add(1, std::memory_order_relaxed) % 5000) == 4999) upload_ring_report();
}

void upload_ring_overflow_begin() { g_up_over.fetch_add(1, std::memory_order_relaxed); }

void upload_ring_overflow_end(uint64_t ns) { g_up_over_ns.fetch_add(ns, std::memory_order_relaxed); }

}  // namespace nxrt::nvn::be::gpu
