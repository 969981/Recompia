// SPDX-License-Identifier: GPL-3.0-or-later
// S38: how much video memory the device-local pool copies ([nvn] pool_mirror) may take. Pure, so the tests can call it.
#pragma once

#include <algorithm>
#include <cstdint>

namespace nxrt::nvn {

struct MirrorCap {
    uint64_t bytes = 0;         // most bytes the copies may take in all (0: no copies)
    const char* why = "";       // for the log line
    uint64_t chunk_bytes = 0;   // the copy's chunk: made whole the first time a draw wants one of its pages
    bool sparse = false;        // mirror_sparse(): one sparse buffer per pool, bound in kMirrorGranule pieces (chunk_bytes unused then)
};

constexpr uint64_t kMirrorCapMax = 2048ull << 20;      // the default cap on a large card
// Below this much video memory there are no copies by default. A "4 GB" card reports less than 4 GiB (a GTX 1650: 3938 MiB), and those
// cards gained the most from the copies (#694: ~13 to ~35 fps with 1280 MiB of them).
constexpr uint64_t kMirrorMinVram = 3584ull << 20;
constexpr uint64_t kMirrorChunkLarge = 64ull << 20;    // the chunk from a cap of kMirrorLargeCap up
// A quarter of 7 GiB: an "8 GB" card reports a little under 8 GiB, so its cap is just under 2048 MiB, and it keeps the large chunk.
constexpr uint64_t kMirrorLargeCap = 1792ull << 20;
// The chunk under a smaller cap. The game's data is spread over its 3.5 GiB pool, so a 64 MiB chunk made for a few wanted pages held
// about 2% valid pages: a 1488 MiB cap (6 GB card) was used up with 32 MiB of them valid, and the draws of later areas read host memory
// over PCIe (#693, #694). 16 MiB chunks cover four times the regions for the same cap; a bind across two of them reads the pool.
constexpr uint64_t kMirrorChunkSmall = 16ull << 20;

// vram_bytes: the largest device-local heap (0 when unknown). explicit_mb >= 0: [nvn] pool_mirror_mb was set in the ini or the
// environment, and that value wins; -1: not set, the cap follows the card: min(2048 MiB, a quarter of the heap), none below 3.5 GiB.
// shared_memory: an integrated GPU or APU, whose "video memory" is system RAM; the copies would only cost RAM there, so none by default.
// The chunk is kMirrorChunkLarge from a cap of kMirrorLargeCap up (cards of 8 GB and more keep what they had), kMirrorChunkSmall below.
inline MirrorCap mirror_cap(uint64_t vram_bytes, int64_t explicit_mb, bool shared_memory = false) {
    MirrorCap c;
    if (explicit_mb >= 0) c = {static_cast<uint64_t>(explicit_mb) << 20, "[nvn] pool_mirror_mb"};
    else if (shared_memory) c = {0, "integrated GPU (shares system memory)"};
    else if (vram_bytes < kMirrorMinVram) c = {0, "under 3.5 GiB of video memory"};
    else c = {std::min(kMirrorCapMax, vram_bytes / 4), vram_bytes / 4 < kMirrorCapMax ? "a quarter of the video memory" : "2048 MiB"};
    c.chunk_bytes = c.bytes >= kMirrorLargeCap ? kMirrorChunkLarge : kMirrorChunkSmall;
    return c;
}

// [nvn] mirror_sparse = auto (default) | off. Under kMirrorLargeCap a chunk was made for a few wanted pages and held mostly invalid ones: a
// GTX 1650 (983 MiB cap, 16 MiB chunks) used the cap up with 30-70 MiB of valid pages, and 6.0M vertex binds a period found no chunk. A
// sparse buffer the size of the pool is bound in granules of this many bytes instead, each the first time a draw wants one of its pages.
constexpr uint64_t kMirrorGranule = 2ull << 20;

// The copies are sparse only where chunks fall short: a cap under kMirrorLargeCap (cards under about 8 GB; a large cap keeps its 64 MiB
// chunks), a discrete GPU (an integrated one has no copies by default), a device and queue that can bind sparse buffers, and `allowed`
// ([nvn] mirror_sparse not off). Every other case keeps the chunks.
inline bool mirror_sparse(const MirrorCap& c, bool discrete, bool supported, bool allowed) {
    return allowed && supported && discrete && c.bytes != 0 && c.bytes < kMirrorLargeCap;
}

}  // namespace nxrt::nvn
