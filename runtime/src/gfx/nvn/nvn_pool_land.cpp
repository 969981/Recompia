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
#include "nvn_gpu_internal.h"
#include "nvn_pool_copy_impl.h"

namespace nxrt::nvn::be::gpu {
namespace {

using pcopy::kPage;
constexpr uint64_t kNone = ~0ull;         // g_land_min with no record

// GPU writes waiting for their serial, in serial order (recorded by the render thread as it records). Lock order:
// g_live_mu -> g_reg_mu -> g_landing_mu -> g_land_mu. g_land_mu guards only the queue, land_refs and g_land_min and is held for short sections
// (the render thread takes it per recorded write and in pcopy_busy); it is a leaf. g_landing_mu serialises landers and is held across a whole
// landing (pop, memcpy, before_os_write, page bits); before_os_write could call the compute-out guard handler, which copy pools never arm.
struct LandRec {
    PoolGpu* pool;
    PoolChunk* chunk;
    uint64_t pool_off, size, serial;
};
std::mutex g_land_mu;
std::mutex g_landing_mu;
std::deque<LandRec> g_land;
std::atomic<uint64_t> g_land_min{kNone};        // serial of g_land.front() (kNone: empty), the lock-free test of pcopy_land
thread_local bool t_landing = false;            // a landing on this thread (a nested read_completed returns at once)

// the landing thread
std::mutex g_th_mu;
std::condition_variable g_th_cv;
std::thread g_th;
bool g_th_stop = false;

void land_min_update() { g_land_min.store(g_land.empty() ? kNone : g_land.front().serial, std::memory_order_release); }

void drop_all_records() {
    std::lock_guard<std::mutex> lk(g_land_mu);
    for (const LandRec& r : g_land)
        if (r.chunk->land_refs) --r.chunk->land_refs;
    g_land.clear();
    land_min_update();
}

void land_loop() {
    log_set_thread_name("nvn-pool-land");
    std::unique_lock<std::mutex> lk(g_th_mu);
    while (!g_th_stop) {
        const uint64_t s = g_land_min.load(std::memory_order_acquire);
        if (s == kNone) {
            g_th_cv.wait_for(lk, std::chrono::milliseconds(50));
            continue;
        }
        lk.unlock();
        // A word the game polls in memory with no API call (an event the GPU signals, a report) lands within a few ms of its serial.
        if (timeline_wait(s, 5ull * 1000 * 1000)) {
            poll_completed();
        } else if (device().device_lost()) {   // nothing lands after a device loss: the records go
            drop_all_records();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));   // (a timeout took 5 ms; an error returns at once)
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
    g_th = std::thread(land_loop);
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

size_t land_pending() {
    std::lock_guard<std::mutex> lk(g_land_mu);
    return g_land.size();
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
        std::vector<LandRec> batch;
        {
            std::lock_guard<std::mutex> lk(g_land_mu);
            while (!g_land.empty() && g_land.front().serial <= reached) {
                batch.push_back(g_land.front());
                g_land.pop_front();
            }
        }
        // pass 1: every copy. Two records of a serial can share a page: its page_gpu must stay set until both have landed, or a touch between
        // them sees 0 and copies guest -> shadow over the page while the second record still lands stale bytes.
        for (const LandRec& r : batch) {
            const PoolChunk* c = r.chunk;
            uint8_t* dst = r.pool->base + r.pool_off;
            const uint8_t* src = static_cast<const uint8_t*>(c->shadow.mapped) + (r.pool_off - c->off);
            mem::ww::before_os_write(dst, r.size);     // pages a write watch made read-only become writable (and count as written)
            bad += pcopy::guarded_copy(dst, src, r.size, false);
            bytes += r.size;
        }
        // pass 2: page_gpu back to 0 where no later write was recorded: after every copy, so a touch that sees 0 copies the landed bytes
        for (const LandRec& r : batch)
            for (uint64_t p = r.pool_off / kPage; p <= (r.pool_off + r.size - 1) / kPage; ++p) {
                r.pool->page_landed[static_cast<size_t>(p)].store(1, std::memory_order_relaxed);
                uint64_t e = r.serial;
                r.pool->page_gpu[static_cast<size_t>(p)].compare_exchange_strong(e, 0, std::memory_order_release, std::memory_order_relaxed);
            }
        {
            std::lock_guard<std::mutex> lk(g_land_mu);
            for (const LandRec& r : batch)
                if (r.chunk->land_refs) --r.chunk->land_refs;
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

}  // namespace nxrt::nvn::be::gpu
