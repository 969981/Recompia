// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] pool_copy (nvn_pool_copy.cpp): pools as host-visible shadows instead of imports. The harness's 64 MiB main pool with 1 MiB cells:
// a GPU copy reads what the CPU wrote (copied into the shadow by the resolve, again in the next submission after the CPU rewrote it), a
// GPU fill reaches guest memory when its serial completes and not before, a touch skips pages with a GPU write in flight, a shadow with
// writes that have not landed is busy (never dropped), the cap evicts the least recently used idle shadows, and a write through a span is
// counted. With NXRT_NVN_POOL_COPY forced off (the A/B run of every gfx_nvn_* group) the cases are skipped.
#include "nvn_app.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <nxrt/config.h>

#include "nvn_gpu_internal.h"
NXTEST_GROUP(gfx_nvn_pool_copy)

using namespace nvnapp;
namespace be = nxrt::nvn::be;
namespace gpu = nxrt::nvn::be::gpu;

namespace {

constexpr int kW = 64, kH = 48;
constexpr uint64_t kCell = 1u << 20;

// The settings are read once per process: every case sets the same ones before its App starts the back end.
void copy_settings() {
    nxrt::config_set("nvn", "pool_copy", "on");
    nxrt::config_set("nvn", "pool_copy_cell_mb", "1");
    nxrt::config_set("nvn", "pool_copy_idle", "4");
    nxrt::config_set("nvn", "pool_copy_mb", "6");
}

// The harness never finalizes its pools, so they stay registered after the App goes, and a later App's pool can be allocated over the
// range of an old one: lookups above the old base then find the old pool (seen as "not in an importable memory pool"). Each case takes
// its pools out of the registry when it ends (declared before the App, so after it is destroyed).
struct Unregister {
    std::vector<be::MemoryPool*> pools;
    ~Unregister() {
        for (be::MemoryPool* p : pools) be::pool_unregister(p);
    }
};

bool init_app(App& app, Unregister& u) {
    copy_settings();
    App::Opts o;
    o.width = kW;
    o.height = kH;
    if (!app.init(o)) return false;
    for (Pool* p : {app.pool_main.get(), app.pool_cmd.get(), app.pool_desc.get()})
        if (p)
            if (be::MemoryPool* mp = be::pool_lookup(app.api.nvnMemoryPoolGetBufferAddress(p->obj.get())).pool) u.pools.push_back(mp);
    return true;
}

// false (the case is skipped) when the environment forces copy mode off: the case has nothing to check then
bool copy_on() {
    if (gpu::pool_copy_on()) return true;
    nxtest::skip("[nvn] pool_copy is forced off (NXRT_NVN_POOL_COPY)");
    return false;
}

uint64_t main_address(App& app) { return app.api.nvnMemoryPoolGetBufferAddress(app.pool_main->obj.get()); }

gpu::BufRef resolve(uint64_t addr, uint64_t size) {
    gpu::BufRef r;
    gpu::run_on_render_thread([&] { r = gpu::resolve_buffer(addr, size); });
    return r;
}

// The shadow chunk (ext) holding GPU address addr, or null. Render thread state: read it only while nothing records.
be::PoolChunk* chunk_of(uint64_t addr) {
    const be::PoolHit hit = be::pool_lookup(addr);
    if (!hit.pool || !hit.pool->gpu || !hit.pool->gpu->copy) return nullptr;
    be::PoolGpu* pg = hit.pool->gpu;
    const uint64_t off = addr - reinterpret_cast<uint64_t>(pg->base);
    const int32_t e = pg->cell_ext[static_cast<size_t>(off / pg->stride)];
    return e >= 0 ? &pg->ext[static_cast<size_t>(e)] : nullptr;
}

bool clear_frame(App& app) {
    const int idx = app.acquire();
    if (idx < 0) { std::printf("    acquire failed\n"); return false; }
    auto cb = app.make_cmdbuf();
    if (!cb) { std::printf("    no command buffer\n"); return false; }
    NVNcommandBuffer* c = cb->obj.get();
    NVNtexture* targets[] = {app.window_textures[idx]->obj.get()};
    app.api.nvnCommandBufferBeginRecording(c);
    app.api.nvnCommandBufferSetRenderTargets(c, 1, targets, nullptr, nullptr, nullptr);
    app.api.nvnCommandBufferSetViewport(c, 0, 0, kW, kH);
    app.api.nvnCommandBufferSetScissor(c, 0, 0, kW, kH);
    const float black[4] = {0, 0, 0, 1};
    app.api.nvnCommandBufferClearColor(c, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
    app.end_and_submit(*cb);
    app.api.nvnQueuePresentTexture(app.queue.get(), app.window.get(), idx);
    return true;
}

}  // namespace

// The GPU copies a buffer the CPU wrote: the copy reads the shadow, which the resolve filled from guest memory, and its result lands in guest
// memory. The CPU writes new bytes and the next submission copies them (the shadow is refreshed per submission). Nothing is imported.
NXTEST(copy_gpu_reads_what_the_cpu_wrote) {
    Unregister unreg;
    App app;
    REQUIRE(init_app(app, unreg));
    if (!copy_on()) return;
    const uint64_t imports0 = gpu::pool_imports();
    const gpu::PoolCopyStats s0 = gpu::pool_copy_stats();
    App::Buf src = app.make_buffer(4096), dst = app.make_buffer(4096);
    int bad[2] = {0, 0};
    for (int round = 0; round < 2; ++round) {
        uint32_t* sw = reinterpret_cast<uint32_t*>(src.ptr);
        for (int i = 0; i < 1024; ++i) sw[i] = 0x10000000u * static_cast<uint32_t>(round + 1) + static_cast<uint32_t>(i);
        std::memset(dst.ptr, 0, 4096);
        auto bc = app.make_cmdbuf();
        REQUIRE(bc != nullptr);
        app.api.nvnCommandBufferBeginRecording(bc->obj.get());
        app.api.nvnCommandBufferCopyBufferToBuffer(bc->obj.get(), src.address, dst.address, 4096, 0);
        app.end_and_submit(*bc);
        app.finish();
        const uint32_t* dw = reinterpret_cast<const uint32_t*>(dst.ptr);
        for (int i = 0; i < 1024; ++i) bad[round] += dw[i] != sw[i] ? 1 : 0;
    }
    const gpu::PoolCopyStats s1 = gpu::pool_copy_stats();
    std::printf("    wrong words: %d then %d; shadows made %llu, copied in %llu KiB, landed %llu KiB, imports %llu, fallbacks %llu\n", bad[0], bad[1],
                static_cast<unsigned long long>(s1.allocs - s0.allocs), static_cast<unsigned long long>((s1.copied_bytes - s0.copied_bytes) >> 10),
                static_cast<unsigned long long>((s1.landed_bytes - s0.landed_bytes) >> 10), static_cast<unsigned long long>(gpu::pool_imports() - imports0),
                static_cast<unsigned long long>(s1.import_fallback - s0.import_fallback));
    CHECK_EQ(bad[0], 0);
    CHECK_EQ(bad[1], 0);
    CHECK(s1.allocs > s0.allocs);
    CHECK(s1.landed_bytes - s0.landed_bytes >= 4096);
    CHECK_EQ(gpu::pool_imports() - imports0, 0ull);
    CHECK_EQ(s1.import_fallback - s0.import_fallback, 0ull);
    CHECK_EQ(app.validation().errors, 0ull);
}

// A fill recorded into the shadow: guest memory keeps its bytes while the submission is recorded (a landing of the serial before it moves
// nothing), the shadow is busy, and a later touch of the range does not copy the CPU's new bytes over the pending GPU write. Once the
// serial completes the fill is in guest memory (over the CPU's bytes: the whole recorded range lands, a phase-1 limit) and the shadow is
// no longer busy.
NXTEST(copy_fill_lands_at_completion_not_before) {
    Unregister unreg;
    App app;
    REQUIRE(init_app(app, unreg));
    if (!copy_on()) return;
    const uint64_t addr = main_address(app) + 7 * kCell + 0x2000;
    uint8_t* guest = app.pool_main->base() + 7 * kCell + 0x2000;
    std::memset(guest, 0x11, 256);
    bool before_ok = false, pending = false, busy = false, kept = false;
    be::PoolChunk* ch = nullptr;
    gpu::run_frame_sync([&](gpu::Frame& f) {
        const gpu::BufRef b = gpu::resolve_buffer(addr, 256);
        if (!b.ok) return;
        vkCmdFillBuffer(f.cb, b.buffer, b.offset, 256, 0x5A5A5A5Au);
        gpu::note_gpu_write(addr, 256, gpu::kWriteFill);
        ch = chunk_of(addr);
        if (!ch || !ch->is_shadow) return;
        gpu::pcopy_land(f.serial - 1);
        before_ok = guest[0] == 0x11 && guest[255] == 0x11;
        pending = gpu::pool_copy_stats().pending >= 1;
        busy = gpu::pcopy_busy(*ch);
        // the CPU writes the range again; a touch in a later submission (pretended: the page counts as copied long ago) must keep the shadow
        std::memset(guest, 0x77, 256);
        const uint64_t coff = addr - reinterpret_cast<uint64_t>(ch->owner->base) - ch->off;    // offset in the shadow
        ch->page_copied[static_cast<size_t>(coff / 4096)] = 1;
        ch->cov_serial = 0;
        (void)gpu::resolve_buffer(addr, 256);
        const uint8_t* sh = static_cast<const uint8_t*>(ch->shadow.mapped) + coff;
        kept = sh[0] == 0x11 && sh[255] == 0x11;     // still the bytes of the first touch (the fill has not run yet)
    });
    gpu::run_on_render_thread([] { gpu::poll_completed(); });     // (run_frame_sync waited for the serial; this lands it if nothing has yet)
    const bool after_ok = guest[0] == 0x5A && guest[255] == 0x5A;
    const bool busy_after = ch && gpu::pcopy_busy(*ch);
    std::printf("    shadow %s; before completion: guest %s, record pending %d, busy %d, shadow kept %d; after: guest %s, busy %d\n",
                ch ? "found" : "missing", before_ok ? "unchanged" : "CHANGED", pending ? 1 : 0, busy ? 1 : 0, kept ? 1 : 0,
                after_ok ? "filled" : "NOT filled", busy_after ? 1 : 0);
    REQUIRE(ch != nullptr);
    CHECK(before_ok);
    CHECK(pending);
    CHECK(busy);
    CHECK(kept);
    CHECK(after_ok);
    CHECK(!busy_after);
    CHECK_EQ(app.validation().errors, 0ull);
}

// An idle shadow goes after pool_copy_idle (4) presents once nothing of it is pending, and its cell is copied again at the next resolve.
NXTEST(copy_idle_shadows_are_dropped) {
    Unregister unreg;
    App app;
    REQUIRE(init_app(app, unreg));
    if (!copy_on()) return;
    const uint64_t addr = main_address(app) + 10 * kCell + 0x40;
    REQUIRE(resolve(addr, 48).ok);
    const gpu::PoolCopyStats s0 = gpu::pool_copy_stats();
    for (int i = 0; i < 12; ++i) REQUIRE(clear_frame(app));
    app.finish();
    const gpu::PoolCopyStats s1 = gpu::pool_copy_stats();
    const gpu::BufRef b = resolve(addr, 48);
    const gpu::PoolCopyStats s2 = gpu::pool_copy_stats();
    std::printf("    shadows %llu -> %llu (live %lld KiB), made again %llu\n", static_cast<unsigned long long>(s0.chunks),
                static_cast<unsigned long long>(s1.chunks), static_cast<long long>(s1.live_bytes >> 10), static_cast<unsigned long long>(s2.allocs - s1.allocs));
    CHECK(s1.chunks < s0.chunks);
    CHECK(b.ok);
    CHECK_EQ(s2.allocs - s1.allocs, 1ull);
    CHECK_EQ(app.validation().errors, 0ull);
}

// Over pool_copy_mb (6 MiB) a new shadow first drops the least recently used idle ones: eight cells resolved one submission after another
// leave at most the cap live (plus the one being made when nothing is idle).
NXTEST(copy_cap_evicts_least_recently_used) {
    Unregister unreg;
    App app;
    REQUIRE(init_app(app, unreg));
    if (!copy_on()) return;
    const uint64_t base = main_address(app);
    const gpu::PoolCopyStats s0 = gpu::pool_copy_stats();
    int64_t peak = 0;
    for (int k = 0; k < 8; ++k) {
        REQUIRE(resolve(base + static_cast<uint64_t>(20 + k) * kCell + 0x10, 64).ok);
        REQUIRE(clear_frame(app));    // the next resolve is in a later submission: this one's shadow is idle by then
        app.finish();
        peak = std::max<int64_t>(peak, gpu::pool_copy_stats().live_bytes);
    }
    const gpu::PoolCopyStats s1 = gpu::pool_copy_stats();
    // shadows made before (the other pools, window textures) may be in use: the bound is the cap or what was live at the start, plus one cell
    const int64_t bound = std::max<int64_t>(static_cast<int64_t>(6 * kCell), s0.live_bytes) + static_cast<int64_t>(kCell);
    std::printf("    evicted %llu, live %lld KiB at the start, peak %lld KiB (cap 6144 KiB)\n", static_cast<unsigned long long>(s1.evicted - s0.evicted),
                static_cast<long long>(s0.live_bytes >> 10), static_cast<long long>(peak >> 10));
    CHECK(s1.evicted > s0.evicted);
    CHECK(peak <= bound);
    CHECK_EQ(app.validation().errors, 0ull);
}

// A write into a range that crosses from one shadow into a cell another shadow holds goes through a span (its own shadow) and is counted.
NXTEST(copy_span_write_is_counted) {
    Unregister unreg;
    App app;
    REQUIRE(init_app(app, unreg));
    if (!copy_on()) return;
    const uint64_t base = main_address(app);
    const gpu::PoolCopyStats s0 = gpu::pool_copy_stats();
    gpu::BufRef a, b, s;
    gpu::run_on_render_thread([&] {
        a = gpu::resolve_buffer(base + 40 * kCell + 0x100, 64);
        b = gpu::resolve_buffer(base + 41 * kCell + 0x100, 64);
        s = gpu::resolve_buffer(base + 41 * kCell - 32, 64);
        gpu::note_gpu_write(base + 41 * kCell - 32, 64, gpu::kWriteStorage);
    });
    const gpu::PoolCopyStats s1 = gpu::pool_copy_stats();
    std::printf("    span %s, span writes %llu\n", s.ok && s.buffer != a.buffer && s.buffer != b.buffer ? "made" : "NOT made",
                static_cast<unsigned long long>(s1.span_writes - s0.span_writes));
    CHECK(a.ok && b.ok && s.ok);
    CHECK(s.buffer != a.buffer && s.buffer != b.buffer);
    CHECK_EQ(s1.span_writes - s0.span_writes, 1ull);
    app.finish();
    CHECK_EQ(app.validation().errors, 0ull);
}
