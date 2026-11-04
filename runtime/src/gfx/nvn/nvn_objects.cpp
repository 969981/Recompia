// SPDX-License-Identifier: GPL-3.0-or-later
// Guest/host object binding and the memory pool registry (GPU address -> pool).
#include <algorithm>
#include <atomic>
#include <map>
#include <shared_mutex>

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nvn_core.h"

namespace nxrt::nvn::be {

void bind_guest(void* guest, Obj* obj) {
    GuestHeader* h = static_cast<GuestHeader*>(guest);
    Obj* old = (h->magic == kGuestMagic) ? h->host : nullptr;
    h->host = obj;
    h->kind = static_cast<uint32_t>(obj->kind);
    h->magic = kGuestMagic;
    if (old) old->unref();
}

void unbind_guest(void* guest) {
    GuestHeader* h = static_cast<GuestHeader*>(guest);
    Obj* old = (h->magic == kGuestMagic) ? h->host : nullptr;
    h->host = nullptr;
    h->magic = 0;
    h->kind = 0;
    if (old) old->unref();
}

namespace {
std::shared_mutex g_pool_mu;
std::map<uint64_t, MemoryPool*> g_pools;   // gpu_base -> pool
// Registry generation: bumped under the unique lock by every change of g_pools (pool_register, pool_unregister; a pool's gpu_base and
// size are set before it is registered and never change while it is, and nvnMemoryPoolMapVirtual remaps tiles inside a pool's range
// without touching it). The last-hit cache of pool_lookup is valid only for the generation it was filled under.
std::atomic<uint64_t> g_pool_gen{1};

// [nvn] pool_cache (default on): pool_lookup first checks the last two pools this thread found (a draw resolves 5-15 addresses, mostly
// in one or two pools) and takes the shared lock and walks the map only on a miss. thread_local: the render thread, the game threads
// and the texture preparation workers all look addresses up.
struct PoolCacheEntry { uint64_t gen = 0, lo = 0, hi = 0; MemoryPool* pool = nullptr; };
thread_local PoolCacheEntry t_pool_cache[2];
bool pool_cache_on() {
    static const bool on = [] {
        const bool v = nxrt::config_get_bool("nvn", "pool_cache", true);
        NXLOG_I("nvn", "pool_lookup last-hit cache %s ([nvn] pool_cache)", v ? "on" : "off");
        return v;
    }();
    return on;
}

// Gauge (nxrt/gauge.h). The callback takes g_pool_mu; it is removed before the map is destroyed.
struct PoolGauges {
    PoolGauges() {
        nxrt::gauge_register("nvn.pool_registry", [] {
            std::shared_lock<std::shared_mutex> lk(g_pool_mu);
            return static_cast<int64_t>(g_pools.size());
        });
    }
    ~PoolGauges() { nxrt::gauge_unregister("nvn.pool_registry"); }
};
PoolGauges g_pool_gauges;
}  // namespace

void pool_register(MemoryPool* p) {
    std::unique_lock<std::shared_mutex> lk(g_pool_mu);
    g_pools[p->gpu_base] = p;
    g_pool_gen.fetch_add(1, std::memory_order_release);
}

void pool_unregister(MemoryPool* p) {
    std::unique_lock<std::shared_mutex> lk(g_pool_mu);
    auto it = g_pools.find(p->gpu_base);
    if (it != g_pools.end() && it->second == p) g_pools.erase(it);
    g_pool_gen.fetch_add(1, std::memory_order_release);
}

uint64_t pool_registry_gen() { return g_pool_gen.load(std::memory_order_acquire); }

PoolHit pool_lookup(uint64_t gpu_address) {
    static const bool cache = pool_cache_on();
    // The generation is read before the cache and before the map: a change made after this load bumps it again, so an entry filled from a
    // map that already had the change is stored under the old generation and missed next time (never the reverse).
    const uint64_t gen = cache ? g_pool_gen.load(std::memory_order_acquire) : 0;
    if (cache) {
        for (int i = 0; i < 2; ++i) {
            const PoolCacheEntry& e = t_pool_cache[i];
            if (e.gen == gen && gpu_address >= e.lo && gpu_address < e.hi) {
                ft::split_count(ft::kCntPoolHit);
                if (i) std::swap(t_pool_cache[0], t_pool_cache[1]);
                return {t_pool_cache[0].pool, gpu_address - t_pool_cache[0].lo};
            }
        }
        ft::split_count(ft::kCntPoolMiss);
    }
    std::shared_lock<std::shared_mutex> lk(g_pool_mu);
    auto it = g_pools.upper_bound(gpu_address);
    if (it == g_pools.begin()) return {};
    --it;
    MemoryPool* p = it->second;
    if (gpu_address >= p->gpu_base + p->size) return {};
    if (cache) {
        t_pool_cache[1] = t_pool_cache[0];
        t_pool_cache[0] = {gen, p->gpu_base, p->gpu_base + p->size, p};
    }
    return {p, gpu_address - p->gpu_base};
}

}  // namespace nxrt::nvn::be
