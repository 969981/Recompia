// SPDX-License-Identifier: GPL-3.0-or-later
// Which pages of a copy pool ([nvn] pool_copy, nvn_pool_copy.cpp) the CPU wrote since a shadow last copied them, so a bind copies a page
// again only after a write. Pages are made read-only when they are copied (nxrt/write_watch.h); the first write faults once and the page
// counts as written. A page written again soon after every copy (a uniform ring the game refills each frame, a range the GPU's results
// land in) is "hot": it is left writable and copied at every use for a while, instead of faulting every frame.
//
// A page has a generation, raised each time it is armed again after a write. A shadow that stores the generation it copied is current
// while the page is still armed and the generation is the same; two shadows over one page (a cell and a span) each keep their own.
// All calls: the render thread, except create / destroy / stop (the thread that makes or destroys the pool; no bind of it runs then).
#pragma once
#include <cstdint>

namespace nxrt::nvn::be::gpu {

struct PoolTrack;

// [nvn] pool_copy_track = true (default) | false: read once.
bool pool_track_on();
// A tracker over [base, base + size) (page aligned). Null: the range cannot be tracked (outside the heap's primary view), or tracking is off.
PoolTrack* ptrack_create(const uint8_t* base, uint64_t size);
void ptrack_destroy(PoolTrack* t);
// Every page becomes writable again and none is tracked any more (a cell of the pool is imported instead of copied: an import must not
// see read-only pages).
void ptrack_stop(PoolTrack* t);
// True when a copy of pool page `page` that stored generation `copied` still holds what guest memory holds.
bool ptrack_current(const PoolTrack* t, uint64_t page, uint32_t copied);
// Pool pages [first, first + count) are about to be copied (call before reading them): those the CPU wrote are armed again under a new
// generation, hot ones are left writable. gens[i] receives the generation to store with the copy of page first + i; 0: copy it at every
// use (hot, or it cannot be armed). `frame`: the present count.
void ptrack_before_copy(PoolTrack* t, uint64_t first, uint64_t count, uint32_t frame, uint32_t* gens);

struct PoolTrackStats {
    uint64_t arms = 0;          // arm calls
    uint64_t armed_pages = 0;   // pages armed by them
    uint64_t rewrites = 0;      // copied pages found written again
    uint64_t hot_now = 0;       // pages currently hot
    uint64_t unarmable = 0;     // pages that could not be armed (copied at every use)
};
PoolTrackStats ptrack_stats();

}  // namespace nxrt::nvn::be::gpu
