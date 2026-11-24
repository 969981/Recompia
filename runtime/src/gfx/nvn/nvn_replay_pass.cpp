// SPDX-License-Identifier: GPL-3.0-or-later
// Render passes of the replay: dynamic rendering begun lazily on the current targets, ended with or without a barrier (tracked barriers).
#include "nvn_replay_internal.h"
#include "nxrt/diag.h"
#include "nxrt/ab_switch.h"
#include "nxrt/platform_facts.h"
#include "nxrt/host_device.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
Pass P;

// ---- [prof] pass_stats ------------------------------------------------------------------------------------------------------
int pass_stats_level() {
    static const int v = static_cast<int>(nxrt::config_get_int("prof", "pass_stats", 0));
    return v;
}
namespace {
struct PassStats {
    uint64_t passes = 0, empty = 0, fbl = 0, ro = 0, layered = 0, draws = 0;
    uint64_t color_att = 0, color_load = 0, depth_att = 0, depth_load = 0;
    uint64_t px_color = 0, px_depth = 0, px_load = 0;            // attachment pixels per pass begin (a full tile load or store each on GMEM)
    uint64_t clears_first = 0, clears_mid = 0, dclears = 0, discard_c = 0, discard_d = 0;
    uint64_t first_present = 0, listed_frame = 0;
};
PassStats g_ps;
}  // namespace

void pass_stats_discard(bool depth) { (depth ? g_ps.discard_d : g_ps.discard_c)++; }

static void pass_stats_end() {
    PassStats& s = g_ps;
    const uint64_t now = counters().presents.load(std::memory_order_relaxed);
    if (!s.first_present) s.first_present = now ? now : 1;
    s.passes++;
    s.draws += P.draws;
    if (P.draws == 0) s.empty++;
    if (P.fbl) s.fbl++;
    if (P.depth_ro) s.ro++;
    if (P.layers > 1) s.layered++;
    const uint64_t px = static_cast<uint64_t>(P.extent.width) * P.extent.height * P.layers;
    for (uint32_t i = 0; i < P.ncolor && i < 8; ++i) {
        if (!P.color_fmt[i]) continue;
        s.color_att++;
        s.px_color += px;
        if ((P.loaded >> i) & 1) { s.color_load++; s.px_load += px; }
    }
    if (P.depth_fmt) {
        s.depth_att++;
        s.px_depth += px;
        if (P.loaded_d) { s.depth_load++; s.px_load += px; }
    }
    s.clears_first += P.clears_first;
    s.clears_mid += P.clears_mid;
    s.dclears += P.dclears;
    if (pass_stats_level() >= 2 && s.listed_frame == now) {
        NXLOG_I("nvn.passes", "frame %llu pass: %ux%u x%u, colour %u (fmt %d, loaded mask %x), depth fmt %d%s%s, draws %u, clears before draws %u / after %u (depth %u)%s",
                static_cast<unsigned long long>(now), P.extent.width, P.extent.height, P.layers, P.ncolor, static_cast<int>(P.color_fmt[0]), P.loaded,
                static_cast<int>(P.depth_fmt), P.loaded_d ? " loaded" : P.depth_fmt ? " cleared/fresh" : "", P.depth_ro ? " read-only" : "", P.draws, P.clears_first,
                P.clears_mid, P.dclears, P.fbl ? ", FEEDBACK LOOP" : "");
    }
    const uint64_t frames = now > s.first_present ? now - s.first_present : 0;
    if (frames >= 300) {
        const double f = static_cast<double>(frames);
        NXLOG_I("nvn.passes", "pass stats, %llu frames, per frame: passes %.1f (empty %.1f, feedback loop %.1f, depth read-only %.1f, layered %.1f), draws %.0f | colour attachments %.1f "
                "(LOAD %.1f), depth %.1f (LOAD %.1f) | Mpx per frame: colour %.2f, depth %.2f, loaded %.2f | clears before the first draw %.1f, after %.1f, depth via "
                "ClearAttachments %.1f | discards colour %.1f, depth %.1f (ignored)",
                static_cast<unsigned long long>(frames), s.passes / f, s.empty / f, s.fbl / f, s.ro / f, s.layered / f, s.draws / f, s.color_att / f, s.color_load / f,
                s.depth_att / f, s.depth_load / f, s.px_color / f / 1e6, s.px_depth / f / 1e6, s.px_load / f / 1e6, s.clears_first / f, s.clears_mid / f, s.dclears / f,
                s.discard_c / f, s.discard_d / f);
        s = PassStats();
        s.first_present = now;
        s.listed_frame = now + 150;   // list the passes of one frame in the middle of the next window (level 2)
    }
}

static VkImageLayout color_layout(uint8_t fbl, uint32_t slot) {
    return (fbl >> slot) & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
PassBarriers g_bar;
void PassBarriers::begin_submit(bool tracked_on) {
    tracked = tracked_on;
    set_narrow_barriers(tracked);
    pending.clear();
    pass_unsafe = false;
}

bool clear_carry_on();
static void carry_folded_clears();

// ---- passes ----------------------------------------------------------------------------------------------------------------
// Ends transform feedback in the open pass, remembering the bytes written in the counter buffer (a pass break must not restart the buffer).
void xfb_end_in_pass() {
    if (!g_xfb.began) return;
    VkBuffer cbuf = g_xfb.counter.buffer;
    const VkDeviceSize off = static_cast<VkDeviceSize>(g_xfb.cur) * kXfbSlotBytes;
    vkCmdEndTransformFeedbackEXT(F->cb, 0, 1, &cbuf, &off);
    g_xfb.began = false;
    g_xfb.have_counter = true;
    g_xfb.slot_written[g_xfb.cur] = true;
}

// Two texture objects over one image (as shares_image further down, which this code cannot see yet).
static bool same_image(Texture* a, Texture* b) { return a == b || (a && b && a->gpu && a->gpu == b->gpu); }

// to_pass: the caller only starts another pass or changes image layouts next (ensure_pass, prepare_textures); with tracked barriers the pass
// then ends without a barrier when it only wrote its attachments. Any other caller gets a full barrier, also for passes ended earlier
// without one.
void end_pass(bool to_pass) {
    if (!P.open) {
        if (!to_pass && !g_bar.pending.empty()) {
            memory_barrier(F->cb);
            g_bar.pending.clear();
        }
        return;
    }
    // Only a pass that was open can have changed what a texture binding resolves to. The bump used to happen before this check, so
    // the many end_pass calls with nothing open (230 of 406 epoch bumps in a world frame) threw away the descriptor-set cache of
    // every draw that followed: set_reuse hit 48%, and each miss rebuilds a set inside the replay.
    rp_bump(ft::kCntEpEndPass);
    if (P.deferred && P.folded) {                        // the folded clears still have to happen
        if (clear_carry_on()) carry_folded_clears();     // ... at the next pass on these attachments, or before anything else uses them
        else pass_flush();
    }
    const bool never_begun = P.deferred;                 // no draw and no clear: LOAD + STORE of untouched attachments, nothing to record
    if (!never_begun) {
        xfb_end_in_pass();
        vkCmdEndRendering(F->cb);
    }
    P.deferred = false;
    P.open = false;
    if (pass_stats_level() > 0) pass_stats_end();
    if (gpass_on()) {
        char b[112];
        std::snprintf(b, sizeof b, "pass %ux%u c%u:%d d%d%s", P.extent.width, P.extent.height, P.ncolor, static_cast<int>(P.color_fmt[0]), static_cast<int>(P.depth_fmt),
                      P.depth_ro ? " ro" : "");
        gpass_stamp(*F, b, P.draws);
        char vb[128];
        std::snprintf(vb, sizeof vb, "gv:%s", b);
        ft::mark(vb, static_cast<uint32_t>(std::min<uint64_t>(P.verts, 0xffffffffull)));
    }
    if (to_pass && g_bar.tracked && !g_bar.pass_unsafe) {
        auto remember = [](Texture* t, VkImageLayout l) {
            if (!t || !tex_gpu(t)) return;
            for (const PendingWrite& w : g_bar.pending) if (w.tex.get() == t) return;
            g_bar.pending.push_back({Ref<Texture>::share(t), l});
        };
        for (uint32_t i = 0; i < P.ncolor; ++i) remember(P.color[i].tex, color_layout(P.fbl, i));
        if (!P.depth_ro) remember(P.depth.tex, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    } else {
        memory_barrier(F->cb);
        g_bar.pending.clear();
    }
    g_bar.pass_unsafe = false;
    g_cmd.bound_pipeline = VK_NULL_HANDLE;
    g_cmd.dirty = kAllDirty;
    if constexpr (facts::kPhone) edge_guard_after_pass();   // nvn_replay_edge.cpp
}

// A pass is about to use `t` as an attachment in `layout`. An earlier pass that ended without a barrier wrote it, and no layout change
// (which would carry the dependency) happens in between: the two passes need a barrier.
static bool pending_conflict(Texture* t, VkImageLayout layout) {
    if (!t || g_bar.pending.empty()) return false;
    for (const PendingWrite& w : g_bar.pending)
        if (same_image(w.tex.get(), t) && tex_layout(t) == w.layout && layout == w.layout) return true;
    return false;
}

static Target make_target(Texture* t, const TextureViewInfo& v, bool has_view) {
    Target tg;
    tg.tex = t;
    if (has_view) {
        if (v.has_levels) tg.level = static_cast<uint32_t>(v.min_level);
        if (v.has_layers) {
            tg.layer = static_cast<uint32_t>(v.min_layer);
            tg.layers = std::max(1, v.num_layers);
        }
        else if (t && t->layers > 1 && t->b.target != NVN_TEXTURE_TARGET_3D) tg.layers = t->layers;   // a view without layers
        if (v.has_format) {
            const FormatInfo* fi = format_info(v.format);
            if (fi && fi->vk != VK_FORMAT_UNDEFINED) tg.view_format = fi->vk;
        }
    } else if (t && t->layers > 1 && t->b.target != NVN_TEXTURE_TARGET_3D) {
        // no view: an array or cube map target is attached with all its layers (layered rendering: a geometry shader picks the layer with
        // gl_Layer; without it the shaders draw to layer 0)
        tg.layers = t->layers;
    }
    return tg;
}

static bool targets_match() {
    if (P.ncolor != S.ncolor) return false;
    for (uint32_t i = 0; i < S.ncolor; ++i)
        if (!(P.color[i] == make_target(S.color_tex[i].get(), S.color_view[i], S.color_has_view[i]))) return false;
    return P.depth == make_target(S.depth_tex.get(), S.depth_view, S.depth_has_view);
}

// The current render targets get their images (scaled when they qualify, tex_gpu_attachment). With `fix` (no pass open), as Ryujinx does
// when it binds render targets: a target whose image was made unscaled (first used another way) is scaled now, and the targets of one
// pass are brought to one scale (the lowest when one of them cannot be scaled). Returns the scale of the targets.
float attach_targets(bool fix) {
    Texture* ts[9];
    uint32_t n = 0;
    for (uint32_t i = 0; i < S.ncolor && i < 8; ++i) if (S.color_tex[i]) ts[n++] = S.color_tex[i].get();
    if (S.depth_tex) ts[n++] = S.depth_tex.get();
    float lo = 0.0f, hi = 0.0f;
    // [gfx] ui_native: a pass on a window texture brings its other attachments (the 2D's depth buffer) to the window texture's scale
    float ui = 0.0f;
    for (uint32_t i = 0; i < n && ui == 0.0f; ++i) ui = ui_native_scale(*ts[i]);
    if (ui > 0.0f && fix && S.depth_tex) ui_native_note_depth(*S.depth_tex);
    for (uint32_t i = 0; i < n; ++i) {
        if (!tex_gpu_attachment(ts[i])) continue;
        if (fix) {
            const float want = ui > 0.0f ? ui : tex_scale_wanted(ts[i]);
            if (want != tex_scale(ts[i])) tex_rescale(ts[i], *F, want, "used as a render target");
        }
        const float sc = tex_scale(ts[i]);
        lo = lo == 0.0f ? sc : std::min(lo, sc);
        hi = std::max(hi, sc);
    }
    if (lo == 0.0f) return 1.0f;
    if (lo != hi && fix) {
        for (uint32_t i = 0; i < n; ++i)
            if (tex_gpu(ts[i]) && tex_scale(ts[i]) != lo) tex_rescale(ts[i], *F, lo, "a render target of the same pass cannot be scaled");
    }
    return lo;
}

// Crash report section: the attachments of the last pass begun. A driver that faults inside vkCmdBeginRendering (#720: Intel Iris Plus,
// igvk64.dll reading 0x68 at world entry) leaves no other trace of what the pass held. Only plain values copied at the pass's start are
// read (the freeze dump runs this on a live game, where a texture of an ended pass may be freed). Read without a lock: best effort.
static Pass::Note note_of(const Texture* t) {
    if (!t) return {};
    return {t->id, t->fmt ? t->fmt->name : nullptr, t->b.width, t->b.height, t->levels};   // fmt->name: the static format table
}

static std::string pass_report() {
    auto target = [](const Target& tg, const Pass::Note& n, bool no_view, VkFormat f) {
        char b[200];
        if (!n.id) return std::string("none");
        std::snprintf(b, sizeof b, "texture %u (%s %dx%d, %u levels), level %u, layer %u+%u, format %d%s", n.id, n.fmt ? n.fmt : "?", n.w, n.h,
                      n.levels, tg.level, tg.layer, tg.layers, static_cast<int>(f), no_view ? ", NO VIEW (null in the pass)" : "");
        return std::string(b);
    };
    char b[200];
    std::snprintf(b, sizeof b, "last render pass begun (%s): %u colour slot(s), %ux%u, %u layer(s), %u sample(s), scale %.3f, depth %s\n", P.open ? "open" : "ended or not begun",
                  P.ncolor, P.extent.width, P.extent.height, P.layers, P.samples, P.scale, P.depth_ro ? "read-only" : "written");
    std::string out = b;
    for (uint32_t i = 0; i < P.ncolor && i < 8; ++i)
        out += "  colour " + std::to_string(i) + ": " + target(P.color[i], P.note[i], (P.no_view >> i) & 1, P.color_fmt[i]) + "\n";
    out += "  depth: " + target(P.depth, P.note[8], P.depth_no_view, P.depth_fmt) + (P.has_stencil ? " (with stencil)" : "") + "\n";
    return out;
}

// [nvn] ro_store_none (phones, default on; A/B name "ro_store_none"): a pass whose depth/stencil attachment is read-only ends with
// STORE_OP_NONE instead of STORE: the pass cannot write it (DEPTH_STENCIL_READ_ONLY_OPTIMAL), and NONE keeps its contents (core in Vulkan
// 1.3, which the runtime requires), so a tiler skips writing the whole depth buffer back at the end of every such pass. The outdoor
// lighting section has about 25 of them per frame (half-size D24S8 targets on a Tensor G2 (Mali-G710)). Same picture by construction.
template <bool kOn = facts::kPhone>
bool ro_store_none() {
    static const bool on = config_get_bool("nvn", "ro_store_none", true);
    return diag::ab("ro_store_none", on);
}

// ---- [nvn] clear_fold ----------------------------------------------------------------------------------------------------
// A ClearColor / ClearDepthStencil over the whole render area before the pass's first draw is the pass's LOAD_OP_CLEAR instead of a
// vkCmdClearAttachments after a LOAD_OP_LOAD. Same result by the Vulkan rules (the render area is the clear rectangle, one layer), but a
// tiler skips the load of that attachment, and Turnip keeps LRZ: a depth vkCmdClearAttachments disables it until a LOAD_OP_CLEAR.
static bool g_for_clear = false;
bool clear_fold_on() {
    static const bool v = nxrt::config_get_bool("nvn", "clear_fold", host_is_phone());   // phones: Turnip keeps LRZ, SYSMEM skips empty passes
    return v;
}
void pass_flush() {
    if (!P.deferred) return;
    P.deferred = false;
    VkRenderingInfo ri = P.ri;
    ri.pColorAttachments = P.att_c;
    if (ri.pDepthAttachment) ri.pDepthAttachment = &P.att_d;
    if (ri.pStencilAttachment) ri.pStencilAttachment = &P.att_s;
    vkCmdBeginRendering(F->cb, &ri);
}
static bool fold_rect_ok(const VkClearRect& r) {
    return P.deferred && P.layers == 1 && r.baseArrayLayer == 0 && r.layerCount == 1 && r.rect.offset.x == 0 && r.rect.offset.y == 0 &&
           r.rect.extent.width == P.extent.width && r.rect.extent.height == P.extent.height;
}
bool pass_fold_clear_color(uint32_t slot, const VkClearColorValue& v, const VkClearRect& r) {
    if (!fold_rect_ok(r) || slot >= P.ncolor || slot >= 8 || !P.att_c[slot].imageView) return false;
    P.att_c[slot].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    P.att_c[slot].clearValue.color = v;
    P.loaded &= static_cast<uint8_t>(~(1u << slot));
    P.folded = true;
    return true;
}
bool pass_fold_clear_ds(VkImageAspectFlags aspect, const VkClearDepthStencilValue& v, const VkClearRect& r) {
    if (!fold_rect_ok(r) || P.depth_ro || !P.att_d.imageView) return false;
    if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
        P.att_d.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        P.att_d.clearValue.depthStencil = v;
        P.loaded_d = false;
    }
    if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        P.att_s.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        P.att_s.clearValue.depthStencil = v;
    }
    P.folded = true;
    return true;
}
// ---- [nvn] clear_carry -------------------------------------------------------------------------------------------------------
// A pass that holds only folded clears (no draw) and ends is not recorded. Its clears wait: the next pass that attaches the same image with
// the same view over the same area begins it with LOAD_OP_CLEAR; anything else first records the clear as a pass of its own, exactly as
// clear_fold would have: a layout change or a remake of the image (g_tex_touch_hook from move_layout, tex_rescale, the feedback-loop
// remake), another way of attaching it, the end of the submission or of a split piece. The image keeps its attachment layout meanwhile, so
// no command can read or write it in between (every other use changes its layout first). Removes the clear-only passes (on an Adreno 750 each
// costs 0.08 ms and more: the 2048x2048 shadow clear pass 0.40 ms).
struct PendingClear {
    Ref<Texture> tex;
    TexGpu* g = nullptr;
    Target tg;
    VkExtent2D extent{};
    bool depth = false, has_d = false, has_s = false;
    VkRenderingAttachmentInfo att{}, att_s{};
};
static std::vector<PendingClear> g_pc;
static uint64_t g_pc_taken = 0, g_pc_recorded = 0, g_pc_carried = 0;   // [prof] pass_stats: carried clears taken by a pass's begin / recorded alone
static void touch_hook(Frame& f, TexGpu* g);
static void pc_sync_hook() { g_tex_touch_hook = g_pc.empty() ? nullptr : &touch_hook; }
bool clear_carry_on() {
    static const bool v = clear_fold_on() && nxrt::config_get_bool("nvn", "clear_carry", host_is_phone());
    return v;
}
// the carried clear as a pass of its own, holding only that attachment (the others of its pass may have changed layout since)
static void record_clear(VkCommandBuffer cb, const PendingClear& c) {
    ++g_pc_recorded;
    if (P.open && !P.deferred) {   // never expected: a pass of its own cannot begin inside a begun pass (the callers come between passes)
        static bool told = false;
        if (!told) { told = true; NXLOG_E("nvn.passes", "clear carry: a carried clear was recorded while a pass was open (please report)"); }
    }
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, c.extent};
    ri.layerCount = 1;
    VkRenderingAttachmentInfo a = c.att, st = c.att_s;
    if (!c.depth) {
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &a;
    } else {
        if (c.has_d) ri.pDepthAttachment = &a;
        if (c.has_s) ri.pStencilAttachment = &st;
    }
    vkCmdBeginRendering(cb, &ri);
    vkCmdEndRendering(cb);
    memory_barrier(cb);
}
static void touch_hook(Frame& f, TexGpu* g) {
    for (size_t i = 0; i < g_pc.size();) {
        if (g_pc[i].g == g) {
            record_clear(f.cb, g_pc[i]);
            g_pc.erase(g_pc.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    pc_sync_hook();
}
void pending_clears_flush_all() {
    if (pass_stats_level() > 0) {   // [prof] pass_stats: the carry's work per frame
        static uint64_t since = 0;
        const uint64_t now = counters().presents.load(std::memory_order_relaxed);
        if (!since) since = now ? now : 1;
        if (now >= since + 300) {
            const double f = static_cast<double>(now - since);
            NXLOG_I("nvn.passes", "clear carry, per frame: carried %.1f, taken by the next pass %.1f, recorded alone %.1f", g_pc_carried / f, g_pc_taken / f,
                    g_pc_recorded / f);
            g_pc_carried = g_pc_taken = g_pc_recorded = 0;
            since = now;
        }
    }
    if (g_pc.empty()) return;
    for (const PendingClear& c : g_pc) record_clear(F->cb, c);
    g_pc.clear();
    pc_sync_hook();
}
static void carry_folded_clears() {
    for (uint32_t i = 0; i < P.ncolor && i < 8; ++i) {
        if (P.att_c[i].loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR || !P.att_c[i].imageView || !P.color[i].tex) continue;
        PendingClear c;
        c.tex = Ref<Texture>::share(P.color[i].tex);
        c.g = tex_gpu(P.color[i].tex);
        c.tg = P.color[i];
        c.extent = P.extent;
        c.att = P.att_c[i];
        touch_hook(*F, c.g);                              // an older carried clear of the same image goes first
        g_pc.push_back(c);
        ++g_pc_carried;
    }
    const bool dclear = P.ri.pDepthAttachment && P.att_d.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
    const bool sclear = P.ri.pStencilAttachment && P.att_s.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
    if ((dclear || sclear) && P.depth.tex) {
        PendingClear c;
        c.tex = Ref<Texture>::share(P.depth.tex);
        c.g = tex_gpu(P.depth.tex);
        c.tg = P.depth;
        c.extent = P.extent;
        c.depth = true;
        c.has_d = P.ri.pDepthAttachment != nullptr;
        c.has_s = P.ri.pStencilAttachment != nullptr;
        c.att = P.att_d;
        c.att_s = P.att_s;
        touch_hook(*F, c.g);
        g_pc.push_back(c);
        ++g_pc_carried;
    }
    pc_sync_hook();                                       // P.deferred stays set: end_pass records nothing for this pass
}
// At a pass's begin: a carried clear of one of its attachments becomes its LOAD_OP_CLEAR when the view and the area match, else it is
// recorded first. Carried clears of images this pass does not attach stay carried.
static void take_carried(VkRenderingAttachmentInfo* colors, VkRenderingAttachmentInfo& depth, VkRenderingAttachmentInfo& stencil, bool have_depth,
                         VkExtent2D extent) {
    if (g_pc.empty()) return;
    for (size_t k = 0; k < g_pc.size();) {
        PendingClear& c = g_pc[k];
        bool used = false, taken = false;
        if (!c.depth) {
            for (uint32_t i = 0; i < P.ncolor && i < 8; ++i) {
                if (!P.color[i].tex || tex_gpu(P.color[i].tex) != c.g) continue;
                used = true;
                if (colors[i].imageView && P.color[i] == c.tg && extent.width == c.extent.width && extent.height == c.extent.height && P.layers == 1) {
                    colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    colors[i].clearValue = c.att.clearValue;
                    taken = true;
                    P.folded = true;                     // a pass with a taken clear is recorded even without draws
                    ++g_pc_taken;
                }
            }
        } else if (have_depth && P.depth.tex && tex_gpu(P.depth.tex) == c.g) {
            used = true;
            if (!P.depth_ro && P.depth == c.tg && extent.width == c.extent.width && extent.height == c.extent.height && P.layers == 1) {
                if (c.has_d && c.att.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) { depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth.clearValue = c.att.clearValue; }
                if (c.has_s && c.att_s.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) { stencil.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; stencil.clearValue = c.att_s.clearValue; }
                taken = true;
                P.folded = true;
                ++g_pc_taken;
            }
        }
        if (used) {
            if (!taken) record_clear(F->cb, c);
            g_pc.erase(g_pc.begin() + static_cast<std::ptrdiff_t>(k));
        } else {
            ++k;
        }
    }
    pc_sync_hook();
}

bool ensure_pass_for_clear() {
    g_for_clear = true;
    const bool ok = ensure_pass();
    g_for_clear = false;
    return ok;
}

// Starts a pass on the current render targets if none is open for them. False when there is nothing to render to. fbl: the colour slots
// that are in the feedback loop layout (a pass in another layout mode is ended: the layouts and the pipelines' feedback flag go together).
bool ensure_pass(bool depth_ro, uint8_t fbl) {
    // Every draw comes through here. The targets are compared only after a SetRenderTargets (the game often sets the ones it has): a
    // target is made from S's target fields, which only that op writes, and from fields of the texture that never change after its init.
    if (P.open && P.depth_ro == depth_ro && P.fbl == fbl && (P.targets_gen == S.targets_gen || targets_match())) {
        P.targets_gen = S.targets_gen;
        if (P.deferred && !g_for_clear) pass_flush();
        return true;
    }
    static const bool report_registered = (diag_add_report(pass_report), true);   // crash report and freeze dump
    (void)report_registered;
    end_pass(true);
    if (!g_bar.pending.empty()) {
        bool conflict = false;
        for (uint32_t i = 0; i < S.ncolor && !conflict; ++i) conflict = pending_conflict(S.color_tex[i].get(), color_layout(fbl, i));
        if (!conflict && S.depth_tex)
            conflict = pending_conflict(S.depth_tex.get(), depth_ro ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        if (conflict) {
            memory_barrier(F->cb);
            g_bar.pending.clear();
        }
    }
    if (gpass_on()) gpass_stamp(*F, "gap", 0);      // copies, dispatches, barriers and layout changes since the previous pass
    const float pass_scale = attach_targets(true);
    P = Pass();
    P.depth_ro = depth_ro;
    P.scale = pass_scale;
    P.fbl = fbl;
    P.targets_gen = S.targets_gen;
    VkRenderingAttachmentInfo colors[8] = {};
    VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool any = false;
    VkExtent2D extent = {UINT32_MAX, UINT32_MAX};
    P.ncolor = S.ncolor;
    for (uint32_t i = 0; i < S.ncolor; ++i) {
        colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        Texture* t = S.color_tex[i].get();
        P.color[i] = make_target(t, S.color_view[i], S.color_has_view[i]);
        if (i < 8) P.note[i] = note_of(t);
        if (!t || !tex_gpu(t)) continue;
        const bool fresh = tex_layout(t) == VK_IMAGE_LAYOUT_UNDEFINED;
        const VkImageLayout cl = color_layout(fbl, i);
        tex_transition_attach(t, *F, cl, P.color[i].level);   // the pass writes this level only (tex_write_gen: the feedback copies)
        tex_mark_gpu_written(t, static_cast<int>(P.color[i].level));
        VkImageView v = tex_attachment_view(t, P.color[i].level, P.color[i].layer, P.color[i].layers, P.color[i].view_format);
        if (!v) {
            P.no_view |= static_cast<uint8_t>(1u << i);
            continue;
        }
        colors[i].imageView = v;
        colors[i].imageLayout = cl;
        colors[i].loadOp = fresh ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        if (!fresh) P.loaded |= static_cast<uint8_t>(1u << i);
        else P.folded = true;     // [nvn] clear_fold: a new image's clear must happen even if the pass draws nothing
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        P.color_fmt[i] = tex_attachment_format(t, P.color[i].view_format);
        const VkExtent3D e = tex_image_extent(t, P.color[i].level);
        extent.width = std::min(extent.width, e.width);
        extent.height = std::min(extent.height, e.height);
        any = true;
    }
    Texture* dt = S.depth_tex.get();
    P.depth = make_target(dt, S.depth_view, S.depth_has_view);
    P.note[8] = note_of(dt);
    bool have_depth = false;
    if (dt && tex_gpu(dt)) {
        const bool fresh = tex_layout(dt) == VK_IMAGE_LAYOUT_UNDEFINED;
        const VkImageLayout dlayout = depth_ro ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        tex_transition(dt, *F, dlayout);
        if (!depth_ro) tex_mark_gpu_written(dt);
        VkImageView v = tex_attachment_view(dt, P.depth.level, P.depth.layer, P.depth.layers, P.depth.view_format);
        if (v) {
            depth.imageView = stencil.imageView = v;
            depth.imageLayout = stencil.imageLayout = dlayout;
            depth.loadOp = stencil.loadOp = (fresh && !depth_ro) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            P.loaded_d = depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
            if (!P.loaded_d) P.folded = true;     // the same for a new depth image
            depth.storeOp = stencil.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if constexpr (facts::kPhone)
                if (depth_ro && ro_store_none()) depth.storeOp = stencil.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
            depth.clearValue.depthStencil = {1.0f, 0};
            stencil.clearValue.depthStencil = {1.0f, 0};
            P.depth_fmt = tex_vk_format(dt);
            P.has_stencil = (tex_aspect(dt) & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
            const VkExtent3D e = tex_image_extent(dt, P.depth.level);
            extent.width = std::min(extent.width, e.width);
            extent.height = std::min(extent.height, e.height);
            have_depth = true;
            any = true;
        } else {
            P.depth_no_view = true;
        }
    }
    if (!any && g_xfb.active && device().features().transform_feedback) {   // a stream-out pass without attachments
        extent = {1, 1};
        any = true;
    }
    if (!any || extent.width == UINT32_MAX) return false;
    P.extent = extent;
    P.samples = std::max<uint32_t>(1, S.color_tex[0] ? static_cast<uint32_t>(S.color_tex[0]->b.samples) : 1);
    if (P.samples == 0) P.samples = 1;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    uint32_t layer_count = UINT32_MAX;     // layered rendering: the layers every attachment has in common
    for (uint32_t i = 0; i < S.ncolor; ++i)
        if (colors[i].imageView) layer_count = std::min(layer_count, std::max<uint32_t>(1, P.color[i].layers));
    if (have_depth) layer_count = std::min(layer_count, std::max<uint32_t>(1, P.depth.layers));
    ri.layerCount = layer_count == UINT32_MAX ? 1 : layer_count;
    P.layers = ri.layerCount;
    uint32_t last = 0;
    for (uint32_t i = 0; i < S.ncolor; ++i)
        if (colors[i].imageView) last = i + 1;
    ri.colorAttachmentCount = S.ncolor;
    ri.pColorAttachments = colors;
    if (have_depth) {
        ri.pDepthAttachment = &depth;
        if (P.has_stencil) ri.pStencilAttachment = &stencil;
    }
    (void)last;
    take_carried(colors, depth, stencil, have_depth, extent);   // [nvn] clear_carry
    gstamp("pass setup");
    if (clear_fold_on() && P.layers == 1) {
        // keep the begin: clears that come before the first draw can still become LOAD_OP_CLEAR (pass_fold_clear_*)
        for (uint32_t i = 0; i < S.ncolor && i < 8; ++i) P.att_c[i] = colors[i];
        P.att_d = depth;
        P.att_s = stencil;
        P.ri = ri;
        P.deferred = true;
        P.open = true;
        if (!g_for_clear) pass_flush();
    } else {
        vkCmdBeginRendering(F->cb, &ri);
        P.open = true;
    }
    rp_bump(ft::kCntEpBeginPass);
    counters().passes.fetch_add(1, std::memory_order_relaxed);
    g_cmd.bound_pipeline = VK_NULL_HANDLE;
    return true;
}

}  // namespace replay

}  // namespace nxrt::nvn::be::gpu
