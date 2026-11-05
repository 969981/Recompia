// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] pool_lazy (nvn_gpu_pool.cpp): a pool larger than one cell is imported a run of cells at a time as binds touch them, and imports no
// bind used for [nvn] pool_lazy_idle presents are dropped. The harness's 64 MiB main pool with 1 MiB cells, resolved on the render thread
// the way the replay resolves vertex, index and storage bindings: only the cells touched are imported, a range across a boundary nothing
// holds yet gets one import over both cells, an idle import goes after clear-only frames and comes back on the next resolve.
#include "nvn_app.h"
#include <cstdint>
#include <cstdio>

#include <nxrt/config.h>

#include "nvn_gpu_internal.h"
NXTEST_GROUP(gfx_nvn_pool_lazy)

using namespace nvnapp;
namespace gpu = nxrt::nvn::be::gpu;

namespace {

constexpr int kW = 64, kH = 48;
constexpr uint64_t kCell = 1u << 20;

// The settings are read once per process: every case sets the same ones before its App starts the back end.
void lazy_settings() {
    nxrt::config_set("nvn", "pool_lazy", "on");
    nxrt::config_set("nvn", "pool_lazy_chunk_mb", "1");
    nxrt::config_set("nvn", "pool_lazy_idle", "4");
}

uint64_t main_address(App& app) { return app.api.nvnMemoryPoolGetBufferAddress(app.pool_main->obj.get()); }

gpu::BufRef resolve(uint64_t addr, uint64_t size) {
    gpu::BufRef r;
    gpu::run_on_render_thread([&] { r = gpu::resolve_buffer(addr, size); });
    return r;
}

// A clear-only frame: one submission and one present (the drop check runs at the start of a submission).
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
    app.api.nvnQueuePresentTexture(app.queue.get(), app.window.get(), idx);   // (no read-back: only the present count matters)
    return true;
}

}  // namespace

// Only the cells resolved are imported: cell 3 alone, then cells 5 and 6 as one import for a range across their boundary; a second range
// in cell 5 lands in that import. Far less than the 64 MiB pool is imported.
NXTEST(lazy_imports_only_the_cells_binds_touch) {
    lazy_settings();
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    if (gpu::pool_copy_on()) { nxtest::skip("[nvn] pool_copy is on (NXRT_NVN_POOL_COPY): pools are shadows, the lazy import path is not used"); return; }
    const gpu::PoolLazyStats s0 = gpu::pool_lazy_stats();
    REQUIRE(s0.on);
    const uint64_t base = main_address(app);
    const gpu::BufRef a = resolve(base + 3 * kCell + 0x100, 48);
    const gpu::PoolLazyStats s1 = gpu::pool_lazy_stats();
    const gpu::BufRef b = resolve(base + 6 * kCell - 8, 24);
    const gpu::BufRef c = resolve(base + 5 * kCell + 0x10, 16);
    const gpu::PoolLazyStats s2 = gpu::pool_lazy_stats();
    std::printf("    imports %llu then %llu, live %lld KiB, spans %llu; a: avail 0x%llx, b: offset 0x%llx avail 0x%llx, c: offset 0x%llx\n",
                static_cast<unsigned long long>(s1.imports - s0.imports), static_cast<unsigned long long>(s2.imports - s1.imports),
                static_cast<long long>((s2.live_bytes - s0.live_bytes) >> 10), static_cast<unsigned long long>(s2.spans - s0.spans),
                static_cast<unsigned long long>(a.avail), static_cast<unsigned long long>(b.offset), static_cast<unsigned long long>(b.avail),
                static_cast<unsigned long long>(c.offset));
    CHECK(a.ok && b.ok && c.ok);
    CHECK(a.buffer != gpu::dummy_buffer() && b.buffer != gpu::dummy_buffer());
    CHECK_EQ(s1.imports - s0.imports, 1ull);
    CHECK_EQ(a.avail, kCell - 0x100);                  // the import is cell 3, nothing more
    CHECK_EQ(s2.imports - s1.imports, 1ull);           // cells 5 and 6 in one import, c found in it
    CHECK(b.buffer == c.buffer);
    CHECK_EQ(b.offset, kCell - 8);
    CHECK_EQ(b.avail, kCell + 8);
    CHECK_EQ(c.offset, 0x10ull);
    CHECK_EQ(s2.spans - s0.spans, 0ull);
    CHECK_EQ(s2.live_bytes - s0.live_bytes, static_cast<int64_t>(3 * kCell));
    // A range from cell 3 into cell 4 (free) needs an import overlapping cell 3's: some drivers refuse importing the same pages twice at
    // one address (the harness pool has no private views), so only the outcome is printed.
    const gpu::BufRef d = resolve(base + 4 * kCell - 16, 64);
    std::printf("    span across cells 3/4: %s\n", d.ok ? "imported" : "refused by the driver (dummy buffer)");
    CHECK_EQ(app.validation().errors, 0ull);
}

// An import no resolve used for pool_lazy_idle (4) presents is dropped once its submissions are done; resolving its cell again imports
// it again.
NXTEST(lazy_idle_imports_are_dropped_and_come_back) {
    lazy_settings();
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    if (gpu::pool_copy_on()) { nxtest::skip("[nvn] pool_copy is on (NXRT_NVN_POOL_COPY): pools are shadows, the lazy import path is not used"); return; }
    REQUIRE(gpu::pool_lazy_stats().on);
    const uint64_t base = main_address(app);
    const gpu::BufRef a = resolve(base + 10 * kCell + 0x40, 48);
    REQUIRE(a.ok);
    const gpu::PoolLazyStats s0 = gpu::pool_lazy_stats();
    for (int i = 0; i < 12; ++i) REQUIRE(clear_frame(app));   // (the harness has command memory for 16 command buffers)
    app.finish();
    const gpu::PoolLazyStats s1 = gpu::pool_lazy_stats();
    const gpu::BufRef b = resolve(base + 10 * kCell + 0x40, 48);
    const gpu::PoolLazyStats s2 = gpu::pool_lazy_stats();
    std::printf("    dropped %llu (%lld KiB live after), re-imports %llu\n", static_cast<unsigned long long>(s1.dropped - s0.dropped),
                static_cast<long long>(s1.live_bytes >> 10), static_cast<unsigned long long>(s2.imports - s1.imports));
    CHECK(s1.dropped - s0.dropped >= 1);
    CHECK(s1.live_bytes < s0.live_bytes);
    CHECK(b.ok && b.buffer != gpu::dummy_buffer());
    CHECK_EQ(s2.imports - s1.imports, 1ull);
    CHECK_EQ(app.validation().errors, 0ull);
}
