// SPDX-License-Identifier: GPL-3.0-or-later
// GraphicsKey::hash (src/gfx/nvn/nvn_pipeline.cpp): word-wise hash of the key. Equal keys hash equal, one changed byte anywhere changes the
// hash, and the buckets spread; the timing case prints ns per hash of the old byte-wise FNV-1a and the new one.
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <unordered_set>

#include "nvn_pipeline.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_pipe_hash)

using nxrt::nvn::be::gpu::GraphicsKey;

namespace {
uint64_t old_hash(const GraphicsKey& k) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof k; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
GraphicsKey base_key() {
    GraphicsKey k;
    k.color_format[0] = 37; k.color_count = 1; k.depth_format = 126; k.attr_count = 3;
    for (int i = 0; i < 3; ++i) { k.attr[i].location = uint8_t(i); k.attr[i].format = 100 + i; k.attr[i].offset = uint16_t(i * 12); }
    k.blend_enable[0] = 1; k.write_mask[0] = 15;
    return k;
}
}  // namespace

NXTEST(pipe_hash_equal_keys_and_every_byte) {
    const GraphicsKey a = base_key(), b = base_key();
    CHECK(a == b);
    CHECK(a.hash() == b.hash());
    for (size_t i = 0; i < sizeof a; ++i) {
        GraphicsKey c = a;
        reinterpret_cast<uint8_t*>(&c)[i] ^= 1;
        CHECK(!(c == a));
        CHECK(c.hash() != a.hash());
    }
}

NXTEST(pipe_hash_spread) {
    std::unordered_set<uint64_t> seen;
    GraphicsKey k = base_key();
    for (uint32_t i = 0; i < 20000; ++i) {
        k.attr[i % 16].format = i; k.samples = 1 + (i >> 4); k.color_format[i % 8] = i * 7;
        seen.insert(k.hash());
    }
    CHECK(seen.size() > 19990);
}

NXTEST(pipe_hash_timing) {
    GraphicsKey k = base_key();
    constexpr int N = 2000000;
    uint64_t sink = 0;
    auto run = [&](auto&& fn) {
        double best = 1e18;
        for (int r = 0; r < 5; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < N; ++i) { k.samples = uint32_t(i); sink += fn(); }
            best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N);
        }
        return best;
    };
    const double o = run([&] { return old_hash(k); });
    const double n = run([&] { return k.hash(); });
    std::printf("[bench] PC GraphicsKey (%zu B) hash: old byte-wise %.1f ns, new word-wise %.1f ns (%.1fx) sink=%llu\n", sizeof k, o, n, o / n,
                static_cast<unsigned long long>(sink));
    CHECK(n < o);
}
