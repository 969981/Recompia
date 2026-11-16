// SPDX-License-Identifier: GPL-3.0-or-later
// Sparse copies of memory pools: one sparse device-local buffer per pool, bound to memory in 2 MiB granules the first time a draw, fill or copy
// wants them, and unbound when idle. Idea from pokopia-native (MIT), windows/src/nxrt/vk/vk_mem.c. The Mirror and the rest of the copies are
// nvn_gpu_pool.cpp; what the two share is nvn_gpu_pool_internal.h.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/write_watch.h"
#include "nvn_gpu_pool_internal.h"

namespace nxrt::nvn::be::gpu {
namespace pool_detail {
namespace {

constexpr const char* kTag = "nvn.pool";

// Sparse copies (the Mirror comment in nvn_gpu_pool.cpp). Render thread.
constexpr uint64_t kGranPages = kMirrorGranule / kPageBytes;
constexpr uint64_t kSparseBlock = 32ull << 20;       // memory is allocated in blocks of this many bytes, cut into granules
constexpr uint64_t kSparseReleaseMin = 16ull << 20;  // a release frees at least this much (fewer, larger batches)
struct SparseOp { Mirror* m; uint32_t g; };
struct SparseState {
    uint32_t mem_type = UINT32_MAX;     // the device-local memory type of the granules (chosen with the first buffer)
    std::vector<VkDeviceMemory> blocks;
    uint64_t block_bytes = 0;           // memory of the blocks (never more than a block over the cap)
    std::vector<SparsePiece> free;      // granule-sized pieces of the blocks bound to nothing
    std::vector<SparseOp> wanted;       // granules asked for (kGranWanted), bound by the next batch
    std::vector<SparseOp> to_unbind;    // granules released (kGranUnbinding), unbound by the next batch
    // the batch in flight (at most one): its binds, its unbinds and their memory (free once it has run: never bound twice meanwhile)
    VkFence fence = VK_NULL_HANDLE;
    bool in_flight = false;
    std::vector<SparseOp> binding, unbinding;
    std::vector<SparsePiece> unbound_mem;
    bool starved = false;               // the last batch had granules no memory was found for (cap used up): binds count as "no chunk"
    uint32_t release_tried_at = UINT32_MAX;
    uint32_t alloc_failed_at = 0;       // present count of a failed block allocation (none tried for kChunkIdle presents after)
    bool alloc_failed = false;
    uint64_t space = 0;                 // sparse address space of the live buffers (VkPhysicalDeviceLimits::sparseAddressSpaceSize)
    int live = 0;                       // sparse mirrors alive
};
SparseState g_sp;
std::atomic<uint64_t> g_sp_bind_ns{0}, g_sp_bind_max_ns{0}, g_sp_bound_n{0}, g_sp_unbound_n{0}, g_sp_batches{0};

}  // namespace

// ---- sparse copies (the Mirror comment) ----------------------------------------------------------------------------------------------
uint32_t gran_of(uint64_t page) { return static_cast<uint32_t>(page / kGranPages); }

// The sparse buffer of m's pool, made unbound. False (m keeps chunks): the device refuses it or the granules cannot use one memory type.
bool sparse_create(Mirror& m) {
    const VkDeviceSize size = (m.pg->size + kMirrorGranule - 1) / kMirrorGranule * kMirrorGranule;
    const char* why = nullptr;
    const VkDeviceSize space_max = device().properties().limits.sparseAddressSpaceSize;
    const uint64_t max_buffer = device().limits_ext().max_buffer_size;
    VkBuffer buf = VK_NULL_HANDLE;
    VkMemoryRequirements mr{};
    if (g_sp.space + size > space_max) why = "sparse address space used up";
    else if (max_buffer && size > max_buffer) why = "larger than the largest buffer";
    else {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
        bi.size = size;
        bi.usage = kMirrorUsage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(vkdev(), &bi, nullptr, &buf) != VK_SUCCESS) { buf = VK_NULL_HANDLE; why = "vkCreateBuffer failed"; }
    }
    if (buf) {
        vkGetBufferMemoryRequirements(vkdev(), buf, &mr);
        if (!mr.alignment || kMirrorGranule % mr.alignment) why = "sparse block size does not divide 2 MiB";
    }
    if (buf && !why && g_sp.mem_type == UINT32_MAX) {   // device-local, preferably not host-visible (the resizable-BAR window is small)
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(device().physical(), &mp);
        for (int pass = 0; pass < 2 && g_sp.mem_type == UINT32_MAX; ++pass)
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
                const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
                if ((mr.memoryTypeBits & (1u << i)) && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (pass || !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) {
                    g_sp.mem_type = i;
                    break;
                }
            }
        if (g_sp.mem_type == UINT32_MAX) why = "no device-local memory type";
    }
    if (buf && !why && !(mr.memoryTypeBits & (1u << g_sp.mem_type))) why = "another memory type than the first sparse copy";
    if (buf && !why && !g_sp.fence) {
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(vkdev(), &fi, nullptr, &g_sp.fence) != VK_SUCCESS) { g_sp.fence = VK_NULL_HANDLE; why = "vkCreateFence failed"; }
    }
    if (why) {
        if (buf) vkDestroyBuffer(vkdev(), buf, nullptr);
        NXLOG_W(kTag, "pool %p (%llu KiB): no sparse copy (%s), chunks of %llu MiB instead", static_cast<void*>(m.pg->base),
                static_cast<unsigned long long>(m.pg->size >> 10), why, static_cast<unsigned long long>(chunk_bytes() >> 20));
        return false;
    }
    const size_t n = static_cast<size_t>(size / kMirrorGranule);
    m.sparse = true;
    m.sbuf = buf;
    m.sbuf_size = size;
    m.gstate.assign(n, kGranUnbound);
    m.gmem.assign(n, SparsePiece{});
    m.chunk_used_at.assign(n, 0);
    m.chunk_used_serial.assign(n, 0);
    g_sp.space += size;
    ++g_sp.live;
    return true;
}

// Marks the granules of pages [first, last] used by the submission being recorded. True when all of them are bound; the unbound ones are
// asked for (the next sparse_submit binds them; commands use them once that bind has run).
bool sparse_ready(Mirror& m, uint64_t first, uint64_t last) {
    const uint32_t now = presents_now();
    const uint64_t cur = current_serial();
    bool ok = true;
    for (uint32_t g = gran_of(first), ge = gran_of(last); g <= ge; ++g) {
        m.chunk_used_at[g] = now;
        m.chunk_used_serial[g] = cur;
        if (m.gstate[g] == kGranBound) continue;
        ok = false;
        if (m.gstate[g] == kGranUnbound) {
            m.gstate[g] = kGranWanted;
            g_sp.wanted.push_back({&m, g});
        }
    }
    return ok;
}

bool sparse_bound(const Mirror& m, uint64_t first, uint64_t last) {
    for (uint32_t g = gran_of(first), ge = gran_of(last); g <= ge; ++g)
        if (m.gstate[g] != kGranBound) return false;
    return true;
}

// The end (exclusive page) of a copy run from page p (its granule bound): within one import of the pool (the copy reads one source buffer,
// mirror_src) and the bound granules that follow p's.
uint64_t sparse_run_end(const Mirror& m, uint64_t p) {
    const uint64_t spages = std::max<uint64_t>(1, m.pg->stride / kPageBytes);
    const uint64_t end = std::min(m.npages, (p / spages + 1) * spages);
    uint64_t g = gran_of(p);
    while ((g + 1) * kGranPages < end && m.gstate[static_cast<size_t>(g + 1)] == kGranBound) ++g;
    return std::min(end, (g + 1) * kGranPages);
}

// The batch in flight has run (wait: wait for it): its granules are bound, the ones it unbound are free, and so is their memory.
void sparse_poll(bool wait) {
    if (!g_sp.in_flight) return;
    const VkResult r = wait ? vkWaitForFences(vkdev(), 1, &g_sp.fence, VK_TRUE, UINT64_MAX) : vkGetFenceStatus(vkdev(), g_sp.fence);
    if (r == VK_NOT_READY || r == VK_TIMEOUT) return;
    if (r != VK_SUCCESS) NXLOG_E(kTag, "sparse copies: waiting for vkQueueBindSparse failed (%d)", static_cast<int>(r));
    vkResetFences(vkdev(), 1, &g_sp.fence);
    g_sp.in_flight = false;
    for (const SparseOp& o : g_sp.binding)
        if (o.m->gstate[o.g] == kGranBinding) o.m->gstate[o.g] = kGranBound;
    for (const SparseOp& o : g_sp.unbinding)
        if (o.m->gstate[o.g] == kGranUnbinding) o.m->gstate[o.g] = kGranUnbound;
    g_sp.free.insert(g_sp.free.end(), g_sp.unbound_mem.begin(), g_sp.unbound_mem.end());
    g_sp.binding.clear();
    g_sp.unbinding.clear();
    g_sp.unbound_mem.clear();
}

// A granule of memory: a free piece, or a new block while the blocks are under the cap. Never one allocation per granule.
static bool sparse_piece(SparsePiece* out) {
    if (g_sp.free.empty()) {
        const uint64_t cap = mirror_cap_bytes();
        const uint32_t now = presents_now();
        if (g_sp.block_bytes >= cap || (g_sp.alloc_failed && now - g_sp.alloc_failed_at <= kChunkIdle)) return false;
        const uint64_t bytes = std::min<uint64_t>(kSparseBlock, (cap - g_sp.block_bytes + kMirrorGranule - 1) / kMirrorGranule * kMirrorGranule);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = bytes;
        ai.memoryTypeIndex = g_sp.mem_type;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkAllocateMemory(vkdev(), &ai, nullptr, &mem) != VK_SUCCESS) {
            if (!g_sp.alloc_failed) NXLOG_W(kTag, "sparse copies: %llu MiB of video memory could not be allocated: those pages stay in the pool for now",
                                            static_cast<unsigned long long>(bytes >> 20));
            g_sp.alloc_failed = true;
            g_sp.alloc_failed_at = now;
            return false;
        }
        g_sp.alloc_failed = false;
        g_sp.blocks.push_back(mem);
        g_sp.block_bytes += bytes;
        for (uint64_t o = bytes; o >= kMirrorGranule; o -= kMirrorGranule) g_sp.free.push_back({mem, o - kMirrorGranule});   // offset 0 taken first
    }
    *out = g_sp.free.back();
    g_sp.free.pop_back();
    return true;
}

// The cap is used up and `need` more bytes are wanted: releases idle granules (chunk_idle) of every sparse copy, the least recently used
// first, at least kSparseReleaseMin. Their pages stop being valid at once; the next sparse_submit unbinds them, and their memory is free once
// that batch has run. At most one search per present.
static void sparse_release(uint64_t need) {
    const uint32_t now = presents_now();
    if (g_sp.release_tried_at == now) return;
    g_sp.release_tried_at = now;
    const uint64_t done = poll_completed(), cur = current_serial();
    std::vector<std::pair<uint32_t, SparseOp>> idle;       // (presents since its last use, granule)
    for (Mirror* m : g_mirrors) {
        if (!m->sparse) continue;
        for (size_t g = 0; g < m->gstate.size(); ++g)
            if (chunk_idle(*m, g, now, done, cur)) idle.push_back({now - m->chunk_used_at[g], {m, static_cast<uint32_t>(g)}});
    }
    std::sort(idle.begin(), idle.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    uint64_t freed = 0;
    const uint64_t want = std::max(need, kSparseReleaseMin);
    for (size_t i = 0; i < idle.size() && freed < want; ++i) {
        Mirror& m = *idle[i].second.m;
        const uint32_t g = idle[i].second.g;
        const uint64_t first = g * kGranPages, n = std::min(m.npages, first + kGranPages) - first;
        mem::ww::invalidate(m.ww, first, n);
        for (uint64_t p = first; p < first + n; ++p) {
            bit_put(m.want, p, false);
            bit_put(m.used, p, false);
            m.copied_at[static_cast<size_t>(p)] = 0;
            m.score[static_cast<size_t>(p)] = 0;
        }
        m.gstate[g] = kGranUnbinding;
        g_sp.to_unbind.push_back(idle[i].second);
        g_mirror_bytes -= std::min<uint64_t>(g_mirror_bytes, kMirrorGranule);
        g_released_bytes += kMirrorGranule;
        ++g_released_n;
        freed += kMirrorGranule;
    }
    g_st_live_bytes.store(static_cast<int64_t>(g_mirror_bytes), std::memory_order_relaxed);
}

// At the start of a submission, before anything of it is submitted: when no batch is in flight, one vkQueueBindSparse binds the granules
// asked for since the last batch and unbinds the released ones. It is not waited for: its granules are used from the first submission
// that begins after it has run (sparse_poll), so no submission depends on it and none waits. Unbinds name only granules whose last
// submission has completed (chunk_idle); memory they free is bound again only by a later batch.
void sparse_submit() {
    sparse_poll(false);
    if (g_sp.in_flight || (g_sp.wanted.empty() && g_sp.to_unbind.empty())) return;
    const uint64_t cap = mirror_cap_bytes();
    std::vector<std::pair<Mirror*, VkSparseMemoryBind>> ops;
    bool starved = false, released = false;
    size_t left = g_sp.wanted.size();
    for (const SparseOp& o : g_sp.wanted) {
        --left;
        if (o.m->gstate[o.g] != kGranWanted) continue;
        if (g_mirror_bytes + kMirrorGranule > cap && !released) {
            released = true;
            sparse_release(g_mirror_bytes + (left + 1) * kMirrorGranule - cap);
        }
        SparsePiece pc;
        if (g_mirror_bytes + kMirrorGranule > cap || !sparse_piece(&pc)) {
            o.m->gstate[o.g] = kGranUnbound;      // asked for again by the next bind that wants it
            starved = true;
            continue;
        }
        o.m->gstate[o.g] = kGranBinding;
        o.m->gmem[o.g] = pc;
        g_sp.binding.push_back(o);
        ops.push_back({o.m, VkSparseMemoryBind{o.g * kMirrorGranule, kMirrorGranule, pc.mem, pc.off, 0}});
        g_mirror_bytes += kMirrorGranule;
        g_st_chunk_bytes_made.fetch_add(kMirrorGranule, std::memory_order_relaxed);
    }
    g_sp.wanted.clear();
    if (starved && !g_sp.starved) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            NXLOG_I(kTag, "pool copies: the budget is used up (%llu MiB): granules no draw used for %u presents are released for new ones, until then "
                    "further granules stay in the pools", static_cast<unsigned long long>(g_mirror_bytes >> 20), kChunkIdle);
        }
    }
    g_sp.starved = starved;
    for (const SparseOp& o : g_sp.to_unbind) {
        ops.push_back({o.m, VkSparseMemoryBind{o.g * kMirrorGranule, kMirrorGranule, VK_NULL_HANDLE, 0, 0}});
        g_sp.unbound_mem.push_back(o.m->gmem[o.g]);
        o.m->gmem[o.g] = SparsePiece{};
        g_sp.unbinding.push_back(o);
    }
    g_sp.to_unbind.clear();
    g_st_live_bytes.store(static_cast<int64_t>(g_mirror_bytes), std::memory_order_relaxed);
    if (ops.empty()) return;
    // one VkSparseBufferMemoryBindInfo per buffer
    std::stable_sort(ops.begin(), ops.end(), [](const auto& a, const auto& b) { return std::less<Mirror*>()(a.first, b.first); });
    std::vector<VkSparseMemoryBind> binds(ops.size());
    std::vector<VkSparseBufferMemoryBindInfo> infos;
    for (size_t i = 0; i < ops.size(); ++i) {
        binds[i] = ops[i].second;
        if (!i || ops[i].first != ops[i - 1].first) infos.push_back({ops[i].first->sbuf, 0, binds.data() + i});
        ++infos.back().bindCount;
    }
    VkBindSparseInfo bi = {VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    bi.bufferBindCount = static_cast<uint32_t>(infos.size());
    bi.pBufferBinds = infos.data();
    const auto t0 = std::chrono::steady_clock::now();
    VkResult r;
    {
        std::lock_guard<std::mutex> lk(device().queue_mutex());
        r = vkQueueBindSparse(device().queue(), 1, &bi, g_sp.fence);
    }
    const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    g_sp_bind_ns.fetch_add(ns, std::memory_order_relaxed);
    if (ns > g_sp_bind_max_ns.load(std::memory_order_relaxed)) g_sp_bind_max_ns.store(ns, std::memory_order_relaxed);
    g_sp_batches.fetch_add(1, std::memory_order_relaxed);
    if (r == VK_SUCCESS) {
        g_sp.in_flight = true;
        g_sp_bound_n.fetch_add(g_sp.binding.size(), std::memory_order_relaxed);
        g_sp_unbound_n.fetch_add(g_sp.unbinding.size(), std::memory_order_relaxed);
        return;
    }
    // nothing of the batch happened: its granules stay unbound (asked for again later); the memory of the ones it should have unbound stays
    // bound where it was, so it is not used again (lost until the copies go away)
    static int logged = 0;
    if (logged++ < 4) NXLOG_E(kTag, "sparse copies: vkQueueBindSparse failed (%d): %zu granules stay in the pool", static_cast<int>(r), g_sp.binding.size());
    for (const SparseOp& o : g_sp.binding) {
        o.m->gstate[o.g] = kGranUnbound;
        g_sp.free.push_back(o.m->gmem[o.g]);
        o.m->gmem[o.g] = SparsePiece{};
        g_mirror_bytes -= std::min<uint64_t>(g_mirror_bytes, kMirrorGranule);
    }
    for (const SparseOp& o : g_sp.unbinding) o.m->gstate[o.g] = kGranUnbound;
    g_sp.binding.clear();
    g_sp.unbinding.clear();
    g_sp.unbound_mem.clear();
    g_st_live_bytes.store(static_cast<int64_t>(g_mirror_bytes), std::memory_order_relaxed);
}

// m goes away (its pool is destroyed, no submission uses it any more): the batch in flight is waited for (it may name m's buffer), m's
// granules give their memory back, and with the last sparse copy the blocks and the fence go too.
void sparse_destroy(Mirror& m) {
    sparse_poll(true);
    const auto mine = [&m](const SparseOp& o) { return o.m == &m; };
    g_sp.wanted.erase(std::remove_if(g_sp.wanted.begin(), g_sp.wanted.end(), mine), g_sp.wanted.end());
    g_sp.to_unbind.erase(std::remove_if(g_sp.to_unbind.begin(), g_sp.to_unbind.end(), mine), g_sp.to_unbind.end());
    for (size_t g = 0; g < m.gstate.size(); ++g) {
        if (m.gmem[g].mem) g_sp.free.push_back(m.gmem[g]);   // bound, or released and not yet unbound (counted off already)
        if (m.gstate[g] == kGranBound) g_mirror_bytes -= std::min<uint64_t>(g_mirror_bytes, kMirrorGranule);
    }
    forget_buffer_views(m.sbuf);     // compute texel buffers bound to the copy
    vkDestroyBuffer(vkdev(), m.sbuf, nullptr);
    m.sbuf = VK_NULL_HANDLE;
    g_sp.space -= std::min(g_sp.space, static_cast<uint64_t>(m.sbuf_size));
    if (--g_sp.live > 0) return;
    for (VkDeviceMemory mem : g_sp.blocks) vkFreeMemory(vkdev(), mem, nullptr);
    g_sp.blocks.clear();
    g_sp.free.clear();
    g_sp.block_bytes = 0;
    if (g_sp.fence) vkDestroyFence(vkdev(), g_sp.fence, nullptr);
    g_sp.fence = VK_NULL_HANDLE;
}

int sparse_live() { return g_sp.live; }
bool sparse_starved() { return g_sp.starved; }
uint64_t sparse_bind_us() { return g_sp_bind_ns.load(std::memory_order_relaxed) / 1000; }

// The sparse part of the pool summary line: granules bound now, and the binds of this period (since the last summary).
void sparse_log(char* sp, size_t size) {
    static uint64_t p_bound = 0, p_unbound = 0, p_batches = 0, p_ns = 0;
    uint64_t bound = 0;
    for (const Mirror* m : g_mirrors)
        for (uint8_t s : m->gstate) bound += s == kGranBound ? 1 : 0;
    const uint64_t b = g_sp_bound_n.load(), u = g_sp_unbound_n.load(), n = g_sp_batches.load(), ns = g_sp_bind_ns.load();
    snprintf(sp, size, "; sparse: %llu granules of %llu MiB bound (%llu MiB), this period %llu bound and %llu unbound in %llu vkQueueBindSparse, %.2f ms in them "
             "(longest %.2f ms)", (unsigned long long)bound, (unsigned long long)(kMirrorGranule >> 20), (unsigned long long)(bound * kMirrorGranule >> 20),
             (unsigned long long)(b - p_bound), (unsigned long long)(u - p_unbound), (unsigned long long)(n - p_batches), (ns - p_ns) / 1e6,
             g_sp_bind_max_ns.exchange(0) / 1e6);
    p_bound = b; p_unbound = u; p_batches = n; p_ns = ns;
}

}  // namespace pool_detail
}  // namespace nxrt::nvn::be::gpu
