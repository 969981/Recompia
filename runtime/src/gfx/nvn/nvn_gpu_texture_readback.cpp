// SPDX-License-Identifier: GPL-3.0-or-later
// Texture readback (download_texture): render targets copied back and tiled into the game storage.
#include "nvn_gpu_texture_internal.h"

#include "nxrt/game.h"
#include "nxrt/game_render.h"

namespace nxrt::nvn::be {

namespace gpu {
// ---- readback (download_texture) ------------------------------------------------------------------------------------------------
// The game reads a render target on the CPU (nvnTextureReadTexels, nvnTextureInvalidateTexels; Pokopia about once a second from its
// worker threads). The old path drained the GPU twice on the render thread for it (finish(), then a submission waited for on the GPU:
// ~65 ms a read on the Deck, 38 % of its in-world spikes). [nvn] readback_async: the render thread records the copy in queue order and
// goes on; the reading thread waits for the GPU, then tiles the levels it asked for into the storage. A level whose write generation did
// not move since the storage last got it is not copied again.

namespace tex {
struct RbConfig {
    bool async = true;          // [nvn] readback_async
    int64_t grace_ms = -1;      // [nvn] readback_grace_ms (-1 auto: 0, flush at once; -2: one frame at the limiter's target, 33 ms when unknown)
    bool check = false;         // [nvn] readback_check
    bool log = false;           // [nvn] readback_log
    bool defer = false;         // [nvn] readback_defer (off: it froze the game, see CONFIG.md)
    uint32_t texture = 0;        // [nvn] readback_defer_texture: explicit diagnostic texture id, 0 disables
    const game::ReadbackShape* shape = nullptr;   // the shape that texture must have (game::Render::readback_defer); null: never deferred
};
static const RbConfig& rb_config() {
    static const RbConfig c = [] {
        RbConfig r;
        r.async = nxrt::config_get_bool("nvn", "readback_async", true);
        r.grace_ms = nxrt::config_get_int("nvn", "readback_grace_ms", -1);
        r.check = nxrt::config_get_bool("nvn", "readback_check", false);
        r.log = nxrt::config_get_bool("nvn", "readback_log", false);
        r.defer = nxrt::config_get_bool("nvn", "readback_defer", false);
        const int64_t texture = nxrt::config_get_int("nvn", "readback_defer_texture", 0);
        if (texture > 0 && texture <= UINT32_MAX) r.texture = static_cast<uint32_t>(texture);
        if (const game::Render* gr = game::info().render) r.shape = gr->readback_defer;
        NXLOG_I(kTag, "readback candidate: private buffers, completion-only polling, selected texture %u (0 disables); experimental stale-data semantics", r.texture);
        NXLOG_I(kTag, "readback: async %s, grace %s ms, check %s, log %s, defer %s", r.async ? "on" : "off",
                r.grace_ms == -1 ? "auto (0)" : r.grace_ms == -2 ? "one frame" : std::to_string(r.grace_ms).c_str(), r.check ? "on" : "off", r.log ? "on" : "off",
                r.defer ? "on" : "off");
        return r;
    }();
    return c;
}

static std::atomic<uint64_t> g_rb_copies{0}, g_rb_skipped{0}, g_rb_calls{0};

// How long the reading thread waits for the render thread to hand the copy to the GPU before it flushes it itself. Auto is 0 (#276): the
// render thread almost never gets to it within a frame, so a frame's wait only held the reading thread (often the game's main thread) for
// about 17 ms and then the flush was forced anyway; measured on a PC at 60 fps, long frames per 10 s 5.9 -> 0.0, the slowest read 154 -> 6 ms.
// -2 is the old auto: one frame at the limiter's target.
static uint64_t rb_grace_ns() {
    const int64_t ms = rb_config().grace_ms;
    if (ms >= 0) return static_cast<uint64_t>(ms) * 1000000ull;
    if (ms == -1) return 0;
    const FrameLimiter* lim = limiter();
    const double fps = lim ? lim->target_fps() : 0.0;
    return fps > 0.0 ? static_cast<uint64_t>(1e9 / fps) : 33000000ull;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            t[i] = v;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

// [nvn] readback_check: the CRC-32 of what the storage holds of each level asked for (untiled, the layers asked for in order), the same
// on both paths, so readback_async true and false can be compared run against run.
static void rb_log_crc(Texture* t, const TexLayout& L, uint32_t l0, uint32_t nl, uint32_t y0, uint32_t ny) {
    for (uint32_t l = l0; l < l0 + nl; ++l) {
        const tegra::LevelLayout& lv = L.layout.levels[l];
        std::vector<uint8_t> lin(static_cast<size_t>(lv.linear_slice_size), 0);
        uint32_t crc = 0;
        for (uint32_t y = y0; y < y0 + ny; ++y) {
            tegra::untile_level(L.desc, lv, t->mem + y * L.layout.tiled_layer_stride + lv.tiled_offset, lin.data());
            crc = crc32_update(crc, lin.data(), lin.size());
        }
        NXLOG_I(kTag, "readback: texture %u level %u crc32 %08x", t->id, l, crc);
    }
}

// One line for each of the first 20 textures read back: what is read, from which thread, and what was done.
static void rb_note(Texture* t, const TexGpu& g, const TexLayout& L, uint32_t l0, uint32_t nl, uint32_t y0, uint32_t ny, const char* what) {
    static std::mutex mu;
    static std::set<uint32_t> seen;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (seen.size() >= 20 || !seen.insert(t->id).second) return;
    }
    const char* thr = nxrt::log_thread_name();
    NXLOG_I(kTag, "readback: texture %u (%s %ux%ux%u, %u levels, %u layers), thread %s: level %u+%u, layer %u+%u: %s", t->id,
            g.fmt ? g.fmt->name : "?", g.base.width, g.base.height, g.base.depth, L.levels, L.layers, (thr && *thr) ? thr : "?", l0, nl, y0, ny,
            what);
}

// [nvn] readback_log: one line per read (the first 200, then at most one a second) with the milliseconds of each stage, and a summary
// every 10 s. Stages: queue (the render thread has recorded the copy), record (the render thread's own time in it), grace (the wait for the
// render thread to hand the serial to the GPU), wait (wait_serial: the forced flush and the GPU), invalidate, tile. forced = the grace ran
// out and wait_serial had to flush an extra submission.
struct RbTimes {
    uint64_t queue = 0, record = 0, grace = 0, wait = 0, invalidate = 0, tile = 0, total = 0;
    bool forced = false, async = true, skipped = false, deferred = false;
};
static void rb_report(const Texture* t, const TexGpu& g, const RbTimes& r) {
    using clock = std::chrono::steady_clock;
    static std::mutex mu;
    static uint64_t lines = 0, n = 0, sum = 0, max = 0, over16 = 0, forced = 0;
    static clock::time_point last_line, window = clock::now();
    const double ms = 1e-6;
    std::lock_guard<std::mutex> lk(mu);
    ++n; sum += r.total; max = std::max(max, r.total); if (r.total > 16000000ull) ++over16; if (r.forced) ++forced;
    const auto now = clock::now();
    if (lines < 200 || now - last_line >= std::chrono::seconds(1)) {
        ++lines;
        last_line = now;
        const char* thr = nxrt::log_thread_name();
        NXLOG_I(kTag, "readback_log: texture %u (%s %ux%u), thread %s, %s%s%s: queue %.2f record %.2f grace %.2f wait %.2f invalidate %.2f tile %.2f total %.2f ms%s",
                t->id, g.fmt ? g.fmt->name : "?", g.base.width, g.base.height, (thr && *thr) ? thr : "?", r.async ? "async" : "sync",
                r.skipped ? " skipped" : "", r.deferred ? " deferred" : "", r.queue * ms, r.record * ms, r.grace * ms, r.wait * ms, r.invalidate * ms, r.tile * ms, r.total * ms,
                r.forced ? " forced-flush" : "");
    }
    if (now - window >= std::chrono::seconds(10)) {
        NXLOG_I(kTag, "readback_log: %llu reads in 10 s: mean %.2f ms, max %.2f ms, %llu over 16 ms, %llu with a forced flush",
                static_cast<unsigned long long>(n), n ? (sum / static_cast<double>(n)) * ms : 0.0, max * ms, static_cast<unsigned long long>(over16),
                static_cast<unsigned long long>(forced));
        n = sum = max = over16 = forced = 0;
        window = now;
    }
}

// The render thread's half of an async readback (rb.record), in queue order: after every submission queued before it, so the write
// generations it compares are those the image has once those ran. Nothing is waited for here.
struct RbJob {
    uint64_t record_ns = 0;     // [nvn] readback_log: time the render thread spent recording it
    bool skipped = false;       // every level and layer asked for is in the storage already: nothing recorded
    bool copied = false;        // the copy is recorded into rb; false with skipped false: nothing to read (no buffer, image changed)
};
static void rb_record(Frame& f, Texture* t, TexGpu* g, const TexLayout& L, uint32_t l0, uint32_t nl, uint32_t y0, uint32_t ny, RbJob& job,
               gfx::Buffer* private_buffer = nullptr) {
    NXFT("rb.record");
    const auto rec0 = rb_config().log ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    struct RecTimer {
        RbJob& j; std::chrono::steady_clock::time_point t0; bool on;
        ~RecTimer() { if (on) j.record_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()); }
    } rec_timer{job, rec0, rb_config().log};
    if (t->gpu != g || g->failed || g->layout == VK_IMAGE_LAYOUT_UNDEFINED || g->scale != 1.0f || l0 + nl > g->levels || y0 + ny > g->layers)
        return;   // the image went or changed after the caller looked
    if (g->rb_gen.size() != g->levels) {
        g->rb_gen.assign(g->levels, 0);
        g->rb_have.assign(g->levels, std::vector<uint8_t>(g->layers, 0));
    }
    auto gen = [g](uint32_t l) { return g->wgen[std::min(l, kGenLevels - 1)]; };
    bool have = private_buffer == nullptr;   // a private deferred buffer never borrows the normal buffer's deduplication state
    for (uint32_t l = l0; l < l0 + nl && have; ++l) {
        if (g->rb_gen[l] != gen(l)) have = false;
        for (uint32_t y = y0; y < y0 + ny && have; ++y) have = g->rb_have[l][y] != 0;
    }
    if (have) {
        job.skipped = true;
        return;
    }
    uint64_t need = 0;
    for (uint32_t l = l0; l < l0 + nl; ++l) {
        const tegra::LevelLayout& lv = L.layout.levels[l];
        need = std::max<uint64_t>(need, lv.linear_offset + static_cast<uint64_t>(y0 + ny) * lv.linear_slice_size);
    }
    gfx::Buffer& destination = private_buffer ? *private_buffer : g->rb;
    if (destination.size < need) {
        gfx::Buffer nb;
        if (device().create_buffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &nb, "nvn texture readback") != VK_SUCCESS)
            return;
        if (destination) {
            gfx::Buffer old = destination;
            defer_destroy([old]() mutable { device().destroy_buffer(old); });
        }
        destination = nb;
    }
    std::vector<uint64_t> before(g->levels);
    for (uint32_t l = 0; l < g->levels; ++l) before[l] = gen(l);
    const VkImageLayout prev = g->layout;
    move_layout(f, *g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t l = l0; l < l0 + nl; ++l) {
        const tegra::LevelLayout& lv = L.layout.levels[l];
        VkBufferImageCopy r = {};
        r.bufferOffset = lv.linear_offset + static_cast<uint64_t>(y0) * lv.linear_slice_size;   // where a whole-image copy puts it
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, y0, ny};
        r.imageExtent = {lv.width, lv.height, lv.depth};
        regions.push_back(r);
    }
    vkCmdCopyImageToBuffer(f.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.buffer, static_cast<uint32_t>(regions.size()),
                           regions.data());
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(f.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    move_layout(f, *g, prev);
    // The generations after the restore: moving back to a writable layout bumps every level, though nothing wrote. The levels asked for
    // take the new numbers; a level the storage was current for before stays current (only the restore moved its number).
    for (uint32_t l = 0; l < g->levels; ++l) {
        const bool current = g->rb_gen[l] == before[l];
        if (private_buffer) continue;   // normal readback still owns its buffer and cached generations
        if (l >= l0 && l < l0 + nl) {
            if (!current) std::fill(g->rb_have[l].begin(), g->rb_have[l].end(), uint8_t{0});
            for (uint32_t y = y0; y < y0 + ny; ++y) g->rb_have[l][y] = 1;
            g->rb_gen[l] = gen(l);
        } else if (current) {
            g->rb_gen[l] = gen(l);
        }
    }
    job.copied = true;
}

// Tiles the levels and layers asked for from the readback buffer into the game's storage (the GPU is done with the copy).
static void rb_tile(Texture* t, const gfx::Buffer& buffer, const TexLayout& L, uint32_t l0, uint32_t nl, uint32_t y0, uint32_t ny) {
    NXFT("rb.tile");
    const uint8_t* src = static_cast<const uint8_t*>(buffer.mapped);
    for (uint32_t l = l0; l < l0 + nl; ++l) {
        const tegra::LevelLayout& lv = L.layout.levels[l];
        for (uint32_t y = y0; y < y0 + ny; ++y)
            tegra::tile_level(L.desc, lv, src + lv.linear_offset + static_cast<uint64_t>(y) * lv.linear_slice_size,
                              t->mem + y * L.layout.tiled_layer_stride + lv.tiled_offset);
    }
}

// Candidate completion-only landing (rb_mu held): pending GPU work and its private buffer remain owned, never waited on or reused.
static ReadbackPoll rb_land(Texture* t, TexGpu* g, const TexLayout& L, const rbdiag::Stamp& request) {
    const auto pending = g->rb_pending;
    if (!pending) return ReadbackPoll::Complete;
    ReadbackProgress progress;
    const ReadbackPoll polled = poll_readback_serial(pending->serial, &progress);
    if (rb_config().log) {
        static std::atomic<unsigned> lines{0};
        if (lines.fetch_add(1, std::memory_order_relaxed) < 200)
            NXLOG_I(kTag, "readback_poll: texture %u serial %llu state %s, reserved %llu flushed %llu submitted %llu timeline %llu", t->id,
                    static_cast<unsigned long long>(pending->serial), polled == ReadbackPoll::Pending ? "pending" : polled == ReadbackPoll::Complete ? "complete" : "failed",
                    static_cast<unsigned long long>(progress.reserved), static_cast<unsigned long long>(progress.flushed),
                    static_cast<unsigned long long>(progress.submitted), static_cast<unsigned long long>(progress.completed));
    }
    if (polled != ReadbackPoll::Complete) return polled;   // retain ownership and all metadata while unfinished
    const auto& r = pending->stamp.range;
    if (!pending->copied.load(std::memory_order_acquire) || !pending->buffer ||
        !(pending->stamp == request) || t->prep_gen.load(std::memory_order_acquire) != request.cpu_generation ||
        g->rb_sync_epoch.load(std::memory_order_acquire) != request.sync_epoch ||
        t->cpu_dirty.load() || t->destroyed.load() || !r.fits(L.levels, L.layers)) {
        g->rb_pending.reset();
        g->rb_previous_valid = false;
        return ReadbackPoll::Failed;
    }
    device().invalidate_buffer(pending->buffer);
    rb_tile(t, pending->buffer, L, r.level, r.levels, r.layer, r.layers);
    // Normal-buffer deduplication must not claim that these older landed bytes are current GPU data.
    g->rb_gen.clear();
    g->rb_have.clear();
    g->rb_previous = request;
    g->rb_previous_valid = true;
    g->rb_previous_at = pending->requested;   // age the data from its request, not from when a late reader finally lands it
    g->rb_pending.reset();
    return ReadbackPoll::Complete;
}

// readback_async = false (and calls on the render thread): the old path. Every level, earlier submissions drained first (finish), the
// copy waited for on the render thread (run_frame_sync), the whole storage tiled again.
static bool download_all_levels_sync(Texture* t, TexGpu* g, const TexLayout& L) {
    const uint64_t size = L.layout.linear_size;
    finish();   // earlier submissions must be done before the copy reads the image
    gfx::Buffer rb;
    run_frame_sync([&](Frame& f) {
        if (device().create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &rb, "nvn texture readback") != VK_SUCCESS)
            return;
        g->read_back = true;
        const VkImageLayout prev = g->layout;
        move_layout(f, *g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        std::vector<VkBufferImageCopy> regions;
        for (uint32_t l = 0; l < L.levels; ++l) {
            const tegra::LevelLayout& lv = L.layout.levels[l];
            VkBufferImageCopy r = {};
            r.bufferOffset = lv.linear_offset;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, g->layers};
            r.imageExtent = {lv.width, lv.height, lv.depth};
            regions.push_back(r);
        }
        vkCmdCopyImageToBuffer(f.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, static_cast<uint32_t>(regions.size()),
                               regions.data());
        move_layout(f, *g, prev);
    });
    if (!rb) return false;
    device().invalidate_buffer(rb);
    std::vector<uint8_t> lin(static_cast<size_t>(size));
    std::memcpy(lin.data(), rb.mapped, static_cast<size_t>(size));
    device().destroy_buffer(rb);
    tegra::tile(L.desc, L.layout, lin.data(), t->mem);
    g->rb_sync_epoch.fetch_add(1, std::memory_order_release);
    return true;
}
}  // namespace tex

ReadbackStats readback_stats() {
    ReadbackStats s;
    s.copies = g_rb_copies.load();
    s.skipped = g_rb_skipped.load();
    s.calls = g_rb_calls.load();
    return s;
}

// Copies levels of a render target back and tiles them into the game's storage (block-linear). Only when the GPU image holds the newest
// data: it exists, it was written by the GPU (or uploaded) and the CPU did not write since. Pitch-linear, transcoded, depth and
// format-fallback textures are not read back.
bool download_texture(Texture* t, uint32_t level_first, uint32_t level_count, uint32_t layer_first, uint32_t layer_count) {
    if (!t || !active() || !t->mem || !t->fmt || !t->block_linear) return false;
    TexGpu* g = t->gpu;
    if (!g || g->failed || g->depth || g->transcoded || g->sparse || g->format != t->fmt->vk) return false;
    if (g->scale != 1.0f) {
        static int logged = 0;
        if (logged++ < 10) NXLOG_W(kTag, "texture %u: a scaled render target is not read back into game memory", t->id);
        return false;
    }
    if (t->cpu_dirty.load()) {
        // CPU notifications also bump prep_gen, so writes followed by an upload between two reads invalidate old results too.
        std::lock_guard<std::mutex> lk(g->rb_mu);
        g->rb_pending.reset();   // the callback / GPU-retirement deleter retains any in-flight private buffer
        g->rb_previous_valid = false;
        return false;
    }
    if (!(g->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || g->layout == VK_IMAGE_LAYOUT_UNDEFINED) return false;
    const TexLayout L = compute_texture_layout(t->b);
    if (!L.valid || !L.block_linear) return false;
    if (level_first >= L.levels || layer_first >= L.layers) return false;
    const uint32_t l0 = level_first, nl = std::min(level_count, L.levels - level_first);
    const uint32_t y0 = layer_first, ny = std::min(layer_count, L.layers - layer_first);
    g_rb_calls.fetch_add(1);
    const RbConfig& cfg = rb_config();
    using rb_clock = std::chrono::steady_clock;
    const auto rb_ns = [](rb_clock::time_point a, rb_clock::time_point b) { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()); };
    RbTimes times;
    const auto t_start = cfg.log ? rb_clock::now() : rb_clock::time_point();

    if (!cfg.async || on_render_thread()) {
        // Do not acquire rb_mu on the render thread: a guest holding it may be waiting for an earlier queued read job.
        // This path uses a local buffer; its epoch invalidates older deferred results without touching their ownership.
        if (!download_all_levels_sync(t, g, L)) return false;
        if (cfg.log) { times.async = false; times.total = rb_ns(t_start, rb_clock::now()); rb_report(t, *g, times); }
        g_rb_copies.fetch_add(1);
        rb_note(t, *g, L, l0, nl, y0, ny, "copied every level (GPU drained)");
        if (cfg.check) rb_log_crc(t, L, l0, nl, y0, ny);
        return true;
    }

    std::lock_guard<std::mutex> lk(g->rb_mu);

    // [nvn] readback_defer (#276): a small texture the game reads again and again (its shape named by the game, game::ReadbackShape) gets
    // the data of its previous read, and this read's copy is queued without waiting; the next read lands it. The reading thread (often
    // the game's main thread) then never waits for the GPU: on a capped frame rate the old forced flush and GPU wait (about 13 ms, 21 ms
    // in all on a Legion Go S at 40 fps) made the frame miss its slot. A first read, or one after a pause, waits as before.
    const uint32_t range[4] = {l0, nl, y0, ny};
    const rbdiag::Stamp request{t->id, t->prep_gen.load(std::memory_order_acquire), reinterpret_cast<uintptr_t>(t->mem),
                               t->storage_size, {l0, nl, y0, ny}, g->rb_sync_epoch.load(std::memory_order_acquire)};
    const auto now = rb_clock::now();
    const bool repeat = g->rb_last.time_since_epoch().count() != 0 && now - g->rb_last < std::chrono::seconds(2) &&
                        std::equal(range, range + 4, g->rb_last_range);
    g->rb_last = now;
    std::copy(range, range + 4, g->rb_last_range);
    // Explicit texture-id selection is required in addition to the exact observed format/extent. This does not prove stale-data safety.
    constexpr uint64_t kDeferMaxBytes = 64 * 1024;
    const game::ReadbackShape* shape = cfg.shape;
    const bool exact_texture = shape && g->format == static_cast<VkFormat>(shape->vk_format) && g->base.width == shape->width &&
                               g->base.height == shape->height && g->base.depth == shape->depth && L.levels == shape->levels &&
                               L.layers == shape->layers && L.layout.linear_size <= kDeferMaxBytes;
    const bool may_defer = rbdiag::eligible(cfg.defer, cfg.texture, exact_texture, repeat,
                                           g->rb_previous_valid, g->rb_previous, request);
    if (g->rb_pending && !may_defer) {
        g->rb_pending.reset();   // incompatible / CPU-written / expired: retire, never reuse, its private buffer
        g->rb_previous_valid = false;
    }
    if (may_defer && g->rb_pending) {
        const double age = std::chrono::duration<double>(now - g->rb_pending->requested).count();
        if (age >= 2.0) {
            g->rb_pending.reset();   // bounded stale-data age: the fresh synchronous path uses the separate normal buffer
            g->rb_previous_valid = false;
        } else {
            const ReadbackPoll landed = rb_land(t, g, L, request);
            if (landed == ReadbackPoll::Failed) return false;
            if (landed == ReadbackPoll::Pending) {
                if (std::chrono::duration<double>(now - g->rb_previous_at).count() < 2.0 &&
                    t->prep_gen.load(std::memory_order_acquire) == request.cpu_generation &&
                    g->rb_sync_epoch.load(std::memory_order_acquire) == request.sync_epoch && !t->cpu_dirty.load()) {
                    if (cfg.log) { times.deferred = true; times.total = rb_ns(t_start, rb_clock::now()); rb_report(t, *g, times); }
                    return true;   // matching cached range only; do not queue another copy or touch the pending buffer
                }
                g->rb_pending.reset();
                g->rb_previous_valid = false;
            }
        }
    }
    if (may_defer && g->rb_previous_valid && !g->rb_pending &&
        std::chrono::duration<double>(now - g->rb_previous_at).count() < 2.0 &&
        t->prep_gen.load(std::memory_order_acquire) == request.cpu_generation &&
        g->rb_sync_epoch.load(std::memory_order_acquire) == request.sync_epoch && !t->cpu_dirty.load()) {
        auto pending = std::make_shared<DeferredReadback>();
        pending->stamp = request;
        pending->requested = now;
        pending->serial = queue_readback_post([keep = Ref<Texture>::share(t), g, L, l0, nl, y0, ny, pending](Frame& f) {
            RbJob job;
            // A notification between request and recording invalidates the job without touching guest storage.
            if (keep->destroyed.load() || keep->prep_gen.load(std::memory_order_acquire) != pending->stamp.cpu_generation) return;
            rb_record(f, keep.get(), g, L, l0, nl, y0, ny, job, &pending->buffer);
            pending->copied.store(job.copied, std::memory_order_release);
        });
        g->rb_pending = pending;
        g_rb_copies.fetch_add(1);
        if (cfg.log) { times.deferred = true; times.total = rb_ns(t_start, rb_clock::now()); rb_report(t, *g, times); }
        rb_note(t, *g, L, l0, nl, y0, ny, "deferred (the previous read's data; this copy lands on the next read)");
        if (cfg.check) rb_log_crc(t, L, l0, nl, y0, ny);
        return true;
    }

    RbJob job;
    uint64_t serial = 0;
    {
        NXFT("rb.queue");   // until the render thread has recorded it (the queue ahead of it is replayed first)
        serial = queue_record([&](Frame& f) { rb_record(f, t, g, L, l0, nl, y0, ny, job); });
    }
    const auto t_queued = cfg.log ? rb_clock::now() : rb_clock::time_point();
    if (cfg.log) { times.queue = rb_ns(t_start, t_queued); times.record = job.record_ns; }
    if (job.skipped) {
        g->rb_previous = request;
        g->rb_previous_valid = true;
        g->rb_previous_at = rb_clock::now();
        if (cfg.log) { times.skipped = true; times.total = rb_ns(t_start, rb_clock::now()); rb_report(t, *g, times); }
        g_rb_skipped.fetch_add(1);
        rb_note(t, *g, L, l0, nl, y0, ny, "skipped (not written since the last readback)");
        if (cfg.check) rb_log_crc(t, L, l0, nl, y0, ny);
        return true;   // the storage holds that data already
    }
    if (!job.copied) {
        g->rb_previous_valid = false;
        return false;   // no buffer, or the image changed: do not treat an old cached result as a new successful read
    }
    g_rb_copies.fetch_add(1);
    {
        NXFT("rb.wait");
        WaitTimes wt;
        wait_serial_grace(serial, rb_grace_ns(), cfg.log ? &wt : nullptr);
        if (cfg.log) { times.grace = wt.grace_ns; times.wait = wt.wait_ns; times.forced = wt.forced_flush; }
    }
    if (device().device_lost()) return false;   // the copy never ran: the storage stays as it is
    const auto t_inv0 = cfg.log ? rb_clock::now() : rb_clock::time_point();
    device().invalidate_buffer(g->rb);
    const auto t_inv1 = cfg.log ? rb_clock::now() : rb_clock::time_point();
    if (cfg.log) times.invalidate = rb_ns(t_inv0, t_inv1);
    rb_tile(t, g->rb, L, l0, nl, y0, ny);
    g->rb_previous = request;
    g->rb_previous_valid = t->prep_gen.load(std::memory_order_acquire) == request.cpu_generation;
    g->rb_previous_at = rb_clock::now();
    if (cfg.log) { const auto t_end = rb_clock::now(); times.tile = rb_ns(t_inv1, t_end); times.total = rb_ns(t_start, t_end); rb_report(t, *g, times); }
    rb_note(t, *g, L, l0, nl, y0, ny, "copied");
    if (cfg.check) rb_log_crc(t, L, l0, nl, y0, ny);
    return true;
}

// Render thread. The image's flag, also through another texture object over the same storage; creates nothing.
bool tex_was_read_back(Texture* t) {
    if (!t) return false;
    const TexGpu* g = t->gpu;
    if (!g)
        if (Ref<Texture> alias = texture_registry_find_alias(t)) g = alias->gpu;
    return g && g->read_back;
}

}  // namespace gpu
}  // namespace nxrt::nvn::be
