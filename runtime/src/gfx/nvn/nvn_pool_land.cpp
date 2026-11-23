// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] pool_copy: GPU writes go back into guest memory ("landing"; the rules are at the top of nvn_pool_copy.cpp). The render thread
// records every range a command may write in pool memory with its submission's serial (land_record). Once the timeline reaches that
// serial, the range is copied from the shadow into guest memory, before the serial is published as complete: by whichever thread reads
// the timeline first (pcopy_land from read_completed), which is the nvn-pool-land thread here unless a game thread polls in the moment
// between the GPU finishing and that thread waking up.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "nxrt/ftrace.h"
#include "nxrt/log.h"
#include "nxrt/write_watch.h"
#include "nxrt/config.h"
#include "nxrt/ab_switch.h"
#include "nxrt/platform_facts.h"
#include "nvn_gpu_internal.h"
#include "nvn_pool_copy_impl.h"
#include "nvn_pool_track.h"
#include <vk_mem_alloc.h>

#include "nvn_thread.h"   // BigThread: 512 KiB is the macOS default stack, and the landing runs guarded_copy

namespace nxrt::nvn::be::gpu {
namespace {

using pcopy::kPage;
using pcopy::kTag;
constexpr uint64_t kNone = ~0ull;         // g_land_min with no record
// [nvn] pool_copy_land_keep: a record of at most this many bytes has every page it kept compared after the copy; a larger one only the
// bytes of its first and last page that lie outside the record (see land_one).
constexpr uint64_t kCompareAll = 64 * 1024;

// GPU writes waiting for their serial, in serial order (recorded by the render thread as it records). Lock order:
// g_live_mu -> g_reg_mu -> g_landing_mu -> g_land_mu. g_land_mu guards only the queue, land_refs and g_land_min and is held for short sections
// (the render thread takes it per recorded write and in pcopy_busy); it is a leaf. g_landing_mu serialises landers and is held across a whole
// landing (pop, memcpy, before_os_write, page bits); before_os_write could call the compute-out guard handler, which copy pools never arm.
struct LandRec {
    PoolGpu* pool;
    PoolChunk* chunk;
    uint64_t pool_off, size, serial;
    uint64_t ring_off = kNone, ring_end = 0;   // phones, [nvn] pool_ring: where the GPU copied the range in the readback ring (kNone: no copy)
};
std::mutex g_land_mu;
std::mutex g_landing_mu;
std::deque<LandRec> g_land;
std::vector<LandRec> g_landing;                 // the batch a lander took out of g_land and is copying (under g_land_mu; one lander at a time)
std::atomic<uint64_t> g_land_min{kNone};        // serial of g_land.front() (kNone: empty), the lock-free test of pcopy_land
thread_local bool t_landing = false;            // a landing on this thread (a nested read_completed returns at once)

// the landing thread
std::mutex g_th_mu;
std::condition_variable g_th_cv;
BigThread g_th;
bool g_th_stop = false;

void land_min_update() { g_land_min.store(g_land.empty() ? kNone : g_land.front().serial, std::memory_order_release); }

// ---- [nvn] pool_ring (phones, default on; A/B name "pool_ring") ---------------------------------------------------------------------------
// The shadows live in the memory type the GPU reads in place, which on the phones' GPUs is coherent but not cached for the CPU: the landing's
// memcpy out of a shadow ran at about 0.6 GB/s (the nvn-pool-land thread 10 to 18 ms of CPU per frame on a Tensor G2 (Mali-G710), 5 MiB a frame). At
// the end of every submission the GPU copies each range it recorded as written (vkCmdCopyBuffer, after a barrier on every write of the
// submission) into this ring: one buffer of host-cached memory ([nvn] pool_ring_mb, default 32) that only the GPU ever writes. The
// landing then invalidates the range (the memory may be non-coherent; VMA rounds to nonCoherentAtomSize) and copies from cached memory.
// The ring holds what the shadow held at the end of the submission, which is what a landing read before (a later submission's writes into
// the same range are its own records). A range that finds no room, or a record made outside the recording of its submission, lands from
// the shadow as before. Head and tail advance in record order (under g_land_mu): a landing frees up to the end of its last ring copy.
struct Ring {
    gfx::Buffer buf;
    uint64_t size = 0, head = 0, tail = 0;   // head / tail: running byte counts (position = count % size)
    bool tried = false;
};
Ring g_ring;
constexpr uint64_t kRingAlign = 256;       // >= every nonCoherentAtomSize and copy alignment in practice

bool ring_wanted() {
    static const bool on = config_get_bool("nvn", "pool_ring", true);
    return facts::kPhone && diag::ab("pool_ring", on);
}

// Render thread. Made the first time it is wanted; false when it cannot be.
bool ring_ready() {
    if (g_ring.buf) return true;
    if (g_ring.tried) return false;
    g_ring.tried = true;
    const uint64_t mb = static_cast<uint64_t>(std::clamp<int64_t>(config_get_int("nvn", "pool_ring_mb", 32), 4, 512));
    gfx::Buffer b;
    if (device().create_buffer(mb << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &b, "nvn pool landing ring") != VK_SUCCESS || !b.mapped) {
        if (b.buffer) device().destroy_buffer(b);
        NXLOG_W(kTag, "pool landing ring: no host-visible buffer of %llu MiB could be made: landings read the shadows", static_cast<unsigned long long>(mb));
        return false;
    }
    VkMemoryPropertyFlags mp = 0;
    vmaGetAllocationMemoryProperties(device().allocator(), b.allocation, &mp);
    NXLOG_I(kTag, "pool landing ring: %llu MiB, memory flags 0x%x (%s%s): the GPU copies each recorded write there at the end of its submission ([nvn] pool_ring)",
            static_cast<unsigned long long>(mb), static_cast<unsigned>(mp), (mp & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "host-cached" : "NOT host-cached",
            (mp & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? ", coherent" : ", non-coherent");
    std::lock_guard<std::mutex> lk(g_land_mu);
    g_ring.buf = b;
    g_ring.size = mb << 20;
    g_ring.head = g_ring.tail = 0;
    return true;
}

void drop_all_records() {
    std::lock_guard<std::mutex> lk(g_land_mu);
    for (const LandRec& r : g_land)
        if (r.chunk->land_refs) --r.chunk->land_refs;
    g_land.clear();
    land_min_update();
    g_ring.tail = g_ring.head;   // no copy is waited for any more
}

// One record goes from its shadow into guest memory. Returns the parts that could not be written.
//
// [nvn] pool_copy_land_keep: the bytes come from the shadow, so a page that was current before (armed by the write tracker, unwritten)
// is current after, and no bind copies it back. Such runs of pages are made writable through the tracker (ptrack_land_begin), which
// keeps them out of the written record, and armed again after the copy. The tracker sees no CPU write made in between, so afterwards
// the CPU's bytes are compared with the shadow's where a game may write while this runs: the bytes of the record's first and last page
// outside the record, which are no part of the GPU's write and may hold other data; and for a small record (an event word, a report,
// a counter the game polls in memory and may answer at once) all of its kept pages. A page that differs counts as written. Inside a
// large record the game does not write before the serial is published, which happens after this returns; a write made there while the
// GPU's write was in flight was lost before too (the limits at the top of nvn_pool_copy.cpp).
// Without it every page of the record is written as an operating-system write: it counts as written, the next bind copies it into the
// shadow again, and a page that happens to twice turns hot.
struct KeptRun { PoolGpu* pool; uint64_t first, count; };
// ring: the record's bytes in the readback ring (phones, [nvn] pool_ring), else null: the record lands from the shadow; the bytes of its
// pages outside the record are compared with the shadow either way.
uint64_t land_one(PoolGpu* pool, const PoolChunk* c, uint64_t off, uint64_t size, std::vector<KeptRun>& kept, const uint8_t* ring = nullptr) {
    uint8_t* const base = pool->base;
    const auto shadow = [c](uint64_t pool_off) { return static_cast<const uint8_t*>(c->shadow.mapped) + (pool_off - c->off); };
    const auto src = [&](uint64_t pool_off) { return ring && pool_off >= off && pool_off < off + size ? ring + (pool_off - off) : shadow(pool_off); };
    const uint64_t p0 = off / kPage, p1 = (off + size - 1) / kPage;
    const size_t k0 = kept.size();
    if (PoolTrack* trk = pcopy::land_keep() ? pool->track : nullptr)
        for (uint64_t p = p0; p <= p1;) {
            const uint64_t n = ptrack_land_begin(trk, p, p1 - p + 1);
            if (n) kept.push_back(KeptRun{pool, p, n});
            p += n ? n : 1;
        }
    mem::ww::before_os_write(base + off, size);     // pages a write watch made read-only become writable (and count as written)
    const uint64_t bad = pcopy::guarded_copy(base + off, src(off), size, false);
    const uint64_t lo = c->off, hi = c->off + c->size;     // what the shadow holds
    const auto same_part = [&](uint64_t a, uint64_t b) { return a >= b || pcopy::guarded_same(base + a, src(a), b - a); };
    const auto same = [&](uint64_t a, uint64_t b) {
        a = std::max(a, lo);
        b = std::min(b, hi);
        if (!ring || a >= b) return a >= b || pcopy::guarded_same(base + a, shadow(a), b - a);
        // the parts before, inside and after the record: the inside from the ring
        const uint64_t i0 = std::clamp(off, a, b), i1 = std::clamp(off + size, a, b);
        return same_part(a, i0) && same_part(i0, i1) && same_part(i1, b);
    };
    for (size_t k = k0; k < kept.size();) {
        KeptRun& r = kept[k];
        bool ok = ptrack_land_end(pool->track, r.first, r.count);
        if (ok && size <= kCompareAll && !same(r.first * kPage, (r.first + r.count) * kPage)) {
            ptrack_land_differs(pool->track, r.first, r.count);
            ok = false;
        }
        if (ok && size > kCompareAll) {      // more than one page (p0 < p1): the run loses an end page that differs
            if (r.first == p0 && !same(p0 * kPage, off)) {
                ptrack_land_differs(pool->track, p0, 1);
                ++r.first;
                --r.count;
            }
            if (r.count && r.first + r.count - 1 == p1 && !same(off + size, (p1 + 1) * kPage)) {
                ptrack_land_differs(pool->track, p1, 1);
                --r.count;
            }
            ok = r.count != 0;
        }
        if (ok) ++k;
        else kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(k));
    }
    return bad;
}

// [nvn] pool_land_slice_ms: the longest single wait for the serial of the oldest record. The wait ends the moment the timeline reaches
// the serial, so its length adds nothing to a landing; it only bounds how long the thread takes to see a stop or a lost device. The
// queue is in serial order and a record is only ever added behind the oldest, so the serial waited for is never too late a one. A wait
// that ran its whole length is followed by the next one at once: only a wait that came back early without the serial (an error) is
// followed by a 1 ms pause. 0: the earlier loop, 5 ms waits with a 1 ms pause after each one that did not reach the serial.
void land_loop() {
    log_set_thread_name("nvn-pool-land");
    const int64_t slice_ms = std::clamp<int64_t>(config_get_int("nvn", "pool_land_slice_ms", 50), 0, 1000);
    const bool pause_always = slice_ms == 0;
    const uint64_t slice_ns = static_cast<uint64_t>(pause_always ? 5 : slice_ms) * 1000 * 1000;
    if (!pause_always)
        NXLOG_I("nvn", "pool landing: waits for a serial up to %lld ms at a time, no pause after a wait that ran out ([nvn] pool_land_slice_ms)",
                static_cast<long long>(slice_ms));
    std::unique_lock<std::mutex> lk(g_th_mu);
    while (!g_th_stop) {
        const uint64_t s = g_land_min.load(std::memory_order_acquire);
        if (s == kNone) {
            g_th_cv.wait_for(lk, std::chrono::milliseconds(50));
            continue;
        }
        lk.unlock();
        // A word the game polls in memory with no API call (an event the GPU signals, a report) lands within a few ms of its serial.
        const auto t0 = std::chrono::steady_clock::now();
        if (timeline_wait(s, slice_ns)) {
            poll_completed();
        } else if (device().device_lost()) {   // nothing lands after a device loss: the records go
            drop_all_records();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } else if (pause_always || std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));   // not a timeout (an error returns at once): no busy loop
        }
        lk.lock();
    }
}

}  // namespace

namespace pcopy {

void land_start() {
    std::lock_guard<std::mutex> lk(g_th_mu);
    if (g_th.joinable()) return;
    g_th_stop = false;
    g_th.start("nvn-pool-land", 16, land_loop);   // the same stack the render thread gets (nvn_thread.h)
}

void land_stop() {
    {
        std::lock_guard<std::mutex> lk(g_th_mu);
        g_th_stop = true;
    }
    g_th_cv.notify_all();
    if (g_th.joinable()) g_th.join();
    {
        std::lock_guard<std::mutex> lk(g_th_mu);
        g_th_stop = false;            // a later back end (tests bring it up and down) starts its own
    }
    drop_all_records();               // the device is idle and the pools go next
}

bool land_record(PoolGpu* pg, PoolChunk* c, uint64_t off, uint64_t size, uint64_t serial) {
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(g_land_mu);
        // the same range bound again in this submission (a storage buffer of every draw): one record
        const size_t n = g_land.size();
        for (size_t i = n; i-- > 0 && i + 8 >= n;) {
            const LandRec& r = g_land[i];
            if (r.serial != serial) break;
            if (r.chunk == c && r.pool_off <= off && off + size <= r.pool_off + r.size) return false;
        }
        first = g_land.empty();
        g_land.push_back(LandRec{pg, c, off, size, serial});
        ++c->land_refs;
        if (first) land_min_update();
    }
    shared().records.fetch_add(1, std::memory_order_relaxed);
    if (first) {
        std::lock_guard<std::mutex> lk(g_th_mu);     // under the mutex the lander waits on: no wake-up lost between its test and its wait
        g_th_cv.notify_one();
    }
    return true;
}

// Records of a chunk that goes (its pool is destroyed) do not land; an idle drop or an eviction happens only with none left. A landing
// holds g_landing_mu from taking its records out of the queue until its last copy, so once this has had the mutex no lander reads the
// chunk's shadow: the caller may destroy it.
void land_forget(PoolChunk& c) {
    std::lock_guard<std::mutex> landing(g_landing_mu);
    std::lock_guard<std::mutex> lk(g_land_mu);
    g_land.erase(std::remove_if(g_land.begin(), g_land.end(), [&c](const LandRec& r) { return r.chunk == &c; }), g_land.end());
    land_min_update();
}

bool land_keep() {
    static const bool on = config_get_bool("nvn", "pool_copy_land_keep", true);
    return on;
}

LandingPause::LandingPause() { g_landing_mu.lock(); }
LandingPause::~LandingPause() { g_landing_mu.unlock(); }

size_t land_pending() {
    std::lock_guard<std::mutex> lk(g_land_mu);
    return g_land.size();
}

void land_busy(const PoolGpu* pg, uint64_t lo, uint64_t hi, std::vector<std::pair<uint64_t, uint64_t>>& out) {
    std::lock_guard<std::mutex> lk(g_land_mu);
    const auto add = [&](const LandRec& r) {
        if (r.pool == pg && r.pool_off < hi && r.pool_off + r.size > lo) out.emplace_back(std::max(r.pool_off, lo), std::min(r.pool_off + r.size, hi));
    };
    for (const LandRec& r : g_land) add(r);
    for (const LandRec& r : g_landing) add(r);
}

}  // namespace pcopy

bool pcopy_busy(const PoolChunk& c) {
    std::lock_guard<std::mutex> lk(g_land_mu);
    return c.land_refs != 0;
}

uint64_t pcopy_land(uint64_t reached) {
    if (t_landing || g_land_min.load(std::memory_order_acquire) > reached) return reached;
    t_landing = true;
    const uint64_t t0 = ft::on() ? ft::now_ns() : 0;
    uint64_t bytes = 0, bad = 0;
    {
        std::lock_guard<std::mutex> landing(g_landing_mu);
        // The records leave the queue now, but g_land_min and land_refs stay as they were until the copy is done: a second caller that tests
        // g_land_min still waits on g_landing_mu, so no serial is published before its bytes landed, and pcopy_busy holds the chunks.
        // g_landing shows the batch to land_busy from the moment it leaves the queue until its bytes are in guest memory (#531).
        std::vector<LandRec>& batch = g_landing;
        std::vector<KeptRun> kept;
        {
            std::lock_guard<std::mutex> lk(g_land_mu);
            while (!g_land.empty() && g_land.front().serial <= reached) {
                batch.push_back(g_land.front());
                g_land.pop_front();
            }
        }
        // pass 1: every copy. Two records of a serial can share a page: its page_gpu must stay set until both have landed, or a touch between
        // them sees 0 and copies guest -> shadow over the page while the second record still lands stale bytes.
        uint64_t ring_free = 0;   // phones: the ring is free up to here once these landed
        for (const LandRec& r : batch) {
            const uint8_t* ring = nullptr;
            if (r.ring_off != kNone && g_ring.buf) {
                vmaInvalidateAllocation(device().allocator(), g_ring.buf.allocation, r.ring_off, r.size);   // the GPU wrote it (maybe non-coherent)
                ring = static_cast<const uint8_t*>(g_ring.buf.mapped) + r.ring_off;
                ring_free = std::max(ring_free, r.ring_end);
            }
            bad += land_one(r.pool, r.chunk, r.pool_off, r.size, kept, ring);
            bytes += r.size;
        }
        if (ring_free) {
            std::lock_guard<std::mutex> lk(g_land_mu);
            g_ring.tail = std::max(g_ring.tail, ring_free);
        }
        // pass 2: page_gpu back to 0 where no later write was recorded: after every copy, so a touch that sees 0 copies the landed bytes
        for (const LandRec& r : batch)
            for (uint64_t p = r.pool_off / kPage; p <= (r.pool_off + r.size - 1) / kPage; ++p) {
                r.pool->page_landed[static_cast<size_t>(p)].store(1, std::memory_order_relaxed);
                uint64_t e = r.serial;
                r.pool->page_gpu[static_cast<size_t>(p)].compare_exchange_strong(e, 0, std::memory_order_release, std::memory_order_relaxed);
            }
        for (const KeptRun& r : kept)      // no copy follows these
            for (uint64_t p = r.first; p < r.first + r.count; ++p) r.pool->page_landed[static_cast<size_t>(p)].store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(g_land_mu);
            for (const LandRec& r : batch)
                if (r.chunk->land_refs) --r.chunk->land_refs;
            batch.clear();
            land_min_update();
        }
    }
    t_landing = false;
    if (bad) pcopy::shared().unreadable.fetch_add(bad, std::memory_order_relaxed);
    if (bytes) {
        pcopy::shared().landed.fetch_add(bytes, std::memory_order_relaxed);
        if (t0) {
            static const uint16_t z = ft::zone_id("pool.copy.land");
            ft::event(z, t0, ft::now_ns(), static_cast<uint32_t>(std::min<uint64_t>(bytes >> 10, 0xffffffffu)));
        }
    }
    return reached;
}

// Phones, [nvn] pool_ring: at the end of the recording of submission `serial` (render thread, no pass open), the GPU copies each range the
// submission recorded as written into the readback ring. Every write of the submission comes before the copies (one barrier), and the copies
// before the host's reads (one barrier); the serial signals after both, so a landing that sees it finds the bytes in the ring.
void pcopy_ring_flush(VkCommandBuffer cb, uint64_t serial) {
    if (!serial || !ring_wanted() || !ring_ready()) return;
    struct Copy { VkBuffer src; VkBufferCopy region; };
    std::vector<Copy> copies;
    {
        std::lock_guard<std::mutex> lk(g_land_mu);
        for (LandRec& r : g_land) {
            if (r.serial != serial || r.ring_off != kNone || !r.chunk->shadow.buffer) continue;
            const uint64_t n = (r.size + kRingAlign - 1) & ~(kRingAlign - 1);
            if (n > g_ring.size) continue;
            uint64_t at = g_ring.head;
            if (at % g_ring.size + n > g_ring.size) at += g_ring.size - at % g_ring.size;   // no copy wraps: start at the ring's beginning
            if (at + n - g_ring.tail > g_ring.size) continue;                              // full: this one lands from the shadow
            r.ring_off = at % g_ring.size;
            g_ring.head = at + n;
            r.ring_end = g_ring.head;
            copies.push_back({r.chunk->shadow.buffer, VkBufferCopy{r.pool_off - r.chunk->off, r.ring_off, r.size}});
        }
    }
    if (copies.empty()) return;
    // Only what can write a shadow buffer is waited for: shader stores (any shader stage), copies / fills / query results (transfer) and
    // transform feedback; the copies block nothing but later copies, and the host read waits for them alone. No graphics or compute work of
    // a later submission waits on these barriers.
    const bool xfb = device().features().transform_feedback;
    VkMemoryBarrier2 mb[2] = {{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2}, {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2}};
    mb[0].srcStageMask = VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | (xfb ? VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT : 0);
    mb[0].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | (xfb ? VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT : 0);
    mb[0].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;   // the copies read the shadows and write the ring (WAW with a copy before)
    mb[1].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb[1].dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb[1].dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb[0];
    vkCmdPipelineBarrier2(cb, &di);
    for (const Copy& k : copies) vkCmdCopyBuffer(cb, k.src, g_ring.buf.buffer, 1, &k.region);
    di.pMemoryBarriers = &mb[1];
    vkCmdPipelineBarrier2(cb, &di);
    static std::atomic<uint64_t> n_copies{0}, n_bytes{0};
    uint64_t b = 0;
    for (const Copy& k : copies) b += k.region.size;
    const uint64_t nc = n_copies.fetch_add(copies.size()) + copies.size();
    const uint64_t nb = n_bytes.fetch_add(b) + b;
    if ((nc - copies.size()) / 20000 != nc / 20000)
        NXLOG_I(kTag, "pool landing ring: %llu GPU copies so far, %llu MiB", static_cast<unsigned long long>(nc), static_cast<unsigned long long>(nb >> 20));
}

}  // namespace nxrt::nvn::be::gpu
