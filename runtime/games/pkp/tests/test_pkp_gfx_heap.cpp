// SPDX-License-Identifier: GPL-3.0-or-later
// The graphics pool plan for render sizes above 1920x1080 (games/pkp/pkp_gfx_heap.h): nothing changes at the console's size and below, the
// general pool grows 28 B per extra pixel (fork #49: 3840x2160 needs +167 MiB), the render-target pool only past its 3500 MiB, the arena by the
// sum, and a size whose pools would pass 4 GiB is clamped to the largest of the same shape that fits.
#include "test_framework.h"

#include "pkp_gfx_heap.h"

NXTEST_GROUP(pkp_gfx_heap)

namespace {

using nxrt::games::pkp::gfx_heap_plan;
using nxrt::games::pkp::GfxHeapPlan;
constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kGeneral = 0x14c00000ull, kRt = 0xdac00000ull, kArena = 0x137c07008ull;

void check_untouched(int w, int h) {
    const GfxHeapPlan p = gfx_heap_plan(w, h);
    CHECK_EQ(p.w, w);
    CHECK_EQ(p.h, h);
    CHECK(!p.clamped);
    CHECK_EQ(p.extra_general, 0ull);
    CHECK_EQ(p.extra_rt, 0ull);
    CHECK_EQ(p.general, kGeneral);
    CHECK_EQ(p.rt, kRt);
    CHECK_EQ(p.arena, kArena);
}

}  // namespace

NXTEST(console_size_and_below_untouched) {
    check_untouched(1920, 1080);
    check_untouched(1280, 720);
    check_untouched(1600, 900);
    check_untouched(1920, 800);            // a wide shape with fewer pixels
}

NXTEST(general_pool_grows_per_pixel) {
    struct { int w, h; uint64_t extra_mib; } cases[] = {{2560, 1440, 44}, {3712, 2088, 152}, {3840, 2160, 167}, {4096, 2304, 197}, {2520, 1080, 18}};
    for (const auto& c : cases) {
        const GfxHeapPlan p = gfx_heap_plan(c.w, c.h);
        CHECK_EQ(p.w, c.w);
        CHECK_EQ(p.h, c.h);
        CHECK(!p.clamped);
        CHECK_EQ(p.extra_general, c.extra_mib * kMiB);
        CHECK_EQ(p.extra_rt, 0ull);                     // 937.6 MiB + 160 B per extra pixel + 1 GiB stays under 3500 MiB up to here
        CHECK_EQ(p.general, kGeneral + p.extra_general);
        CHECK_EQ(p.arena, kArena + p.extra_general);
    }
    CHECK_EQ(gfx_heap_plan(3840, 2160).arena, 0x142307008ull);
}

NXTEST(render_target_pool_grows_when_needed) {
    const GfxHeapPlan p = gfx_heap_plan(5120, 2880);  // 937.6 + 1934 + 1024 MiB = 3896 MiB
    CHECK(!p.clamped);
    CHECK_EQ(p.rt, 3896 * kMiB);
    CHECK_EQ(p.extra_rt, 3896 * kMiB - kRt);
    CHECK_EQ(p.arena, kArena + p.extra_general + p.extra_rt);
}

NXTEST(oversize_clamped_keeping_shape) {
    const GfxHeapPlan p = gfx_heap_plan(7680, 4320);
    CHECK(p.clamped);
    CHECK_EQ(p.w, 5312);                               // the largest 16:9 size in 32x18 steps whose pools stay under 4 GiB
    CHECK_EQ(p.h, 2988);
    CHECK(p.general <= 0xfff00000ull);
    CHECK(p.rt <= 0xfff00000ull);
    CHECK(gfx_heap_plan(5344, 3006).clamped);          // one step up does not fit
    const GfxHeapPlan q = gfx_heap_plan(8192, 3500);   // another shape: even sides, aspect kept
    CHECK(q.clamped);
    CHECK_EQ(q.w % 2, 0);
    CHECK_EQ(q.h % 2, 0);
    CHECK(q.rt <= 0xfff00000ull);
    CHECK(static_cast<double>(q.w) / q.h > 2.33 && static_cast<double>(q.w) / q.h < 2.35);
    CHECK(!gfx_heap_plan(q.w, q.h).clamped);
}
