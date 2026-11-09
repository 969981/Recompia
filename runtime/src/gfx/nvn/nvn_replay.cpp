// SPDX-License-Identifier: GPL-3.0-or-later
// The command list interpreter of the NVN back end (render thread). Walks recorded lists in order, keeps the GPU state the
// console keeps (it persists across lists and submissions), and turns it into Vulkan: dynamic-rendering passes opened lazily on the
// first draw or clear of a render target set, graphics pipelines keyed by the baked state, descriptor sets built from the
// translated shaders' binding tables (nvn_shaders.h), vertex and index data straight from the imported pool memory.
// Lessons applied from SuperMarioWonderRecomp docs/nvn/DEBUG.md and DEBUG-2.md (see work notes in docs/S4d.md): viewport flip by a
// negative-height viewport, a memory barrier after every pass, textures sampled by a draw are made current before the pass,
// handles never dropped by count, deferred bind arrays read at record time.
#include "nvn_replay_internal.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
constexpr int kMaxDepth = 8;

GState S;
uint8_t g_fbl_want = 0;         // colour slots the current draw samples through the feedback loop layout (feedback_prepare -> ensure_pass)
bool g_mirror_stale = false;    // a GPU write made pages of a pool copy stale while a draw was being set up: bind again at the next draw
Frame* F = nullptr;
std::unordered_map<uint64_t, VkDescriptorSet> g_set_cache;
VkPipeline g_bound_pipeline = VK_NULL_HANDLE;
UploadAlloc g_support;
UploadAlloc g_draw_support;     // the support buffer the next descriptor sets bind: g_support, or one with render scales (support_for_draw)
std::unordered_map<uint64_t, UploadAlloc> g_support_scaled;   // this submission's support buffers with render scales, by content
std::unordered_map<uint64_t, UploadAlloc> g_driver_banks;     // this submission's filled c[0] banks (driver_bank_for), by content
uint32_t g_draw_base_instance = 0;   // the base instance of the draw being set up, for graphics c[0] + 0x644 (0 for indirect draws)
uint32_t g_bank_base_instance = 0;   // the one the bound descriptor sets hold
MaskClear g_mask;
bool g_mask_ready = false, g_mask_ok = false;
// [nvn] set_reuse: bumped by whatever in the replay may change what a texture binding resolves to (a pass opens or ends, other render
// targets, a feedback or 3D copy, a GPU write, a new submission). With be::g_bind_gen (pools, texture images) it keys the per-set memo of build_set.
uint64_t g_rp_epoch = 1;
// [nvn] epoch_exact (default off: no measured gain): the two bumps that cannot change what a memoized set resolves to are left out: end_pass while no pass is
// open, and SetRenderTargets that sets the targets already set (see end_pass and mark_dirty).
bool epoch_exact_on() {
    static const bool on = [] {
        const bool v = nxrt::config_get_bool("nvn", "epoch_exact", false);
        NXLOG_I(kTag, "set_reuse epoch: %s ([nvn] epoch_exact)", v ? "exact (no bump for end_pass without a pass or for the same render targets)" : "every pass end and render target op");
        return v;
    }();
    return on;
}
constexpr const char* kRpNames[kRpCount] = {"rp.draw", "rp.sets", "rp.set.build", "rp.set.write", "rp.set.bind", "rp.prep", "rp.pass",
                                            "rp.pipe", "rp.dyn", "rp.vb", "rp.dispatch", "rp.set0", "rp.set1", "rp.set2", "rp.set3",
                                            "rp.miss0", "rp.miss1", "rp.miss2", "rp.miss3", "rp.buf", "rp.tex", "rp.texel", "rp.same0", "rp.same1", "rp.same2", "rp.same3", "rp.reuse",
                                            "rp.begin", "rp.end", "rp.opin", "rp.op", "rp.tail", "rp.texpoll", "rp.mirror", "rp.outend"};
// Stamps are TSC ticks (ft::tsc), converted to ns in rp_flush. Disjoint "top" phases that together cover the replay zone: rp.begin and rp.end
// (per replay call: tex_poll_cpu_writes, mirror_begin_submit, out_end_submit, end_pass, ...), rp.opin (mark_dirty + out_before_op per op),
// rp.draw (begin_draw), rp.tail (what a draw op does after begin_draw: index resolve, vkCmdDraw*), rp.op (every other op, dispatch setup
// excluded: rp.dispatch), plus the NXFT zones rp.cbsetup / rp.cbend of nvn_gpu.cpp (slot, begin / end command buffer, query resets, batch
// flush). rp.texpoll, rp.mirror, rp.outend are parts of rp.begin / rp.end. The per-binding phases (rp.buf, rp.tex, rp.texel) run only with
// [prof] replay_fine (a scope each costs more than the work of a cached binding).
uint64_t g_rp_ns[kRpCount] = {};     // ticks until rp_flush
uint32_t g_rp_n[kRpCount] = {};
static uint64_t g_rp_t0 = 0;
uint64_t g_rp_inner = 0;             // ticks of rp.draw / rp.dispatch scopes and finished ops, so an op's own time can be told from them
bool g_rp_fine = false;
static void rp_flush() {
    if (!g_rp_t0) return;
    static uint16_t ids[kRpCount];
    static bool named = false;
    if (!named) { for (int i = 0; i < kRpCount; ++i) ids[i] = ft::zone_id(kRpNames[i]); named = true; }
    for (int i = 0; i < kRpCount; ++i)
        if (g_rp_n[i]) ft::event(ids[i], g_rp_t0, g_rp_t0 + ft::tsc_to_ns(g_rp_ns[i]), g_rp_n[i]);
    std::memset(g_rp_ns, 0, sizeof g_rp_ns);
    std::memset(g_rp_n, 0, sizeof g_rp_n);
    g_rp_t0 = 0;
    g_rp_inner = 0;
}
uint32_t g_dirty = kAllDirty;
DynShadow g_dsh;
VbShadow g_vbsh[16];
IbShadow g_ibsh;
// vkCmdBindIndexBuffer unless the same (buffer, offset, type) is already bound in this command buffer.
static void bind_index_buffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkIndexType type) {
    if (bind_coalesce_on()) {
        if (g_ibsh.ok && g_ibsh.buf == buf && g_ibsh.off == off && g_ibsh.type == type) { ft::split_count(ft::kCntSkipped); return; }
        g_ibsh = {buf, off, type, true};
    }
    vkCmdBindIndexBuffer(cb, buf, off, type);
}
GpuWrite g_gw[kGpuWriteRing];
size_t g_gw_next = 0, g_gw_count = 0;
uint64_t g_gw_lost_serial = 0;      // the newest serial among ranges dropped from the ring
static void note_gw(uint64_t addr, uint64_t size) {
    if (!size || !F) return;
    GpuWrite& last = g_gw[(g_gw_next + kGpuWriteRing - 1) % kGpuWriteRing];
    if (g_gw_count && last.lo == addr && last.hi == addr + size) { last.serial = F->serial; return; }
    GpuWrite& w = g_gw[g_gw_next];
    if (g_gw_count == kGpuWriteRing) g_gw_lost_serial = std::max(g_gw_lost_serial, w.serial);
    else ++g_gw_count;
    w = {addr, addr + size, F->serial};
    g_gw_next = (g_gw_next + 1) % kGpuWriteRing;
}

void gpu_wrote(uint64_t addr, uint64_t size, int use) {
    rp_bump(ft::kCntEpGpuWrote);
    note_gw(addr, size);
    if (note_gpu_write(addr, size, use)) {
        g_dirty |= kVb | kDesc;
        g_mirror_stale = true;
    }
}
const ProgLayout* g_pl = nullptr;
VkPipeline g_pipe = VK_NULL_HANDLE;
VkPrimitiveTopology g_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
VkPrimitiveTopology g_pipe_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
static bool g_pipe_fbl = false;        // g_pipe was made for a pass with a feedback loop attachment (GraphicsKey::fb_loop)
bool g_restart = false, g_restart_valid = false;
static uint32_t g_key_attr_count = 0;
static uint8_t g_key_binding[16] = {};
VkQueryPool g_occ_pool = VK_NULL_HANDLE;
bool g_occ_tried = false;
uint32_t g_occ_next = 0;
int32_t g_occ_active = -1, g_occ_last = -1;

bool occ_ready() {
    if (g_occ_pool) return true;
    if (g_occ_tried || !device().features().occlusion_query) return false;
    g_occ_tried = true;
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = kOccSlots;
    if (vkCreateQueryPool(vkdev(), &qi, nullptr, &g_occ_pool) != VK_SUCCESS) g_occ_pool = VK_NULL_HANDLE;
    return g_occ_pool != VK_NULL_HANDLE;
}

uint8_t g_key_dummy_binding = 0;      // 1 + the binding that feeds the defaults of unset vertex attributes (0: none)
bool g_want_depth_ro = false;         // some texture this draw samples is the current depth attachment
bool g_vb_bound_in_cb = false;        // vkCmdBindVertexBuffers2 was called in this command buffer (dynamic strides need one)

// [nvn] trace_frame = N (NXRT_NVN_TRACE_FRAME): every draw, clear, copy and dispatch of the frame that is presented as number N is logged
// at Info ("trace ..." lines): programs, render targets, sampled textures, blend and depth state. For finding out what a frame does.
bool g_xfb_active = false;
static bool g_pipe_xfb = false;             // the bound pipeline is a stream-out variant
bool g_xfb_began = false;            // vkCmdBeginTransformFeedbackEXT is in effect in the open pass
bool g_xfb_have_counter = false;     // a counter buffer holds the bytes written so far: the next begin continues after them
gfx::Buffer g_xfb_counter;
std::unordered_map<uint64_t, uint32_t> g_xfb_slot_of;
uint32_t g_xfb_slot_next = 0, g_xfb_cur = 0;
bool g_xfb_slot_written[kXfbSlots] = {};
CounterWindow g_cw[2];
static int g_dbg_depth = 0;                  // debug labels open in this submission (PushDebugGroup / PopDebugGroup)
// Primitive counters (NVN_COUNTER_TYPE_PRIMITIVES_GENERATED = 13, TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN = 14): counted on the CPU by every
// draw (count_primitives), process-wide like the console's, zeroed by ResetCounter of their type, dropped at shutdown.
uint64_t g_prims_generated = 0, g_prims_xfb = 0;
std::unordered_map<uint64_t, uint64_t> g_reports;
nxrt::GaugeCounter g_gauge_pipeline_binds("nvn.cnt.pipeline_binds");   // graphics pipeline binds (cumulative)
static VkQueryPool g_xq_pool = VK_NULL_HANDLE;     // transform feedback stream queries (counters 13 and 14)

void log_once(const char* key, const char* msg) {
    static std::unordered_map<std::string, bool> seen;
    if (seen.emplace(key, true).second) NXLOG_W(kTag, "%s", msg);
}

bool first_time_key(const std::string& key) {   // render thread only
    static std::unordered_map<std::string, bool> seen;
    return seen.size() < 4096 && seen.emplace(key, true).second;
}

// ---- helpers ----------------------------------------------------------------------------------------------------------------
static VkFormat scaled_for(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UINT: return VK_FORMAT_R8_USCALED;
        case VK_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_USCALED;
        case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8_USCALED;
        case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_USCALED;
        case VK_FORMAT_R16_UINT: return VK_FORMAT_R16_USCALED;
        case VK_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_USCALED;
        case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16_USCALED;
        case VK_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_USCALED;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        case VK_FORMAT_R8_SINT: return VK_FORMAT_R8_SSCALED;
        case VK_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_SSCALED;
        case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8_SSCALED;
        case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SSCALED;
        case VK_FORMAT_R16_SINT: return VK_FORMAT_R16_SSCALED;
        case VK_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SSCALED;
        case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16_SSCALED;
        case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SSCALED;
        case VK_FORMAT_A2B10G10R10_SINT_PACK32: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
        default: return f;
    }
}

static bool is_int_format(VkFormat f) { return scaled_for(f) != f; }

// ---- viewport conversion -------------------------------------------------------------------------------------------------
VkRect2D scissor_rect(const int32_t* r) {
    int32_t x = r[0], y = r[1], w = r[2], h = r[3];
    if (P.scale != 1.0f) {     // the edges scale (a rectangle covering the target still covers it)
        const float sc = P.scale;
        const int32_t x1 = static_cast<int32_t>(std::ceil((static_cast<float>(x) + static_cast<float>(w)) * sc - 0.001f));
        const int32_t y1 = static_cast<int32_t>(std::ceil((static_cast<float>(y) + static_cast<float>(h)) * sc - 0.001f));
        x = static_cast<int32_t>(std::floor(static_cast<float>(x) * sc + 0.001f));
        y = static_cast<int32_t>(std::floor(static_cast<float>(y) * sc + 0.001f));
        w = x1 - x;
        h = y1 - y;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    w = std::max(0, std::min<int32_t>(w, static_cast<int32_t>(P.extent.width) - x));
    h = std::max(0, std::min<int32_t>(h, static_cast<int32_t>(P.extent.height) - y));
    return {{x, y}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
}

static void set_viewports_and_scissors() {
    VkCommandBuffer cb = F->cb;
    VkViewport vp[16];
    VkRect2D sc[16];
    const uint32_t nvp = S.nvp ? S.nvp : 1;
    for (uint32_t i = 0; i < nvp; ++i) {
        float x = S.vp[i][0], y = S.vp[i][1], w = S.vp[i][2], h = S.vp[i][3];
        if (!S.nvp) { x = 0; y = 0; w = static_cast<float>(P.extent.width); h = static_cast<float>(P.extent.height); }
        else if (P.scale != 1.0f) { x *= P.scale; y *= P.scale; w *= P.scale; h *= P.scale; }
        // Upper-left origin: window y is the image row from the top and NDC +y is the top edge of the rectangle (negative height).
        // Lower-left origin (the GL convention): window y 0 is memory row 0 and NDC -y is the first row, so no flip.
        const bool lower_left = S.window_origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
        vp[i] = lower_left ? VkViewport{x, y, w, h, S.dr[i][0], S.dr[i][1]}
                           : VkViewport{x, y + h, w, -h, S.dr[i][0], S.dr[i][1]};
        if (i < S.nsc) sc[i] = scissor_rect(S.sc[i]);
        else sc[i] = {{0, 0}, P.extent};
    }
    if (bind_coalesce_on()) {
        // compare the whole arrays (the unused tail is zero) and the count
        std::array<VkViewport, 16> av = {};
        std::array<VkRect2D, 16> as = {};
        std::memcpy(av.data(), vp, sizeof(VkViewport) * nvp);
        std::memcpy(as.data(), sc, sizeof(VkRect2D) * nvp);
        const bool cv = g_dsh.vp.changed(av) || g_dsh.nvp != nvp;
        const bool cs = g_dsh.sc.changed(as) || g_dsh.nsc != nvp;
        g_dsh.nvp = g_dsh.nsc = nvp;
        if (cv) vkCmdSetViewportWithCount(cb, nvp, vp); else ft::split_count(ft::kCntSkipped);
        if (cs) vkCmdSetScissorWithCount(cb, nvp, sc); else ft::split_count(ft::kCntSkipped);
        return;
    }
    vkCmdSetViewportWithCount(cb, nvp, vp);
    vkCmdSetScissorWithCount(cb, nvp, sc);
}

// The scale nxrt/gfx_tuning.h asks for on uniform block `idx` of this binding's shader (1 = none): only the chosen fragment shader's block.
static float ufs_scale_for(const BindingDesc& d, int idx) {
    const nxrt::gfx::Tuning& t = nxrt::gfx::tuning();
    const uint64_t want = t.ufs_hash.load(std::memory_order_relaxed);
    if (!want || d.nvn_stage != kFragment || idx != t.ufs_block.load(std::memory_order_relaxed)) return 1.0f;
    const float sc = t.ufs_scale.load(std::memory_order_relaxed);
    if (sc == 1.0f || !d.shader || std::strtoull(d.shader->hash.c_str(), nullptr, 16) != want) return 1.0f;
    return sc;
}
std::vector<ResolveSlot> g_rc;
const ProgLayout* g_rc_pl = nullptr;   // the layout of the draw scope (null: no scope, resolve_texture resolves every time)
uint64_t g_rc_stamp = 0;
bool resolve_once_on() {
    static const bool on = [] {
        const bool v = nxrt::config_get_bool("nvn", "resolve_once", true);
        NXLOG_I(kTag, "texture bindings: %s ([nvn] resolve_once)", v ? "resolved once per draw" : "resolved by every caller");
        return v;
    }();
    return on;
}

static TexResolved resolve_handle(uint64_t handle) {
    TexResolved r;
    ft::split_count(ft::kCntResolve);
    const uint32_t tid = handle_texture_id(handle), sid = handle_sampler_id(handle);
    if (S.tpool && static_cast<int>(tid) < S.tpool->count) {
        std::shared_lock<std::shared_mutex> lk(S.tpool->lock);
        const TexturePool::Entry& e = S.tpool->entries[tid];
        if (e.used && e.tex) {
            r.tex = e.tex.get();
            r.view = e.view;
            r.has_view = e.has_view;
        }
    }
    if (S.spool && static_cast<int>(sid) < S.spool->count) {
        std::shared_lock<std::shared_mutex> lk(S.spool->lock);
        if (S.spool->used[sid]) {
            r.smp = S.spool->entries[sid];
            r.has_sampler = true;
        }
    }
    return r;
}

// ImageBinding: an NVN2 compute texture binding over the program's image slot n (its size query, apply_nvn2_compute_table): the image
// bound there (an image handle names no sampler; image_as_texture binds the dummy sampler).
TexResolved resolve_texture(const BindingDesc& d) {
    const DescriptorBinding& b = d.shader->refl.bindings[d.index];
    uint64_t handle = 0;
    if (b.nvn.source == NvnSource::SamplerBinding) {
        const int idx = b.nvn.index;
        if (idx >= 0 && idx < 64) handle = S.tex[d.nvn_stage][idx];
    } else if (b.nvn.source == NvnSource::HandleInUniform) {
        handle = read_handle_word(d.nvn_stage, b.nvn.bank, b.nvn.word);
    } else if (b.nvn.source == NvnSource::ImageBinding) {
        const int idx = b.nvn.index;
        if (idx >= 0 && idx < 16) handle = handle_texture_id(S.img[d.nvn_stage][idx]);   // texture id only (image_as_texture ignores the sampler)
    }
    const ProgLayout* pl = g_rc_pl;
    if (!pl || &d < pl->bindings.data() || &d >= pl->bindings.data() + pl->bindings.size()) return resolve_handle(handle);
    ResolveSlot& sl = g_rc[static_cast<size_t>(&d - pl->bindings.data())];
    const uint64_t gen = be::g_bind_gen.load(std::memory_order_acquire);   // before the resolve: a write after it is seen next time
    if (sl.stamp == g_rc_stamp && sl.gen == gen && sl.handle == handle && sl.tpool == S.tpool.get() && sl.spool == S.spool.get()) {
        ft::split_count(ft::kCntResolveHit);
        return sl.r;
    }
    sl.r = resolve_handle(handle);
    sl.stamp = g_rc_stamp;
    sl.gen = gen;
    sl.handle = handle;
    sl.tpool = S.tpool.get();
    sl.spool = S.spool.get();
    return sl.r;
}

ViewRequest make_request(const DescriptorBinding& b, const TexResolved& r) {
    ViewRequest q;
    q.type = view_type_for(b);
    q.depth_compare = b.image.depth;
    const Texture& t = *r.tex;
    int sw[4] = {t.b.swizzle[0], t.b.swizzle[1], t.b.swizzle[2], t.b.swizzle[3]};
    int dsmode = t.b.depth_stencil_mode;
    if (r.has_view) {
        const TextureViewInfo& v = r.view;
        if (v.has_levels) { q.base_level = static_cast<uint32_t>(v.min_level); q.level_count = static_cast<uint32_t>(v.num_levels); }
        if (v.has_layers) { q.base_layer = static_cast<uint32_t>(v.min_layer); q.layer_count = static_cast<uint32_t>(v.num_layers); }
        if (v.has_format) {
            const FormatInfo* fi = format_info(v.format);
            if (fi) q.format = fi->vk;
        }
        if (v.has_swizzle) for (int i = 0; i < 4; ++i) sw[i] = v.swizzle[i];
        if (v.has_depth_stencil_mode) dsmode = v.depth_stencil_mode;
    }
    q.swizzle.r = vk_swizzle(sw[0]);
    q.swizzle.g = vk_swizzle(sw[1]);
    q.swizzle.b = vk_swizzle(sw[2]);
    q.swizzle.a = vk_swizzle(sw[3]);
    q.depth_aspect = dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH;
    switch (b.image.component) {
        case ScalarType::Float: q.component = 1; break;
        case ScalarType::Sint: q.component = 2; break;
        case ScalarType::Uint: q.component = 3; break;
        default: break;
    }
    return q;
}

// The image a draw reads or writes through an image binding (nvnCommandBufferBindImage): the handle is a texture pool id.
TexResolved resolve_image(const BindingDesc& d) {
    TexResolved r;
    const DescriptorBinding& b = d.shader->refl.bindings[d.index];
    uint64_t handle = 0;
    if (b.nvn.source == NvnSource::ImageBinding) {
        const int idx = b.nvn.index;
        if (idx >= 0 && idx < 16) handle = S.img[d.nvn_stage][idx];
    } else if (b.nvn.source == NvnSource::HandleInUniform) {
        handle = read_handle_word(d.nvn_stage, b.nvn.bank, b.nvn.word);
    }
    const uint32_t tid = handle_texture_id(handle);
    if (S.tpool && static_cast<int>(tid) < S.tpool->count) {
        std::shared_lock<std::shared_mutex> lk(S.tpool->lock);
        const TexturePool::Entry& e = S.tpool->entries[tid];
        if (e.used && e.tex) {
            r.tex = e.tex.get();
            r.view = e.view;
            r.has_view = e.has_view;
        }
    }
    return r;
}

// True when a texture the program samples is the depth attachment of the current target set: the draw then runs in a pass whose depth
// attachment is read-only (DEPTH_STENCIL_READ_ONLY_OPTIMAL serves as attachment and as sampled layout).
bool samples_depth_target(const ProgLayout& pl) {
    if (!S.depth_tex) return false;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        if (shares_image(resolve_texture(d).tex, S.depth_tex.get())) return true;
    }
    return false;
}
std::unordered_map<Texture*, FbClone> g_fb_clone;          // attachment -> its copy (same shape, no storage in game memory)
std::unordered_map<Texture*, Texture*> g_fb_use;           // attachment -> the copy the current draw samples
bool g_fb_seen = false;                                    // the last check found a feedback binding: check every draw until it goes away
std::atomic<int64_t> g_fb_clone_n{0};                      // g_fb_clone's size, mirrored for the gauge (the map itself is render thread only)

// [nvn] feedback_loop: auto (default) = the feedback loop layout where the GPU has VK_EXT_attachment_feedback_loop_layout, off = the copy.
static bool fb_loop_on() {
    static const bool on = [] {
        const std::string v = nxrt::config_get_str("nvn", "feedback_loop", "auto");
        const bool want = v != "off" && v != "0" && v != "false";
        const bool have = device().features().attachment_feedback_loop;
        NXLOG_I(kTag, "feedback draws: %s ([nvn] feedback_loop = %s, VK_EXT_attachment_feedback_loop_layout %s)", want && have ? "feedback loop layout" : "copy",
                v.c_str(), have ? "yes" : "no");
        return want && have;
    }();
    return on;
}
// [nvn] feedback_copy_levels: auto (default) = the copy path copies only the levels that changed, off = every level each time.
static bool fb_levels_on() {
    static const bool on = [] {
        const std::string v = nxrt::config_get_str("nvn", "feedback_copy_levels", "auto");
        return v != "off" && v != "0" && v != "false";
    }();
    return on;
}

static Texture* feedback_clone(Texture* t) {
    FbClone& fe = g_fb_clone[t];
    fe.last = F ? F->serial : 0;
    Ref<Texture>& c = fe.tex;
    g_fb_clone_n.store(static_cast<int64_t>(g_fb_clone.size()), std::memory_order_relaxed);
    if (c && (c->b.width != t->b.width || c->b.height != t->b.height || c->b.depth != t->b.depth || c->fmt != t->fmt || c->levels != t->levels || c->layers != t->layers ||
              (c->gpu && tex_scale(c.get()) != tex_scale(t))))
        c = Ref<Texture>();                                 // the game made another texture at this address
    if (!c) {
        Texture* n = new Texture();
        n->b = t->b;
        n->fmt = t->fmt;
        n->levels = t->levels;
        n->layers = t->layers;
        n->block_linear = t->block_linear;
        n->cpu_dirty.store(false);
        c = Ref<Texture>(n);
        static uint32_t next_id = 0x7f000000;
        n->id = next_id++;
        for (uint64_t& g : fe.gen) g = 0;
    }
    return tex_gpu_scaled(c.get(), tex_scale(t)) ? c.get() : nullptr;
}

// GPU pass profiler label: size, format, levels, the level the pass writes and the levels the draw samples.
static void feedback_label(const TexResolved& r, const char* what) {
    uint32_t att_lv = 0;
    for (uint32_t i = 0; i < S.ncolor && i < 8; ++i) if (shares_image(r.tex, S.color_tex[i].get())) { att_lv = S.color_level[i]; break; }
    char fb[112];
    std::snprintf(fb, sizeof fb, "feedback %s %ux%u %s lv%u w%u s%d+%d", what, r.tex->b.width, r.tex->b.height, r.tex->fmt ? r.tex->fmt->name : "?",
                  tex_levels(r.tex), att_lv, r.has_view && r.view.has_levels ? r.view.min_level : 0,
                  r.has_view && r.view.has_levels ? r.view.num_levels : static_cast<int>(tex_levels(r.tex)));
    gpass_stamp(*F, fb, 0);
}

void feedback_prepare(const ProgLayout& pl) {
    if (!g_fb_use.empty()) { g_fb_use.clear(); rp_bump(ft::kCntEpFeedback); }
    g_fbl_want = 0;
    bool any = false;
    TexResolved loop_label;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        const TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || !is_color_feedback(r.tex) || r.tex->b.samples > 1 || g_fb_use.count(r.tex)) continue;
        any = true;
        if (fb_loop_on()) {
            uint8_t slots = 0;
            for (uint32_t i = 0; i < S.ncolor && i < 8; ++i) if (shares_image(r.tex, S.color_tex[i].get())) slots |= static_cast<uint8_t>(1u << i);
            if (slots && (g_fbl_want & slots) == slots) continue;          // another binding of the same image
            if (!tex_has_feedback_loop(r.tex)) {                          // the first feedback draw of this image: remade with the usage (once)
                end_pass();
                tex_enable_feedback_loop(r.tex, *F);
            }
            if (slots && tex_has_feedback_loop(r.tex)) {
                if (!tex_current_on_gpu(r.tex)) {
                    end_pass();
                    tex_ensure_current(r.tex, *F);
                }
                g_fbl_want |= slots;
                if (gpass_on() && !loop_label.tex) loop_label = r;   // stamped after the pass ends below (the open pass's draws are its own interval)
                continue;
            }
        }
        Texture* c = feedback_clone(r.tex);
        if (!c) continue;
        if (!tex_current_on_gpu(r.tex)) {
            end_pass();
            tex_ensure_current(r.tex, *F);                                // storage the game wrote since the image was made reaches the copy too
        }
        FbClone& fe = g_fb_clone[r.tex];
        const uint32_t nlv = tex_levels(r.tex);
        // the levels the draw's view covers (a level outside it may stay stale: its generation says so when a later view covers it)
        uint32_t lo = 0, hi = nlv;
        if (r.has_view && r.view.has_levels) {
            lo = std::min<uint32_t>(static_cast<uint32_t>(std::max(0, static_cast<int>(r.view.min_level))), nlv);
            if (r.view.num_levels > 0) hi = std::min<uint32_t>(nlv, lo + static_cast<uint32_t>(r.view.num_levels));
        }
        const bool by_level = fb_levels_on() && nlv <= 16;
        if (!by_level) { lo = 0; hi = nlv; }
        std::vector<VkImageCopy> regions;
        uint64_t gens[16] = {};
        for (uint32_t lv = lo; lv < hi; ++lv) {
            if (by_level) {
                gens[lv] = tex_write_gen(r.tex, lv);
                if (gens[lv] != 0 && gens[lv] == fe.gen[lv]) continue;  // not written since it was copied
            }
            VkImageCopy rc = {};
            rc.srcSubresource = {tex_aspect(r.tex), lv, 0, tex_layers(r.tex)};
            rc.dstSubresource = {tex_aspect(c), lv, 0, tex_layers(c)};
            rc.extent = tex_image_extent(r.tex, lv);
            regions.push_back(rc);
        }
        g_fb_use[r.tex] = c;
        rp_bump(ft::kCntEpFeedback);   // [nvn] set_reuse: what this texture resolves to changed
        if (regions.empty()) continue;                                    // the copy is current for every level the draw samples (no pass break)
        end_pass();
        tex_transition(r.tex, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);   // the next pass moves it back to the attachment layout and loads it
        tex_transition(c, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyImage(F->cb, tex_image(r.tex), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_image(c), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
        tex_mark_gpu_written(c);
        tex_transition(c, *F, tex_sampled_layout(c));
        if (by_level)
            for (const VkImageCopy& rc : regions) fe.gen[rc.srcSubresource.mipLevel] = gens[rc.srcSubresource.mipLevel];
        rp_bump(ft::kCntEpFeedback);
        counters().copies.fetch_add(1, std::memory_order_relaxed);
        if (gpass_on()) {
            char what[24];
            if (regions.size() == hi - lo) std::snprintf(what, sizeof what, "copy");
            else std::snprintf(what, sizeof what, "copy %zul", regions.size());   // only that many of the sampled levels were copied
            feedback_label(r, what);
        }
    }
    // The draws before this one wrote the attachment it reads through the feedback loop layout: the pass ends (with a barrier) and the draw's
    // pass begins in that layout (ensure_pass). Each such draw gets its own pass, as each copy did.
    if (g_fbl_want) {
        end_pass();
        if (loop_label.tex) feedback_label(loop_label, "loop");
    }
    g_fb_seen = any;
}
// [addr, +size) of game memory a texture upload reads: held compute results there go back first (compute_out_lazy; else all of them).
void out_texture_read(Texture* t) {
    if (!out_lazy()) { flush_compute_out(kOutTexture); return; }
    if (t && t->mem && t->cpu_dirty.load()) compute_out_guard(reinterpret_cast<uint64_t>(t->mem), t->storage_size ? t->storage_size : 4096, kOutTexture);
}
std::unordered_map<Texture*, Vol3dCopy> g_vol3d;           // 2D texture -> its 3D copy
std::unordered_map<Texture*, Texture*> g_vol3d_use;        // 2D texture -> the copy the current draw samples

static void vol3d_prepare(const ProgLayout& pl) {
    g_vol3d_use.clear();
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        const TexResolved r = resolve_texture(d);
        if (!wants_vol3d(b, r) || g_vol3d_use.count(r.tex) || !tex_gpu(r.tex)) continue;
        const uint32_t layer = r.has_view && r.view.has_layers ? static_cast<uint32_t>(r.view.min_layer) : 0;
        if (layer >= tex_layers(r.tex)) continue;
        Vol3dCopy& e = g_vol3d[r.tex];
        if (e.src.get() != r.tex) e.src = Ref<Texture>::share(r.tex);
        e.last = F ? F->serial : 0;
        Ref<Texture>& c = e.tex;
        if (c && (c->b.width != r.tex->b.width || c->b.height != r.tex->b.height || c->fmt != r.tex->fmt || c->levels != r.tex->levels || e.layer != layer ||
                  (c->gpu && tex_scale(c.get()) != tex_scale(r.tex))))
            c = Ref<Texture>();
        if (!c) {
            Texture* n = new Texture();
            n->b = r.tex->b;
            n->b.target = NVN_TEXTURE_TARGET_3D;
            n->b.depth = 1;
            n->fmt = r.tex->fmt;
            n->levels = r.tex->levels;
            n->layers = 1;
            n->block_linear = r.tex->block_linear;
            n->cpu_dirty.store(false);
            static uint32_t next_id = 0x7e000000;
            n->id = next_id++;
            c = Ref<Texture>(n);
            e.layer = layer;
            e.copied = UINT64_MAX;
        }
        if (!tex_gpu_scaled(c.get(), tex_scale(r.tex))) continue;
        const uint64_t serial = F ? F->serial : 0;
        if (e.copied != serial || r.tex->cpu_dirty.load()) {
            out_texture_read(r.tex);
            end_pass();
            tex_ensure_current(r.tex, *F);
            tex_transition(r.tex, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            tex_transition(c.get(), *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            std::vector<VkImageCopy> regions;
            for (uint32_t lv = 0; lv < tex_levels(r.tex); ++lv) {
                VkImageCopy rc = {};
                rc.srcSubresource = {tex_aspect(r.tex), lv, layer, 1};
                rc.dstSubresource = {tex_aspect(c.get()), lv, 0, 1};
                rc.extent = tex_image_extent(r.tex, lv);
                rc.extent.depth = 1;
                regions.push_back(rc);
            }
            vkCmdCopyImage(F->cb, tex_image(r.tex), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_image(c.get()), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
            tex_mark_gpu_written(c.get());
            tex_transition(c.get(), *F, tex_sampled_layout(c.get()));
            tex_transition(r.tex, *F, tex_sampled_layout(r.tex));   // the same draw may sample it as 2D too
            e.copied = serial;
            counters().copies.fetch_add(1, std::memory_order_relaxed);
            gstamp("3D copy of a 2D texture");
            if (first_time_key("vol3d " + std::to_string(r.tex->id)))
                NXLOG_I(kTag, "texture %u (%s %dx%d, target %d) is sampled as 3D by %s binding %s: it gets a 3D copy one slice deep", r.tex->id,
                        r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.width, r.tex->b.height, r.tex->b.target, d.shader->hash.c_str(), b.name.c_str());
        }
        g_vol3d_use[r.tex] = c.get();
        rp_bump(ft::kCntEpVol3d);
    }
}

// NVN2 compute: a texture binding over the program's image slot n (apply_nvn2_compute_table) is the shader's size query (TXQ) on the image it
// writes (255 compute blocks: volumetric fog, light injection). Before, it was Unknown and the query saw the 1x1 dummy.
// - real view: a sampled view of the image with the binding's type (the image always has SAMPLED usage), read in GENERAL: the same dispatch
//   binds the image for storage (prepare_textures leaves it in GENERAL);
// - stand-in: the translator types these handles 2D (StaticGpuAccessor answers 2D for every compute handle), and a 2D view of a 3D image is no
//   sampled view without VK_EXT_image_2d_view_of_3d. Such a binding gets a black 2D (or 2D array) texture of the image's level size: the size
//   query sees the real x/y (the depth needs the translator to type the handle 3D, which then takes the real view).
static bool image_as_texture_real(const DescriptorBinding& b, const TexResolved& r) {
    return r.tex->b.target != NVN_TEXTURE_TARGET_3D || view_type_for(b) == VK_IMAGE_VIEW_TYPE_3D;
}

void image_as_texture(const BindingDesc& d, const DescriptorBinding& b, const TexResolved& r, VkDescriptorImageInfo& ii) {
    if (!r.tex || !tex_gpu(r.tex)) return;
    const ViewRequest q = make_request(b, r);
    if (image_as_texture_real(b, r)) {
        if (VkImageView v = tex_sampled_view(r.tex, q)) {
            ii.imageView = v;
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            return;
        }
    }
    const VkExtent3D e = tex_extent(r.tex, q.base_level);
    const uint32_t layers = r.tex->b.target == NVN_TEXTURE_TARGET_3D ? e.depth : q.layer_count ? q.layer_count : tex_layers(r.tex) - std::min(q.base_layer, tex_layers(r.tex));
    VkImageView v = dummy_view_sized(q.type, e.width, e.height, layers);
    if (v) ii.imageView = v;
    if (first_time_key("image-size " + d.shader->hash + " " + b.name + " " + std::to_string(r.tex->id)))
        NXLOG_I(kTag, "compute %s binding %s: size query on image %u (%s, target %d, %ux%ux%u) through a %s stand-in of %ux%u (x%u)", d.shader->hash.c_str(),
                b.name.c_str(), r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.target, e.width, e.height, e.depth, v ? "black" : "missing (dummy)",
                e.width, e.height, layers);
}

// Makes the textures a draw samples current (uploaded, in their sampled layout) and the storage images it binds writable (GENERAL).
// Ends the pass when it has to touch images. The depth attachment itself is left to the pass.
void prepare_textures(const ProgLayout& pl) {
    vol3d_prepare(pl);
    std::vector<Texture*> need, need_storage, need_general;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            TexResolved r = resolve_image(d);
            if (!r.tex || !tex_gpu(r.tex) || !tex_has_storage(r.tex)) continue;
            if (r.tex->cpu_dirty.load() || tex_layout(r.tex) != VK_IMAGE_LAYOUT_GENERAL || tex_scale(r.tex) != 1.0f) need_storage.push_back(r.tex);
            continue;
        }
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source == NvnSource::ImageBinding) {     // the size query over a compute image: read in GENERAL, as the image binding
            TexResolved r = resolve_texture(d);
            if (!r.tex || !tex_gpu(r.tex) || !image_as_texture_real(b, r)) continue;
            if (r.tex->cpu_dirty.load() || tex_layout(r.tex) != VK_IMAGE_LAYOUT_GENERAL) need_general.push_back(r.tex);
            continue;
        }
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex) continue;
        if (!tex_gpu(r.tex)) continue;
        // owned by the pass; a compute dispatch runs outside every pass, so the graphics attachments do not own what it samples
        if (shares_image(r.tex, S.depth_tex.get()) || (d.nvn_stage != kCompute && is_color_feedback(r.tex))) continue;
        if (r.tex->cpu_dirty.load() || tex_layout(r.tex) != tex_sampled_layout(r.tex)) {
            need.push_back(r.tex);
            if (r.tex->cpu_dirty.load() && ft::on()) ft::mark(b.nvn.source == NvnSource::SamplerBinding ? "tx.dirty_bind" : "tx.dirty_uniform", r.tex->id);
        }
    }
    if (need.empty() && need_storage.empty() && need_general.empty()) return;
    for (Texture* t : need) out_texture_read(t);           // an upload reads game memory: compute results held there go back first
    for (Texture* t : need_storage) out_texture_read(t);
    for (Texture* t : need_general) out_texture_read(t);
    end_pass(true);     // layout changes and uploads follow: each carries the dependency for its own image
    counters().pass_breaks.fetch_add(1, std::memory_order_relaxed);
    for (Texture* t : need) {
        tex_ensure_current(t, *F);
        tex_transition(t, *F, tex_sampled_layout(t));
    }
    for (Texture* t : need_storage) {
        if (tex_scale(t) != 1.0f) tex_rescale(t, *F, 1.0f, "bound as a storage image");   // imageStore coordinates are not scaled (Ryujinx: ResScaleUnsupported)
        tex_ensure_current(t, *F);
        tex_transition(t, *F, VK_IMAGE_LAYOUT_GENERAL);
        tex_mark_gpu_written(t);
    }
    for (Texture* t : need_general) {                      // after the storage images: the same image is left in GENERAL either way
        tex_ensure_current(t, *F);
        tex_transition(t, *F, VK_IMAGE_LAYOUT_GENERAL);
    }
    gstamp("uploads/layouts");
}

// ---- compute shadows: storage and texel buffers of a dispatch in device-local memory ---------------------------------------------
// Pools are the game's own memory imported into Vulkan, so a shader reaches storage buffers and texel buffers over PCIe. Compute shaders
// that work on such buffers (the exposure histogram's atomics, the 4 MiB simulation buffers) took 9 ms each on an RTX 3080. [nvn]
// compute_shadows (default on): each storage or texel buffer range a dispatch binds (up to 16 MiB) is bound from device-local memory.
//  * Direct ([nvn] compute_direct, default on): the range is in a pool with a device-local copy (nvn_gpu_pool.cpp, the pool mirror). The
//    dispatch binds the copy itself. Pages the copy does not hold (the CPU wrote them since the GPU last used them) are copied in from the
//    pool first (mirror_fill), so only the CPU's writes cross PCIe. A range the shader may write is written in the copy; it goes back to the
//    pool once, when something other than a compute binding of the same range could look at it: before any command other than a dispatch,
//    a state change or a barrier, before a dispatch reads it another way (uniform block, indirect arguments, another range on its pages,
//    a texture upload), and at the end of the submission, before the serial the CPU waits for can signal. A run of dispatches over the
//    same buffers thus copies each result out once instead of once per dispatch.
//  * Shadow (otherwise: no copy for the pool, hot or banned pages, a range over two chunks): the range is copied into a buffer of its own
//    before the dispatch and copied back after it when the shader can write it (and into the pool copy, when there is one).
// Game memory holds every result by the end of the submission, in queue order, as before. Ranges that overlap another range of the same
// dispatch stay in the pool.
struct Shadow { gfx::Buffer buf; VkDeviceSize size = 0; uint64_t last = 0; };
static std::unordered_map<uint64_t, Shadow> g_shadows;            // by GPU address and size
struct ShadowUse { uint64_t addr; VkDeviceSize size; BufRef host; VkBuffer buf; VkDeviceSize off; bool writes; bool direct; MirrorSpan mirror; };
static std::vector<ShadowUse> g_shadow_uses;                       // this dispatch
// Results of direct bindings still only in the pool copy: nvn_gpu_pool.cpp (out_add ... out_end_submit, "compute out").
bool g_shadowing = false;
static bool g_sh_barrier = false, g_sh_copied = false;             // this dispatch: the barrier before the copies in is recorded / some copy in is
static uint64_t g_sh_uses = 0, g_sh_from_mirror = 0, g_sh_bytes_mirror = 0, g_sh_bytes_pool = 0, g_sh_bytes_out = 0, g_sh_no_span = 0;   // summary line
static uint64_t g_sh_direct = 0, g_sh_direct_filled = 0, g_sh_bytes_fill = 0, g_sh_bytes_dout = 0, g_sh_merged = 0, g_sh_flushes = 0;
constexpr VkDeviceSize kShadowMax = 16ull << 20;

static bool compute_shadows_on() {
    static const bool on = nxrt::config_get_bool("nvn", "compute_shadows", true);
    return on;
}
static bool compute_direct_on() {
    static const bool on = nxrt::config_get_bool("nvn", "compute_direct", true);
    return on;
}

// Every result still only in the pool copy goes back to the pool (in queue order, before what is recorded next).
void flush_compute_out(int why) {
    if (!out_any()) return;
    // [nvn] pool_copy: out entries need a pool copy (mirror) or a held result, and copy pools have neither (lazy pools get no mirror,
    // out_lazy is off), so nothing is held; were one there, its copy-back into the shadow would be unrecorded: say so once.
    if (pool_copy_on()) {
        static bool noted = false;
        if (!noted) { noted = true; NXLOG_W("nvn.replay", "pool_copy: a compute result is held in a pool copy; its copy-back is not recorded as a GPU write"); }
    }
    end_pass();
    if (out_flush(F->cb, 0, UINT64_MAX, why)) {
        ++g_sh_flushes;
        gstamp("compute out");
    }
}

// [addr, addr + size) is about to be read or written some other way than a direct binding of exactly a held range: the results sharing a
// page with it go back first (a page granular test: the pool copy is filled and tracked by whole pages).
void compute_out_guard(uint64_t addr, uint64_t size, int why) {
    if (!size || !out_overlaps(addr, size)) return;
    end_pass();
    if (out_flush(F->cb, addr, size, why)) {
        ++g_sh_flushes;
        gstamp("compute out");
    }
}

// A buffer a draw reads: results it shares a page with go back first, unless the draw binds the pool copy for it (vertex, index and aligned
// uniform buffers whose pages are all current there, resolve_read).
void out_draw_read(uint64_t addr, uint64_t size, bool copy_ok, int why) {
    if (!addr) return;
    if (!size) size = 65536;
    if (!out_overlaps(addr, size)) return;
    if (copy_ok && out_servable(addr, size)) return;
    compute_out_guard(addr, size, why);
}

// The buffer for [addr, +size) that a command reads through a plain buffer binding (storage buffers, indirect arguments, copy sources): the
// pool copy when it holds a compute result there and is current for the whole range, else the pool, after the results there went back
// (flush: false inside a pass, where out_before_op has done that already).
BufRef held_or_pool(uint64_t addr, uint64_t size, int why, bool flush) {
    if (size && out_overlaps(addr, size)) {
        if (out_lazy() && out_servable(addr, size)) {
            const MirrorSpan ms = mirror_span(addr, size);
            BufRef r = resolve_buffer(addr, size);
            if (ms.buf && r.ok) {
                r.buffer = ms.buf;
                r.offset = ms.offset;
                r.avail = ms.avail;
                return r;
            }
        }
        if (flush) compute_out_guard(addr, size, why);
    }
    return resolve_buffer(addr, size);
}

// The reads of a draw's bound buffers (out_before_op).
static void out_draw_bindings() {
    for (const GState::Bind& b : S.vtx) out_draw_read(b.addr, b.size, true, kOutDrawBuffer);
    const VkDeviceSize ualign = device().properties().limits.minUniformBufferOffsetAlignment;
    for (int st = 0; st < kCompute; ++st) {
        if (!S.stage[st]) continue;
        for (const GState::Bind& b : S.ubo[st]) out_draw_read(b.addr, b.size, ualign && (b.addr % ualign) == 0, kOutDrawBuffer);
        for (const GState::Bind& b : S.ssbo[st]) out_draw_read(b.addr, b.size, true, kOutStorageXfb);   // bound from the copy (build_set)
    }
    for (const GState::Bind& b : S.xfb) out_draw_read(b.addr, b.size, false, kOutStorageXfb);
}

// The bytes from index_address an indexed indirect draw reads: max over its draws of (first index + count) * index size, from the
// arguments (and draw count) when the CPU can read them (not a held result, not written by a submission that has not completed).
// Otherwise the GPU writes them too and nobody knows the count here: everything from index_address to the end of its pool.
uint64_t indirect_index_span(const P_DrawIndirect& p) {
    PoolReasonScope pr_(kReasonIndirect);
    const uint32_t isz0 = index_size_bytes(static_cast<int>(p.type));
    const uint64_t isz = isz0 ? isz0 : 4;
    const uint64_t stride = p.stride ? static_cast<uint64_t>(p.stride) : 20;
    const auto whole = [&] { const BufRef b = resolve_buffer(p.index_address, 1); return b.ok && b.avail ? static_cast<uint64_t>(b.avail) : uint64_t{1} << 20; };
    uint64_t n = 1;
    if (p.multi && p.count_address) {
        const uint8_t* c = out_overlaps(p.count_address, 4) ? nullptr : cpu_view(p.count_address, 4);
        if (!c) return whole();
        uint32_t v = 0;
        std::memcpy(&v, c, 4);
        n = std::min<uint64_t>(v, p.max_count);
    }
    if (!n) return 0;
    const uint64_t args = stride * (n - 1) + 20;
    const uint8_t* a = out_overlaps(p.indirect_address, args) ? nullptr : cpu_view(p.indirect_address, args);
    if (!a) return whole();
    uint64_t end = 0;
    for (uint64_t i = 0; i < n; ++i) {
        uint32_t v[3];
        std::memcpy(v, a + i * stride, sizeof v);   // count, instance count, first index
        if (!indirect_args_plausible(v[0], v[1])) return whole();   // not what the GPU will read (stale): the whole pool, as when unreadable
        if (v[0] && v[1]) end = std::max(end, (static_cast<uint64_t>(v[2]) + v[0]) * isz);
    }
    return end;
}

// [nvn] compute_out_lazy: before op, the results it may read or overwrite through the pool go back (instead of all of them before any
// command that is not a dispatch). Ops that only touch images, state and syncs leave them in the copy; a fence's serial is the
// submission's, and out_end_submit decides at its end.
void out_before_op(const RecordReader& r) {
    if (!out_any()) return;
    switch (r.op()) {
        case Op::Dispatch: case Op::DispatchIndirect: case Op::Barrier: case Op::CallCommands: case Op::PushDebugGroup: case Op::PopDebugGroup:
        case Op::ClearColor: case Op::ClearDepthStencil: case Op::ClearTexture: case Op::CopyTextureToTexture: case Op::Downsample:
        case Op::DrawTexture: case Op::DiscardColor: case Op::DiscardDepthStencil: case Op::ResolveDepth: case Op::FenceSync: case Op::WaitSync:
        case Op::ResetCounter: case Op::WaitEvent: case Op::SetRenderEnable:
            return;
        case Op::CopyBufferToTexture: case Op::CopyTextureToBuffer:    // do_copy_buffer_texture: its own range (it knows the size)
            return;
        case Op::DrawArrays:
        case Op::DrawElements: {
            out_draw_bindings();
            if (r.op() == Op::DrawElements) {
                const auto& p = r.payload<P_DrawElements>();
                const uint32_t isz = index_size_bytes(static_cast<int>(p.type));
                out_draw_read(p.index_address, static_cast<uint64_t>(p.count) * (isz ? isz : 4), true, kOutDrawBuffer);
            }
            return;
        }
        case Op::DrawIndirect: {
            out_draw_bindings();
            const auto& p = r.payload<P_DrawIndirect>();
            const bool indexed = p.type != 0xffffffffu;
            const uint64_t cmd = indexed ? 20 : 16;
            const uint64_t stride = p.stride ? static_cast<uint64_t>(p.stride) : cmd;
            const uint64_t n = p.multi && p.count_address ? std::max<uint64_t>(1, p.max_count) : 1;
            out_draw_read(p.indirect_address, stride * (n - 1) + cmd, true, kOutIndirect);   // do_draw_indirect binds the copy for them
            if (p.multi && p.count_address) out_draw_read(p.count_address, 4, true, kOutIndirect);
            if (indexed) compute_out_guard(p.index_address, indirect_index_span(p), kOutIndirect);   // the draw binds the pool for its indices
            return;
        }
        case Op::ClearBuffer: {
            const auto& p = r.payload<P_ClearBuffer>();
            compute_out_guard(p.address, p.size, kOutBufferWrite);
            return;
        }
        case Op::CopyBufferToBuffer: {
            const auto& p = r.payload<P_CopyBufferToBuffer>();
            out_draw_read(p.src, p.size, true, kOutBufferRead);       // the copy reads the pool copy (held_or_pool)
            compute_out_guard(p.dst, p.size, kOutBufferWrite);
            return;
        }
        case Op::UpdateUniformBuffer: {
            const auto& p = r.payload<P_UpdateUniformBuffer>();
            compute_out_guard(p.address + static_cast<uint64_t>(p.offset), p.size, kOutBufferWrite);
            return;
        }
        case Op::ReportCounter: compute_out_guard(r.payload<P_Counter>().address, 32, kOutBufferWrite); return;
        case Op::ReportValue: compute_out_guard(r.payload<P_ReportValue>().address, 32, kOutBufferWrite); return;
        case Op::SignalEvent: {
            const auto& p = r.payload<P_Event>();
            if (p.event && p.event->pool && p.event->pool->base)
                compute_out_guard(p.event->pool->gpu_base + static_cast<uint64_t>(p.event->offset), 4, kOutEvent);
            return;
        }
        case Op::DrawXfb: flush_compute_out(kOutIndirect); return;
        default:
            if (r.op() < Op::DrawArrays) return;    // state
            flush_compute_out(kOutOther);           // feedback begin/end and the rest: everything first
            return;
    }
}

static void sh_barrier_once() {
    if (g_sh_barrier) return;
    memory_barrier(F->cb);       // earlier writes to these ranges (any command) before the copies read them
    g_sh_barrier = true;
}

// The buffer a dispatch binds for [addr, addr + size): the pool copy, the shadow (its copy-in recorded now), or `host` when it cannot have
// either.
BufRef shadow_of(uint64_t addr, VkDeviceSize size, const BufRef& host, bool writes) {
    if (!g_shadowing || !host.ok || size == 0) return host;
    for (ShadowUse& u : g_shadow_uses) {
        if (u.addr == addr && u.size == size) {           // the same range bound twice: one binding
            u.writes |= writes;
            BufRef r = host; r.buffer = u.buf; r.offset = u.off; r.avail = size;
            return r;
        }
        if (addr < u.addr + u.size && u.addr < addr + size) {   // overlapping ranges stay coherent in the pool
            compute_out_guard(addr, size, kOutDispatch);
            return host;
        }
    }
    // a result an earlier dispatch left in the pool copy: this dispatch binds it there again (the pages are not valid for the tracker
    // until the result goes back, so mirror_span would not say so)
    {
        VkBuffer ob = VK_NULL_HANDLE;
        VkDeviceSize oo = 0;
        if (out_exact(addr, size, &ob, &oo)) {
            ++g_sh_uses;
            ++g_sh_direct;
            g_shadow_uses.push_back({addr, size, host, ob, oo, writes, true, MirrorSpan{ob, oo, true}});
            BufRef r = host; r.buffer = ob; r.offset = oo; r.avail = size;
            return r;
        }
    }
    if (!out_lazy()) compute_out_guard(addr, size, kOutDispatch);
    if (size > kShadowMax) { compute_out_guard(addr, size, kOutDispatch); return host; }
    const MirrorSpan ms = mirror_span(addr, size);    // pages holding a compute result count as current there (compute_out_lazy)
    if (ms.buf && compute_direct_on()) {
        bool ok = ms.valid;
        if (!ok) {
            sh_barrier_once();
            uint64_t filled = 0;
            ok = mirror_fill(F->cb, addr, size, &filled);
            if (filled) { g_sh_copied = true; g_sh_bytes_fill += filled; }
            if (ok) ++g_sh_direct_filled;
        }
        if (ok) {
            ++g_sh_uses;
            ++g_sh_direct;
            g_shadow_uses.push_back({addr, size, host, ms.buf, ms.offset, writes, true, ms});
            BufRef r = host; r.buffer = ms.buf; r.offset = ms.offset; r.avail = size;
            return r;
        }
    }
    compute_out_guard(addr, size, kOutDispatch);      // the shadow reads the pool: held results there go back first
    const uint64_t key = addr ^ (static_cast<uint64_t>(size) << 40) ^ (static_cast<uint64_t>(size) >> 24);
    Shadow& sh = g_shadows[key];
    if (!sh.buf || sh.size != size) {
        if (sh.buf) { gfx::Buffer old = sh.buf; forget_buffer_views(old.buffer); defer_destroy([old]() mutable { device().destroy_buffer(old); }); sh.buf = gfx::Buffer{}; }
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (device().create_buffer(size, usage, gfx::MemoryUse::GpuOnly, &sh.buf, "nvn compute shadow") != VK_SUCCESS) {
            sh.buf = gfx::Buffer{};
            g_shadows.erase(key);
            return host;
        }
        sh.size = size;
    }
    sh.last = F->serial;
    sh_barrier_once();
    // The device-local pool copy, when it holds the range, is the cheap source (video memory to video memory instead of over PCIe); after a
    // dispatch that writes the range, the result goes to both, so the next dispatch finds the copy valid again (mirror_span_written).
    ++g_sh_uses;
    if (!ms.buf) ++g_sh_no_span;
    if (ms.valid) { ++g_sh_from_mirror; g_sh_bytes_mirror += size; } else g_sh_bytes_pool += size;
    VkBufferCopy c = {ms.valid ? ms.offset : host.offset, 0, size};
    vkCmdCopyBuffer(F->cb, ms.valid ? ms.buf : host.buffer, sh.buf.buffer, 1, &c);
    g_sh_copied = true;
    g_shadow_uses.push_back({addr, size, host, sh.buf.buffer, 0, writes, false, ms});
    BufRef r = host;
    r.buffer = sh.buf.buffer;
    r.offset = 0;
    r.avail = size;
    return r;
}

// After the dispatch: the written shadows go back to the pool; written direct ranges wait in g_out_pending. Shadows unused for a while
// are freed.
static void shadows_after_dispatch() {
    bool any = false;
    for (const ShadowUse& u : g_shadow_uses) {
        if (!u.writes) continue;
        if (u.direct) {
            if (out_add(u.addr, u.size, u.buf, u.off, u.host)) ++g_sh_merged;   // written again before it went back: one copy out for both
            continue;
        }
        if (!any) { memory_barrier(F->cb); any = true; }
        VkBufferCopy c = {0, u.host.offset, u.size};
        vkCmdCopyBuffer(F->cb, u.buf, u.host.buffer, 1, &c);
        // [nvn] pool_copy: u.host is a shadow; the binding recorded this range as a GPU write already (gpu_wrote), the copy-back is that write
        if (pool_copy_on()) pcopy_note_write(u.addr, u.size, u.host.buffer);
        g_sh_bytes_out += u.size;
        if (u.mirror.buf) {
            VkBufferCopy mc = {0, u.mirror.offset, u.size};
            vkCmdCopyBuffer(F->cb, u.buf, u.mirror.buf, 1, &mc);
            mirror_span_written(u.addr, u.size);
        }
    }
    if (any) gstamp("shadow out");
    g_shadow_uses.clear();
    g_shadowing = false;
    static uint64_t last_sweep = 0;
    if (F->serial - last_sweep > 600) {
        last_sweep = F->serial;
        NXLOG_I("nvn.replay", "compute shadows: %llu bound (%llu from the pool copy, %llu with no copy), in from the copy %llu MiB, in from the pool %llu MiB, out %llu MiB; "
                "direct to the pool copy %llu (%llu after filling pages, in %llu MiB, out %llu MiB in %llu flushes, %llu writes merged)",
                (unsigned long long)g_sh_uses, (unsigned long long)g_sh_from_mirror, (unsigned long long)g_sh_no_span, (unsigned long long)(g_sh_bytes_mirror >> 20),
                (unsigned long long)(g_sh_bytes_pool >> 20), (unsigned long long)(g_sh_bytes_out >> 20), (unsigned long long)g_sh_direct,
                (unsigned long long)g_sh_direct_filled, (unsigned long long)(g_sh_bytes_fill >> 20), (unsigned long long)(out_bytes_total() >> 20),
                (unsigned long long)g_sh_flushes, (unsigned long long)g_sh_merged);
        for (auto it = g_shadows.begin(); it != g_shadows.end();) {
            if (F->serial - it->second.last > 600) {
                gfx::Buffer old = it->second.buf;
                forget_buffer_views(old.buffer);
                defer_destroy([old]() mutable { device().destroy_buffer(old); });
                it = g_shadows.erase(it);
            } else {
                ++it;
            }
        }
    }
}

Texture* g_su_tex = nullptr;                 // scene upscaler override (S14), see scene_upscale_for_draw; without g_su_view: sampled nearest
VkImageView g_su_view = VK_NULL_HANDLE;
static Ref<Texture> g_tsr_depth;                   // temporal upscaling: the depth target of the G-buffer draw that gave this frame's camera
static uint64_t g_tsr_frame = 0;                    // temporal upscaling: tone-map draws so far (the jitter phase of the frame being drawn)
// Temporal upscaling: a copy of a vertex slot 0 camera block with this frame's jitter added (extras::temporal_jitter_block). False: left alone.
bool tsr_jitter(uint32_t* words, size_t n_words) {
    Texture* d = S.depth_tex.get();
    if (!d || !tex_gpu(d)) return false;
    const VkExtent3D e = tex_image_extent(d, 0);
    float fx = 1.0f, fy = 1.0f;
    extras::scene_fraction(&fx, &fy);   // the scene's viewport is that part of the target: half a pixel of it
    const uint32_t w = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(e.width * fx)));
    const uint32_t h = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(e.height * fy)));
    return extras::temporal_jitter_block(words, n_words, w, h, g_tsr_frame);
}

// Descriptor set number `set` of the bound program: the one of the last draw (SetMemo below), a set of this submission with the same contents (g_set_cache), or a new one.
// Render thread only (scratch arrays below are reused).
// [nvn] set_reuse: the inputs of the last set built for each set number. A set whose bindings are all sampled textures (combined image
// samplers by binding or by handle in a uniform block) resolves to the same descriptors as long as the program, the handles, the pools,
// the replay epoch (g_rp_epoch: passes, render targets, feedback and 3D copies, GPU writes, submissions), be::g_bind_gen (pool
// registrations, texture images made, replaced or released) and the scene upscaler's override are the same: such a draw takes the set of
// the one before without resolving every texture again (about 45 % of the texture sets of a frame are the one before, typically a
// per-object uniform block changed between two draws of one material). Texel buffer, storage and uniform bindings are never reused
// (their resolution has side effects or reads pool copies that change inside a submission).
struct SetMemo {
    const ProgLayout* pl = nullptr;
    uint64_t epoch = 0, gen = 0;
    const void* tpool = nullptr;
    const void* spool = nullptr;
    const void* su_tex = nullptr;
    VkImageView su_view = VK_NULL_HANDLE;
    uint32_t n = 0;
    uint64_t h[32] = {};
    VkDescriptorSet ds = VK_NULL_HANDLE;
};
static SetMemo g_set_memo[4];
// Fills m with the current inputs of set set of pl; false when the set cannot be reused (a binding of another kind, too many bindings).
static bool set_memo_inputs(const ProgLayout& pl, uint32_t set, SetMemo& m) {
    m.pl = &pl;
    m.epoch = g_rp_epoch;
    m.gen = be::g_bind_gen.load(std::memory_order_relaxed);
    m.tpool = S.tpool.get();
    m.spool = S.spool.get();
    m.su_tex = g_su_tex;
    m.su_view = g_su_view;
    m.n = 0;
    for (const BindingDesc& d : pl.bindings) {
        if (d.set != set) continue;
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || m.n >= 32) return false;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        uint64_t handle = 0;
        if (b.nvn.source == NvnSource::SamplerBinding) {
            if (b.nvn.index >= 0 && b.nvn.index < 64) handle = S.tex[d.nvn_stage][b.nvn.index];
        } else if (b.nvn.source == NvnSource::HandleInUniform) {
            handle = read_handle_word(d.nvn_stage, b.nvn.bank, b.nvn.word);
        } else if (b.nvn.source == NvnSource::ImageBinding) {
            if (b.nvn.index >= 0 && b.nvn.index < 16) handle = S.img[d.nvn_stage][b.nvn.index];
        } else {
            return false;
        }
        m.h[m.n++] = handle;
    }
    return m.n > 0;
}
static bool set_memo_same(const SetMemo& a, const SetMemo& b) {
    return a.pl == b.pl && a.epoch == b.epoch && a.gen == b.gen && a.tpool == b.tpool && a.spool == b.spool && a.su_tex == b.su_tex &&
           a.su_view == b.su_view && a.n == b.n && std::memcmp(a.h, b.h, sizeof(uint64_t) * a.n) == 0;
}
// c[0] of a stage that reads it, filled from the current bindings (nvn_driver_bank.h): a Switch 2 compute program's resource table
// (storage buffer sizes for the bounds checks of the light culling, GPU particle and volume field kernels); a graphics stage's storage
// buffer table, point size and the draw's base instance. Copied into the upload ring once per distinct content per submission. A stage
// the fill does not cover (Switch 1 compute) gets the shared bank (zeros, point size).
static VkDescriptorBufferInfo driver_bank_for(const BindingDesc& d) {
    VkDeviceSize range = 0;
    VkBuffer shared = driver_constants_buffer(&range);
    VkDescriptorBufferInfo bi = {shared ? shared : dummy_buffer(), 0, shared ? range : 65536};
    const ShaderControl& c = d.shader->control;
    const bool compute = d.nvn_stage == kCompute;
    if (compute && (!c.nvn2 || c.stage != 5)) return bi;
    if (d.nvn_stage < 0 || d.nvn_stage > kCompute) return bi;
    uint8_t bank[kDriverBankBytes];
    DriverRange ubo[16], ssbo[16];
    for (int n = 0; n < 16; ++n) {
        ubo[n] = {S.ubo[d.nvn_stage][n].addr, S.ubo[d.nvn_stage][n].size};
        ssbo[n] = {S.ssbo[d.nvn_stage][n].addr, S.ssbo[d.nvn_stage][n].size};
    }
    const uint32_t used = compute ? fill_driver_bank_nvn2_compute(bank, c, ubo, ssbo, S.tex[kCompute], S.img[kCompute])
                                  : fill_driver_bank_graphics(bank, d.nvn_stage, ssbo, g_draw_base_instance);
    uint64_t key = 1469598103934665603ull ^ used;
    for (uint32_t i = 0; i < used; ++i) key = (key ^ bank[i]) * 1099511628211ull;
    auto it = g_driver_banks.find(key);
    if (it == g_driver_banks.end()) {
        const VkDeviceSize align = std::max<VkDeviceSize>(device().properties().limits.minUniformBufferOffsetAlignment, 256);
        UploadAlloc u = F->upload(kDriverBankBytes, align);
        if (!u) return bi;
        std::memcpy(u.ptr, bank, kDriverBankBytes);
        it = g_driver_banks.emplace(key, u).first;
    }
    return {it->second.buffer, it->second.offset, kDriverBankBytes};
}

static VkDescriptorSet build_set_resolved(const ProgLayout& pl, uint32_t set);
static VkDescriptorSet build_set(const ProgLayout& pl, uint32_t set) {
    static const bool reuse_on = set_reuse_on();
    if (!reuse_on || g_trace || g_shadowing) return build_set_resolved(pl, set);
    SetMemo now;
    if (!set_memo_inputs(pl, set, now)) return build_set_resolved(pl, set);
    SetMemo& last = g_set_memo[set & 3];
    if (last.ds && set_memo_same(now, last)) {
        rp_count(kRpReuse);
        ft::split_count(ft::kCntSetHit);
        return last.ds;
    }
    ft::split_count(ft::kCntSetMiss);
    now.ds = build_set_resolved(pl, set);
    last = now;
    return now.ds;
}

// The set resolved from the current state: a set of this submission with the same contents (g_set_cache), or a new one.
static VkDescriptorSet build_set_resolved(const ProgLayout& pl, uint32_t set) {
    RpScope rp_(kRpSetBuild);
    RpScope rps_(kRpSet0 + static_cast<int>(set & 3));
    // reused across calls (no heap allocation per draw); the pointers the writes keep stay valid: capacity is reserved for every binding
    static std::vector<VkWriteDescriptorSet> writes;
    static std::vector<VkDescriptorBufferInfo> binfo;
    static std::vector<VkDescriptorImageInfo> iinfo;
    static std::vector<VkBufferView> tinfo;
    writes.clear();
    binfo.clear();
    iinfo.clear();
    tinfo.clear();
    const size_t nb = pl.bindings.size();
    writes.reserve(nb);
    binfo.reserve(nb);
    iinfo.reserve(nb);
    tinfo.reserve(nb);
    uint64_t key = 1469598103934665603ull + set;
    auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
    mix(reinterpret_cast<uint64_t>(pl.layout));
    for (const BindingDesc& d : pl.bindings) {
        if (d.set != set) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = d.binding;
        w.descriptorCount = 1;
        w.descriptorType = d.type;
        PoolReasonScope pr_(d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? kReasonUniform : d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? kReasonStorage :
                            d.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ? kReasonTexel : pool_reason());
        if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            RpScope rpb_(kRpBuf, g_rp_fine);
            VkDescriptorBufferInfo bi = {dummy_buffer(), 0, 65536};
            switch (b.nvn.source) {
                case NvnSource::SupportBuffer:
                    bi = g_draw_support ? VkDescriptorBufferInfo{g_draw_support.buffer, g_draw_support.offset, sizeof(SupportBuffer)}
                                        : VkDescriptorBufferInfo{g_support.buffer, g_support.offset, sizeof(SupportBuffer)};
                    break;
                case NvnSource::DriverConstants:
                    bi = driver_bank_for(d);
                    break;
                case NvnSource::ConstantData: {
                    VkDeviceSize range = 0;
                    VkBuffer buf = constant_data_buffer(d.shader, &range);
                    if (buf) bi = {buf, 0, range};
                    break;
                }
                case NvnSource::UniformBlock: {
                    const int idx = b.nvn.index;
                    if (idx >= 0 && idx < 16 && S.ubo[d.nvn_stage][idx].addr) {
                        const GState::Bind& bind = S.ubo[d.nvn_stage][idx];
                        if (d.nvn_stage == kCompute || !out_lazy()) {                   // draws: checked before the op (out_before_op)
                            const VkDeviceSize ua = device().properties().limits.minUniformBufferOffsetAlignment;
                            out_draw_read(bind.addr, bind.size ? bind.size : 65536, ua && (bind.addr % ua) == 0, kOutDrawBuffer);   // resolve_read serves the copy
                        }
                        BufRef br = resolve_read(bind.addr, bind.size, kReadUniform);
                        const VkDeviceSize align = device().properties().limits.minUniformBufferOffsetAlignment;
                        const float ufs = ufs_scale_for(d, idx);
                        const bool tsr = d.nvn_stage == kVertex && idx == 0 && bind.size >= 272 && extras::temporal_wanted() && S.depth_tex;
                        if (br.ok && (ufs != 1.0f || tsr)) {
                            // the game's scale on some floats of this block (nxrt/gfx_tuning.h): bound from a patched copy in the upload ring
                            const VkDeviceSize n = std::min<VkDeviceSize>({bind.size ? bind.size : 65536, br.avail, 65536});
                            UploadAlloc u = F->upload(n, std::max<VkDeviceSize>(align, 256));
                            if (u) {
                                std::memcpy(u.ptr, host_ptr(bind.addr), n);
                                if (tsr && n >= 272) tsr_jitter(reinterpret_cast<uint32_t*>(u.ptr), static_cast<size_t>(n / 4));   // temporal upscaling
                                const uint32_t mask = ufs != 1.0f ? nxrt::gfx::tuning().ufs_mask.load(std::memory_order_relaxed) : 0u;
                                for (uint32_t i = 0; i < 32; ++i) {
                                    if (!(mask & (1u << i)) || (i + 1) * 4 > n) continue;
                                    float v;
                                    std::memcpy(&v, static_cast<uint8_t*>(u.ptr) + i * 4, 4);
                                    v *= ufs;
                                    std::memcpy(static_cast<uint8_t*>(u.ptr) + i * 4, &v, 4);
                                }
                                bi = {u.buffer, u.offset, n};
                            }
                        } else if (br.ok && (br.offset % align) == 0) {
                            bi = {br.buffer, br.offset, std::min<VkDeviceSize>({bind.size ? bind.size : 65536, br.avail, 65536})};
                        } else if (br.ok) {
                            // unaligned binding: copy into the upload ring
                            const VkDeviceSize n = std::min<VkDeviceSize>(bind.size ? bind.size : 65536, 65536);
                            UploadAlloc u = F->upload(n, std::max<VkDeviceSize>(align, 256));
                            if (u) {
                                std::memcpy(u.ptr, host_ptr(bind.addr), n);
                                bi = {u.buffer, u.offset, n};
                            }
                        }
                    }
                    break;
                }
                case NvnSource::StorageBuffer: {
                    const int slot = b.nvn.index >= 0 ? b.nvn.index : storage_slot_for(d.nvn_stage, b.nvn.c0_offset);   // NVN2 compute: index from the c[0] table
                    if (slot >= 0 && slot < 16 && S.ssbo[d.nvn_stage][slot].addr) {
                        const GState::Bind& bind = S.ssbo[d.nvn_stage][slot];
                        BufRef br = resolve_buffer(bind.addr, bind.size);
                        bi = {br.buffer, br.offset, std::min<VkDeviceSize>(bind.size ? bind.size : br.avail, br.avail)};
                        if (!g_shadowing && br.ok && out_lazy() && out_overlaps(bind.addr, bi.range)) {   // a draw over a held compute result: the copy
                            const BufRef hb = held_or_pool(bind.addr, bi.range, kOutStorageXfb, false);
                            if (hb.buffer != br.buffer) {
                                bi.buffer = hb.buffer;
                                bi.offset = hb.offset;
                                gpu_wrote(bind.addr, bi.range, kWriteStorage);
                                out_add(bind.addr, bi.range, hb.buffer, hb.offset, br);   // what the draw writes there is held like a dispatch's
                                break;
                            }
                        }
                        if (g_shadowing && br.ok) {                                  // before gpu_wrote: the shadow reads the pool copy while valid
                            const BufRef sh = shadow_of(bind.addr, bi.range, br, true);
                            bi.buffer = sh.buffer;
                            bi.offset = sh.offset;
                        }
                        if (br.ok) gpu_wrote(bind.addr, bi.range, kWriteStorage);      // a storage buffer may be written by the shader
                    }
                    break;
                }
                case NvnSource::UniformAsStorage: {
                    // a compute shader that reads uniform block n through its c[0] descriptor: the block's buffer as a storage buffer
                    const int idx = b.nvn.index;
                    if (idx >= 0 && idx < 16 && S.ubo[d.nvn_stage][idx].addr) {
                        const GState::Bind& bind = S.ubo[d.nvn_stage][idx];
                        const BufRef br0 = resolve_buffer(bind.addr, bind.size);
                        const VkDeviceSize range = std::min<VkDeviceSize>(bind.size ? bind.size : br0.avail, br0.avail);
                        const BufRef br = held_or_pool(bind.addr, range, kOutDispatch, true);
                        bi = {br.buffer, br.offset, range};
                    }
                    break;
                }
                default:
                    note_unsupported("shader buffer binding of an unknown NVN source");
                    break;
            }
            binfo.push_back(bi);
            w.pBufferInfo = &binfo.back();
            mix(reinterpret_cast<uint64_t>(bi.buffer)); mix(bi.offset); mix(bi.range);
        } else if (d.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            RpScope rpt_(kRpTex, g_rp_fine);
            VkDescriptorImageInfo ii = {dummy_sampler(), dummy_view(view_type_for(b), b.image.depth), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            TexResolved r = resolve_texture(d);
            if (b.nvn.source == NvnSource::ImageBinding) {
                image_as_texture(d, b, r, ii);
                r.tex = nullptr;                                   // none of the texture paths below apply
            }
            if (r.tex) {
                const auto fb = g_fb_use.find(r.tex);
                if (fb != g_fb_use.end()) r.tex = fb->second;      // a feedback draw samples the copy made before it
            }
            if (r.tex && wants_vol3d(b, r)) {                      // a sampler3D over 2D storage samples the 3D copy (vol3d_prepare)
                const auto vc = g_vol3d_use.find(r.tex);
                if (vc != g_vol3d_use.end()) {
                    r.tex = vc->second;
                    r.view.has_layers = false;                     // the copy is the chosen layer, one slice deep
                }
            }
            // [nvn] debug_null_format = "BPTC_UFLOAT" or "RGBA8:256x64": every sampled texture of that NVN format (and size) gets the dummy view
            // (term isolation, debugging only)
            static const std::string null_fmt = nxrt::config_get_str("nvn", "debug_null_format", "");
            if (r.tex && !null_fmt.empty() && r.tex->fmt) {
                const std::string sized = std::string(r.tex->fmt->name) + ":" + std::to_string(r.tex->b.width) + "x" + std::to_string(r.tex->b.height);
                if (null_fmt == r.tex->fmt->name || null_fmt == sized) r.tex = nullptr;
            }
            // a compute dispatch is never inside the graphics pass: a colour attachment of the last pass is an ordinary texture to it
            const bool feedback = r.tex && d.nvn_stage != kCompute && is_color_feedback(r.tex);
            const bool loop = feedback && fbl_sampled(r.tex);      // sampled in the feedback loop layout of the open pass (feedback_prepare)
            // a shader that samples with depth comparison (sampler2DShadow) needs a depth-format image: the game leaves colour textures in some
            // of those slots, which Vulkan does not allow (VUID-vkCmdDrawIndexed-None-06479). They get the depth dummy instead.
            const bool shadow_mismatch = r.tex && b.image.depth && !tex_is_depth(r.tex);
            if (shadow_mismatch) {   // the stand-in holds the texture's red value (dcmp_view), the console compares against it
                log_once("shadow-colour", "a depth-comparison sampler is bound to a colour texture: a depth stand-in with its red value is bound");
                if (VkImageView sv = dcmp_view(r.tex, view_type_for(b))) ii.imageView = sv;
                if (r.has_sampler) ii.sampler = sampler_for(r.smp);
            }
            if (r.tex && tex_gpu(r.tex) && (!feedback || loop) && !shadow_mismatch) {
                VkImageView v = tex_sampled_view(r.tex, make_request(b, r));
                if (v) {
                    ii.imageView = v;
                    ii.imageLayout = loop ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : tex_sampled_layout(r.tex);
                    if (r.has_sampler) ii.sampler = sampler_for(r.smp, r.tex);
                    if (r.tex == g_su_tex && g_su_view) {            // the upscaled scene replaces the small one for this draw (S14)
                        ii.imageView = g_su_view;
                        ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    } else if (r.tex == g_su_tex) {                  // Nearest: the small scene is enlarged unfiltered (nearest, clamp to edge)
                        ii.sampler = dummy_sampler();
                    }
                } else if (first_time_key("dummy-view " + d.shader->hash + " " + b.name + " " + std::to_string(r.tex->id))) {
                    NXLOG_W(kTag, "sampled texture %u (%s, target %d, %dx%dx%d) has no view of the shader's type %d: shader %s binding %s (NVN slot %d, set %u binding %u) gets the dummy view",
                            r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.target, r.tex->b.width, r.tex->b.height, r.tex->b.depth,
                            static_cast<int>(view_type_for(b)), d.shader->hash.c_str(), b.name.c_str(), b.nvn.index, d.set, d.binding);
                }
            } else if (feedback) {
                log_once("color-feedback", "a texture is sampled while it is a colour attachment of the pass: the dummy texture is bound");
                static int reported = 0;
                if (reported < 40) {   // which draw: the texture, the attachment's subresource, the sampled view, the shaders (S4g)
                    ++reported;
                    char tb[96];
                    uint32_t lvl = 0, lay = 0, nlay = 0;
                    for (uint32_t i = 0; i < S.ncolor; ++i) if (S.color_tex[i].get() == r.tex) { lvl = S.color_level[i]; lay = S.color_layer[i]; nlay = S.color_layers[i]; }
                    NXLOG_I("nvn.replay", "feedback: %s binding %s vs %s fs %s | attachment level %u layer %u layers %u | sampled view levels %d+%d layers %d+%d", tex_desc(r.tex, tb, sizeof tb),
                            b.name.c_str(), S.stage[kVertex] ? S.stage[kVertex]->hash.c_str() : "-", S.stage[kFragment] ? S.stage[kFragment]->hash.c_str() : "-", lvl, lay, nlay,
                            r.has_view && r.view.has_levels ? r.view.min_level : -1, r.has_view && r.view.has_levels ? r.view.num_levels : -1,
                            r.has_view && r.view.has_layers ? r.view.min_layer : -1, r.has_view && r.view.has_layers ? r.view.num_layers : -1);
                }
            }
            iinfo.push_back(ii);
            w.pImageInfo = &iinfo.back();
            mix(reinterpret_cast<uint64_t>(ii.imageView));
            mix(reinterpret_cast<uint64_t>(ii.sampler));
            mix(static_cast<uint64_t>(ii.imageLayout));
        } else if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            const VkFormat shader_fmt = image_format_vk(b.image.format);
            VkDescriptorImageInfo ii = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            TexResolved r = resolve_image(d);
            if (r.tex && tex_gpu(r.tex) && tex_has_storage(r.tex)) {
                ViewRequest q = make_request(b, r);
                q.swizzle = {};
                if (shader_fmt != VK_FORMAT_UNDEFINED) q.format = shader_fmt;
                q.format = tex_storage_format(r.tex, q.format);   // an sRGB image is stored to through its UNORM view (raw bits, as on the console)
                ii.imageView = tex_sampled_view(r.tex, q);
            }
            if (!ii.imageView) {
                if (r.tex) NXLOG_W(kTag, "storage image: texture %u (%s %dx%dx%d) has no storage view (gpu %d, storage usage %d)", r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?",
                                   r.tex->b.width, r.tex->b.height, r.tex->b.depth, tex_gpu(r.tex) ? 1 : 0, tex_has_storage(r.tex) ? 1 : 0);
                log_once("storage-image-dummy", "a storage image binding has no usable texture: a dummy storage image is bound");
                ii.imageView = dummy_storage_view(view_type_for(b), shader_fmt);
            }
            iinfo.push_back(ii);
            w.pImageInfo = &iinfo.back();
            mix(reinterpret_cast<uint64_t>(ii.imageView));
        } else if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            RpScope rpx_(kRpTexel, g_rp_fine);
            // NVN BUFFER-target textures (samplerBuffer / imageBuffer): a texel buffer view over the pool memory that holds the texels
            const bool storage = d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            const VkFormat shader_fmt = storage ? image_format_vk(b.image.format) : VK_FORMAT_UNDEFINED;
            const int component = storage ? 0 : b.image.component == ScalarType::Float ? 1 : b.image.component == ScalarType::Sint ? 2
                                  : b.image.component == ScalarType::Uint ? 3 : 0;
            TexResolved r = storage ? resolve_image(d) : resolve_texture(d);
            uint64_t addr = 0, bytes = 0;
            VkFormat vfmt = VK_FORMAT_UNDEFINED;
            VkBufferView v = r.tex ? tex_buffer_view(r.tex, r.has_view ? &r.view : nullptr, component, shader_fmt, storage, &addr, &bytes, &vfmt) : VK_NULL_HANDLE;
            // a 2D/3D texture the game left in a samplerBuffer slot (e.g. KTGL's 8x8 default texture 3 in the foliage wind shader's
            // interaction buffers): read as a run of texels over its storage, as the console does, instead of the dummy
            if (v && r.tex->b.target != NVN_TEXTURE_TARGET_BUFFER && first_time_key("texel-nonbuf " + d.shader->hash + " " + b.name + " " + std::to_string(r.tex->id)))
                NXLOG_I(kTag, "texture %u (%s %dx%dx%d, target %d) is read as a texel buffer by %s binding %s: it gets a buffer view over its storage (%llu bytes)",
                        r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.width, r.tex->b.height, r.tex->b.depth, r.tex->b.target,
                        d.shader->hash.c_str(), b.name.c_str(), static_cast<unsigned long long>(bytes));
            if (v && g_shadowing) {                                        // before gpu_wrote: the shadow reads the pool copy while valid
                const BufRef host = resolve_buffer(addr, bytes);
                const BufRef sh = shadow_of(addr, bytes, host, storage);
                if (sh.buffer != host.buffer)
                    if (VkBufferView sv = make_buffer_view(sh.buffer, sh.offset, bytes, vfmt)) v = sv;   // offset: non-zero in the pool copy
            }
            if (v && !g_shadowing && out_lazy() && out_overlaps(addr, bytes)) {   // a draw reads a held compute result as texels: from the copy
                const MirrorSpan ms = out_servable(addr, bytes) ? mirror_span(addr, bytes) : MirrorSpan{};
                VkBufferView mv = ms.buf ? make_buffer_view(ms.buf, ms.offset, bytes, vfmt) : VK_NULL_HANDLE;
                if (mv) v = mv;
                else log_once("out-texel", "a draw reads a compute result held in the pool copy as a texel buffer the copy cannot serve: it reads the pool");
            }
            if (v && storage) gpu_wrote(addr, bytes, kWriteStorage);      // an image buffer may be written by the shader
            if (!v) {
                if (r.tex && first_time_key("texel-buffer " + d.shader->hash + " " + b.name + " " + std::to_string(r.tex->id)))
                    NXLOG_W(kTag, "texel buffer: texture %u (%s, target %d, %dx%dx%d, storage %llu bytes at %p) has no buffer view: shader %s binding %s (NVN slot %d, set %u binding %u)",
                            r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.target, r.tex->b.width, r.tex->b.height, r.tex->b.depth,
                            static_cast<unsigned long long>(r.tex->storage_size), static_cast<void*>(r.tex->mem), d.shader->hash.c_str(), b.name.c_str(),
                            b.nvn.index, d.set, d.binding);
                log_once("texel-buffer-dummy", "a texel buffer binding has no usable buffer texture: a dummy buffer view is bound");
                v = dummy_buffer_view(shader_fmt, storage);
            }
            if (!v) {
                note_unsupported("texel buffer descriptor without any view (the binding stays unwritten)");
                continue;
            }
            tinfo.push_back(v);
            w.pTexelBufferView = &tinfo.back();
            mix(reinterpret_cast<uint64_t>(v));
        } else {
            note_unsupported("descriptor of an unhandled type (the binding stays unwritten)");
            continue;
        }
        writes.push_back(w);
    }
    if (ft::on()) {   // profile: the same contents as the last build of this set number
        static uint64_t last[4];
        if (last[set & 3] == key) rp_count(kRpSame0 + static_cast<int>(set & 3));
        last[set & 3] = key;
    }
    auto it = g_set_cache.find(key);
    if (it != g_set_cache.end()) return it->second;
    RpScope rpw_(kRpSetWrite);
    rp_count(kRpMiss0 + static_cast<int>(set & 3));
    VkDescriptorSet ds = F->alloc_set(pl.sets[set]);
    if (!ds) return VK_NULL_HANDLE;
    for (auto& w : writes) w.dstSet = ds;
    vkUpdateDescriptorSets(vkdev(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    g_set_cache[key] = ds;
    return ds;
}

// ---- resolution scale: the support buffer's render_scale for a draw or dispatch (Ryujinx TextureBindingsManager.UpdateRenderScale) ----
// The translator scales texelFetch / imageLoad coordinates by render_scale[1 + i] and divides textureSize and gl_FragCoord.xy by the scale
// of texture i and of the render target ([0]). Texture i is the shader's i-th texture descriptor: the harness numbers single textures
// with one counter per shader (binding = stage slot * 128 + counter, + 64 for texel buffers), images follow the textures. Vertex shaders
// read their scales after the fragment shader's (frag_scale_count). Storage images are never scaled (tex_rescale), so images read 1.
void support_for_draw(const ProgLayout& pl, bool compute) {
    g_draw_support = UploadAlloc();
    if (render_scale() == 1.0f) return;
    float sc[73];
    for (float& v : sc) v = 1.0f;
    int frag_count = 0;
    bool any = false;
    if (!compute && P.scale != 1.0f) { sc[0] = P.scale; any = true; }
    // the fragment shader's textures first, then the vertex shader's after them
    const ShaderInfo* fs = compute ? nullptr : S.stage[kFragment];
    const ShaderInfo* vs = compute ? S.stage[kCompute] : S.stage[kVertex];
    auto tex_count = [](const ShaderInfo* sh) {
        int n = 0;
        for (const DescriptorBinding& b : sh->refl.bindings)
            if (b.count == 1 && (b.type == DescriptorType::CombinedImageSampler || b.type == DescriptorType::SampledImage || b.type == DescriptorType::UniformTexelBuffer ||
                                 b.type == DescriptorType::StorageImage || b.type == DescriptorType::StorageTexelBuffer))
                ++n;
        return n;
    };
    if (fs) frag_count = tex_count(fs);
    // the native size of the render target, for the fragment rule below
    Texture* rt = S.ncolor && S.color_tex[0] ? S.color_tex[0].get() : S.depth_tex.get();
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.count != 1 || b.binding >= 640) continue;
        const bool is_fs = d.shader == fs;
        if (!is_fs && d.shader != vs) continue;
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex) continue;
        const auto fb = g_fb_use.find(r.tex);
        if (fb != g_fb_use.end()) r.tex = fb->second;
        float s = tex_scale(r.tex);
        if (s == 1.0f) continue;
        const int idx = static_cast<int>(b.binding % 64u);
        const int slot = is_fs ? 1 + idx : (compute ? 1 + idx : 1 + frag_count + idx);
        if (slot >= 73) continue;
        if (is_fs && rt) {
            // Ryujinx: a texture whose size is the render target's size times one factor on both axes interpolates the scaled pixels
            // with gl_FragCoord (a negative scale tells TexelFetchScale to)
            const float rw = static_cast<float>(rt->b.width), rh = static_cast<float>(rt->b.height);
            if (r.tex->b.width > 0 && r.tex->b.height > 0 && rw / static_cast<float>(r.tex->b.width) == rh / static_cast<float>(r.tex->b.height)) s = -s;
        }
        sc[slot] = s;
        any = true;
    }
    if (!any) return;
    uint64_t key = 1469598103934665603ull + static_cast<uint64_t>(frag_count);
    for (float v : sc) { uint32_t u; std::memcpy(&u, &v, 4); key = (key ^ u) * 1099511628211ull; }
    auto it = g_support_scaled.find(key);
    if (it != g_support_scaled.end()) { g_draw_support = it->second; return; }
    UploadAlloc u = F->upload(sizeof(SupportBuffer), 256);
    if (!u) return;
    SupportBuffer sb = support_buffer_defaults();
    sb.frag_scale_count[0] = frag_count;
    for (int i = 0; i < 73; ++i) sb.render_scale[i][0] = sc[i];
    std::memcpy(u.ptr, &sb, sizeof sb);
    g_support_scaled[key] = u;
    g_draw_support = u;
}

void bind_sets(const ProgLayout& pl, VkPipelineBindPoint bp) {
    if (!bind_coalesce_on()) {
        for (uint32_t set = 0; set < 4; ++set) {
            if (!pl.has_set[set]) continue;
            VkDescriptorSet ds = build_set(pl, set);
            RpScope rp_(kRpSetBind);
            if (ds) vkCmdBindDescriptorSets(F->cb, bp, pl.layout, set, 1, &ds, 0, nullptr);
        }
        return;
    }
    // one call per run of consecutive sets
    VkDescriptorSet run[4];
    uint32_t first = 0, n = 0;
    auto flush = [&] {
        if (!n) return;
        RpScope rp_(kRpSetBind);
        vkCmdBindDescriptorSets(F->cb, bp, pl.layout, first, n, run, 0, nullptr);
        if (n > 1) ft::split_count(ft::kCntMerged, n - 1);
        n = 0;
    };
    for (uint32_t set = 0; set < 4; ++set) {
        if (!pl.has_set[set]) { flush(); continue; }
        VkDescriptorSet ds = build_set(pl, set);
        if (!ds) { flush(); continue; }
        if (!n) first = set;
        run[n++] = ds;
    }
    flush();
}

// ---- draws -------------------------------------------------------------------------------------------------------------------
static const ShaderInfo* tcs_now();   // the program's control shader, or the generated pass-through one (below)
static bool build_key(GraphicsKey& key, VkPrimitiveTopology topo) {
    std::memset(&key, 0, sizeof key);
    key.stage[0] = S.stage[kVertex];
    key.stage[1] = S.stage[kFragment];
    key.stage[2] = S.stage[kGeometry];
    if (topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) {      // begin_draw checked: both tessellation stages, patches
        key.tess[0] = tcs_now();
        key.tess[1] = S.stage[kTessEval];
        key.patch_control_points = static_cast<uint8_t>(std::clamp<uint32_t>(S.patch_size, 1, 32));
    }
    const ShaderInfo* vs = key.stage[0];
    // attributes: those the vertex shader reads. An input array takes one location per element. A location the game did not set up
    // (no attribute, or a disabled one) still has to exist in the pipeline; it reads (0, 0, 0, 1) from a dummy binding.
    struct Missing { uint32_t location; ScalarType type; };
    Missing missing[32];
    uint32_t nmissing = 0;
    for (const ShaderIo& in : vs->refl.inputs) {
        const uint32_t elems = in.array ? in.array : 1;
        for (uint32_t e = 0; e < elems; ++e) {
            const uint32_t loc = in.location + e;
            if (loc >= 32) break;
            VkFormat f = VK_FORMAT_UNDEFINED;
            const VertexAttribInfo* a = nullptr;
            if (loc < S.attr_count && loc < 16) {
                a = &S.attr[loc];
                f = vertex_format(a->format);
            }
            if (f == VK_FORMAT_UNDEFINED) {
                if (nmissing < 32) missing[nmissing++] = {loc, in.type};
                continue;
            }
            // a float input fed by an integer attribute gets the raw bits: the integer format stays and the pipeline patches the shader
            // (spirv_int_attributes); stream-out draws use the shader's stream-out variant, which is not patched: scaled floats there
            if (in.type == ScalarType::Float && is_int_format(f) && g_xfb_active) f = scaled_for(f);
            else if ((in.type == ScalarType::Uint || in.type == ScalarType::Sint) && !is_int_format(f))
                log_once("attr-type-mismatch", "a vertex attribute with a float format feeds an integer shader input (needs a SPIR-V variant)");
            if (key.attr_count >= 16) break;
            KeyAttr& k = key.attr[key.attr_count++];
            k.location = static_cast<uint8_t>(loc);
            k.binding = static_cast<uint8_t>(a->stream & 15);
            k.offset = static_cast<uint16_t>(a->relative_offset);
            k.format = f;
            key.binding_instance[k.binding] = S.stream[k.binding].divisor > 0 ? 1 : 0;
        }
    }
    if (nmissing) {
        uint32_t used = 0;
        for (uint32_t i = 0; i < key.attr_count; ++i) used |= 1u << key.attr[i].binding;
        uint32_t free_binding = 0;
        while (free_binding < 16 && (used >> free_binding & 1)) ++free_binding;
        if (free_binding < 16) {
            key.dummy_binding = static_cast<uint8_t>(free_binding + 1);
            key.binding_instance[free_binding] = 0;
            for (uint32_t i = 0; i < nmissing && key.attr_count < 16; ++i) {
                KeyAttr& k = key.attr[key.attr_count++];
                k.location = static_cast<uint8_t>(missing[i].location);
                k.binding = static_cast<uint8_t>(free_binding);
                const bool is_float = missing[i].type != ScalarType::Uint && missing[i].type != ScalarType::Sint;
                k.offset = is_float ? 0 : 16;
                k.format = is_float ? VK_FORMAT_R32G32B32A32_SFLOAT : (missing[i].type == ScalarType::Uint ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SINT);
            }
        }
    }
    key.color_count = P.ncolor;
    if (!P.ncolor && S.stage[kFragment] && !S.stage[kFragment]->refl.outputs.empty()) key.no_out = 1;   // a depth-only pass: no outputs
    if (!key.no_out && device().features().dynamic_rendering_unused_attachments) {
        // the fragment shader may write outputs the pass has no attachment for: they are listed as unused (format UNDEFINED)
        if (const ShaderInfo* fs = S.stage[kFragment])
            for (const ShaderIo& o : fs->refl.outputs)
                for (uint32_t e = 0; e < (o.array ? o.array : 1); ++e)
                    if (o.location + e < 8 && o.location + e >= key.color_count) key.color_count = o.location + e + 1;
    }
    for (uint32_t i = 0; i < key.color_count && i < 8; ++i) {
        if (i >= P.ncolor) {
            key.color_format[i] = VK_FORMAT_UNDEFINED;
            key.write_mask[i] = 0;
            continue;
        }
        key.color_format[i] = P.color_fmt[i];
        key.blend_enable[i] = (S.color.blend_enable_mask >> i) & 1;
        const BlendInfo& b = S.blend[i];
        key.blend_src_rgb[i] = b.src_rgb ? vk_blend_factor(b.src_rgb) : VK_BLEND_FACTOR_ONE;
        key.blend_dst_rgb[i] = b.dst_rgb ? vk_blend_factor(b.dst_rgb) : VK_BLEND_FACTOR_ZERO;
        key.blend_src_a[i] = b.src_a ? vk_blend_factor(b.src_a) : VK_BLEND_FACTOR_ONE;
        key.blend_dst_a[i] = b.dst_a ? vk_blend_factor(b.dst_a) : VK_BLEND_FACTOR_ZERO;
        key.blend_op_rgb[i] = b.eq_rgb ? vk_blend_op(b.eq_rgb) : VK_BLEND_OP_ADD;
        key.blend_op_a[i] = b.eq_a ? vk_blend_op(b.eq_a) : VK_BLEND_OP_ADD;
        key.write_mask[i] = S.chmask.mask[i] & 0xf;
        if (P.color_fmt[i] == VK_FORMAT_UNDEFINED) key.write_mask[i] = 0;
        auto uses_src1 = [](VkBlendFactor f) {
            return f == VK_BLEND_FACTOR_SRC1_COLOR || f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR || f == VK_BLEND_FACTOR_SRC1_ALPHA ||
                   f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        };
        if (key.blend_enable[i] && (uses_src1(static_cast<VkBlendFactor>(key.blend_src_rgb[i])) || uses_src1(static_cast<VkBlendFactor>(key.blend_dst_rgb[i])) ||
                                    uses_src1(static_cast<VkBlendFactor>(key.blend_src_a[i])) || uses_src1(static_cast<VkBlendFactor>(key.blend_dst_a[i]))))
            key.dual_src = 1;
    }
    key.xfb = g_xfb_active && device().features().transform_feedback && S.stage[kVertex] && xfb_stride(S.stage[kVertex]) ? 1 : 0;
    key.depth_format = P.depth_fmt;
    key.stencil_format = P.has_stencil ? P.depth_fmt : VK_FORMAT_UNDEFINED;
    key.samples = P.samples;
    key.topology = static_cast<uint8_t>(topo);
    key.polygon_mode = static_cast<uint8_t>(vk_polygon_mode(S.poly.polygon_mode));
    if (key.polygon_mode != VK_POLYGON_MODE_FILL && !device().features().fill_mode_non_solid) key.polygon_mode = VK_POLYGON_MODE_FILL;
    key.depth_clamp = S.depth_clamp && device().features().depth_clamp;
    key.alpha_to_coverage = S.ms.alpha_to_coverage && S.ms.enable ? 1 : 0;
    key.depth_neg_one = S.depth_mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W ? 1 : 0;
    if (S.color.logic_op != 0 && S.color.logic_op != NVN_LOGIC_OP_COPY && device().features().logic_op) {
        key.logic_op_enable = 1;
        key.logic_op = static_cast<uint8_t>(vk_logic_op(S.color.logic_op));
        for (uint32_t i = 0; i < 8; ++i) key.blend_enable[i] = 0;
    }
    if (S.color.alpha_test != 0 && S.color.alpha_test != NVN_ALPHA_FUNC_ALWAYS) log_once("alpha-test", "alpha test is enabled by the game: not implemented (needs a shader variant)");
    return true;
}

static void set_dynamic_state(uint32_t dirty, VkPrimitiveTopology topo, bool indexed, int index_type) {
    VkCommandBuffer cb = F->cb;
    const bool co = bind_coalesce_on();
    // true: send the value (always when the key is off)
    auto ch = [co](auto& sh, const auto& v) {
        if (!co || sh.changed(v)) return true;
        ft::split_count(ft::kCntSkipped);
        return false;
    };
    if (dirty & kDynVp) set_viewports_and_scissors();
    if (dirty & kDynRaster) {
        const VkCullModeFlags cull = vk_cull_mode(S.poly.cull_face);
        if (ch(g_dsh.cull, static_cast<uint32_t>(cull))) vkCmdSetCullMode(cb, cull);
        VkFrontFace ff = vk_front_face(S.poly.front_face);
        if (S.window_origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT) ff = ff == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;   // unflipped viewport
        if (ch(g_dsh.front, static_cast<uint32_t>(ff))) vkCmdSetFrontFace(cb, ff);
        if (ch(g_dsh.discard, static_cast<uint32_t>(S.rasterizer_discard ? 1 : 0))) vkCmdSetRasterizerDiscardEnable(cb, S.rasterizer_discard);
        const float lw = device().features().wide_lines ? std::max(1.0f, S.line_width) : 1.0f;
        if (ch(g_dsh.line_width, lw)) vkCmdSetLineWidth(cb, lw);
    }
    if (topo != g_topo) {
        vkCmdSetPrimitiveTopology(cb, topo);
        g_topo = topo;
    }
    const bool restart = S.prim_restart && indexed &&
                         ((index_type == NVN_INDEX_TYPE_UNSIGNED_INT && S.restart_index == 0xffffffffu) || (index_type == NVN_INDEX_TYPE_UNSIGNED_SHORT && S.restart_index == 0xffffu) ||
                          (index_type == NVN_INDEX_TYPE_UNSIGNED_BYTE && S.restart_index == 0xffu));
    if (!g_restart_valid || restart != g_restart || (!co && (dirty & kDynRaster))) {
        vkCmdSetPrimitiveRestartEnable(cb, restart);
        g_restart = restart;
        g_restart_valid = true;
    }
    const bool have_depth = P.depth_fmt != VK_FORMAT_UNDEFINED;
    if (dirty & kDynDepth) {
        const bool dt = have_depth && S.ds.depth_test, dw = have_depth && !P.depth_ro && S.ds.depth_test && S.ds.depth_write;
        const VkCompareOp dc = vk_compare_op(S.ds.depth_func ? S.ds.depth_func : NVN_DEPTH_FUNC_LESS);
        const bool dbt = have_depth && S.depth_bounds && device().features().depth_bounds;
        if (ch(g_dsh.depth_test, static_cast<uint32_t>(dt))) vkCmdSetDepthTestEnable(cb, dt);
        if (ch(g_dsh.depth_write, static_cast<uint32_t>(dw))) vkCmdSetDepthWriteEnable(cb, dw);
        if (ch(g_dsh.depth_cmp, static_cast<uint32_t>(dc))) vkCmdSetDepthCompareOp(cb, dc);
        if (ch(g_dsh.depth_bounds_test, static_cast<uint32_t>(dbt))) vkCmdSetDepthBoundsTestEnable(cb, dbt);
        if (S.depth_bounds && device().features().depth_bounds && ch(g_dsh.depth_bounds, std::array<float, 2>{S.db_near, S.db_far})) vkCmdSetDepthBounds(cb, S.db_near, S.db_far);
    }
    if (dirty & kDynStencil) {
        const bool ste = P.has_stencil && S.ds.stencil_test;
        if (ch(g_dsh.stencil_test, static_cast<uint32_t>(ste))) vkCmdSetStencilTestEnable(cb, ste);
        for (int f = 0; f < 2; ++f) {
            const VkStencilFaceFlags face = f == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT;
            const std::array<uint32_t, 5> op = {static_cast<uint32_t>(face), static_cast<uint32_t>(vk_stencil_op(S.ds.stencil_fail[f])), static_cast<uint32_t>(vk_stencil_op(S.ds.depth_pass[f])),
                                                static_cast<uint32_t>(vk_stencil_op(S.ds.depth_fail[f])), static_cast<uint32_t>(vk_compare_op(S.ds.stencil_func[f] ? S.ds.stencil_func[f] : NVN_STENCIL_FUNC_ALWAYS))};
            if (ch(g_dsh.stencil_op[f], op))
                vkCmdSetStencilOp(cb, face, static_cast<VkStencilOp>(op[1]), static_cast<VkStencilOp>(op[2]), static_cast<VkStencilOp>(op[3]), static_cast<VkCompareOp>(op[4]));
            if (ch(g_dsh.stencil_ref[f], static_cast<uint32_t>(S.stencil_ref[f]))) vkCmdSetStencilReference(cb, face, S.stencil_ref[f]);
            if (ch(g_dsh.stencil_cmask[f], static_cast<uint32_t>(S.stencil_vmask[f]))) vkCmdSetStencilCompareMask(cb, face, S.stencil_vmask[f]);
            const uint32_t wm = P.depth_ro ? 0u : static_cast<uint32_t>(S.stencil_wmask[f]);
            if (ch(g_dsh.stencil_wmask[f], wm)) vkCmdSetStencilWriteMask(cb, face, wm);
        }
    }
    if (dirty & kDynBias) {
        const int off = S.poly.offset_enables;
        const int mode = S.poly.polygon_mode;
        const bool bias = (mode == NVN_POLYGON_MODE_FILL && (off & NVN_POLYGON_OFFSET_ENABLE_FILL)) ||
                          (mode == NVN_POLYGON_MODE_LINE && (off & NVN_POLYGON_OFFSET_ENABLE_LINE)) ||
                          (mode == NVN_POLYGON_MODE_POINT && (off & NVN_POLYGON_OFFSET_ENABLE_POINT));
        if (ch(g_dsh.bias_enable, static_cast<uint32_t>(bias))) vkCmdSetDepthBiasEnable(cb, bias);
        const float bclamp = device().features().depth_bias_clamp ? S.bias_clamp : 0.0f;
        if (ch(g_dsh.bias, std::array<float, 3>{S.bias_units, bclamp, S.bias_factor})) vkCmdSetDepthBias(cb, S.bias_units, bclamp, S.bias_factor);
    }
    if (dirty & kDynBlend) {
        const std::array<float, 4> bc = {S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]};
        if (ch(g_dsh.blend, bc)) vkCmdSetBlendConstants(cb, S.blend_color);
    }
}

// Scene upscaler (S14, src/gfx/extras/hdr_nvn.h): before a tone-map draw whose HDR scene input is smaller than its target, FSR 1 enlarges the scene
// to the target's size and the draw samples that instead. The override lives until the next draw.
// Temporal upscaling (FSR 3 / DLSS / DLAA, src/gfx/extras/temporal.h): a G-buffer draw (4+ colour targets and a depth target) offers its vertex
// uniform slot 0 as the camera block; the first valid one of the frame is kept with its depth target.
void temporal_gbuffer_note() {
    if (S.ncolor < 4 || !S.depth_tex || !S.stage[kVertex] || extras::temporal_camera_ready()) return;
    const GState::Bind& u = S.ubo[kVertex][0];
    if (!u.addr || u.size < 272) return;
    uint32_t w[68];
    std::memcpy(w, host_ptr(u.addr), sizeof w);
    tsr_jitter(w, 68);                                                    // the camera as the draw sees it (the bound copy is jittered the same way)
    extras::temporal_note_gbuffer(w, 68);
    if (extras::temporal_camera_ready()) g_tsr_depth = S.depth_tex;
}

// The game's scene covers only part of its targets (extras::scene_fraction, Pokopia's [upscale] scene_scale, #116): the part of a scene
// image of size `scene` that holds it, and the size of the image the tone-map draw then needs, out / fraction, so that its own lookup at
// uv * fraction lands on the upscaled result in that image's top-left out-sized corner. False: the whole image is the scene.
static bool scene_part(VkExtent2D scene, VkExtent2D out, VkExtent2D* part, VkExtent2D* out_tex) {
    float fx = 1.0f, fy = 1.0f;
    extras::scene_fraction(&fx, &fy);
    if (fx >= 1.0f && fy >= 1.0f) return false;
    part->width = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(scene.width * fx)), 1, scene.width);
    part->height = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(scene.height * fy)), 1, scene.height);
    // rounded down: fraction * out_tex <= out, so the draw's lookups at uv * fraction stay inside the written out-sized corner
    out_tex->width = std::max<uint32_t>(out.width, static_cast<uint32_t>(out.width / fx));
    out_tex->height = std::max<uint32_t>(out.height, static_cast<uint32_t>(out.height / fy));
    static bool told = false;
    if (!told) {
        told = true;
        NXLOG_I(kTag, "scene upscale: the scene covers %ux%u of its %ux%u target (fraction %.3f x %.3f): %ux%u -> %ux%u, handed to the tone-map draw "
                "in a %ux%u image", part->width, part->height, scene.width, scene.height, fx, fy, part->width, part->height, out.width, out.height,
                out_tex->width, out_tex->height);
    }
    return true;
}

// The tone-map draw with a temporal mode: the largest 2D colour input is the HDR scene; the SDK's result (target size) replaces it for this draw.
static void temporal_for_draw(const ProgLayout& pl) {
    ++g_tsr_frame;                                                        // one tone-map draw per frame: the next frame's jitter phase
    Texture* target = S.color_tex[0].get();
    Texture* depth = g_tsr_depth.get();
    static bool told_intercept = false, told_input = false, told_depth = false;
    if (!told_intercept) {
        told_intercept = true;
        NXLOG_I(kTag, "temporal: tone-map draw %s intercepted (target %ux%u)", S.stage[kFragment]->hash.c_str(), target ? target->b.width : 0,
                target ? target->b.height : 0);
    }
    if (!extras::temporal_camera_ready()) {                              // counts the frames without a camera (warns after 120)
        extras::temporal_evaluate(nullptr, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, {}, {}, false);
        return;
    }
    if (!tex_gpu(target) || !depth || !tex_gpu(depth)) {
        if (!told_depth) {
            told_depth = true;
            NXLOG_W(kTag, "temporal: camera found but no usable target / G-buffer depth texture at the tone-map draw");
        }
        return;
    }
    const BindingDesc* best = nullptr;
    TexResolved best_r{};
    uint64_t best_area = 0;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        if (view_type_for(b) != VK_IMAGE_VIEW_TYPE_2D) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || tex_is_depth(r.tex) || shares_image(r.tex, depth)) continue;
        const VkExtent3D e = tex_image_extent(r.tex, 0);
        const uint64_t area = static_cast<uint64_t>(e.width) * e.height;
        if (area > best_area) { best_area = area; best = &d; best_r = r; }
    }
    if (!best) {
        if (!told_input) {
            told_input = true;
            NXLOG_W(kTag, "temporal: the tone-map draw samples no 2D colour texture to use as the scene");
        }
        return;
    }
    const DescriptorBinding& b = best->shader->refl.bindings[best->index];
    const VkExtent3D ce = tex_image_extent(best_r.tex, 0), de = tex_image_extent(depth, 0), te = tex_image_extent(target, 0);
    VkExtent2D render{std::min(ce.width, de.width), std::min(ce.height, de.height)}, out_tex{};
    if (VkExtent2D part{}; scene_part(render, VkExtent2D{te.width, te.height}, &part, &out_tex)) render = part;
    end_pass();                                                           // compute work cannot run inside a render pass
    tex_ensure_current(best_r.tex, *F);
    tex_transition(best_r.tex, *F, tex_sampled_layout(best_r.tex));
    tex_transition(depth, *F, tex_sampled_layout(depth));
    VkImageView src = tex_sampled_view(best_r.tex, make_request(b, best_r));
    ViewRequest dr;
    dr.depth_aspect = true;
    VkImageView dv = tex_sampled_view(depth, dr);
    if (!src || !dv) return;
    VkImageView out = extras::temporal_evaluate(&device(), F->cb, src, dv, tex_sampled_layout(depth), render, VkExtent2D{te.width, te.height},
                                                S.depth_mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W, out_tex);
    g_dirty = kAllDirty;                                                  // the SDKs bind their own compute state
    shadow_invalidate();
    if (out) {
        g_su_tex = best_r.tex;
        g_su_view = out;
    }
}

void scene_upscale_for_draw(const ProgLayout& pl) {
    g_su_tex = nullptr;
    g_su_view = VK_NULL_HANDLE;
    const ShaderInfo* fs = S.stage[kFragment];
    if (fs && extras::temporal_wanted() && S.ncolor > 0 && S.color_tex[0] && extras::hdr_is_tonemap(fs->spirv, fs->hash)) {
        temporal_for_draw(pl);
        return;
    }
    if (!fs || !extras::scene_upscale_wanted() || S.ncolor == 0 || !S.color_tex[0] || !extras::hdr_is_tonemap(fs->spirv, fs->hash)) return;
    Texture* target = S.color_tex[0].get();
    if (!tex_gpu(target)) return;
    const VkExtent3D te = tex_image_extent(target, 0);   // image sizes: with a render scale the upscaler works on the real pixels
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.nvn.source != NvnSource::SamplerBinding && b.nvn.source != NvnSource::HandleInUniform) continue;
        if (view_type_for(b) != VK_IMAGE_VIEW_TYPE_2D) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || shares_image(r.tex, S.depth_tex.get())) continue;
        const VkExtent3D se = tex_image_extent(r.tex, 0);
        // the scene is either a smaller image or the top-left part of a full-size one (scene_part)
        VkExtent2D in_view{se.width, se.height}, out_tex{};
        if (!scene_part(in_view, VkExtent2D{te.width, te.height}, &in_view, &out_tex) && se.width >= te.width && se.height >= te.height) return;   // nothing to enlarge
        if (in_view.width >= te.width && in_view.height >= te.height) return;
        if (extras::upscale_mode() == extras::UpscaleMode::Nearest) {   // no pass: the draw samples the small scene with a nearest sampler (#232)
            g_su_tex = r.tex;
            return;
        }
        extras::SpatialUpscaler* su = extras::scene_upscaler(&device(), in_view, VkExtent2D{te.width, te.height});
        if (!su) return;
        end_pass();                                                           // compute work cannot run inside a render pass
        tex_ensure_current(r.tex, *F);
        tex_transition(r.tex, *F, tex_sampled_layout(r.tex));
        VkImageView src = tex_sampled_view(r.tex, make_request(b, r));
        if (!src) return;
        VkImageView up = su->upscale(F->cb, src, in_view, VkExtent2D{se.width, se.height}, VkExtent2D{te.width, te.height}, true,
                                     extras::scene_upscale_prescale(), extras::upscale_sharpness(), out_tex);
        shadow_invalidate();
        if (up) {
            g_su_tex = r.tex;
            g_su_view = up;
        }
        return;
    }
}

// Evaluation-only programs (NVN tessellates with the fixed SetInner/OuterTessellationLevels): a generated pass-through control shader
// (nvn_tess_gen.h), one per (vertex shader, evaluation shader, patch size). It copies each per-vertex input of the evaluation shader from the
// vertex shader output at the same location (zero where the vertex shader writes none) and gl_Position, zeroes the per-patch inputs, and reads
// the levels from push constants (pushed before each such draw, so level changes need no new pipeline). Null when an interface variable has a
// type the generator does not handle.
static const ShaderInfo* g_gen_tcs = nullptr;   // the generated control shader of the current draw (null: the program has its own, or none)
static const ShaderInfo* passthrough_tcs_for(const ShaderInfo* vs, const ShaderInfo* tes) {
    struct Key {
        const ShaderInfo* vs; const ShaderInfo* tes; uint32_t n;
        bool operator<(const Key& o) const { return std::tie(vs, tes, n) < std::tie(o.vs, o.tes, o.n); }
    };
    static std::map<Key, std::unique_ptr<ShaderInfo>> cache;
    const uint32_t n = std::clamp<uint32_t>(S.patch_size, 1, 32);
    const Key key{vs, tes, n};
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    auto to_io = [](const ShaderIo& v, tessgen::Io& o) {
        if (v.components < 1 || v.components > 4) return false;
        if (v.type == ScalarType::Float) o.kind = tessgen::Kind::Float;
        else if (v.type == ScalarType::Sint) o.kind = tessgen::Kind::Sint;
        else if (v.type == ScalarType::Uint) o.kind = tessgen::Kind::Uint;
        else return false;
        o.location = v.location;
        o.component = v.component;
        o.components = v.components;
        return true;
    };
    std::vector<tessgen::Io> vs_out, per_vertex, per_patch;
    bool ok = true;
    for (const ShaderIo& o : vs->refl.outputs) {   // an output array (several locations) is not matched: its readers get zero
        tessgen::Io io;
        if (o.array <= 1 && to_io(o, io)) vs_out.push_back(io);
    }
    for (const ShaderIo& in : tes->refl.inputs) {
        tessgen::Io io;
        if (!to_io(in, io)) { ok = false; break; }
        (in.array > 1 ? per_vertex : per_patch).push_back(io);   // in an evaluation shader a non-array input is per patch
    }
    std::unique_ptr<ShaderInfo> si;
    if (ok) {
        const std::vector<tessgen::Copy> copies = tessgen::match(vs_out, per_vertex);
        size_t copied = 0;
        for (const tessgen::Copy& c : copies) copied += c.has_src;
        si = std::make_unique<ShaderInfo>();
        si->hash = "tcs-pass-" + vs->hash + "-" + tes->hash + "-" + std::to_string(n);
        si->stage = ShaderStage::TessControl;
        // gl_ClipDistance / gl_CullDistance: copied when the vertex shader writes them and the evaluation shader reads gl_in[]'s
        auto uses = [](const std::vector<uint32_t>& v, uint32_t b) { return std::find(v.begin(), v.end(), b) != v.end(); };
        constexpr uint32_t kBuiltInClip = 3, kBuiltInCull = 4;
        const bool clip = uses(vs->refl.builtins_used_out, kBuiltInClip) && uses(tes->refl.builtins_used_in, kBuiltInClip);
        const bool cull = uses(vs->refl.builtins_used_out, kBuiltInCull) && uses(tes->refl.builtins_used_in, kBuiltInCull);
        si->spirv = tessgen::passthrough_tcs(copies, per_patch, n, 7, 1, clip, cull);
        si->push_bytes = tessgen::kPushBytes;
        std::string err;
        if (!reflect_spirv(si->spirv.data(), si->spirv.size(), si->refl, &err)) {
            NXLOG_W(kTag, "generated control shader for %s does not reflect: %s", tes->hash.c_str(), err.c_str());
            si.reset();
        } else {
            NXLOG_I(kTag, "evaluation shader %s (vs %s): pass-through control shader, %u vertices, %zu per-vertex locations (%zu from the vertex shader, %zu zero), %zu per-patch%s%s",
                    tes->hash.c_str(), vs->hash.c_str(), n, copies.size(), copied, copies.size() - copied, per_patch.size(), clip ? ", clip distances" : "",
                    cull ? ", cull distances" : "");
        }
    } else {
        NXLOG_W(kTag, "evaluation shader %s: an input is not a 1..4 component float/int, no pass-through control shader", tes->hash.c_str());
    }
    const ShaderInfo* out = si.get();
    cache.emplace(key, std::move(si));
    return out;
}
static const ShaderInfo* tcs_now() { return S.stage[kTessCtl] ? S.stage[kTessCtl] : g_gen_tcs; }

// Prepares everything a draw needs and binds it. False: skip the draw.
static bool begin_draw(int mode, bool indexed, int index_type, VkPrimitiveTopology* topo_out) {
    RpScope rp_all(kRpDraw);
    PoolReasonScope pr_(kReasonVertex);
    ResolveScope rs_;
    const ShaderInfo* vs = S.stage[kVertex];
    if (!vs) { log_once("draw-no-vs", "draw without a vertex shader skipped"); return false; }
    if (!S.render_enable) return false;
    ++g_draw_idx;
    if (key_check_on()) key_check_draw(counters().presents.load(std::memory_order_relaxed));
    if (S.stage[kFragment] && skip_fs_draw(S.stage[kFragment]->hash)) return false;
    // Tessellation: Vulkan runs it only with both shaders and a patch list. NVN also allows an evaluation shader alone (fixed levels from
    // SetInner/OuterTessellationLevels): those draws get a generated pass-through control shader (passthrough_tcs_for).
    if (S.stage[kTessCtl] || S.stage[kTessEval] || mode == NVN_DRAW_PRIMITIVE_PATCHES) {
        if (mode != NVN_DRAW_PRIMITIVE_PATCHES) { note_unsupported("tessellation draw: tessellation program drawn without patches"); return false; }
        if (!S.stage[kTessEval]) { note_unsupported("tessellation draw: patches without a tessellation evaluation shader"); return false; }
        if (!S.stage[kTessCtl]) {
            const ShaderInfo* gen = passthrough_tcs_for(vs, S.stage[kTessEval]);
            if (!gen) { note_unsupported("tessellation draw: evaluation shader without a control shader, no pass-through could be made"); return false; }
            if (gen != g_gen_tcs) g_dirty |= kPipe;
            g_gen_tcs = gen;
        }
    } else {
        g_gen_tcs = nullptr;
    }
    if (g_dirty & (kPipe | kDesc)) {
        const ShaderInfo* stages[6] = {vs, S.stage[kFragment], S.stage[kGeometry], tcs_now(), S.stage[kTessEval], nullptr};
        g_pl = prog_layout(stages);
    }
    if (!g_trigger_fs.empty() && !g_trigger_done && trigger_time_ok() && S.stage[kFragment] && S.stage[kFragment]->hash.compare(0, g_trigger_fs.size(), g_trigger_fs) == 0) {
        g_trigger_seen = true;
        g_trigger_present = static_cast<int64_t>(counters().presents.load());
    }
    const ProgLayout* pl = g_pl;
    if (!pl) return false;
    if (pl->c0_graphics && g_draw_base_instance != g_bank_base_instance) g_dirty |= kDesc;   // c[0] + 0x644 changed: new sets
    rs_.arm(pl);
    if (extras::temporal_wanted()) temporal_gbuffer_note();
    if (g_dirty & (kDesc | kPipe)) {
        RpScope rp_(kRpPrep);
        g_prog_writes = false;
        for (const BindingDesc& d : pl->bindings) g_prog_writes |= d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        attach_targets(false);
        g_want_depth_ro = samples_depth_target(*pl);
        prepare_textures(*pl);
        if (g_trace) {   // [nvn] trace_dump_tex: also the block-compressed level 0 as the GPU holds it, to compare with the prepared data
            static const std::string dump_dir = nxrt::config_get_str("nvn", "trace_dump_tex", "");
            static std::unordered_map<uint32_t, bool> raw_done;
            if (!dump_dir.empty()) {
                for (const BindingDesc& d : pl->bindings) {
                    if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
                    const TexResolved r = resolve_texture(d);
                    if (!r.tex || !raw_done.emplace(r.tex->id, true).second) continue;
                    end_pass();
                    const int ri = snapshot_record_raw(r.tex, *F);
                    if (ri >= 0) g_trace_snaps.push_back({ri, g_trace_n, dump_dir + "/gpu_t" + std::to_string(r.tex->id) + ".bin", "raw"});
                }
            }
        }
        scene_upscale_for_draw(*pl);
    }
    if (g_fb_seen || (g_dirty & (kDesc | kPipe))) feedback_prepare(*pl);
    bool pass_ok;
    { RpScope rp_(kRpPass); pass_ok = ensure_pass(g_want_depth_ro, g_fbl_want); }
    if (!pass_ok) { log_once("draw-no-target", "draw without a usable render target skipped"); return false; }
    if (g_prog_writes || g_xfb_active) g_pass_unsafe = true;
    const VkPrimitiveTopology topo = vk_topology(mode);
    if ((g_dirty & kPipe) || topo != g_pipe_topo || !g_pipe || g_pipe_fbl != (P.fbl != 0)) {
        RpScope rp_(kRpPipe);
        GraphicsKey key;
        build_key(key, topo);
        key.fb_loop = P.fbl != 0 ? 1 : 0;   // the pass has an attachment in the feedback loop layout
        g_pipe_fbl = key.fb_loop != 0;
        g_pipe = graphics_pipeline(key, *pl);
        g_pipe_xfb = key.xfb != 0;
        g_pipe_topo = topo;
        g_key_attr_count = key.attr_count;
        g_key_dummy_binding = key.dummy_binding;
        for (uint32_t i = 0; i < key.attr_count; ++i) g_key_binding[i] = key.attr[i].binding;
    }
    if (!g_pipe) return false;
    // a draw inside a stream-out section that has no stream-out variant and nothing to render to has no effect
    if (g_xfb_active && !g_pipe_xfb && !P.ncolor && !S.depth_tex) return false;
    if (g_trace) {
        char a[96], b[96], c[96];
        std::string line;
        auto remember = [](Texture* t) {
            if (!t) return;
            for (const Ref<Texture>& r : g_trace_targets) if (r.get() == t) return;
            g_trace_targets.push_back(Ref<Texture>::share(t));
        };
        for (uint32_t i = 0; i < P.ncolor; ++i) remember(S.color_tex[i].get());
        remember(S.depth_tex.get());
        for (uint32_t i = 0; i < P.ncolor; ++i) { line += tex_desc(S.color_tex[i].get(), a, sizeof a); line += " | "; }
        TRACE("draw #%d vs %s fs %s gs %s mode %d idx %d | rt %s ds %s%s | blend %u mask %x depth t%d w%d f%d cull %d | ubo0 %llx/%llu",
              ++g_trace_n, S.stage[kVertex] ? S.stage[kVertex]->hash.c_str() : "-", S.stage[kFragment] ? S.stage[kFragment]->hash.c_str() : "-",
              S.stage[kGeometry] ? S.stage[kGeometry]->hash.c_str() : "-", mode, indexed ? 1 : 0, line.c_str(), tex_desc(S.depth_tex.get(), b, sizeof b),
              P.depth_ro ? " (ro)" : "", S.color.blend_enable_mask, S.chmask.mask[0], S.ds.depth_test ? 1 : 0, S.ds.depth_write ? 1 : 0, S.ds.depth_func,
              S.poly.cull_face, static_cast<unsigned long long>(S.ubo[kVertex][0].addr), static_cast<unsigned long long>(S.ubo[kVertex][0].size));
        TRACE("    poly mode %d offset %x factor %g units %g clamp %g | depth mode %d origin %d clamp %d", S.poly.polygon_mode, S.poly.offset_enables, S.bias_factor, S.bias_units,
              S.bias_clamp, S.depth_mode, S.window_origin, S.depth_clamp ? 1 : 0);
        if (S.color.blend_enable_mask)
            TRACE("    blend rt0 rgb %d,%d,%d a %d,%d,%d (nvn enums src,dst,eq)", S.blend[0].src_rgb, S.blend[0].dst_rgb, S.blend[0].eq_rgb, S.blend[0].src_a, S.blend[0].dst_a, S.blend[0].eq_a);
        // [nvn] trace_ubo_bytes = N (NXRT_NVN_TRACE_UBO_BYTES, default 768, at most 65536): how many leading bytes of each uniform block the trace prints
        static const uint64_t ubo_bytes = static_cast<uint64_t>(std::clamp<int64_t>(nxrt::config_get_int("nvn", "trace_ubo_bytes", 768), 16, 65536));
        for (int stg = kVertex; stg <= kFragment; ++stg)
        for (uint32_t slot = 0; slot < 16; ++slot) {    // the first words of every uniform block bound to the vertex and the fragment stage
            const GState::Bind& ub = S.ubo[stg][slot];
            if (!ub.addr) continue;
            BufRef ur = resolve_buffer(ub.addr, std::min<uint64_t>(ub.size, ubo_bytes));
            std::string words;
            if (ur.ok) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(ub.addr);
                for (uint32_t i = 0; i * 4 < ubo_bytes && i * 4 < ub.size; ++i) { char t[16]; std::snprintf(t, sizeof t, " %08x", w[i]); words += t; }
            }
            TRACE("    %s ubo%u %llx/%llu%s:%s", stg == kVertex ? "vs" : "fs", slot, static_cast<unsigned long long>(ub.addr), static_cast<unsigned long long>(ub.size), ur.ok ? "" : " (outside every pool)", words.c_str());
        }
        TRACE("    viewport %u: %g %g %g %g depth %g..%g scissor %u: %d %d %d %d", S.nvp, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.dr[0][0], S.dr[0][1], S.nsc,
              S.sc[0][0], S.sc[0][1], S.sc[0][2], S.sc[0][3]);
        for (uint32_t i = 0; i < P.ncolor; ++i)
            if (Texture* ct = S.color_tex[i].get()) {
                const FormatInfo* vf = (S.color_has_view[i] && S.color_view[i].has_format) ? format_info(S.color_view[i].format) : nullptr;
                TRACE("    rt%u t%u mem %p storage %llu cpu-dirty %d view level %u layer %u layers %u format %s", i, ct->id, static_cast<void*>(ct->mem),
                      static_cast<unsigned long long>(ct->storage_size), ct->cpu_dirty.load() ? 1 : 0, S.color_level[i], S.color_layer[i], S.color_layers[i],
                      vf && vf->name ? vf->name : "-");
            }
        g_trace_sampled.clear();
        for (const BindingDesc& d : pl->bindings) {   // texel buffers (light lists and other BUFFER-target textures): the view the draw gets
            if (d.type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && d.type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) continue;
            const bool storage = d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            const TexResolved r = storage ? resolve_image(d) : resolve_texture(d);
            if (!r.tex) { TRACE("    texel set%u b%u -> (no texture)", d.set, d.binding); continue; }
            uint64_t addr = 0, bytes = 0;
            const VkBufferView v = tex_buffer_view(r.tex, r.has_view ? &r.view : nullptr, 0, VK_FORMAT_UNDEFINED, storage, &addr, &bytes);
            std::string words;
            if (r.tex->mem) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(r.tex->mem);
                const size_t nw = std::min<size_t>(32, static_cast<size_t>(r.tex->b.width) * (r.tex->fmt ? r.tex->fmt->bytes : 4) / 4);
                char wb[12];
                for (size_t i = 0; i < nw; ++i) { std::snprintf(wb, sizeof wb, " %08x", w[i]); words += wb; }
                const size_t all = static_cast<size_t>(r.tex->b.width) * (r.tex->fmt ? r.tex->fmt->bytes : 4) / 4;
                size_t nz = 0;
                for (size_t i = 0; i < all; ++i) nz += w[i] != 0;
                std::snprintf(wb, sizeof wb, " |nz %zu", nz);
                words = std::string(wb) + "/" + std::to_string(all) + words;
            }
            TRACE("    texel set%u b%u -> t%u %s width %d @%p view %s addr %llx bytes %llu view-format %s:%s", d.set, d.binding, r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.width,
                  static_cast<void*>(r.tex->mem), v ? "ok" : "NONE (dummy)", static_cast<unsigned long long>(addr), static_cast<unsigned long long>(bytes),
                  (r.has_view && r.view.has_format) ? "set" : "-", words.c_str());
        }
        for (const BindingDesc& d : pl->bindings) {
            if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && d.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
            const TexResolved r = d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? resolve_image(d) : resolve_texture(d);
            if (r.tex) g_trace_sampled.push_back(r.tex);
            if (r.tex) {
                static const std::string dump_dir = nxrt::config_get_str("nvn", "trace_dump_tex", "");
                if (!dump_dir.empty()) trace_dump_prepared(r.tex, dump_dir);
            }
            char smp[200] = "", vw[160] = "";
            if (r.has_sampler)
                std::snprintf(smp, sizeof smp, " | sampler min %d mag %d wrap %d,%d,%d lod %g..%g bias %g aniso %g cmp %d func %d", r.smp.min_filter, r.smp.mag_filter, r.smp.wrap_s, r.smp.wrap_t,
                              r.smp.wrap_r, r.smp.lod_min, r.smp.lod_max, r.smp.lod_bias, r.smp.max_anisotropy, r.smp.compare_mode, r.smp.compare_func);
            if (r.has_view) {
                // the view's own format by name (a view naming the sRGB partner of its texture's format, or the reverse, changes the decode)
                const FormatInfo* vf = r.view.has_format ? format_info(r.view.format) : nullptr;
                char vfn[48] = "";
                if (r.view.has_format) std::snprintf(vfn, sizeof vfn, " format %s", vf && vf->name ? vf->name : "?");
                std::snprintf(vw, sizeof vw, " | view levels %d+%d layers %d+%d%s", r.view.has_levels ? r.view.min_level : -1, r.view.has_levels ? r.view.num_levels : -1,
                              r.view.has_layers ? r.view.min_layer : -1, r.view.has_layers ? r.view.num_layers : -1, vfn);
            }
            if (r.tex) {
                const int* sw = (r.has_view && r.view.has_swizzle) ? r.view.swizzle : r.tex->b.swizzle;
                const size_t n = std::strlen(vw);
                std::snprintf(vw + n, sizeof vw - n, " swz %d%d%d%d", sw[0], sw[1], sw[2], sw[3]);
            }
            TRACE("    %s set%u b%u %s -> %s @%p%s mem %d%%%s%s%s", d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? "image" : "tex", d.set, d.binding,
                  d.shader->refl.bindings[d.index].name.c_str(), tex_desc(r.tex, c, sizeof c), r.tex ? static_cast<void*>(r.tex->mem) : nullptr, r.has_sampler ? "" : " (no sampler)", storage_nonzero_pct(r.tex),
                  r.tex && r.tex->cpu_dirty.load() ? " cpu-dirty" : "", smp, vw);
        }
    }
    if (g_xfb_active) {
        static std::unordered_map<std::string, bool> seen;
        if (seen.emplace(S.stage[kVertex]->hash, true).second)
            NXLOG_I("nvn.xfb", "draw under transform feedback: vs %s mode %d attrs %u xfb0 %llx/%llu", S.stage[kVertex]->hash.c_str(), mode, S.attr_count,
                    static_cast<unsigned long long>(S.xfb[0].addr), static_cast<unsigned long long>(S.xfb[0].size));
    }
    if (g_pipe != g_bound_pipeline) {
        xfb_end_in_pass();     // the pipeline cannot change while feedback is active; the byte counter carries the position over
        vkCmdBindPipeline(F->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
        g_gauge_pipeline_binds.add(1);
        g_bound_pipeline = g_pipe;
    }
    if (g_xfb_active && g_pipe_xfb && !g_xfb_began && device().features().transform_feedback && S.xfb[0].addr) {
        BufRef xb = resolve_buffer(S.xfb[0].addr, S.xfb[0].size);
        if (xb.ok) {
            gpu_wrote(S.xfb[0].addr, S.xfb[0].size ? S.xfb[0].size : xb.avail, kWriteXfb);
            if (!g_xfb_counter) {
                if (device().create_buffer(kXfbSlots * kXfbSlotBytes, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Upload,
                                           &g_xfb_counter, "nvn xfb counter") != VK_SUCCESS)
                    g_xfb_counter = gfx::Buffer();
            }
            const VkDeviceSize size = std::min<VkDeviceSize>(S.xfb[0].size ? S.xfb[0].size : xb.avail, xb.avail) & ~static_cast<VkDeviceSize>(3);
            vkCmdBindTransformFeedbackBuffersEXT(F->cb, 0, 1, &xb.buffer, &xb.offset, &size);
            VkBuffer cbuf = g_xfb_counter.buffer;
            const VkDeviceSize coff = static_cast<VkDeviceSize>(g_xfb_cur) * kXfbSlotBytes;
            if (g_xfb_have_counter && cbuf) vkCmdBeginTransformFeedbackEXT(F->cb, 0, 1, &cbuf, &coff);
            else vkCmdBeginTransformFeedbackEXT(F->cb, 0, 0, nullptr, nullptr);
            g_xfb_began = true;
        } else {
            note_unsupported("transform feedback into an address outside every pool");
        }
    }
    { RpScope rp_(kRpDyn); set_dynamic_state(g_dirty, topo, indexed, index_type); }
    if (g_dirty & kDesc) {
        RpScope rp_(kRpSets);
        support_for_draw(*pl, false);
        bind_sets(*pl, VK_PIPELINE_BIND_POINT_GRAPHICS);
        g_bank_base_instance = g_draw_base_instance;
    }
    if (pl->push_bytes && g_gen_tcs) {
        // the pass-through control shader's tessellation levels (NVN's fixed SetInner/OuterTessellationLevels), every draw: cheap, and
        // the layout may have changed since the last push
        tessgen::TessPush push = {};
        std::memcpy(push.outer, S.tess_outer, sizeof push.outer);
        std::memcpy(push.inner, S.tess_inner, sizeof push.inner);
        vkCmdPushConstants(F->cb, pl->layout, pl->push_stages, 0, std::min<uint32_t>(pl->push_bytes, sizeof push), &push);
        static std::set<const ShaderInfo*> logged;
        if (logged.insert(g_gen_tcs).second)
            NXLOG_I(kTag, "tessellation draw: evaluation shader %s drawn with pass-through control shader %s, patch size %u, levels %g %g %g %g / %g %g",
                    S.stage[kTessEval]->hash.c_str(), g_gen_tcs->hash.c_str(), static_cast<unsigned>(std::clamp<uint32_t>(S.patch_size, 1, 32)),
                    S.tess_outer[0], S.tess_outer[1], S.tess_outer[2], S.tess_outer[3], S.tess_inner[0], S.tess_inner[1]);
    }
    if (g_dirty & (kVb | kPipe)) {
        RpScope rp_(kRpVb);
        const bool co = bind_coalesce_on();
        // pending run of bindings to bind with one call
        VkBuffer rb[16];
        VkDeviceSize ro[16], rs[16], rst[16];
        uint32_t rfirst = 0, rn = 0;
        auto flush = [&] {
            if (!rn) return;
            vkCmdBindVertexBuffers2(F->cb, rfirst, rn, rb, ro, rs, rst);
            if (rn > 1) ft::split_count(ft::kCntMerged, rn - 1);
            g_vb_bound_in_cb = true;
            rn = 0;
        };
        auto add = [&](uint32_t binding, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, VkDeviceSize stride) {
            if (co && binding < 16) {
                VbShadow& sh = g_vbsh[binding];
                if (sh.ok && sh.buf == buf && sh.off == off && sh.size == size && sh.stride == stride) { ft::split_count(ft::kCntSkipped); flush(); return; }
                sh = {buf, off, size, stride, true};
                if (rn && rfirst + rn == binding) {
                    rb[rn] = buf; ro[rn] = off; rs[rn] = size; rst[rn] = stride; ++rn;
                    return;
                }
                flush();
                rfirst = binding;
                rb[0] = buf; ro[0] = off; rs[0] = size; rst[0] = stride;
                rn = 1;
                return;
            }
            vkCmdBindVertexBuffers2(F->cb, binding, 1, &buf, &off, &size, &stride);
            g_vb_bound_in_cb = true;
        };
        // bindings in ascending order so that neighbours merge
        uint32_t order[16], norder = 0;
        for (uint32_t i = 0; i < g_key_attr_count; ++i) {
            const uint32_t binding = g_key_binding[i];
            bool done = false;
            for (uint32_t j = 0; j < norder; ++j) if (order[j] == binding) done = true;
            if (!done && norder < 16) order[norder++] = binding;
        }
        if (co) std::sort(order, order + norder);
        for (uint32_t k = 0; k < norder; ++k) {
            const uint32_t binding = order[k];
            if (g_key_dummy_binding && binding == static_cast<uint32_t>(g_key_dummy_binding - 1)) {
                add(binding, dummy_buffer(), kDummyAttrFloatOffset, 32, 0);
                continue;
            }
            const GState::Bind& vb = S.vtx[binding];
            BufRef br = vb.addr ? resolve_read(vb.addr, vb.size, kReadVertex) : BufRef{dummy_buffer(), 0, 65536, false};
            VkDeviceSize size = vb.size ? std::min<VkDeviceSize>(vb.size, br.avail) : br.avail;
            VkDeviceSize stride = static_cast<VkDeviceSize>(S.stream[binding].stride);
            add(binding, br.buffer, br.offset, size, stride);
        }
        flush();
        if (!g_vb_bound_in_cb) {
            // the pipeline's vertex binding strides are dynamic: a command buffer that never bound a vertex buffer must still bind one
            VkBuffer dummy = dummy_buffer();
            VkDeviceSize off = 0, size = 65536, stride = 0;
            vkCmdBindVertexBuffers2(F->cb, 0, 1, &dummy, &off, &size, &stride);
            g_vb_bound_in_cb = true;
            if (co) g_vbsh[0] = {dummy, off, size, stride, true};
        }
    }
    g_dirty = g_mirror_stale ? (kVb | kDesc) : 0;     // a storage buffer of this draw made copied pages stale: the next draw binds again
    g_mirror_stale = false;
    *topo_out = topo;
    return true;
}

// Counts a draw for the primitive counters, before the replay decides whether to skip it: the console generates the primitives of every
// draw it runs, and a draw of a feedback section also writes them (14). A draw with rendering disabled is not run by the console: not counted.
static void count_primitives(uint32_t mode, uint32_t count, uint32_t instances) {
    if (!S.render_enable) return;
    const uint64_t per = mode == NVN_DRAW_PRIMITIVE_PATCHES ? count / std::max<uint32_t>(1, S.patch_size) : primitives_for(static_cast<int>(mode), count);   // patches: input patches
    const uint64_t prims = per * std::max<uint32_t>(1, instances);
    g_prims_generated += prims;
    if (g_xfb_active) g_prims_xfb += prims;
}

// Quads, quad strips, polygons and line loops, which Vulkan has no topology for, as the triangle (or line strip) index list of the same vertices.
// `in` holds the vertex indices of the draw in order (for DrawArrays: first, first + 1, ...). Shared by the direct and the indirect draws.
static std::vector<uint32_t> convert_prim_indices(PrimConversion conv, const std::vector<uint32_t>& in) {
    std::vector<uint32_t> idx;
    const size_t n = in.size();
    switch (conv) {
        case PrimConversion::Quads:
            for (size_t q = 0; q + 3 < n; q += 4) idx.insert(idx.end(), {in[q], in[q + 1], in[q + 2], in[q], in[q + 2], in[q + 3]});
            break;
        case PrimConversion::QuadStrip:
            for (size_t q = 0; q + 3 < n; q += 2) idx.insert(idx.end(), {in[q], in[q + 1], in[q + 2], in[q + 2], in[q + 1], in[q + 3]});
            break;
        case PrimConversion::Polygon:
            for (size_t k = 1; k + 1 < n; ++k) idx.insert(idx.end(), {in[0], in[k], in[k + 1]});
            break;
        case PrimConversion::LineLoop:
            idx = in;
            if (!in.empty()) idx.push_back(in[0]);
            break;
        default: idx = in; break;
    }
    return idx;
}

// Binds an index list of 32-bit indices from the upload buffer. False: no room.
static bool bind_u32_indices(const std::vector<uint32_t>& idx) {
    UploadAlloc u = F->upload(idx.size() * 4, 4);
    if (!u) return false;
    std::memcpy(u.ptr, idx.data(), idx.size() * 4);
    bind_index_buffer(F->cb, u.buffer, u.offset, VK_INDEX_TYPE_UINT32);
    return true;
}

// [gfx] gpu_checkpoints = all: a checkpoint before the draw names it (vertex and fragment shader, counts) if the device is lost while it runs.
// One compare when off or without NVIDIA's extension. Indirect: a, b = the guest address of the arguments, c = the draw count limit.
static void checkpoint_draw(uint32_t a, uint32_t b, uint32_t c, bool indirect) {
    gfx::Device& d = device();
    if (!d.checkpoints_on(gfx::Device::Checkpoint::Draw)) return;
    d.checkpoint(F->cb, gfx::Device::Checkpoint::Draw, S.stage[kVertex] ? &S.stage[kVertex]->hash : nullptr,
                 S.stage[kFragment] ? &S.stage[kFragment]->hash : nullptr, a, b, c, indirect);
}

void do_draw_arrays(const P_DrawArrays& p) {
    count_primitives(p.mode, p.count, p.instance_count);
    g_draw_base_instance = p.base_instance;
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), false, 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    checkpoint_draw(p.count, std::max<uint32_t>(1, p.instance_count), 0, false);
    if (conv == PrimConversion::None) {
        TRACE("    vertices %u instances %u first %u", p.count, p.instance_count, p.first);
        vkCmdDraw(F->cb, p.count, std::max<uint32_t>(1, p.instance_count), p.first, p.base_instance);
    } else {
        // QUADS / QUAD_STRIP / POLYGON / LINE_LOOP: generate an index buffer
        std::vector<uint32_t> in(p.count);
        for (uint32_t k = 0; k < p.count; ++k) in[k] = p.first + k;
        const std::vector<uint32_t> idx = convert_prim_indices(conv, in);
        if (idx.empty()) return;
        if (!bind_u32_indices(idx)) return;
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(idx.size()), std::max<uint32_t>(1, p.instance_count), 0, 0, p.base_instance);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
    g_pass_draws++;
    g_pass_verts += static_cast<uint64_t>(p.count) * std::max<uint32_t>(1, p.instance_count);
    trace_snap_after_draw();
}

void do_draw_elements(const P_DrawElements& p) {
    PoolReasonScope pr_(kReasonIndex);
    count_primitives(p.mode, p.count, p.instance_count);
    g_draw_base_instance = p.base_instance;
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), true, static_cast<int>(p.type), &topo)) { counters().draws_skipped.fetch_add(1); return; }
    if (g_trace) {   // the indices and the first attributes of the first vertices
        const uint32_t isz0 = index_size_bytes(static_cast<int>(p.type));
        const uint8_t* ix = host_ptr(p.index_address);
        TRACE("    indexed: count %u type %d instances %u base_vertex %d base_instance %u index address %llx", p.count, static_cast<int>(p.type), p.instance_count, static_cast<int>(p.base_vertex),
              p.base_instance, static_cast<unsigned long long>(p.index_address));
        for (uint32_t i = 0; ix && i < std::min<uint32_t>(p.count, 12); ++i) {
            uint32_t v = 0;
            if (isz0 == 1) v = ix[i]; else if (isz0 == 2) { uint16_t t; std::memcpy(&t, ix + i * 2, 2); v = t; } else std::memcpy(&v, ix + i * 4, 4);
            std::string a;
            for (uint32_t k = 0; k < S.attr_count && k < 3; ++k) {
                const VertexAttribInfo& at = S.attr[k];
                const auto& st = S.stream[at.stream];
                const GState::Bind& vb = S.vtx[at.stream];
                if (!vb.addr) continue;
                const float* f = reinterpret_cast<const float*>(vb.addr + static_cast<uint64_t>(static_cast<int64_t>(v) + p.base_vertex) * st.stride + at.relative_offset);
                char t[96]; std::snprintf(t, sizeof t, " a%u(fmt %d)=(%g %g %g)", k, at.format, f[0], f[1], f[2]);
                a += t;
            }
            TRACE("      index %u:%s", v, a.c_str());
        }
    }
    if (g_trace) {   // bounds of the first attributes over all indices of the draw (half and float formats decoded), to see what part of a texture a mesh maps
        const uint32_t isz1 = index_size_bytes(static_cast<int>(p.type));
        const uint8_t* ix = host_ptr(p.index_address);
        if (ix && p.count) {
            std::string a;
            for (uint32_t k = 0; k < S.attr_count && k < 3; ++k) {
                const VertexAttribInfo& at = S.attr[k];
                const auto& st = S.stream[at.stream];
                const GState::Bind& vb = S.vtx[at.stream];
                if (!vb.addr) continue;
                float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
                int nc = at.format == 41 ? 4 : at.format == 34 ? 3 : at.format == 17 ? 2 : at.format == 22 ? 2 : 0;
                for (uint32_t i = 0; nc && i < p.count && i < 20000; ++i) {
                    uint32_t v = 0;
                    if (isz1 == 1) v = ix[i]; else if (isz1 == 2) { uint16_t t; std::memcpy(&t, ix + i * 2, 2); v = t; } else std::memcpy(&v, ix + i * 4, 4);
                    const uint8_t* ve = reinterpret_cast<const uint8_t*>(vb.addr + static_cast<uint64_t>(static_cast<int64_t>(v) + p.base_vertex) * st.stride + at.relative_offset);
                    for (int c = 0; c < nc; ++c) {
                        float f;
                        if (at.format == 41 || at.format == 17) {
                            uint16_t h; std::memcpy(&h, ve + c * 2, 2);
                            const int e = (h >> 10) & 31, m = h & 1023; const float sg = (h & 0x8000) ? -1.f : 1.f;
                            f = e == 0 ? sg * m * (1.f / 16777216.f) : e == 31 ? sg * 65504.f : sg * (1.f + m / 1024.f) * std::ldexp(1.f, e - 15);
                        } else std::memcpy(&f, ve + c * 4, 4);
                        lo[c] = std::min(lo[c], f); hi[c] = std::max(hi[c], f);
                    }
                }
                char t[200];
                if (nc) std::snprintf(t, sizeof t, " a%u[%g..%g, %g..%g, %g..%g, %g..%g]", k, lo[0], hi[0], lo[1], hi[1], nc > 2 ? lo[2] : 0.f, nc > 2 ? hi[2] : 0.f, nc > 3 ? lo[3] : 0.f, nc > 3 ? hi[3] : 0.f);
                else std::snprintf(t, sizeof t, " a%u(fmt %d: not decoded)", k, at.format);
                a += t;
            }
            TRACE("    bounds:%s", a.c_str());
        }
    }
    const uint32_t isz = index_size_bytes(static_cast<int>(p.type));
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    const uint8_t* src = host_ptr(p.index_address);
    checkpoint_draw(p.count, std::max<uint32_t>(1, p.instance_count), 0, false);
    if (conv != PrimConversion::None || isz == 1) {
        // widen to 32 bit and/or convert the primitive
        std::vector<uint32_t> in(p.count);
        for (uint32_t i = 0; i < p.count; ++i) {
            uint32_t v = 0;
            if (isz == 1) v = src[i];
            else if (isz == 2) { uint16_t t; std::memcpy(&t, src + i * 2, 2); v = t; }
            else std::memcpy(&v, src + i * 4, 4);
            in[i] = v;
        }
        const std::vector<uint32_t> idx = convert_prim_indices(conv, in);
        if (idx.empty()) return;
        if (!bind_u32_indices(idx)) return;
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(idx.size()), std::max<uint32_t>(1, p.instance_count), 0, p.base_vertex, p.base_instance);
    } else {
        BufRef br = resolve_read(p.index_address, static_cast<uint64_t>(p.count) * isz, kReadIndex);
        bind_index_buffer(F->cb, br.buffer, br.offset, vk_index_type(static_cast<int>(p.type)));
        vkCmdDrawIndexed(F->cb, p.count, std::max<uint32_t>(1, p.instance_count), 0, p.base_vertex, p.base_instance);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
    g_pass_draws++;
    g_pass_verts += static_cast<uint64_t>(p.count) * std::max<uint32_t>(1, p.instance_count);
    trace_snap_after_draw();
}

// The bytes at a GPU address as the CPU sees them, when they are current: inside a pool, and not a range a submission that has not completed
// may write (gw_settled). Null otherwise. For decisions that need data the GPU would otherwise hold (indirect arguments, draw counts).
// Compute results held in the device copy ([nvn] compute_out_lazy) are newer than the pool: they are fetched back first (out_fetch: a
// completed submission's result, or one whose copy-back is recorded but has not run); one the submission being recorded writes cannot be
// read (null). Without the fetch an unguarded held result would be read stale, and a guarded one through a fault.
const uint8_t* cpu_view(uint64_t addr, uint64_t size) {
    if (!addr || !size) return nullptr;
    const PoolHit hit = pool_lookup(addr);
    if (!hit.pool || hit.pool->is_virtual || !hit.pool->base || hit.offset + size > hit.pool->size) return nullptr;
    if (!gw_settled(addr, size)) return nullptr;
    if (out_any()) {
        out_fetch(addr, size, false);
        if (out_overlaps(addr, size)) return nullptr;
    }
    return host_ptr(addr);
}

// ---- indirect quads on the GPU ([nvn] indirect_quads_gpu, default on) ----------------------------------------------------------
// Based on pokopia-native (MIT) windows/src/nxrt/vk/vk_replay.c + shaders/quad_args.comp @27a6df0 (quad_args).
// A single non-indexed indirect quad draw whose arguments a command earlier in the same submission writes (particle effects: a compute
// pass counts the live quads) cannot be converted by the CPU (cpu_view: null) and was skipped (#120). A one-thread dispatch
// (nvn_quad_args_shader.inc) turns the arguments into those of an indexed indirect draw over a shared quad index list (0,1,2, 0,2,3, ...
// for 65536 quads, made once), outside the pass: begin_draw reopens it, loading what the targets hold. Counts are clamped to 65536 quads
// and 65536 instances, so a count the GPU has not written yet cannot keep it busy past the OS timeout.
#include "nvn_quad_args_shader.inc"
constexpr uint32_t kQuadMax = 65536;
QuadArgs g_qa;

static bool indirect_quads_gpu_on() {
    static const bool on = nxrt::config_get_bool("nvn", "indirect_quads_gpu", true);
    return on;
}

static bool quad_args_init() {
    QuadArgs& q = g_qa;
    if (q.tried) return q.ok;
    q.tried = true;
    VkDevice d = vkdev();
    q.ssbo_align = std::max<VkDeviceSize>(16, device().properties().limits.minStorageBufferOffsetAlignment);
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 2;
    sli.pBindings = b;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &q.set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kQuadArgsComp;
    mi.pCode = kQuadArgsComp;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(d, &sli, nullptr, &q.set_layout) != VK_SUCCESS || vkCreatePipelineLayout(d, &pli, nullptr, &q.layout) != VK_SUCCESS ||
        vkCreateShaderModule(d, &mi, nullptr, &m) != VK_SUCCESS) {
        NXLOG_W("nvn.replay", "indirect quads: the argument shader could not be made: those draws take the CPU path");
        return false;
    }
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
    ci.layout = q.layout;
    const VkResult pr = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &q.pipe);
    vkDestroyShaderModule(d, m, nullptr);
    if (pr != VK_SUCCESS ||
        device().create_buffer(static_cast<VkDeviceSize>(kQuadMax) * 6 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               gfx::MemoryUse::GpuOnly, &q.index, "nvn quad indices") != VK_SUCCESS) {
        q.pipe = pr == VK_SUCCESS ? q.pipe : VK_NULL_HANDLE;
        NXLOG_W("nvn.replay", "indirect quads: the argument pipeline or the quad index buffer could not be made: those draws take the CPU path");
        return false;
    }
    q.ok = true;
    NXLOG_I("nvn.replay", "indirect quads whose arguments the GPU writes: made into an indexed indirect draw on the GPU ([nvn] indirect_quads_gpu)");
    return true;
}

// Records the dispatch that makes the indexed draw's 20 bytes of arguments from the 16 at p.indirect_address (ending the pass); *out = where
// they are. False: nothing recorded (the draw takes the CPU path).
static bool quad_args_gpu(const P_DrawIndirect& p, BufRef* out) {
    if (!quad_args_init()) return false;
    QuadArgs& q = g_qa;
    const BufRef src = held_or_pool(p.indirect_address, 16, kOutIndirect, false);   // a held compute result: the copy (out_before_op)
    if (!src.ok || src.offset % 4) return false;
    const VkDeviceSize base = src.offset / q.ssbo_align * q.ssbo_align;
    const UploadAlloc dst = F->upload(20, q.ssbo_align);
    if (!dst) return false;
    const VkDescriptorSet set = F->alloc_set(q.set_layout);
    if (!set) return false;
    const VkDescriptorBufferInfo bi[2] = {{src.buffer, base, src.offset - base + 16}, {dst.buffer, dst.offset, 20}};
    VkWriteDescriptorSet w[2] = {};
    for (int i = 0; i < 2; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = static_cast<uint32_t>(i);
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(vkdev(), 2, w, 0, nullptr);
    end_pass();
    if (!q.index_filled) {   // the shared quad index list, once (copied from this submission's staging)
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(kQuadMax) * 6 * 4;
        const UploadAlloc st = F->staging(bytes);
        if (!st) return false;
        uint32_t* ix = reinterpret_cast<uint32_t*>(st.ptr);
        for (uint32_t k = 0; k < kQuadMax; ++k) {
            const uint32_t v = k * 4;
            uint32_t* t = ix + static_cast<size_t>(k) * 6;
            t[0] = v; t[1] = v + 1; t[2] = v + 2; t[3] = v; t[4] = v + 2; t[5] = v + 3;
        }
        const VkBufferCopy r = {st.offset, 0, bytes};
        vkCmdCopyBuffer(F->cb, st.buffer, q.index.buffer, 1, &r);
        q.index_filled = true;
    }
    memory_barrier(F->cb);   // the GPU's own write of the arguments earlier in this submission (and the index copy) before the dispatch
    vkCmdBindPipeline(F->cb, VK_PIPELINE_BIND_POINT_COMPUTE, q.pipe);
    vkCmdBindDescriptorSets(F->cb, VK_PIPELINE_BIND_POINT_COMPUTE, q.layout, 0, 1, &set, 0, nullptr);
    const uint32_t at = static_cast<uint32_t>((src.offset - base) / 4);
    vkCmdPushConstants(F->cb, q.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &at);
    vkCmdDispatch(F->cb, 1, 1, 1);
    memory_barrier(F->cb);   // the dispatch's write before the indirect read
    g_bound_pipeline = VK_NULL_HANDLE;
    out->buffer = dst.buffer;
    out->offset = dst.offset;
    out->ok = true;
    return true;
}

// An indirect draw Vulkan cannot take as it is (quads, quad strips, polygons and line loops have no topology; 8-bit indices need
// VK_EXT_index_type_uint8): each draw's arguments (and its indices) are read by the CPU and the draw is made as a direct indexed draw of
// the converted 32-bit index list, like DrawArrays / DrawElements. That needs the arguments in memory the CPU can read: arguments a compute
// shader or a copy wrote and no completed submission has handed back are not (skipped, logged). A multi-draw reads its count the same way.
static void draw_indirect_cpu(const P_DrawIndirect& p, PrimConversion conv, uint32_t isz, bool indexed) {
    static bool noted = false;
    if (!noted) {
        noted = true;
        NXLOG_I("nvn.replay", "replay: indirect draw by CPU index conversion (first): mode %u, %s, index size %u, %s", p.mode, indexed ? "indexed" : "arrays", isz,
                p.multi ? "multi-draw" : "single");
    }
    const uint64_t cmd_size = indexed ? 20 : 16;
    const uint64_t stride = p.stride > 0 ? static_cast<uint64_t>(p.stride) : cmd_size;
    uint32_t n = 1;
    if (p.multi && p.count_address) {
        const uint8_t* c = cpu_view(p.count_address, 4);
        if (!c) { note_unsupported("indirect draw conversion: the draw count is not readable by the CPU (written by the GPU)"); counters().draws_skipped.fetch_add(1); return; }
        std::memcpy(&n, c, 4);
        n = std::min(n, p.max_count);
    }
    bool drew = false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* a = cpu_view(p.indirect_address + static_cast<uint64_t>(i) * stride, cmd_size);
        if (!a) { note_unsupported("indirect draw conversion: the arguments are not readable by the CPU (written by the GPU)"); break; }
        uint32_t v[5] = {};
        std::memcpy(v, a, cmd_size);
        const uint32_t count = v[0], instances = v[1];
        if (!count || !instances) continue;
        if (!indirect_args_plausible(count, instances)) { note_unsupported("indirect draw conversion: implausible arguments (stale guest memory?)"); continue; }
        std::vector<uint32_t> in(count);
        int32_t base_vertex = 0;
        uint32_t base_instance = v[3];
        if (indexed) {
            base_vertex = static_cast<int32_t>(v[3]);
            base_instance = v[4];
            const uint8_t* src = cpu_view(p.index_address + static_cast<uint64_t>(v[2]) * isz, static_cast<uint64_t>(count) * isz);
            if (!src) { note_unsupported("indirect draw conversion: the indices are not readable by the CPU (written by the GPU)"); break; }
            for (uint32_t k = 0; k < count; ++k) {
                uint32_t x = 0;
                if (isz == 1) x = src[k];
                else if (isz == 2) { uint16_t t; std::memcpy(&t, src + k * 2, 2); x = t; }
                else std::memcpy(&x, src + k * 4, 4);
                in[k] = x;
            }
        } else {
            for (uint32_t k = 0; k < count; ++k) in[k] = v[2] + k;
        }
        count_primitives(p.mode, count, instances);
        const std::vector<uint32_t> idx = convert_prim_indices(conv, in);
        if (idx.empty() || !bind_u32_indices(idx)) continue;
        checkpoint_draw(count, instances, 0, false);   // the CPU read the arguments: a direct draw
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(idx.size()), instances, 0, base_vertex, base_instance);
        drew = true;
    }
    if (drew) {
        counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
        g_pass_draws++;
    } else {
        counters().draws_skipped.fetch_add(1);
    }
    trace_snap_after_draw();
}

void do_draw_indirect(const P_DrawIndirect& p) {
    PoolReasonScope pr_(kReasonIndirect);
    const bool indexed = p.type != 0xffffffffu;
    // the base instance is in the arguments, which the GPU may write and a multi-draw holds one of per draw: the vertex shaders read it
    // as Vulkan's BaseInstance (spirv_base_instance_from_c0), so the c[0] word is not needed and stays 0
    g_draw_base_instance = 0;
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    // indirect quads whose arguments the CPU cannot read (the GPU writes them): the indexed arguments made on the GPU, before the pass opens
    BufRef qa{};
    if (conv == PrimConversion::Quads && !indexed && !p.multi && indirect_quads_gpu_on() && S.render_enable && S.stage[kVertex] &&
        !cpu_view(p.indirect_address, 16))
        quad_args_gpu(p, &qa);
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), indexed, indexed ? static_cast<int>(p.type) : 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    if (qa.ok) {
        bind_index_buffer(F->cb, g_qa.index.buffer, 0, VK_INDEX_TYPE_UINT32);
        checkpoint_draw(static_cast<uint32_t>(p.indirect_address), static_cast<uint32_t>(p.indirect_address >> 32), 1, true);
        vkCmdDrawIndexedIndirect(F->cb, qa.buffer, qa.offset, 1, 20);
        const uint64_t n = ++g_qa.draws;
        if ((n & (n - 1)) == 0)   // 1, 2, 4, 8, ...
            NXLOG_I("nvn.replay", "indirect quads drawn with GPU-made arguments: %llu so far", static_cast<unsigned long long>(n));
        counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
        g_pass_draws++;
        trace_snap_after_draw();
        return;
    }
    const uint32_t isz = indexed ? index_size_bytes(static_cast<int>(p.type)) : 0;
    if (conv != PrimConversion::None || (indexed && isz == 1 && !device().features().index_type_uint8)) {
        draw_indirect_cpu(p, conv, isz, indexed);
        return;
    }
    // GPU path. The primitives of these draws are not known to the CPU: the primitive counters see them through a query window (ResetCounter).
    static bool noted_indirect = false, noted_u8 = false;
    if (!noted_indirect) { noted_indirect = true; NXLOG_I("nvn.replay", "replay: indirect draw (first): mode %u, %s%s", p.mode, indexed ? "indexed" : "arrays", p.multi ? ", multi-draw" : ""); }
    if (indexed && isz == 1 && !noted_u8) { noted_u8 = true; NXLOG_I("nvn.replay", "replay: indirect draw with 8-bit indices (first): VK_EXT_index_type_uint8"); }
    const uint64_t cmd_size = indexed ? 20 : 16;
    const uint64_t n_cmds = p.multi && p.count_address ? std::max<uint64_t>(1, p.max_count) : 1;
    BufRef ib = held_or_pool(p.indirect_address, (p.stride ? static_cast<uint64_t>(p.stride) : cmd_size) * (n_cmds - 1) + cmd_size, kOutIndirect, false);
    if (indexed) {
        BufRef xb = resolve_buffer(p.index_address, 4);
        if (pool_copy_on()) pool_copy_touch(p.index_address, indirect_index_span(p));   // [nvn] pool_copy: every index the draw may read
        bind_index_buffer(F->cb, xb.buffer, xb.offset, vk_index_type(static_cast<int>(p.type)));
    }
    checkpoint_draw(static_cast<uint32_t>(p.indirect_address), static_cast<uint32_t>(p.indirect_address >> 32), static_cast<uint32_t>(n_cmds), true);
    if (p.multi && p.count_address) {
        BufRef cb = held_or_pool(p.count_address, 4, kOutIndirect, false);
        const uint32_t stride = static_cast<uint32_t>(p.stride ? p.stride : cmd_size);
        if (indexed) vkCmdDrawIndexedIndirectCount(F->cb, ib.buffer, ib.offset, cb.buffer, cb.offset, p.max_count, stride);
        else vkCmdDrawIndirectCount(F->cb, ib.buffer, ib.offset, cb.buffer, cb.offset, p.max_count, stride);
    } else if (indexed) {
        vkCmdDrawIndexedIndirect(F->cb, ib.buffer, ib.offset, 1, 20);
    } else {
        vkCmdDrawIndirect(F->cb, ib.buffer, ib.offset, 1, 16);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
    g_pass_draws++;
    trace_snap_after_draw();
}

// DrawTransformFeedback(mode, control): draws what the feedback section begun with this control buffer wrote, as many vertices as it wrote
// (vkCmdDrawIndirectByteCountEXT over the section's byte counter, divided by the stride of the vertex stream that reads the feedback buffer). The game
// binds the feedback buffer as an ordinary vertex buffer. The feedback writes are ordered before the vertex fetch by ending the pass first.
void do_draw_xfb(const P_DrawXfb& p) {
    static bool noted = false;
    if (!noted) { noted = true; NXLOG_I("nvn.xfb", "replay: transform feedback draw (first): mode %u control %llx", p.mode, static_cast<unsigned long long>(p.control)); }
    if (!S.render_enable) return;
    if (!device().features().transform_feedback) { note_unsupported("DrawTransformFeedback (the device has no VK_EXT_transform_feedback)"); counters().draws_skipped.fetch_add(1); return; }
    const auto it = g_xfb_slot_of.find(p.control);
    if (!g_xfb_counter || it == g_xfb_slot_of.end() || !g_xfb_slot_written[it->second]) {
        TRACE("DrawTransformFeedback control %llx: the section wrote nothing", static_cast<unsigned long long>(p.control));   // zero bytes: zero vertices
        counters().draws_skipped.fetch_add(1);
        return;
    }
    if (prim_conversion(static_cast<int>(p.mode)) != PrimConversion::None) { note_unsupported("DrawTransformFeedback of a quad/polygon/line-loop primitive"); return; }
    const uint32_t slot = it->second;
    end_pass();
    memory_barrier(F->cb);   // the section's writes (feedback buffer, counter) before the vertex fetch and the indirect read
    g_draw_base_instance = 0;   // vkCmdDrawIndirectByteCountEXT draws from instance 0
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), false, 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    uint32_t stride = g_key_attr_count ? S.stream[g_key_binding[0]].stride : 0;
    for (uint32_t b = 0; !stride && b < 16; ++b) stride = S.stream[b].stride;
    if (!stride) { note_unsupported("DrawTransformFeedback with no vertex stream stride"); counters().draws_skipped.fetch_add(1); return; }
    checkpoint_draw(static_cast<uint32_t>(p.control), static_cast<uint32_t>(p.control >> 32), 1, true);   // the count is the section's byte counter
    vkCmdDrawIndirectByteCountEXT(F->cb, 1, 0, g_xfb_counter.buffer, static_cast<VkDeviceSize>(slot) * kXfbSlotBytes, 0, stride);
    counters().draws.fetch_add(1, std::memory_order_relaxed); ft::split_count(ft::kCntDraws);
    g_pass_draws++;
    trace_snap_after_draw();
}

std::string g_cs_missing;   // trace: the untranslated block(s) of the bound compute program ("cs -" dispatches)

// ---- [nvn] async_compute: which dispatches may be skipped while their pipeline builds -----------------------------------------------
// Only one whose writes the CPU cannot see: every storage buffer it binds lies in one pool with NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS (not
// virtual: its physical backing may be CPU-visible) and in no 64 KiB granule a buffer copy ever read into a CPU-visible pool, and no storage
// image it binds is a texture download_texture read back. Image buffers (storage texel buffers) are not checked: such a dispatch is never
// skipped. Render thread.
static std::unordered_set<uint64_t> g_cs_cpu_read;   // granules (address >> 16) copied into CPU-visible pools (run_list, CopyBufferToBuffer)

void note_copy_for_cpu(uint64_t src, uint64_t dst, uint64_t size) {
    static const bool on = async_compute_on();
    if (!on || !size || (pool_flags_at(dst) & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) return;
    size = std::min<uint64_t>(size, 1ull << 34);              // a malformed size: bounded (the copy itself fails to resolve)
    for (uint64_t g = src >> 16, e = (src + size - 1) >> 16; g <= e; ++g) g_cs_cpu_read.insert(g);
}

static bool dispatch_cpu_blind(const ProgLayout& pl) {
    for (const BindingDesc& d : pl.bindings) {
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) return false;
        if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            const TexResolved r = resolve_image(d);
            if (r.tex && tex_was_read_back(r.tex)) return false;
            continue;
        }
        if (d.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || b.nvn.source != NvnSource::StorageBuffer) continue;   // read only, or ours
        const int slot = b.nvn.index >= 0 ? b.nvn.index : storage_slot_for(d.nvn_stage, b.nvn.c0_offset);       // as bind_sets
        if (slot < 0 || slot >= 16 || !S.ssbo[d.nvn_stage][slot].addr) continue;                                // the dummy is bound
        const GState::Bind& bind = S.ssbo[d.nvn_stage][slot];
        if (!bind.size) return false;                         // the range would be the rest of the pool: not checked
        const uint64_t lo = bind.addr, hi = bind.addr + bind.size - 1;
        const uint32_t f = pool_flags_at(lo);
        if (!(f & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) || (f & NVN_MEMORY_POOL_FLAGS_VIRTUAL)) return false;
        if (pool_lookup(lo).pool != pool_lookup(hi).pool) return false;
        for (uint64_t g = lo >> 16; g <= (hi >> 16); ++g)
            if (g_cs_cpu_read.count(g)) return false;
    }
    return true;
}

void do_dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {
    RpScope rp_all(kRpDispatch);
    const ShaderInfo* cs = S.stage[kCompute];
    TRACE("dispatch %u %u %u indirect %llx cs %s%s%s", x, y, z, static_cast<unsigned long long>(indirect), cs ? cs->hash.c_str() : "-",
          !cs && !g_cs_missing.empty() ? " missing " : "", !cs ? g_cs_missing.c_str() : "");
    if (!cs) return;
    const ShaderInfo* stages[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, cs};
    const ProgLayout* pl = prog_layout(stages);
    if (!pl) return;
    ResolveScope rs_;
    rs_.arm(pl);
    prepare_textures(*pl);
    end_pass();
    if (g_trace) {   // the compute bindings: buffers (source, slot, address), texel buffers and textures, and the first words of each uniform block
        for (const BindingDesc& d : pl->bindings) {
            const DescriptorBinding& b = d.shader->refl.bindings[d.index];
            if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                const int slot = b.nvn.source == NvnSource::StorageBuffer ? (b.nvn.index >= 0 ? b.nvn.index : storage_slot_for(d.nvn_stage, b.nvn.c0_offset)) : b.nvn.index;
                const bool ssbo = b.nvn.source == NvnSource::StorageBuffer;
                const GState::Bind* bd = (slot >= 0 && slot < 16) ? (ssbo ? &S.ssbo[d.nvn_stage][slot] : &S.ubo[d.nvn_stage][slot]) : nullptr;
                std::string words;
                if (bd && bd->addr && resolve_buffer(bd->addr, 4).ok) {
                    const uint32_t* w = reinterpret_cast<const uint32_t*>(host_ptr(bd->addr));
                    for (uint32_t i = 0; i < 128 && i * 4 < bd->size; ++i) { char t[16]; std::snprintf(t, sizeof t, " %08x", w[i]); words += t; }
                }
                TRACE("    cs b%u %s src %d slot %d c0 %d addr %llx/%llu:%s", d.binding, b.name.c_str(), static_cast<int>(b.nvn.source), slot, b.nvn.c0_offset,
                      bd ? static_cast<unsigned long long>(bd->addr) : 0ull, bd ? static_cast<unsigned long long>(bd->size) : 0ull, words.c_str());
            } else {
                const bool img = d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
                const TexResolved r = img ? resolve_image(d) : resolve_texture(d);
                char c[96];
                std::string words;
                if (r.tex && r.tex->mem) {
                    const uint32_t* w = reinterpret_cast<const uint32_t*>(r.tex->mem);
                    for (uint32_t i = 0; i < 64; ++i) { char t[16]; std::snprintf(t, sizeof t, " %08x", w[i]); words += t; }
                }
                TRACE("    cs b%u %s type %d -> %s @%p:%s", d.binding, b.name.c_str(), static_cast<int>(d.type), tex_desc(r.tex, c, sizeof c), r.tex ? static_cast<void*>(r.tex->mem) : nullptr, words.c_str());
            }
        }
    }
    // [nvn] async_compute in play (the loading gate): a dispatch whose pipeline is not built and whose writes the CPU cannot see is skipped
    // while its pipeline goes first in line on the async threads (pop-in for a frame or two). Any other is built here as before and logged.
    if (async_compute_on() && gpl_gate_open() && !compute_pipeline_done(cs, *pl) && dispatch_cpu_blind(*pl) && compute_queue_front(cs, *pl)) {
        compute_note_skipped();
        ft::mark("cs.skip");
        static int logged = 0;
        if (logged < 50) {
            ++logged;
            NXLOG_I(kTag, "compute: dispatch skipped while its pipeline builds in the background: %s", cs->hash.c_str());
        }
        return;
    }
    VkPipeline pipe = compute_pipeline(cs, *pl);
    if (!pipe) return;
    vkCmdBindPipeline(F->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    g_bound_pipeline = VK_NULL_HANDLE;
    support_for_draw(*pl, true);
    const BufRef ind = indirect ? held_or_pool(indirect, 12, kOutIndirect, true) : BufRef{};   // arguments a dispatch wrote: from the copy
    g_shadowing = compute_shadows_on();
    g_shadow_uses.clear();
    g_sh_barrier = g_sh_copied = false;
    bind_sets(*pl, VK_PIPELINE_BIND_POINT_COMPUTE);
    if (g_sh_copied) {
        memory_barrier(F->cb);     // the copies into the shadows before the dispatch reads them
        gstamp("shadow in");
    }
    // [gfx] gpu_checkpoints: a checkpoint before the dispatch names it (shader, groups or the guest address of indirect arguments) if the
    // device is lost while it runs. One compare when off or without NVIDIA's extension.
    if (device().checkpoints_on(gfx::Device::Checkpoint::Dispatch)) {
        if (indirect) device().checkpoint(F->cb, gfx::Device::Checkpoint::Dispatch, &cs->hash, nullptr, static_cast<uint32_t>(indirect),
                                          static_cast<uint32_t>(indirect >> 32), 0, true);
        else device().checkpoint(F->cb, gfx::Device::Checkpoint::Dispatch, &cs->hash, nullptr, x, y, z, false);
    }
    if (indirect) {
        vkCmdDispatchIndirect(F->cb, ind.buffer, ind.offset);
    } else {
        vkCmdDispatch(F->cb, x, y, z);
        if (gpass_on()) gstamp(("cs " + cs->hash.substr(0, 8)).c_str());
    }
    shadows_after_dispatch();
    memory_barrier(F->cb);
    g_dirty = kAllDirty;
    shadow_invalidate();
    counters().dispatches.fetch_add(1, std::memory_order_relaxed);
}

// ---- conditional render ---------------------------------------------------------------------------------------------------------
void note_report(uint64_t address, uint64_t counter) {
    if (!address) return;
    if (g_reports.size() > 65536) g_reports.clear();   // bounded: a forgotten address reads from memory again
    g_reports[address] = counter;
}

// The counter word at a report address; false when it is not known before the GPU runs.
static bool report_counter(uint64_t address, uint64_t* out) {
    auto it = g_reports.find(address);
    if (it != g_reports.end()) {
        if (it->second == kReportUnknown) return false;
        *out = it->second;
        return true;
    }
    const PoolHit hit = pool_lookup(address);
    if (!hit.pool || hit.pool->is_virtual || !hit.pool->base || hit.offset + 8 > hit.pool->size) return false;
    std::memcpy(out, host_ptr(address), 8);
    return true;
}

bool conditional_render_enabled(uint32_t mode, uint64_t address) {
    uint64_t a = 0, b = 0;
    if (!report_counter(address, &a) || !report_counter(address + 16, &b)) return true;
    if (mode == NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_EQUAL) return a == b;
    if (mode == NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_NOT_EQUAL) return a != b;
    return true;
}

// ---- clears -------------------------------------------------------------------------------------------------------------------
static VkClearRect full_rect() {
    VkClearRect r = {};
    r.rect = S.nsc ? scissor_rect(S.sc[0]) : VkRect2D{{0, 0}, P.extent};
    r.baseArrayLayer = 0;
    r.layerCount = 1;
    return r;
}

void do_clear_color(const P_ClearColor& p) {
    if (g_trace) {   // the value (as floats for kind 0, else as words) and the rectangle the clear covers (the scissor when one is set)
        char a[96], v[96];
        if (p.kind == 0) {
            float f[4];
            std::memcpy(f, p.value, sizeof f);
            std::snprintf(v, sizeof v, "%g %g %g %g", f[0], f[1], f[2], f[3]);
        } else {
            std::snprintf(v, sizeof v, "%08x %08x %08x %08x", p.value[0], p.value[1], p.value[2], p.value[3]);
        }
        const VkClearRect cr = full_rect();
        TRACE("clear color rt%u mask %x kind %u %s value %s rect %d %d %u %u", p.target, p.mask, p.kind, tex_desc(p.target < 8 ? S.color_tex[p.target].get() : nullptr, a, sizeof a), v,
              cr.rect.offset.x, cr.rect.offset.y, cr.rect.extent.width, cr.rect.extent.height);
    }
    if (!S.render_enable) return;   // render enable gates clears as it gates draws (yuzu DrawManager::Clear)
    if (p.target >= 8 || !S.color_tex[p.target]) return;
    if (p.mask == 0) return;
    if (!ensure_pass()) return;
    if (!P.color_fmt[p.target]) return;
    if (p.mask != 0xf) {
        // vkCmdClearAttachments ignores the colour write mask: a masked clear is a draw with that mask
        if (!g_mask_ready) { g_mask_ok = g_mask.init(device()); g_mask_ready = true; }
        const VkClearRect cr = full_rect();
        if (g_mask_ok && g_mask.record(F->cb, P.color_fmt[p.target], P.ncolor, p.target, p.mask, p.kind, p.value, cr.rect, P.samples)) {
            static bool noted = false;
            if (!noted) { noted = true; NXLOG_I("nvn.replay", "replay: ClearColor with a partial channel mask (first): rt%u mask %x, drawn with that colour write mask", p.target, p.mask); }
            counters().clears.fetch_add(1, std::memory_order_relaxed);
            g_bound_pipeline = VK_NULL_HANDLE;
            g_dirty = kAllDirty;
            g_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;   // binding a pipeline without these dynamic states invalidates them
            g_restart_valid = false;
            shadow_invalidate();
            return;
        }
        note_unsupported("ClearColor with a partial channel mask (all channels are cleared)");
    }
    VkClearAttachment ca = {};
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.colorAttachment = p.target;
    std::memcpy(ca.clearValue.color.uint32, p.value, 16);
    VkClearRect r = full_rect();
    vkCmdClearAttachments(F->cb, 1, &ca, 1, &r);
    counters().clears.fetch_add(1, std::memory_order_relaxed);
}

void do_clear_depth_stencil(const P_ClearDepthStencil& p) {
    TRACE("clear depth/stencil d %g (mask %d) s %d (mask %x)", p.depth, p.depth_mask, p.stencil, p.stencil_mask);
    if (!S.render_enable) return;
    if (!S.depth_tex) return;
    if (!ensure_pass()) return;
    VkClearAttachment ca = {};
    if (p.depth_mask) ca.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (p.stencil_mask && P.has_stencil) ca.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!ca.aspectMask) return;
    if (p.stencil_mask && p.stencil_mask != 0xff && P.has_stencil && !P.depth_ro) {
        // vkCmdClearAttachments has no stencil write mask: a masked stencil clear is a draw with that write mask (depth goes along)
        if (!g_mask_ready) { g_mask_ok = g_mask.init(device()); g_mask_ready = true; }
        const VkClearRect cr = full_rect();
        if (g_mask_ok && g_mask.record_depth_stencil(F->cb, P.color_fmt, P.ncolor, P.depth_fmt, true, P.samples, p.depth_mask != 0, p.depth, p.stencil_mask,
                                                    static_cast<uint32_t>(p.stencil), cr.rect)) {
            static bool noted = false;
            if (!noted) { noted = true; NXLOG_I("nvn.replay", "replay: ClearDepthStencil with a partial stencil mask (first): mask %x, drawn with that stencil write mask", p.stencil_mask); }
            counters().clears.fetch_add(1, std::memory_order_relaxed);
            g_bound_pipeline = VK_NULL_HANDLE;
            g_dirty = kAllDirty;
            g_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;   // binding a pipeline without these dynamic states invalidates them
            g_restart_valid = false;
            shadow_invalidate();
            return;
        }
        note_unsupported("ClearDepthStencil with a partial stencil mask (the whole stencil is cleared)");
    }
    ca.clearValue.depthStencil = {p.depth, static_cast<uint32_t>(p.stencil)};
    VkClearRect r = full_rect();
    vkCmdClearAttachments(F->cb, 1, &ca, 1, &r);
    counters().clears.fetch_add(1, std::memory_order_relaxed);
}

// ---- copies ------------------------------------------------------------------------------------------------------------------
struct SubRes { uint32_t level = 0, layer = 0, layers = 1; };
static SubRes sub_of(const TextureViewInfo& v, bool has_view) {
    SubRes s;
    if (has_view) {
        if (v.has_levels) s.level = static_cast<uint32_t>(v.min_level);
        if (v.has_layers) { s.layer = static_cast<uint32_t>(v.min_layer); s.layers = 1; }
    }
    return s;
}

void do_clear_texture(const P_ClearTexture& p) {
    Texture* t = p.tex;
    if (!t || !tex_gpu(t)) return;
    end_pass();
    SubRes s = sub_of(p.view, p.has_view);
    tex_ensure_current(t, *F);
    tex_transition(t, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    tex_mark_gpu_written(t);
    if (p.has_region) {
        const VkExtent3D e = tex_extent(t, s.level);
        if (p.region.x != 0 || p.region.y != 0 || (p.region.w && static_cast<uint32_t>(p.region.w) < e.width) || (p.region.h && static_cast<uint32_t>(p.region.h) < e.height))
            note_unsupported("ClearTexture of a sub-region (the whole level is cleared)");
    }
    VkImageSubresourceRange rng = {tex_aspect(t), s.level, 1, s.layer, p.has_view && p.view.has_layers ? static_cast<uint32_t>(std::max(1, p.view.num_layers)) : tex_layers(t)};
    if (tex_is_depth(t)) {
        VkClearDepthStencilValue v = {};
        std::memcpy(&v.depth, &p.value[0], 4);
        vkCmdClearDepthStencilImage(F->cb, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rng);
    } else {
        VkClearColorValue v = {};
        std::memcpy(v.uint32, p.value, 16);
        vkCmdClearColorImage(F->cb, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rng);
    }
    counters().clears.fetch_add(1, std::memory_order_relaxed);
    gstamp("clear texture");
}

void do_copy_buffer_texture(const P_CopyBufTex& p, bool to_texture) {
    PoolReasonScope pr_(kReasonImageCopy);
    Texture* t = p.tex;
    if (t && t->b.target == NVN_TEXTURE_TARGET_3D) {
        static int logged = 0;
        if (logged < 30) {
            ++logged;
            char a[96];
            NXLOG_I("nvn.copy", "%s 3D %s region %s (%d,%d,%d %dx%dx%d) buffer %llx row %lld image %lld", to_texture ? "buffer -> texture" : "texture -> buffer", tex_desc(t, a, sizeof a),
                    p.has_region ? "given" : "whole", p.has_region ? p.region.x : 0, p.has_region ? p.region.y : 0, p.has_region ? p.region.z : 0,
                    p.has_region ? p.region.w : 0, p.has_region ? p.region.h : 0, p.has_region ? p.region.d : 0, static_cast<unsigned long long>(p.address),
                    static_cast<long long>(p.row_stride), static_cast<long long>(p.image_stride));
        }
    }
    if (!t || !tex_gpu(t)) return;
    end_pass();
    SubRes s = sub_of(p.view, p.has_view);
    const VkExtent3D ext = tex_extent(t, s.level);
    P_CopyRegion r = p.has_region ? p.region : P_CopyRegion{0, 0, 0, static_cast<int32_t>(ext.width), static_cast<int32_t>(ext.height), static_cast<int32_t>(ext.depth)};
    const FormatInfo* fi = t->fmt;
    const uint32_t bw = fi ? fi->block_w : 1, bh = fi ? fi->block_h : 1, bpb = fi ? fi->bytes : 4;
    uint32_t rw = static_cast<uint32_t>(r.w), rh = static_cast<uint32_t>(r.h), rd = std::max<int32_t>(1, r.d);
    VkBufferImageCopy c = {};
    const uint64_t row_blocks = (rw + bw - 1) / bw;
    const uint64_t row_bytes = row_blocks * bpb;
    const uint64_t stride = p.row_stride > 0 ? static_cast<uint64_t>(p.row_stride) : row_bytes;
    c.bufferRowLength = static_cast<uint32_t>(stride / bpb * bw);
    c.bufferImageHeight = p.image_stride > 0 ? static_cast<uint32_t>(static_cast<uint64_t>(p.image_stride) / stride * bh) : 0;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s.level, s.layer, 1};
    if (tex_is_depth(t)) c.imageSubresource.aspectMask = tex_aspect(t) & VK_IMAGE_ASPECT_DEPTH_BIT;
    c.imageOffset = {r.x, r.y, r.z};
    c.imageExtent = {rw, rh, rd};
    if (t->b.target != NVN_TEXTURE_TARGET_3D && t->layers > 1 && !p.has_view) { c.imageSubresource.layerCount = tex_layers(t); c.imageExtent.depth = 1; }
    if (t->b.target != NVN_TEXTURE_TARGET_3D && rd > 1) { c.imageSubresource.layerCount = rd; c.imageSubresource.baseArrayLayer = s.layer + static_cast<uint32_t>(std::max(0, r.z)); c.imageOffset.z = 0; c.imageExtent.depth = 1; }
    // The bytes Vulkan touches: every layer / slice and row at the buffer's strides, the last row only as wide as the region. A copy
    // whose buffer does not hold all of it (an address in no importable pool resolves to the 64 KiB dummy buffer, a chunk or span
    // import shorter than the range, a copy of every layer sized for one) would read or write past the end of a VkBuffer: device lost
    // on NVIDIA (issue #121, saving a Pokédex photo reads a whole screen back into a buffer). Such copies are skipped.
    const uint64_t rows = (rh + bh - 1) / bh;
    if (c.imageSubresource.layerCount <= 1 && c.imageExtent.depth <= 1) c.bufferImageHeight = 0;   // one slice: the image stride is not used
    const uint64_t row_len = c.bufferRowLength ? (c.bufferRowLength + bw - 1) / bw * bpb : row_bytes;
    const uint64_t slice_rows = c.bufferImageHeight ? (c.bufferImageHeight + bh - 1) / bh : rows;
    const uint64_t slices = static_cast<uint64_t>(c.imageSubresource.layerCount) * std::max<uint32_t>(1, c.imageExtent.depth);
    const bool layout_ok = rw && rh && rows && (!c.bufferRowLength || c.bufferRowLength >= rw) && (!c.bufferImageHeight || c.bufferImageHeight >= rh);
    const uint64_t buf_bytes = layout_ok ? ((slices - 1) * slice_rows + (rows - 1)) * row_len + row_bytes : 0;
    const auto refused = [&](const char* why) {
        static int logged = 0;
        if (logged++ < 20) {
            char a[96];
            NXLOG_W(kTag, "%s %s skipped: %s (buffer %llx, %llu bytes, %ux%u region, row %lld image %lld)", to_texture ? "buffer copy into" : "copy to a buffer from",
                    tex_desc(t, a, sizeof a), why, static_cast<unsigned long long>(p.address), static_cast<unsigned long long>(buf_bytes), rw, rh,
                    static_cast<long long>(p.row_stride), static_cast<long long>(p.image_stride));
        }
    };
    if (!layout_ok) { refused("the region is empty or the buffer strides are smaller than it"); return; }
    if (!to_texture) compute_out_guard(p.address, buf_bytes, kOutBufferWrite);     // written: held results there go back first
    BufRef br = to_texture ? held_or_pool(p.address, buf_bytes, kOutBufferRead, true) : resolve_buffer(p.address, buf_bytes);
    if (!br.ok || br.avail < buf_bytes) { refused("the buffer range is not wholly in an imported memory pool"); return; }
    c.bufferOffset = br.offset;
    // The region and buffer layout above are in the NVN format. An image in another layout (an HDR-promoted RGBA16F target, a wider
    // fallback, transcoded ASTC) would make Vulkan address the buffer in its own texel size: past the game's bytes (issue #76: device
    // lost on NVIDIA) and in the wrong encoding. Such copies convert through a blit, or are skipped. A scaled render target goes through
    // the same blit (its region in image texels, filtered down to the NVN size) so the buffer gets the whole picture at the size the
    // game asked for; when that blit is not possible it is copied as before (the NVN texels: the top-left part of the image).
    const bool convert = fi && !vk_copy_layout_matches(tex_vk_format(t), fi->vk, c.imageSubresource.aspectMask);
    const bool rescale = fi && !convert && tex_scale(t) != 1.0f;
    const auto unscaled = [&](const char* why) {
        static int logged = 0;
        if (logged++ < 10) {
            char a[96];
            NXLOG_W(kTag, "%s a scaled render target %s: no scaling blit (%s), the copy uses NVN texel coordinates (wrong part of the image)",
                    to_texture ? "buffer copied into" : "copy from", tex_desc(t, a, sizeof a), why ? why : "?");
        }
    };
    const auto skipped = [&](const char* why) {
        static int logged = 0;
        if (logged++ < 20) {
            char a[96];
            NXLOG_W(kTag, "%s %s skipped: the image (format %d) is not laid out like the NVN format and cannot be converted (%s)",
                    to_texture ? "buffer copy into" : "copy to a buffer from", tex_desc(t, a, sizeof a), static_cast<int>(tex_vk_format(t)), why);
        }
    };
    if (to_texture) {
        tex_ensure_current(t, *F);
        tex_transition(t, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        tev(t, "copy_buffer_to_texture", p.address, p.has_view ? 1 : 0);
        for (uint32_t i = 0; i < c.imageSubresource.layerCount; ++i) tex_sparse_before_copy(t, c.imageSubresource.baseArrayLayer + i, s.level);
        const char* why = nullptr;
        if (convert) {
            if (!tex_copy_buffer_converted(t, *F, br.buffer, c, true, &why)) { skipped(why); return; }
        } else if (!rescale || !tex_copy_buffer_converted(t, *F, br.buffer, c, true, &why)) {
            if (rescale) unscaled(why);
            vkCmdCopyBufferToImage(F->cb, br.buffer, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        tex_mark_gpu_written(t);
    } else {
        tex_ensure_current(t, *F);
        tex_transition(t, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const char* why = nullptr;
        if (convert) {
            if (!tex_copy_buffer_converted(t, *F, br.buffer, c, false, &why)) { skipped(why); return; }
        } else if (!rescale || !tex_copy_buffer_converted(t, *F, br.buffer, c, false, &why)) {
            if (rescale) unscaled(why);
            vkCmdCopyImageToBuffer(F->cb, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, br.buffer, 1, &c);
        }
        gpu_wrote(p.address, buf_bytes, kWriteImageCopy);
    }
    memory_barrier(F->cb);
    counters().copies.fetch_add(1, std::memory_order_relaxed);
    gstamp(to_texture ? "copy buffer->texture" : "copy texture->buffer");
}

void do_copy_texture_texture(const P_CopyTexTex& p) {
    Texture *s = p.src, *d = p.dst;
    if (!s || !d || !tex_gpu(s) || !tex_gpu(d)) return;
    end_pass();
    const SubRes ss = sub_of(p.src_view, p.has_src_view), ds = sub_of(p.dst_view, p.has_dst_view);
    const VkExtent3D se = tex_extent(s, ss.level), de = tex_extent(d, ds.level);
    P_CopyRegion sr = p.has_src_region ? p.src_region : P_CopyRegion{0, 0, 0, static_cast<int32_t>(se.width), static_cast<int32_t>(se.height), static_cast<int32_t>(se.depth)};
    P_CopyRegion dr = p.has_dst_region ? p.dst_region : P_CopyRegion{0, 0, 0, static_cast<int32_t>(de.width), static_cast<int32_t>(de.height), static_cast<int32_t>(de.depth)};
    // scaled images: the NVN regions in image texels (x, y and the size times each image's scale, clipped to the image)
    auto scale_region = [](P_CopyRegion& r, Texture* t, uint32_t level) {
        const float sc = tex_scale(t);
        if (sc == 1.0f) return;
        const VkExtent3D ie = tex_image_extent(t, level);
        const int32_t x1 = std::min<int32_t>(static_cast<int32_t>(std::lround((r.x + r.w) * sc)), static_cast<int32_t>(ie.width));
        const int32_t y1 = std::min<int32_t>(static_cast<int32_t>(std::lround((r.y + r.h) * sc)), static_cast<int32_t>(ie.height));
        r.x = static_cast<int32_t>(std::lround(r.x * sc));
        r.y = static_cast<int32_t>(std::lround(r.y * sc));
        r.w = std::max(0, x1 - r.x);
        r.h = std::max(0, y1 - r.y);
    };
    scale_region(sr, s, ss.level);
    scale_region(dr, d, ds.level);
    if (sr.w <= 0 || sr.h <= 0 || dr.w <= 0 || dr.h <= 0) return;
    tex_ensure_current(s, *F);
    if (s != d) tex_ensure_current(d, *F);
    const bool same = s == d;
    tex_transition(s, *F, same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (!same) tex_transition(d, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    tev(d, "copy_texture_to_texture", reinterpret_cast<uint64_t>(s), 0);
    for (int32_t i = 0; i < std::max(1, d->b.target != NVN_TEXTURE_TARGET_3D ? dr.d : 1); ++i) tex_sparse_before_copy(d, ds.layer + static_cast<uint32_t>(i), ds.level);
    tex_mark_gpu_written(d);
    const VkImageLayout sl = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkImageLayout dl = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkImageAspectFlags sa = tex_aspect(s), da = tex_aspect(d);
    const bool same_size = sr.w == dr.w && sr.h == dr.h && sr.d == dr.d;
    const bool same_fmt = tex_vk_format(s) == tex_vk_format(d);
    if (same_size && same_fmt) {
        VkImageCopy c = {};
        c.srcSubresource = {sa, ss.level, ss.layer, 1};
        c.dstSubresource = {da, ds.level, ds.layer, 1};
        c.srcOffset = {sr.x, sr.y, sr.z};
        c.dstOffset = {dr.x, dr.y, dr.z};
        c.extent = {static_cast<uint32_t>(sr.w), static_cast<uint32_t>(sr.h), static_cast<uint32_t>(std::max(1, sr.d))};
        if (s->b.target != NVN_TEXTURE_TARGET_3D && sr.d > 1) { c.srcSubresource.layerCount = c.dstSubresource.layerCount = sr.d; c.extent.depth = 1; c.srcOffset.z = c.dstOffset.z = 0; }
        vkCmdCopyImage(F->cb, tex_image(s), sl, tex_image(d), dl, 1, &c);
    } else {
        VkImageBlit b = {};
        b.srcSubresource = {sa, ss.level, ss.layer, 1};
        b.dstSubresource = {da, ds.level, ds.layer, 1};
        b.srcOffsets[0] = {sr.x, sr.y, sr.z};
        b.srcOffsets[1] = {sr.x + sr.w, sr.y + sr.h, sr.z + std::max(1, sr.d)};
        b.dstOffsets[0] = {dr.x, dr.y, dr.z};
        b.dstOffsets[1] = {dr.x + dr.w, dr.y + dr.h, dr.z + std::max(1, dr.d)};
        const bool linear = (p.flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) || (tex_scale(s) != tex_scale(d) && !tex_is_depth(s) && !tex_is_depth(d));
        vkCmdBlitImage(F->cb, tex_image(s), sl, tex_image(d), dl, 1, &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    memory_barrier(F->cb);
    counters().copies.fetch_add(1, std::memory_order_relaxed);
    gstamp("copy texture");
}

void do_downsample(const P_Downsample& p) {
    Texture *s = p.src, *d = p.dst;
    if (!s || !d || !tex_gpu(s) || !tex_gpu(d)) return;
    end_pass();
    if (tex_is_depth(s)) { note_unsupported("Downsample of a depth texture"); return; }
    tex_transition(s, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    tex_transition(d, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    tex_mark_gpu_written(d);
    VkImageResolve rs = {};
    rs.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rs.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (tex_scale(s) != tex_scale(d)) log_once("downsample-scale", "Downsample between a scaled and an unscaled texture: only the common part is resolved");
    const VkExtent3D e = tex_image_extent(d, 0), es = tex_image_extent(s, 0);
    rs.extent = {std::min(e.width, es.width), std::min(e.height, es.height), 1};
    // a resolve needs one format on both sides: an HDR-promoted destination (RGBA16F for the source's RGBA8) resolves through a blit
    const char* why = nullptr;
    if (tex_vk_format(s) == tex_vk_format(d)) vkCmdResolveImage(F->cb, tex_image(s), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_image(d), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rs);
    else if (!tex_resolve_converted(s, d, *F, rs, &why)) {
        static int logged = 0;
        if (logged++ < 20) {
            char a[96], b[96];
            NXLOG_W(kTag, "downsample of %s into %s skipped: formats %d and %d differ and cannot be converted (%s)", tex_desc(s, a, sizeof a), tex_desc(d, b, sizeof b),
                    static_cast<int>(tex_vk_format(s)), static_cast<int>(tex_vk_format(d)), why);
        }
    }
    memory_barrier(F->cb);
    counters().copies.fetch_add(1, std::memory_order_relaxed);
    gstamp("downsample");
}

void do_write_memory(uint64_t gpu_addr, const void* data, uint32_t size) {
    PoolReasonScope pr_(kReasonUpdate);
    // a GPU-ordered write of up to 64 KiB into pool memory (UpdateUniformBuffer, counters, events)
    end_pass();
    memory_barrier(F->cb);
    BufRef b = resolve_buffer(gpu_addr, size);
    if (!b.ok) return;
    gpu_wrote(gpu_addr, size, kWriteUpdate);
    vkCmdUpdateBuffer(F->cb, b.buffer, b.offset, size, data);
    memory_barrier(F->cb);
    gstamp("write memory");
}

// ---- the interpreter ----------------------------------------------------------------------------------------------------------
static void apply_bind_program(const P_BindProgram& p) {
    for (int st = 0; st < 6; ++st)
        if (p.stages & (1u << st)) S.stage[st] = p.program ? p.program->stage[st] : nullptr;
    if (key_check_on() && p.program) {   // [nvn] shader_key_check: a by-value copy of what SetShaders hashed, re-hashed at the first draw
        KeyStage ks[6];
        for (int st = 0; st < 6; ++st) ks[st] = {p.program->src_code[st], p.program->src_control[st], p.program->code_hash[st]};
        key_check_bind(reinterpret_cast<uintptr_t>(p.program) ^ (static_cast<uintptr_t>(p.program->serial) << 48), ks);
    }
    if (g_trace && (p.stages & (1u << kCompute))) {
        g_cs_missing = (p.program && !p.program->stage[kCompute]) ? std::string(p.program->missing) : std::string();
        if (!g_cs_missing.empty()) TRACE("bind program: compute block(s) %s not in the shader cache", g_cs_missing.c_str());
    }
}

// [nvn] epoch_exact: SetRenderTargets that sets exactly the render target state already set (count, every colour and the depth texture,
// whether each has a view and that view field by field). The state it would write is then the state there is, so nothing the set memo
// depends on through the targets changes: is_color_feedback reads S.color_tex / S.ncolor, the depth sampled by a draw reads S.depth_tex,
// and the pass (P, fbl_sampled) is begun by ensure_pass, which bumps on its own.
static bool same_view(const TextureViewInfo& a, const TextureViewInfo& b) {
    return a.has_levels == b.has_levels && a.has_layers == b.has_layers && a.has_format == b.has_format && a.has_swizzle == b.has_swizzle &&
           a.has_depth_stencil_mode == b.has_depth_stencil_mode && a.has_target == b.has_target && a.min_level == b.min_level &&
           a.num_levels == b.num_levels && a.min_layer == b.min_layer && a.num_layers == b.num_layers && a.format == b.format &&
           a.swizzle[0] == b.swizzle[0] && a.swizzle[1] == b.swizzle[1] && a.swizzle[2] == b.swizzle[2] && a.swizzle[3] == b.swizzle[3] &&
           a.depth_stencil_mode == b.depth_stencil_mode && a.target == b.target;
}
static bool same_targets(const P_RenderTargets& p) {
    if (p.count != S.ncolor || p.count > 8) return false;
    for (uint32_t i = 0; i < p.count; ++i) {
        const TargetRef& t = p.color[i];
        if (t.tex != S.color_tex[i].get() || (t.has_view != 0) != S.color_has_view[i]) return false;
        if (t.has_view && !same_view(t.view, S.color_view[i])) return false;
    }
    if (p.depth.tex != S.depth_tex.get() || (p.depth.has_view != 0) != S.depth_has_view) return false;
    return !p.depth.has_view || same_view(p.depth.view, S.depth_view);
}

static void mark_dirty(const RecordReader& r) {
    switch (r.op()) {
        case Op::BindProgram: g_dirty |= kPipe | kDesc | kVb; break;
        case Op::BindBuffers: g_dirty |= r.payload<P_BindBuffers>().kind == kBufVertex ? kVb : kDesc; break;
        case Op::BindTextures: case Op::BindImages: case Op::SetTexturePool: case Op::SetSamplerPool: g_dirty |= kDesc; break;
        case Op::BindVertexAttribs: case Op::BindVertexStreams: g_dirty |= kPipe | kVb; break;
        case Op::BindBlend: case Op::BindColor: case Op::BindChannelMask: case Op::BindMultisample: case Op::SetDepthClamp: g_dirty |= kPipe; break;
        case Op::BindPolygon: g_dirty |= kPipe | kDynRaster | kDynBias; break;
        case Op::BindDepthStencil: case Op::SetDepthBounds: g_dirty |= kDynDepth | kDynStencil; break;
        case Op::SetViewports: case Op::SetScissors: case Op::SetDepthRanges: g_dirty |= kDynVp; break;
        case Op::SetStencil: g_dirty |= kDynStencil; break;
        case Op::SetBlendColor: g_dirty |= kDynBlend; break;
        case Op::SetLineWidth: case Op::SetRasterizerDiscard: case Op::SetPrimitiveRestart: g_dirty |= kDynRaster; break;
        case Op::SetPatchSize: g_dirty |= kPipe; break;   // patch control points are part of the pipeline
        case Op::SetPolygonOffsetClamp: g_dirty |= kDynBias; break;
        case Op::SetRenderTargets: {
            g_dirty = kAllDirty;
            static const bool exact = epoch_exact_on();
            if (exact && same_targets(r.payload<P_RenderTargets>())) ft::split_count(ft::kCntEpSkipTargets);
            else rp_bump(ft::kCntEpTargets);
            break;
        }
        default: break;
    }
}

// ---- windowed GPU counters (see g_cw) ----------------------------------------------------------------------------------------
bool cw_ready(CounterWindow& w, bool xfb) {
    if (w.pool) return true;
    if (w.tried) return false;
    w.tried = true;
    const gfx::DeviceFeatures& f = device().features();
    if (xfb ? !(f.transform_feedback && f.transform_feedback_queries) : !f.pipeline_statistics_query) return false;
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = xfb ? VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT : VK_QUERY_TYPE_PIPELINE_STATISTICS;
    qi.queryCount = kCounterSlots;
    if (!xfb) qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT;
    if (vkCreateQueryPool(vkdev(), &qi, nullptr, &w.pool) != VK_SUCCESS) w.pool = VK_NULL_HANDLE;
    return w.pool != VK_NULL_HANDLE;
}

void cw_end_query(CounterWindow& w, bool xfb) {
    if (w.active < 0) return;
    if (xfb) vkCmdEndQueryIndexedEXT(F->cb, w.pool, static_cast<uint32_t>(w.active), 0);
    else vkCmdEndQuery(F->cb, w.pool, static_cast<uint32_t>(w.active));
}

// Does the window that a ResetCounter of `type` opens (up to the next report or reset of that type in the same list) hold a draw whose
// primitives the CPU cannot count: an indirect draw or a DrawTransformFeedback? The record reader is copied; the list itself is not touched.
bool counter_window_has_gpu_draw(RecordReader look, uint32_t type) {
    while (look.next()) {
        const Op op = look.op();
        if ((op == Op::ReportCounter || op == Op::ResetCounter) && look.payload<P_Counter>().type == type) return false;
        if (op == Op::DrawIndirect || op == Op::DrawXfb) return true;
    }
    return false;
}

// Time of one op of the list, split into rp.opin (mark_dirty, out_before_op) and the op itself (rp.tail for draws, rp.op otherwise).
struct RpOpScope {
    bool on;
    Op op;
    uint64_t ta = 0, tb = 0, inner0 = 0;
    RpOpScope(bool rt, Op o) : on(rt), op(o) { if (on) { inner0 = g_rp_inner; ta = ft::tsc(); } }
    void mid() { if (on) tb = ft::tsc(); }
    ~RpOpScope() {
        if (!on) return;
        const uint64_t tc = ft::tsc();
        const uint64_t exec = tc - tb, inner = g_rp_inner - inner0;
        const int ph = (op == Op::DrawArrays || op == Op::DrawElements || op == Op::DrawIndirect || op == Op::DrawXfb) ? kRpTail : kRpOp;
        g_rp_ns[ph] += exec > inner ? exec - inner : 0;
        ++g_rp_n[ph];
        g_rp_ns[kRpOpIn] += tb - ta;
        ++g_rp_n[kRpOpIn];
        g_rp_inner = inner0 + (tc - ta);
    }
};

static void run_list(const CmdList& list, int depth) {
    if (depth > kMaxDepth) { log_once("depth", "CallCommands nesting too deep"); return; }
    counters().lists_replayed.fetch_add(1, std::memory_order_relaxed);
    const bool rt = ft::on() && g_rp_t0;
    for (RecordReader r(list); r.next();) {
        RpOpScope rpo(rt, r.op());
        mark_dirty(r);
        if (out_lazy()) out_before_op(r);                                                   // compute shadows: results back to the pool first
        else if (out_any() && !compute_out_may_wait(r.op())) flush_compute_out(kOutOther);
        rpo.mid();
        switch (r.op()) {
            case Op::Nop: break;
            case Op::BindProgram: apply_bind_program(r.payload<P_BindProgram>()); break;
            case Op::BindBuffers: {
                const auto& p = r.payload<P_BindBuffers>();
                const NVNbufferRange* rg = reinterpret_cast<const NVNbufferRange*>(r.blob<P_BindBuffers>());
                for (uint32_t i = 0; i < p.count; ++i) {
                    const uint32_t k = p.first + i;
                    if (p.kind == kBufVertex && k < 16) S.vtx[k] = {rg[i].address, rg[i].size};
                    else if (p.kind == kBufUniform && p.stage < 6 && k < 16) S.ubo[p.stage][k] = {rg[i].address, rg[i].size};
                    else if (p.kind == kBufStorage && p.stage < 6 && k < 16) S.ssbo[p.stage][k] = {rg[i].address, rg[i].size};
                    else if (p.kind == kBufXfb && k < 4) S.xfb[k] = {rg[i].address, rg[i].size};
                }
                break;
            }
            case Op::BindTextures: {
                const auto& p = r.payload<P_BindTextures>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_BindTextures>());
                for (uint32_t i = 0; i < p.count; ++i)
                    if (p.stage < 6 && p.first + i < 64) S.tex[p.stage][p.first + i] = h[i];
                break;
            }
            case Op::BindImages: {
                const auto& p = r.payload<P_BindTextures>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_BindTextures>());
                for (uint32_t i = 0; i < p.count; ++i)
                    if (p.stage < 6 && p.first + i < 16) S.img[p.stage][p.first + i] = h[i];
                break;
            }
            case Op::BindVertexAttribs: {
                const auto& p = r.payload<P_BindVertexAttribs>();
                const NVNvertexAttribState* a = reinterpret_cast<const NVNvertexAttribState*>(r.blob<P_BindVertexAttribs>());
                S.attr_count = std::min<uint32_t>(p.count, 16);
                for (uint32_t i = 0; i < S.attr_count; ++i) S.attr[i] = decode(&a[i]);
                break;
            }
            case Op::BindVertexStreams: {
                const auto& p = r.payload<P_BindVertexStreams>();
                const NVNvertexStreamState* a = reinterpret_cast<const NVNvertexStreamState*>(r.blob<P_BindVertexStreams>());
                S.stream_count = std::min<uint32_t>(p.count, 16);
                for (uint32_t i = 0; i < S.stream_count; ++i) S.stream[i] = decode(&a[i]);
                break;
            }
            case Op::BindBlend: {
                NVNblendState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st);
                BlendInfo b = decode(&st);
                if (b.target >= 0 && b.target < 8) S.blend[b.target] = b;
                break;
            }
            case Op::BindColor: { NVNcolorState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.color = decode(&st); break; }
            case Op::BindChannelMask: { NVNchannelMaskState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.chmask = decode(&st); break; }
            case Op::BindDepthStencil: { NVNdepthStencilState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.ds = decode(&st); break; }
            case Op::BindPolygon: { NVNpolygonState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.poly = decode(&st); break; }
            case Op::BindMultisample: { NVNmultisampleState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.ms = decode(&st); break; }
            case Op::SetTexturePool: S.tpool = Ref<TexturePool>::share(static_cast<TexturePool*>(r.payload<P_Pool>().pool)); break;
            case Op::SetSamplerPool: S.spool = Ref<SamplerPool>::share(static_cast<SamplerPool*>(r.payload<P_Pool>().pool)); break;
            case Op::SetViewports: {
                const auto& p = r.payload<P_Rects>();
                const float* v = reinterpret_cast<const float*>(r.blob<P_Rects>());
                for (uint32_t i = 0; i < p.count && p.first + i < 16; ++i) std::memcpy(S.vp[p.first + i], v + 4 * i, 16);
                S.nvp = std::max(S.nvp, std::min<uint32_t>(p.first + p.count, 16));
                break;
            }
            case Op::SetScissors: {
                const auto& p = r.payload<P_Rects>();
                const int32_t* v = reinterpret_cast<const int32_t*>(r.blob<P_Rects>());
                for (uint32_t i = 0; i < p.count && p.first + i < 16; ++i) std::memcpy(S.sc[p.first + i], v + 4 * i, 16);
                S.nsc = std::max(S.nsc, std::min<uint32_t>(p.first + p.count, 16));
                break;
            }
            case Op::SetDepthRanges: {
                const auto& p = r.payload<P_DepthRanges>();
                const float* v = reinterpret_cast<const float*>(r.blob<P_DepthRanges>());
                for (uint32_t i = 0; i < p.count && p.first + i < 16; ++i) std::memcpy(S.dr[p.first + i], v + 2 * i, 8);
                break;
            }
            case Op::SetDepthBounds: { const auto& p = r.payload<P_DepthBounds>(); S.depth_bounds = p.enable; S.db_near = p.near_; S.db_far = p.far_; break; }
            case Op::SetStencil: {
                const auto& p = r.payload<P_Stencil>();
                for (int f = 0; f < 2; ++f) {
                    const bool hit = p.face == NVN_FACE_FRONT_AND_BACK || (f == 0 && p.face == NVN_FACE_FRONT) || (f == 1 && p.face == NVN_FACE_BACK);
                    if (!hit) continue;
                    if (p.what == 0) S.stencil_ref[f] = p.value;
                    else if (p.what == 1) S.stencil_vmask[f] = p.value;
                    else S.stencil_wmask[f] = p.value;
                }
                break;
            }
            case Op::SetBlendColor: std::memcpy(S.blend_color, r.payload<P_BlendColor>().c, 16); break;
            case Op::SetPointSize: S.point_size = r.payload<P_Float>().v; break;
            case Op::SetLineWidth: S.line_width = r.payload<P_Float>().v; break;
            case Op::SetAlphaRef: S.alpha_ref = r.payload<P_Float>().v; break;
            case Op::SetPolygonOffsetClamp: { const auto& p = r.payload<P_PolygonOffsetClamp>(); S.bias_factor = p.factor; S.bias_units = p.units; S.bias_clamp = p.clamp; break; }
            case Op::SetSampleMask: S.sample_mask = r.payload<P_U32>().v; break;
            case Op::SetRasterizerDiscard: S.rasterizer_discard = r.payload<P_U32>().v != 0; break;
            case Op::SetDepthClamp: S.depth_clamp = r.payload<P_U32>().v != 0; break;
            case Op::SetPrimitiveRestart: { const auto& p = r.payload<P_PrimitiveRestart>(); S.prim_restart = p.enable != 0; S.restart_index = p.index; break; }
            case Op::SetPatchSize: S.patch_size = r.payload<P_U32>().v; g_dirty |= kPipe; break;   // the patch size is in the pipeline key
            case Op::SetTessLevels: {
                const auto& p = r.payload<P_TessLevels>();
                if (p.outer) std::memcpy(S.tess_outer, p.v, sizeof S.tess_outer);
                else std::memcpy(S.tess_inner, p.v, sizeof S.tess_inner);
                break;
            }
            case Op::SetRenderEnable:
                if (r.size() >= sizeof(RecHeader) + sizeof(P_RenderEnableCond)) {
                    const auto& p = r.payload<P_RenderEnableCond>();
                    S.render_enable = conditional_render_enabled(p.mode, p.address);
                } else {
                    S.render_enable = r.payload<P_U32>().v != 0;
                }
                break;
            case Op::SetCoverageModulationTable: case Op::SetConservativeRaster: break;
            case Op::SetRenderTargets: {
                const auto& p = r.payload<P_RenderTargets>();
                S.ncolor = p.count;
                for (uint32_t i = 0; i < 8; ++i) {
                    S.color_tex[i] = i < p.count ? Ref<Texture>::share(p.color[i].tex) : Ref<Texture>();
                    if (i < p.count) { S.color_view[i] = p.color[i].view; S.color_has_view[i] = p.color[i].has_view != 0; }
                    else S.color_has_view[i] = false;
                    const TextureViewInfo& v = S.color_view[i];
                    const bool hv = S.color_has_view[i];
                    S.color_level[i] = hv && v.has_levels ? static_cast<uint32_t>(v.min_level) : 0;
                    S.color_layer[i] = hv && v.has_layers ? static_cast<uint32_t>(v.min_layer) : 0;
                    S.color_layers[i] = hv && v.has_layers ? static_cast<uint32_t>(v.num_layers) : (S.color_tex[i] ? static_cast<uint32_t>(S.color_tex[i]->layers) : 0u);
                }
                S.depth_tex = Ref<Texture>::share(p.depth.tex);
                S.depth_view = p.depth.view;
                S.depth_has_view = p.depth.has_view != 0;
                break;
            }
            case Op::DrawArrays: do_draw_arrays(r.payload<P_DrawArrays>()); break;
            case Op::DrawElements: do_draw_elements(r.payload<P_DrawElements>()); break;
            case Op::DrawIndirect: do_draw_indirect(r.payload<P_DrawIndirect>()); break;
            case Op::DrawXfb: do_draw_xfb(r.payload<P_DrawXfb>()); break;
            case Op::PushDebugGroup:
                if (device().features().debug_labels && vkCmdBeginDebugUtilsLabelEXT) {
                    VkDebugUtilsLabelEXT l = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
                    l.pLabelName = reinterpret_cast<const char*>(r.blob<P_DebugGroup>());
                    vkCmdBeginDebugUtilsLabelEXT(F->cb, &l);
                    ++g_dbg_depth;
                }
                break;
            case Op::PopDebugGroup:
                if (g_dbg_depth > 0 && vkCmdEndDebugUtilsLabelEXT) { vkCmdEndDebugUtilsLabelEXT(F->cb); --g_dbg_depth; }   // a pop whose push was in an earlier submission has nothing to close
                break;
            case Op::DrawTexture: note_unsupported("DrawTexture"); break;
            case Op::ClearColor: do_clear_color(r.payload<P_ClearColor>()); break;
            case Op::ClearDepthStencil: do_clear_depth_stencil(r.payload<P_ClearDepthStencil>()); break;
            case Op::ClearTexture: do_clear_texture(r.payload<P_ClearTexture>()); break;
            case Op::ClearBuffer: {
                const auto& p = r.payload<P_ClearBuffer>();
                PoolReasonScope pr_(kReasonCopy);
                end_pass();
                BufRef b = resolve_buffer(p.address, p.size);
                if (b.ok) vkCmdFillBuffer(F->cb, b.buffer, b.offset, p.size & ~3ull, p.value);
                if (b.ok) gpu_wrote(p.address, p.size, kWriteFill);
                memory_barrier(F->cb);
                counters().clears.fetch_add(1);
                gstamp("fill buffer");
                break;
            }
            case Op::CopyBufferToBuffer: {
                const auto& p = r.payload<P_CopyBufferToBuffer>();
                note_copy_for_cpu(p.src, p.dst, p.size);   // [nvn] async_compute: what is copied into a CPU-visible pool, the CPU reads
                PoolReasonScope pr_(kReasonCopy);
                end_pass();
                BufRef s = held_or_pool(p.src, p.size, kOutBufferRead, true), d = resolve_buffer(p.dst, p.size);
                if (s.ok && d.ok) {
                    VkBufferCopy c = {s.offset, d.offset, p.size};
                    vkCmdCopyBuffer(F->cb, s.buffer, d.buffer, 1, &c);
                    gpu_wrote(p.dst, p.size, kWriteCopy);
                }
                memory_barrier(F->cb);
                counters().copies.fetch_add(1);
                gstamp("copy buffer");
                break;
            }
            case Op::CopyBufferToTexture: do_copy_buffer_texture(r.payload<P_CopyBufTex>(), true); break;
            case Op::CopyTextureToBuffer: do_copy_buffer_texture(r.payload<P_CopyBufTex>(), false); break;
            case Op::CopyTextureToTexture: do_copy_texture_texture(r.payload<P_CopyTexTex>()); break;
            case Op::Downsample: do_downsample(r.payload<P_Downsample>()); break;
            case Op::UpdateUniformBuffer: {
                const auto& p = r.payload<P_UpdateUniformBuffer>();
                const uint8_t* data = r.blob<P_UpdateUniformBuffer>();
                const uint32_t n = static_cast<uint32_t>(p.size & ~3ull);
                if (n) do_write_memory(p.address + static_cast<uint64_t>(p.offset), data, n);
                break;
            }
            case Op::Dispatch: { const auto& p = r.payload<P_Dispatch>(); do_dispatch(p.x, p.y, p.z, 0); break; }
            case Op::DispatchIndirect: do_dispatch(0, 0, 0, r.payload<P_DispatchIndirect>().address); break;
            case Op::ReportCounter: {
                const auto& p = r.payload<P_Counter>();
                struct { uint64_t counter, timestamp; } rep = {1, timestamp_ns()};   // counters other than the ones handled below are reported as non-zero
                if (p.type == NVN_COUNTER_TYPE_SAMPLES_PASSED && occ_ready()) {
                    end_pass();
                    if (g_occ_active >= 0) {
                        vkCmdEndQuery(F->cb, g_occ_pool, static_cast<uint32_t>(g_occ_active));
                        g_occ_last = g_occ_active;
                        g_occ_active = -1;
                    }
                    if (p.address) {
                        rep.counter = 0;
                        g_reports[p.address] = kReportUnknown;   // the query result lands on the GPU
                        do_write_memory(p.address, &rep, sizeof rep);
                        BufRef b = resolve_buffer(p.address, sizeof rep);
                        if (b.ok && g_occ_last >= 0) {
                            vkCmdCopyQueryPoolResults(F->cb, g_occ_pool, static_cast<uint32_t>(g_occ_last), 1, b.buffer, b.offset, 16,
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                            if (pool_copy_on()) gpu_wrote(p.address, 16, kWriteCopy);   // [nvn] pool_copy: the result lands in guest memory at completion
                            memory_barrier(F->cb);
                        }
                    }
                    break;
                }
                if (p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED || p.type == NVN_COUNTER_TYPE_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) {
                    CounterWindow& cw = g_cw[p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED ? 0 : 1];
                    if (cw.active >= 0) {   // the GPU counted this window (it holds indirect or feedback draws): the report is the query's result
                        end_pass();
                        cw_end_query(cw, &cw == &g_cw[1]);
                        cw.last = cw.active;
                        cw.active = -1;
                        if (p.address) {
                            rep.counter = 0;
                            g_reports[p.address] = kReportUnknown;   // the result lands on the GPU
                            do_write_memory(p.address, &rep, sizeof rep);
                            BufRef b = resolve_buffer(p.address, sizeof rep);
                            if (b.ok) {
                                // the feedback query has two results (written, needed): the second would land on the timestamp word, which is written again after it
                                vkCmdCopyQueryPoolResults(F->cb, cw.pool, static_cast<uint32_t>(cw.last), 1, b.buffer, b.offset, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                                if (pool_copy_on()) gpu_wrote(p.address, 16, kWriteCopy);   // [nvn] pool_copy: lands at completion
                                memory_barrier(F->cb);
                                if (&cw == &g_cw[1]) do_write_memory(p.address + 8, &rep.timestamp, 8);
                            }
                        }
                        break;
                    }
                    rep.counter = p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED ? g_prims_generated : g_prims_xfb;
                    note_report(p.address, rep.counter);
                    if (p.address) do_write_memory(p.address, &rep, sizeof rep);
                    break;
                }
                // TIMESTAMP_TOP is a timestamp too (taken when the command reaches the top of the pipe): its counter word is 0 like TIMESTAMP's
                const bool stamp = p.type == NVN_COUNTER_TYPE_TIMESTAMP || p.type == NVN_COUNTER_TYPE_TIMESTAMP_TOP;
                if (!stamp) note_unsupported("statistics counters other than samples passed, primitives generated and feedback primitives written are reported as non-zero");
                if (stamp) rep.counter = 0;
                note_report(p.address, rep.counter);
                if (p.address) do_write_memory(p.address, &rep, sizeof rep);
                break;
            }
            case Op::ReportValue: {
                const auto& p = r.payload<P_ReportValue>();
                struct { uint64_t counter, timestamp; } rep = {p.value, timestamp_ns()};
                note_report(p.address, rep.counter);
                if (p.address) do_write_memory(p.address, &rep, sizeof rep);
                break;
            }
            case Op::ResetCounter: {
                const auto& p = r.payload<P_Counter>();
                if (p.type == NVN_COUNTER_TYPE_SAMPLES_PASSED && occ_ready()) {
                    end_pass();
                    if (g_occ_active >= 0) vkCmdEndQuery(F->cb, g_occ_pool, static_cast<uint32_t>(g_occ_active));
                    const uint32_t slot = g_occ_next++ % kOccSlots;
                    vkCmdResetQueryPool(F->cb, g_occ_pool, slot, 1);
                    vkCmdBeginQuery(F->cb, g_occ_pool, slot, device().features().occlusion_query_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
                    g_occ_active = static_cast<int32_t>(slot);
                    g_occ_last = -1;
                } else if (p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED || p.type == NVN_COUNTER_TYPE_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) {
                    const bool gen = p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED;
                    (gen ? g_prims_generated : g_prims_xfb) = 0;
                    CounterWindow& cw = g_cw[gen ? 0 : 1];
                    if (cw.active >= 0) { end_pass(); cw_end_query(cw, !gen); cw.active = -1; }   // an unreported window is over
                    if (counter_window_has_gpu_draw(r, p.type) && cw_ready(cw, !gen)) {
                        static bool noted = false;
                        if (!noted) { noted = true; NXLOG_I("nvn.replay", "replay: primitive counter %u counted by a GPU query (first): its window holds an indirect or feedback draw", p.type); }
                        end_pass();
                        const uint32_t slot = cw.next++ % kCounterSlots;
                        vkCmdResetQueryPool(F->cb, cw.pool, slot, 1);
                        if (gen) vkCmdBeginQuery(F->cb, cw.pool, slot, 0);
                        else vkCmdBeginQueryIndexedEXT(F->cb, cw.pool, slot, 0, 0);
                        cw.active = static_cast<int32_t>(slot);
                    }
                }
                break;
            }
            case Op::Barrier: end_pass(); memory_barrier(F->cb); break;
            case Op::FenceSync: {
                const auto& p = r.payload<P_Sync>();
                if (p.sync) { p.sync->serial.store(F->serial); p.sync->armed.store(true); }
                break;
            }
            case Op::WaitSync: break;
            case Op::SignalEvent: {
                const auto& p = r.payload<P_Event>();
                if (p.event && p.event->pool && p.event->pool->base) {
                    Event* ev = p.event;
                    uint32_t v = p.value;
                    if (p.mode != NVN_EVENT_SIGNAL_MODE_WRITE) {
                        // ADD: the GPU cannot add into memory; the sum is computed here from the value after the signals recorded so far
                        uint32_t base = 0;
                        if (ev->shadow_valid.load()) base = ev->shadow.load();
                        else std::memcpy(&base, ev->pool->base + ev->offset, 4);
                        v = base + p.value;
                    }
                    ev->shadow.store(v);
                    ev->shadow_valid.store(true);
                    do_write_memory(ev->pool->gpu_base + static_cast<uint64_t>(ev->offset), &v, 4);
                }
                break;
            }
            case Op::WaitEvent: note_unsupported("GPU-side WaitEvent"); break;
            case Op::CallCommands: {
                const auto& p = r.payload<P_CallCommands>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_CallCommands>());
                for (uint32_t i = 0; i < p.count; ++i) {
                    Ref<CmdList> l = handle_lookup(h[i]);
                    if (l) run_list(*l, depth + 1);
                    else log_once("missing-handle", "CallCommands of an unknown handle skipped");
                }
                break;
            }
            case Op::DiscardColor: case Op::DiscardDepthStencil: break;
            case Op::ResolveDepth: note_unsupported("ResolveDepthBuffer"); break;
            case Op::BeginXfb: case Op::EndXfb: case Op::PauseXfb: case Op::ResumeXfb: {
                const auto& p = r.payload<P_Counter>();
                static std::unordered_map<std::string, bool> seen;
                const std::string key = std::string(op_name(r.op())) + (S.stage[kVertex] ? S.stage[kVertex]->hash : "-");
                if (seen.emplace(key, true).second)
                    NXLOG_I("nvn.xfb", "%s control %llx vs %s xfb buffer0 %llx/%llu", op_name(r.op()), static_cast<unsigned long long>(p.address),
                            S.stage[kVertex] ? S.stage[kVertex]->hash.c_str() : "-", static_cast<unsigned long long>(S.xfb[0].addr),
                            static_cast<unsigned long long>(S.xfb[0].size));
                TRACE("%s control %llx", op_name(r.op()), static_cast<unsigned long long>(p.address));
                if (!device().features().transform_feedback) {
                    note_unsupported("transform feedback (the device has no VK_EXT_transform_feedback)");
                    break;
                }
                if (r.op() == Op::BeginXfb) {
                    xfb_end_in_pass();    // a section still open in the pass ends where the new one starts
                    auto sit = g_xfb_slot_of.find(p.address);
                    if (sit == g_xfb_slot_of.end()) {
                        if (g_xfb_slot_next >= kXfbSlots) {   // all counters in use: start over (a DrawTransformFeedback of an older control buffer then draws nothing)
                            g_xfb_slot_of.clear();
                            g_xfb_slot_next = 0;
                        }
                        sit = g_xfb_slot_of.emplace(p.address, g_xfb_slot_next++).first;
                    }
                    g_xfb_cur = sit->second;
                    g_xfb_slot_written[g_xfb_cur] = false;
                    g_xfb_active = true;
                    g_xfb_have_counter = false;
                } else if (r.op() == Op::EndXfb) {
                    xfb_end_in_pass();
                    g_xfb_active = false;
                    g_xfb_have_counter = false;
                }
                break;
            }
            default: note_unsupported(op_name(r.op())); break;
        }
    }
}

}  // namespace replay

// [display] unpaced_loading (default on): the limiter stops waiting while the game loads, since its loading code advances once per
// presented frame. Loading = presented frames with fewer than 16 draws (boot's black screen draws 3: exposure, bloom, tone map; the
// loading screen a few more) AND the game still stalling: a present gap over 100 ms in the last 1.5 s. A light screen that presents
// steadily (the title screen, a menu) is paced again within 1.5 s, so it does not run at 1000 fps; so is any frame with 16+ draws.
// Boot only: until the first frame with 16+ draws, a boot loading screen presents steadily but its loading still advances once per frame
// (Pokopia: 14 s at 60 fps before the title), so a run of light frames stays unpaced for up to [display] unpaced_loading_s (20, 0 = off)
// seconds even without stalls. Once the game has drawn a real scene the stall rule alone applies, so a light screen in the world (a
// menu, a fade) is never unpaced just for being light, and a frame with 16+ draws is always paced.
// The detection always runs: its result (`want`) is also the loading gate of the compile paths (replay_loading, [nvn] gpl_gate = state).
// Only the limiter action depends on [display] unpaced_loading.
static bool g_loading = true;      // the detector's current `want`; loading until the first present (render thread)
bool replay_loading() { return g_loading; }

void note_frame_for_pacing() {
    static const bool on = nxrt::config_get_bool("display", "unpaced_loading", true);
    using clk = std::chrono::steady_clock;
    static uint64_t last_p = 0, last_d = 0;
    static int low = 0;
    static bool full = false;
    static clk::time_point last_present{}, last_stall{}, low_since{};
    const uint64_t p = counters().presents.load(std::memory_order_relaxed);
    if (p == last_p) return;
    const auto now = clk::now();
    const uint64_t d = counters().draws.load(std::memory_order_relaxed) + counters().draws_skipped.load(std::memory_order_relaxed);
    const uint64_t per = (d - last_d) / (p - last_p);
    last_p = p;
    last_d = d;
    if (last_present == clk::time_point{} || now - last_present > std::chrono::milliseconds(100)) last_stall = now;
    last_present = now;
    low = per < 16 ? low + 1 : 0;
    if (low == 1) low_since = now;
    static const double boot_s = std::max(0.0, nxrt::config_get_double("display", "unpaced_loading_s", 20.0));
    const bool boot_run = !full && now - low_since < std::chrono::duration<double>(boot_s);
    const bool want = low >= 3 && (now - last_stall < std::chrono::milliseconds(1500) || boot_run);
    if (want != g_loading) {
        g_loading = want;
        ft::mark(want ? "pace.loading" : "pace.playing", static_cast<uint32_t>(per));
        if (!on)
            NXLOG_I(kTag, "%s (present %llu, %llu draws; pacing unchanged: [display] unpaced_loading is false)", want ? "loading" : "playing",
                    static_cast<unsigned long long>(p), static_cast<unsigned long long>(per));
    }
    if (on && want != nxrt::frame_limiter_unpaced()) {
        nxrt::frame_limiter_set_unpaced(want);
        NXLOG_I(kTag, "pacing %s (present %llu, %llu draws)", want ? "off: loading" : "on", static_cast<unsigned long long>(p),
                static_cast<unsigned long long>(per));
    }
    if (!full && per >= 16) {
        full = true;
        NXLOG_I(kTag, "first frame with 16+ draws (present %llu, %llu draws)", static_cast<unsigned long long>(p), static_cast<unsigned long long>(per));
    }
}

void replay_begin_submit(Frame& f) {
    static const bool fine_init = (g_rp_fine = nxrt::config_get_bool("prof", "replay_fine", false), true);
    (void)fine_init;
    rp_flush();
    rp_bump(ft::kCntEpSubmit);    // the sets of the last submission are gone with its descriptor pools
    ft::split_count(ft::kCntSubmissions);
    g_rp_t0 = ft::on() ? ft::now_ns() : 0;
    RpScope rp_begin(kRpBegin);
    gpl_note_presents(counters().presents.load(std::memory_order_relaxed));   // [nvn] gpl_gate = time: frames flowing = play
    note_frame_for_pacing();
    gpl_latch_play(!replay_loading());   // [nvn] gpl_gate = state: play vs loading for the whole submission (a stall inside it cannot flip it)
    F = &f;
    { RpScope rp_(kRpTexPoll); ft::SplitScope sp_(ft::kSplSetup); tex_poll_cpu_writes(); }   // per call: see setup_coalesce_on (nvn_gpu_texture.cpp)
    if (extras::LowLatency* ll = extras::lowlat()) ll->submit_begin();     // RENDERSUBMIT_START of the frame (S14)
    if (g_trace_frame == -2) {
        // [nvn] trace_frame = "6050" or "6050,9300": every listed presented frame is traced (the first one is the main one)
        const std::string list = nxrt::config_get_str("nvn", "trace_frame", "");
        g_trace_frame = -1;
        size_t i = 0;
        while (i < list.size()) {
            size_t j = list.find(',', i);
            if (j == std::string::npos) j = list.size();
            const long long v = std::atoll(list.substr(i, j - i).c_str());
            if (g_trace_frame < 0) g_trace_frame = v; else g_trace_extra.push_back(v);
            i = j + 1;
        }
    }
    if (!g_trigger_read) {
        g_trigger_read = true;
        g_trigger_fs = nxrt::config_get_str("nvn", "trace_after_fs", "");
    }
    if (!g_trigger_fs.empty() && g_trigger_seen && !g_trigger_done && static_cast<int64_t>(counters().presents.load()) > g_trigger_present) {
        g_trigger_done = true;
        g_trace_frame = static_cast<int64_t>(counters().presents.load());
    }
    {   // [nvn] trace_sec = N (NXRT_NVN_TRACE_SEC): trace the first frame presented N seconds after the first draw (frame numbers differ between runs)
        static const double trace_sec = static_cast<double>(nxrt::config_get_int("nvn", "trace_sec", 0));
        static const auto t0 = std::chrono::steady_clock::now();
        static bool trace_sec_done = false;
        if (trace_sec > 0 && !trace_sec_done && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= trace_sec) {
            trace_sec_done = true;
            g_trace_frame = static_cast<int64_t>(counters().presents.load());
        }
    }
    if (g_trace_req.exchange(false)) g_trace_frame = static_cast<int64_t>(counters().presents.load()) + 1;   // armed from a script (@shot nvntrace_*)
    g_trace = g_trace_frame >= 0 && static_cast<int64_t>(counters().presents.load()) == g_trace_frame;
    for (int64_t e : g_trace_extra) g_trace |= static_cast<int64_t>(counters().presents.load()) == e;
    if (g_trace) { g_trace_n = 0; NXLOG_I("nvn.trace", "---- frame %lld ----", static_cast<long long>(counters().presents.load())); }
    {   // S38: device-local pool copies and tracked barriers. [nvn] gpuperf_ab = N: both are off in even windows of N presents and on in odd
        // ones (the paired-window measurement of S29); without it they follow their own settings.
        static const int64_t ab = config_get_int("nvn", "gpuperf_ab", 0);
        static const bool tracked = config_get_str("nvn", "pass_barriers", "tracked") != "full";
        static int last_on = -1;
        bool on = true;
        if (ab > 0) on = ((counters().presents.load() / static_cast<uint64_t>(ab)) & 1) != 0;
        if (static_cast<int>(on) != last_on) {
            last_on = on ? 1 : 0;
            if (ab > 0) ft::mark(on ? "gpuperf.on" : "gpuperf.off", static_cast<uint32_t>(counters().presents.load()));
        }
        g_tracked = on && tracked;
        set_narrow_barriers(g_tracked);
        g_pending.clear();
        g_pass_unsafe = false;
        RpScope rp_(kRpMirror);
        ft::SplitScope sp_(ft::kSplSetup);
        mirror_begin_submit(f, on);
    }
    S.window_origin = window_origin_mode();
    S.depth_mode = depth_mode();
    g_draw_idx = 0;
    P = Pass();
    g_set_cache.clear();
    g_bound_pipeline = VK_NULL_HANDLE;
    g_dirty = kAllDirty;
    g_pl = nullptr;
    g_pipe = VK_NULL_HANDLE;
    g_topo = g_pipe_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    g_restart_valid = false;
    g_vb_bound_in_cb = false;
    shadow_invalidate();
    g_want_depth_ro = false;
    g_fb_use.clear();
    g_fb_seen = false;
    g_key_dummy_binding = 0;
    // one support buffer per submission (Ryujinx SupportBuffer: render scales 1.0, no BGRA remap)
    g_support = f.upload(sizeof(SupportBuffer), 256);
    g_support_scaled.clear();
    g_driver_banks.clear();
    g_draw_support = UploadAlloc();
    if (g_support) {
        SupportBuffer sb = support_buffer_defaults();
        std::memcpy(g_support.ptr, &sb, sizeof sb);
    }
}

void replay_lists(Frame& f, const std::vector<Ref<CmdList>>& lists) {
    F = &f;
    for (const Ref<CmdList>& l : lists)
        if (l) run_list(*l, 0);
}
void fb_sweep(uint64_t serial) {
    g_fb_use.clear();       // raw pointers into the copies: none is in use between two submissions
    for (auto it = g_fb_clone.begin(); it != g_fb_clone.end();) {
        if (serial > it->second.last + kFbIdleSubmits) it = g_fb_clone.erase(it);
        else ++it;
    }
    g_fb_clone_n.store(static_cast<int64_t>(g_fb_clone.size()), std::memory_order_relaxed);
}

void replay_end_submit(Frame& f) {
    const uint64_t rp_te = ft::on() ? ft::tsc() : 0;
    F = &f;
    if (out_any()) {         // every compute result is in game memory, or held and guarded, before the submission's serial signals
        end_pass();
        RpScope rp_(kRpOutEnd);
        ft::SplitScope sp_(ft::kSplSetup);
        out_end_submit(F->cb, F->serial);
    }
    end_pass();
    if ((f.serial & 127) == 0 && !g_fb_clone.empty()) fb_sweep(f.serial);
    if ((f.serial & 127) == 0 && !g_vol3d.empty()) {   // 3D copies of 2D textures no submission used for kFbIdleSubmits are released
        g_vol3d_use.clear();
        for (auto it = g_vol3d.begin(); it != g_vol3d.end();) {
            if (f.serial > it->second.last + kFbIdleSubmits) it = g_vol3d.erase(it);
            else ++it;
        }
    }
    if (g_occ_active >= 0) {   // a query cannot outlive its command buffer: the count of an unreported counter is dropped
        vkCmdEndQuery(f.cb, g_occ_pool, static_cast<uint32_t>(g_occ_active));
        g_occ_active = -1;
    }
    for (int k = 0; k < 2; ++k) {
        cw_end_query(g_cw[k], k == 1);
        g_cw[k].active = -1;
    }
    for (; g_dbg_depth > 0; --g_dbg_depth) vkCmdEndDebugUtilsLabelEXT(f.cb);   // labels cannot outlive their command buffer either
    if (rp_te) { g_rp_ns[kRpEnd] += ft::tsc() - rp_te; ++g_rp_n[kRpEnd]; }
    rp_flush();
    F = nullptr;
}

void replay_shutdown() {
    if (g_xfb_counter) device().destroy_buffer(g_xfb_counter);
    g_xfb_counter = gfx::Buffer();
    g_xfb_began = g_xfb_active = g_xfb_have_counter = false;
    g_xfb_slot_of.clear();
    g_xfb_slot_next = g_xfb_cur = 0;
    std::memset(g_xfb_slot_written, 0, sizeof g_xfb_slot_written);
    for (CounterWindow& w : g_cw) {
        if (w.pool) vkDestroyQueryPool(vkdev(), w.pool, nullptr);
        w = CounterWindow();
    }
    g_dbg_depth = 0;
    g_gw_next = g_gw_count = 0;
    g_gw_lost_serial = 0;
    if (g_occ_pool) vkDestroyQueryPool(vkdev(), g_occ_pool, nullptr);
    g_occ_pool = VK_NULL_HANDLE;
    g_occ_tried = false;
    g_occ_active = g_occ_last = -1;
    g_prims_generated = g_prims_xfb = 0;
    g_mask.shutdown();
    g_mask_ready = false;
    if (g_qa.pipe) vkDestroyPipeline(vkdev(), g_qa.pipe, nullptr);
    if (g_qa.layout) vkDestroyPipelineLayout(vkdev(), g_qa.layout, nullptr);
    if (g_qa.set_layout) vkDestroyDescriptorSetLayout(vkdev(), g_qa.set_layout, nullptr);
    if (g_qa.index) device().destroy_buffer(g_qa.index);
    g_qa = QuadArgs();
    pipeline_shutdown();
    S = GState();
    P = Pass();
    g_pending.clear();
    out_reset();
    g_set_cache.clear();
    g_fb_use.clear();
    g_fb_clone.clear();
    g_vol3d_use.clear();
    g_vol3d.clear();
    g_fb_clone_n.store(0, std::memory_order_relaxed);
}

// Gauge (nxrt/gauge.h): reads the atomic mirror above, never the map. Removed before the globals are destroyed.
namespace {
struct ReplayGauges {
    ReplayGauges() {
        nxrt::gauge_register("nvn.replay.fb_clones", [] { return g_fb_clone_n.load(std::memory_order_relaxed); });
    }
    ~ReplayGauges() { nxrt::gauge_unregister("nvn.replay.fb_clones"); }
};
ReplayGauges g_replay_gauges;
}  // namespace

}  // namespace nxrt::nvn::be::gpu
