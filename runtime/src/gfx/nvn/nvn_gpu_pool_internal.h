// SPDX-License-Identifier: GPL-3.0-or-later
// What nvn_gpu_pool.cpp (the pool imports and the device-local copies of their pages) and nvn_gpu_pool_sparse.cpp (the sparse copies:
// granules bound on demand) share: the Mirror, the constants and helpers the sparse code reads, and its entry points.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "nxrt/write_watch.h"
#include "nvn_gpu_internal.h"
#include "nvn_mirror_cap.h"

namespace nxrt::nvn::be::gpu {

// The device-local copy of a pool's pages; the design is described in nvn_gpu_pool.cpp (device-local copies of pool pages).
struct SparsePiece { VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceSize off = 0; };
struct Mirror {
    std::vector<gfx::Buffer> chunks;    // the copy, chunk_bytes() each (the last one may be shorter); empty until a draw wants a page in it
    std::vector<uint32_t> chunk_used_at;      // per chunk (sparse: per granule): present count of the last bind, fill or copy that named it (release_idle_chunks)
    std::vector<uint64_t> chunk_used_serial;  // per chunk (sparse: per granule): the submission being recorded then
    bool sparse = false;                // one sparse buffer (sbuf) instead of chunks (chunks stays empty)
    VkBuffer sbuf = VK_NULL_HANDLE;     // sparse: the copy, pool offset = buffer offset, size sbuf_size (the pool rounded up to granules)
    VkDeviceSize sbuf_size = 0;
    std::vector<uint8_t> gstate;        // sparse: per granule, a GranState
    std::vector<SparsePiece> gmem;      // sparse: per granule, the memory bound (or being bound) to it
    PoolGpu* pg = nullptr;              // the pool (the copies read its chunk imports, mirror_src)
    mem::ww::Region* ww = nullptr;      // the CPU's writes to the pool's pages
    uint64_t npages = 0;
    std::vector<uint64_t> want;         // bitmap: a draw wanted the page while it was not valid
    std::vector<uint32_t> copied_at;    // present count of the last copy (0: never)
    std::vector<uint64_t> used;         // bitmap: a draw read the page from the copy since it was copied
    std::vector<uint8_t> score;         // copies in a row that the CPU overwrote before any draw read them
    std::vector<uint8_t> backoff;       // how often the page has been hot in a row (the hold doubles each time)
    std::vector<uint32_t> hot_until;    // present count until which the page is not copied
    bool any_want = false;
    uint64_t want_lo = UINT64_MAX, want_hi = 0;   // [nvn] setup_coalesce: the pages [want_lo, want_hi] hold every set bit of `want`
    int pool_flags = 0;
    int64_t st_written = 0, st_copied = 0;   // pages the CPU wrote after a copy, pages copied (summary line)
    uint64_t pool_bytes = 0;
    // compute out (below): pages where a compute result in the copy is newer than the pool, and pages the CPU has read compute results
    // from (any thread sets them; atomics)
    std::unique_ptr<std::atomic<uint64_t>[]> gpu_new, cpu_read;
};

namespace pool_detail {

// Sparse copies (the Mirror comment). Render thread. A granule goes Unbound -> Wanted (a bind, fill or copy asked for it) -> Binding (in the
// batch in flight) -> Bound (the batch has run: commands may use it) -> Unbinding (released; its pages invalid) -> Unbound.
enum GranState : uint8_t { kGranUnbound = 0, kGranWanted, kGranBinding, kGranBound, kGranUnbinding };
constexpr uint64_t kPageBytes = 4096;
// A chunk no bind, fill or copy named for this many presents may be released when the cap is used up (release_idle_chunks). Long enough that
// chunks the current area draws from are never taken, so a scene that needs more than the cap keeps its chunks, as before.
constexpr uint32_t kChunkIdle = 600;
// The copy's usage (chunks and sparse buffers). Texel buffer and transfer source usage: compute dispatches bind the copy directly as storage
// and texel buffers, and their results go back to the pool from it (nvn_replay.cpp, compute shadows).
constexpr VkBufferUsageFlags kMirrorUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

// Mirror state of nvn_gpu_pool.cpp (render thread unless noted).
extern std::vector<Mirror*> g_mirrors;
extern uint64_t g_mirror_bytes;       // video memory of the chunks (sparse: granules) made
extern uint64_t g_released_bytes;     // video memory released for new ones, ever
extern uint64_t g_released_n;
extern std::atomic<int64_t> g_st_live_bytes;
extern std::atomic<uint64_t> g_st_chunk_bytes_made;   // video memory of the copy chunks made, ever (the hitch line)
uint32_t presents_now();
uint64_t mirror_cap_bytes();
uint64_t chunk_bytes();
void bit_put(std::vector<uint64_t>& v, uint64_t i, bool on);
bool chunk_idle(const Mirror& m, size_t ci, uint32_t now, uint64_t done, uint64_t cur);

// nvn_gpu_pool_sparse.cpp (the Mirror comment in nvn_gpu_pool.cpp). Render thread.
uint32_t gran_of(uint64_t page);
bool sparse_create(Mirror& m);
bool sparse_ready(Mirror& m, uint64_t first, uint64_t last);
bool sparse_bound(const Mirror& m, uint64_t first, uint64_t last);
uint64_t sparse_run_end(const Mirror& m, uint64_t p);
void sparse_poll(bool wait);
void sparse_submit();
void sparse_destroy(Mirror& m);
int sparse_live();                          // sparse mirrors alive
bool sparse_starved();                      // the last bind batch found no memory for some granules (cap used up)
uint64_t sparse_bind_us();                  // microseconds in vkQueueBindSparse, ever (any thread)
void sparse_log(char* out, size_t size);    // the sparse part of the summary line, for the period since the last call

}  // namespace pool_detail
}  // namespace nxrt::nvn::be::gpu
