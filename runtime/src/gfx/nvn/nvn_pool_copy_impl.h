// SPDX-License-Identifier: GPL-3.0-or-later
// What the files of [nvn] pool_copy share with each other: nvn_pool_copy.cpp (the shadows and the copy into them) and nvn_pool_land.cpp
// (GPU writes going back into guest memory). Everybody else uses the interface in nvn_gpu_internal.h.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu::pcopy {

constexpr const char* kTag = "nvn.pool";
constexpr uint64_t kPage = 4096;

// statistics both files add to (the summary line, gauges, hitch counters)
struct Shared {
    std::atomic<uint64_t> landed{0};       // bytes copied from shadows into guest memory
    std::atomic<uint64_t> records{0};      // GPU writes recorded
    std::atomic<uint64_t> unreadable{0};   // pages a copy could not read or write (decommitted under it)
};
Shared& shared();

// [d, d + n) <- [s, s + n); on a fault, page by page of `s` (in) or `d` (out): a part that cannot be copied is zero-filled when it is the
// shadow side (in) or left alone (out). Returns the parts that could not be copied.
uint64_t guarded_copy(uint8_t* d, const uint8_t* s, uint64_t n, bool in);

// ---- nvn_pool_land.cpp ----------------------------------------------------------------------------------------------------------
void land_start();                 // the landing thread, once a copy pool exists
void land_stop();                  // pcopy_shutdown: the thread stops and every record goes
// Render thread: submission `serial` may write pool bytes [off, off + size) of pg through chunk c; they land when it completes. False:
// a record of this submission holds the range already.
bool land_record(PoolGpu* pg, PoolChunk* c, uint64_t off, uint64_t size, uint64_t serial);
void land_forget(PoolChunk& c);    // the chunk is destroyed: its records go, and no landing reads its shadow after this returns
size_t land_pending();             // records waiting for their serial

}  // namespace nxrt::nvn::be::gpu::pcopy
