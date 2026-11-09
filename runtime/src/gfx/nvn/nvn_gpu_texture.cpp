// SPDX-License-Identifier: GPL-3.0-or-later
// Texture images of the NVN back end (render thread only). Each be::Texture gets one VkImage on first use (tex_gpu). The image is
// created without a view: every view (sampled, attachment, present alias) is made here and cached on the TexGpu. Formats: the NVN
// format when the GPU has it, its vk_fallback when the GPU refuses it, ASTC as BC7 when the GPU has no ASTC.
// Uploads: prepare_texture (nvn_texdata.cpp) gives the bytes, they go through a staging buffer into one buffer-to-image copy per
// level. Layouts are tracked per image (all levels and layers share one layout). Samplers are cached by their NVN state.
// Readback (download_texture) copies a render target back and tiles it into the game's storage.
#include "nvn_gpu_texture_internal.h"

namespace nxrt::nvn::be {
static uint64_t g_wgen_next = 0;                        // render thread only (move_layout, the replay)

// The Vulkan side of a texture: Texture::gpu points to it (created by gpu::tex_gpu, freed by gpu::tex_destroy).
static std::mutex g_live_mu;
static std::set<struct TexGpu*> g_live_set;

Texture::~Texture() {
    if (gpu) gpu::destroy_texture_gpu(gpu);
    gpu = nullptr;
}

namespace gpu {

namespace tex {
std::atomic<uint64_t> g_live_images{0};
static nxrt::GaugeCounter g_gauge_textures_live("nvn.textures_live");               // TexGpu objects alive (g_live_set)
nxrt::GaugeCounter g_gauge_texture_snapshots("nvn.texture_snapshots");       // debug snapshots kept (g_snaps; the list never shrinks)

// Logs a message only the first time its key is seen (render thread only).
bool first_time(const std::string& key) {
    static std::set<std::string> seen;
    return seen.insert(key).second;
}

// A promoted image answers the 8-bit RGBA formats the game asks for (its sRGB and UNORM views) with its own format: the values are linear floats.
VkFormat promoted_view_format(const TexGpu& g, VkFormat f) {
    // An image made in its format's fallback (D24S8 as D32_SFLOAT_S8_UINT on GPUs without D24S8, e.g. AMD) answers views of the
    // NVN format with its own: before, every shader sampling such a depth buffer got the dummy view.
    if (g.fmt && f == g.fmt->vk && g.format == g.fmt->vk_fallback && f != g.format) return g.format;
    if (g.promoted && (f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_R8G8B8A8_UNORM)) return g.format;
    // Views in another NVN format of an image made in its fallback (AMD has no 24-bit depth formats) take the image's own format too:
    // depth reads as the same 0..1 values. Before, those views got the dummy, and on the Steam Deck the fog and soft particles that read
    // the depth buffer covered the ground in white.
    if (f == VK_FORMAT_D24_UNORM_S8_UINT && g.format == VK_FORMAT_D32_SFLOAT_S8_UINT) return g.format;   // a view in another NVN format
    if (f == VK_FORMAT_X8_D24_UNORM_PACK32 && g.format == VK_FORMAT_D32_SFLOAT) return g.format;
    // An ASTC view (a texture view naming its ASTC format, sRGB or not) of an image stored transcoded (BC7, or the disk cache's RGBA8/BC5)
    // reads the image's own format, as its sRGB or UNORM partner like the view asks. Mario Kart World's character and course materials
    // name their format in the view; before, they sampled the black dummy.
    if (g.transcoded && f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
        const bool want_srgb = ((f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1) != 0;
        const VkFormat partner = srgb_partner(g.format);
        if (partner == VK_FORMAT_UNDEFINED) return g.format;
        const bool img_srgb = is_srgb(g.format);
        return want_srgb == img_srgb ? g.format : partner;
    }
    return f;
}

static VkImageAspectFlags aspect_bits(const FormatInfo* f) {
    switch (f->aspect) {
        case kAspectDepth: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case kAspectStencil: return VK_IMAGE_ASPECT_STENCIL_BIT;
        case kAspectDepthStencil: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

static Shape shape_of(const Texture& t) {
    const TextureBuilderInfo& b = t.b;
    Shape s;
    const bool is1d = b.target == NVN_TEXTURE_TARGET_1D || b.target == NVN_TEXTURE_TARGET_1D_ARRAY;
    const bool is3d = b.target == NVN_TEXTURE_TARGET_3D;
    s.type = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    s.base.width = static_cast<uint32_t>(std::max(1, b.width));
    s.base.height = is1d ? 1u : static_cast<uint32_t>(std::max(1, b.height));
    s.base.depth = is3d ? static_cast<uint32_t>(std::max(1, b.depth)) : 1u;
    s.levels = std::max<uint32_t>(1, t.levels);
    s.layers = std::max<uint32_t>(1, t.layers);
    s.cube = target_is_cube(b.target);
    if (target_is_multisample(b.target) || b.samples > 1) {
        s.samples = static_cast<uint32_t>(vk_sample_count(b.samples));
        s.levels = 1;
    }
    return s;
}

static gfx::ImageDesc make_desc(const Shape& s, VkFormat fmt, VkImageUsageFlags usage, VkImageCreateFlags flags) {
    gfx::ImageDesc d;
    d.type = s.type;
    d.format = fmt;
    d.width = s.base.width;
    d.height = s.base.height;
    d.depth = s.base.depth;
    d.mip_levels = s.levels;
    d.array_layers = s.layers;
    d.usage = usage;
    d.flags = flags;
    d.samples = static_cast<VkSampleCountFlagBits>(s.samples);
    d.create_view = false;
    d.name = "nvn texture";
    return d;
}

// ---- creation -------------------------------------------------------------------------------------------------------------
// Makes the image of t (or the failed marker when no format works). Stores the result in t->gpu, which is never null afterwards.
// A new write generation for one level (level >= 0) or for all of them (level < 0).
static void bump_gen(TexGpu& g, int level) {
    if (level >= 0) { g.wgen[std::min<uint32_t>(static_cast<uint32_t>(level), kGenLevels - 1)] = ++g_wgen_next; return; }
    for (uint64_t& w : g.wgen) w = ++g_wgen_next;
}

// Layouts nothing writes the image in: a move into any other one counts as a write of every level (or of `level` alone).
static bool read_only_layout(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
           l == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || l == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
}

TexGpu* create_texture_gpu(Texture* t, float want_scale) {
    auto* g = new TexGpu();
    { std::lock_guard<std::mutex> lk(g_live_mu); g_live_set.insert(g); }
    g_gauge_textures_live.add(1);
    t->gpu = g;
    g_bind_gen.fetch_add(1, std::memory_order_relaxed);
    const Shape s = shape_of(*t);
    g->fmt = t->fmt;
    g->type = s.type;
    g->base = s.base;
    g->levels = s.levels;
    g->layers = s.layers;
    g->cube = s.cube;
    auto fail = [&](const std::string& why) -> TexGpu* {
        NXLOG_E(kTag, "texture %u: no image (%s): the texture is not usable on the GPU", t->id, why.c_str());
        g->failed = true;
        g_bind_gen.fetch_add(1, std::memory_order_relaxed);
        counters().textures_failed.fetch_add(1);
        return g;
    };
    if (!g->fmt) return fail("no format");
    // A texture of a virtual (sparse) pool would be a fully backed image here: Mario Kart World's virtual texture caches are 8192x8192 arrays
    // of 68 layers (5.8 GB each as BC7) of which the console maps a few hundred MB of tiles. Until sparse residency images exist, such a
    // texture over [nvn] sparse_image_max_mb gets no image (it samples as missing) instead of exhausting the host's memory.
    if (t->vpool) {
        static const uint64_t max_mb = static_cast<uint64_t>(std::max<int64_t>(0, config_get_int("nvn", "sparse_image_max_mb", 1024)));
        if (max_mb && (t->storage_size >> 20) > max_mb && s.type == VK_IMAGE_TYPE_2D && !s.cube && s.samples == 1 &&
            device().features().sparse_binding && device().features().sparse_residency_image2d && config_get_bool("nvn", "sparse_residency", true)) {
            std::string why;
            if (create_sparse_texture(t, g, s, &why)) return g;
            return fail("sparse residency image: " + why);
        }
        if (max_mb && (t->storage_size >> 20) > max_mb)
            return fail("sparse texture of " + std::to_string(t->storage_size >> 20) + " MB (" + std::to_string(s.base.width) + "x" + std::to_string(s.base.height) +
                        ", " + std::to_string(s.layers) + " layers, " + std::to_string(s.levels) + " levels, NVN format " + std::to_string(t->b.format) +
                        ") is over [nvn] sparse_image_max_mb; needs sparse residency");
    }

    const gfx::DeviceFeatures& ft = device().features();
    const bool to_bc7 = g->fmt->astc && !ft.astc_ldr;
    if (to_bc7 && !ft.bc) return fail("ASTC is stored as BC7 and the GPU has no BC compression");
    const VkFormat bc7_format = g->fmt->srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
    // A disk cache entry that is not BC7 (RGBA8 for a texture BC7 cannot reproduce, BC5 for a two-channel normal map) makes the image in the
    // entry's format; prepare_texture is then told the image's format and delivers the entry's data (docs/S4f.md).
    VkFormat entry = VK_FORMAT_UNDEFINED;
    if (to_bc7 && !texprep::peek_entry_format(t, entry)) entry = cache_entry_format(*t);   // the background job already found the cache entry
    const VkFormat want = to_bc7 ? (entry != VK_FORMAT_UNDEFINED ? entry : bc7_format) : g->fmt->vk;
    const VkFormat alt = to_bc7 ? (entry != VK_FORMAT_UNDEFINED ? bc7_format : VK_FORMAT_UNDEFINED) : g->fmt->vk_fallback;

    std::string why = std::string("no Vulkan format for NVN format ") + g->fmt->name;
    VkFormat chosen = VK_FORMAT_UNDEFINED;
    // [nvn] test_no_d24 = 1 (testing): behave like a GPU without 24-bit depth formats (AMD): DEPTH24 and DEPTH24_STENCIL8 get their fallback.
    static const bool no_d24 = config_get_bool("nvn", "test_no_d24", false);
    for (VkFormat c : {want, alt}) {
        if (c == VK_FORMAT_UNDEFINED) continue;
        if (no_d24 && (c == VK_FORMAT_D24_UNORM_S8_UINT || c == VK_FORMAT_X8_D24_UNORM_PACK32)) {
            if (first_time("test_no_d24")) NXLOG_I(kTag, "texture %u: %s made in its fallback format ([nvn] test_no_d24)", t->id, g->fmt->name);
            continue;
        }
        std::string r;
        if (device().image_supported(make_desc(s, c, VK_IMAGE_USAGE_SAMPLED_BIT, base_flags(s, c)), &r)) {
            chosen = c;
            break;
        }
        why = r;
    }
    if (chosen == VK_FORMAT_UNDEFINED) return fail(why);

    if (const VkFormat hf = extras::hdr_promote_target(chosen, t->b.flags, s.type == VK_IMAGE_TYPE_2D, s.levels, s.layers, s.samples, g->fmt->srgb)) {
        if (device().image_supported(make_desc(s, hf, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, base_flags(s, hf)))) {
            NXLOG_I(kTag, "texture %u: %s %ux%u (flags 0x%x) kept as float16 for the HDR scene", t->id, g->fmt->name, s.base.width, s.base.height, static_cast<unsigned>(t->b.flags));
            chosen = hf;
            g->promoted = true;
        }
    }

    g->format = chosen;
    g->transcoded = to_bc7;
    if (to_bc7 && (chosen == VK_FORMAT_R8G8B8A8_UNORM || chosen == VK_FORMAT_R8G8B8A8_SRGB) && astc_quality() == AstcQuality::Uncompressed) note_uncompressed_image(*t);
    g->mutable_fmt = srgb_partner(chosen) != VK_FORMAT_UNDEFINED || bit_alias_class(chosen);
    g->aspect = aspect_bits(g->fmt);
    g->depth = (g->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;

    // Required usage is sampling. Each optional usage is kept only when the GPU accepts the image with it.
    VkImageCreateFlags flags = base_flags(s, chosen);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto add = [&](VkImageUsageFlags bit) {
        if (device().image_supported(make_desc(s, chosen, usage | bit, flags))) usage |= bit;
    };
    add(VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    add(VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!g->depth && !is_block_format(chosen)) add(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (g->depth) add(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    // The console reads and writes any colour texture through an image handle; the builder's IMAGE flag is a hint the game does not always
    // set (the froxel volume of the fog is written by a vertex shader without it). Every uncompressed colour image therefore keeps the
    // storage usage when the GPU accepts the combination.
    if (!g->depth && !is_block_format(chosen)) {
        add(VK_IMAGE_USAGE_STORAGE_BIT);
        // sRGB formats have no storage support. The console's image stores write the raw bits (no sRGB encode), which is what a UNORM view
        // of the same image does: storage usage is then allowed through EXTENDED_USAGE when the UNORM partner can be stored to.
        const VkFormat unorm = is_srgb(chosen) ? srgb_partner(chosen) : VK_FORMAT_UNDEFINED;
        if (!(usage & VK_IMAGE_USAGE_STORAGE_BIT) && unorm != VK_FORMAT_UNDEFINED && (flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) &&
            device().image_supported(make_desc(s, unorm, usage | VK_IMAGE_USAGE_STORAGE_BIT, flags))) {
            flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
            usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
        if ((t->b.flags & NVN_TEXTURE_FLAGS_IMAGE) && !(usage & VK_IMAGE_USAGE_STORAGE_BIT))
            NXLOG_W(kTag, "texture %u (%s, image flag set): the GPU refuses storage usage with the other usages (flags 0x%x, usage 0x%x)", t->id, t->fmt ? t->fmt->name : "?",
                    static_cast<unsigned>(flags), static_cast<unsigned>(usage));
    }
    g->usage = usage;
    g->cflags = flags;
    bump_gen(*g, -1);

    // Resolution scale: the image is the NVN size times the scale. Other modules keep the NVN size (tex_extent) and ask tex_scale /
    // tex_image_extent where the image size matters (render area, viewports, copies, the support buffer's render_scale).
    Shape is = s;
    if (want_scale != 1.0f && !g->transcoded && !is_block_format(chosen)) {
        const uint32_t lim = device().properties().limits.maxImageDimension2D;
        const uint32_t w = static_cast<uint32_t>(std::lround(s.base.width * want_scale)), h = static_cast<uint32_t>(std::lround(s.base.height * want_scale));
        if (w >= 1 && h >= 1 && w <= lim && h <= lim) {
            is.base.width = w;
            is.base.height = h;
            g->scale = want_scale;
            g->base = is.base;
            static int logged = 0;
            if (logged++ < 60)
                NXLOG_I(kTag, "texture %u: %s %ux%u (flags 0x%x, %u levels, %u layers, %u samples) rendered at %ux%u (scale %.3f)", t->id, g->fmt->name, s.base.width,
                        s.base.height, static_cast<unsigned>(t->b.flags), s.levels, s.layers, s.samples, w, h, want_scale);
        }
    }
    std::string why2;
    if (device().create_image(make_desc(is, chosen, usage, flags), &g->img, &why2) != VK_SUCCESS)
        return fail("create_image: " + why2);
    g_live_images.fetch_add(1);
    counters().textures_created.fetch_add(1);
    return g;
}

// The resolution scale: the game's play settings ([display] render_scale, turned into a factor by the game module and handed over in
// gfx::tuning) or, for tests, [gfx] render_scale (a number, NXRT_GFX_RENDER_SCALE), which wins. Fixed once known: images keep their size.
// Stays 1 (not fixed) until one of them is there.
static float g_render_scale = 0.0f;
static float render_scale_now() {
    if (g_render_scale > 0.0f) return g_render_scale;
    const std::string v = nxrt::config_get_str("gfx", "render_scale", "");
    float sc = 1.0f;
    if (!v.empty()) {
        sc = static_cast<float>(std::atof(v.c_str()));
    } else {
        sc = nxrt::gfx::tuning().render_scale.load();
        if (!(sc > 0.0f)) return 1.0f;
    }
    if (!(sc > 0.0f)) sc = 1.0f;
    sc = std::clamp(sc, 0.25f, 4.0f);
    sc = std::round(sc * 1080.0f) / 1080.0f;   // a whole number of lines at 1080 (1.333 -> 4/3: 1440, not 1439)
    if (std::fabs(sc - 1.0f) < 0.01f) sc = 1.0f;
    g_render_scale = sc;
    NXLOG_I(kTag, "render scale %.3f (%s)", sc, v.empty() ? "play settings" : "[gfx] render_scale");
    return sc;
}

// The scale a texture gets when its image is made for a render target (after Ryujinx's TextureScaling rules): 2D colour or depth targets
// of at least 32x32, not block-compressed, not pitch-linear, and not square powers of two (shadow
// maps, LUTs). Everything else, and every texture first used another way, keeps the NVN size.
static float attachment_scale(const Texture& t) {
    const float sc = render_scale_now();
    if (sc == 1.0f) return 1.0f;
    const TextureBuilderInfo& b = t.b;
    if (!t.fmt || t.fmt->compressed || t.fmt->block_w != 1 || t.fmt->block_h != 1) return 1.0f;
    if (b.target != NVN_TEXTURE_TARGET_2D && b.target != NVN_TEXTURE_TARGET_2D_ARRAY && b.target != NVN_TEXTURE_TARGET_2D_MULTISAMPLE &&
        b.target != NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY && b.target != NVN_TEXTURE_TARGET_RECTANGLE)
        return 1.0f;
    // not the IMAGE flag: the game sets it on every render target; storage use brings a scaled image back instead (tex_rescale)
    if (b.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_VIDEO_DECODE)) return 1.0f;
    const int w = b.width, h = b.height;
    if (w < 32 || h < 32) return 1.0f;
    if (w == h && (w & (w - 1)) == 0) return 1.0f;
    return sc;
}

// ---- barriers and copies ----------------------------------------------------------------------------------------------------
void move_layout(Frame& f, TexGpu& g, VkImageLayout to, int write_level) {
    if (g.layout == to) return;
    if (!read_only_layout(to)) bump_gen(g, write_level);
    // A 3D image created with 2D_ARRAY_COMPATIBLE: without maintenance9 the layer range of a barrier means all depth slices.
    const uint32_t layer_count = g.type == VK_IMAGE_TYPE_3D ? VK_REMAINING_ARRAY_LAYERS : g.layers;
    image_barrier(f.cb, g.img.image, g.layout, to, g.aspect, 0, g.levels, 0, layer_count);
    g.layout = to;
}

// Copies buffer regions (already offset to the staging buffer) into every level of the image, then leaves it readable by shaders.
void copy_regions_to_image(Frame& f, TexGpu& g, VkBuffer buf, const std::vector<VkBufferImageCopy>& regions) {
    move_layout(f, g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(f.cb, buf, g.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
    move_layout(f, g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
// ---- CPU writes without a flush --------------------------------------------------------------------------------------------
// A texture in a CPU_UNCACHED pool is coherent on the console: the game writes it and the GPU sees the bytes without FlushMappedRange.
// ACNH's text renderer rasterizes the glyphs it needs into such a texture (an R8 2048x4096 glyph cache) as new characters come up; an
// image uploaded once kept the old glyph set and every later new glyph drew as nothing. The storage pages of an uploaded coherent texture
// are therefore armed in one write-watch region over the heap (nxrt/write_watch.h), before the storage is read for the upload. At the start
// of every submission the pages the CPU wrote since are handed to textures_cpu_wrote, as a flush of them would be: the textures there are
// uploaded again before their next use (and armed again). A texture the GPU renders to is not armed. [nvn] tex_write_watch = auto (off under Wine) | true | false.
static mem::ww::Region* g_tex_ww = nullptr;
static bool g_tex_ww_tried = false;
static std::vector<uint64_t> g_tex_ww_pages;

static mem::ww::Region* tex_ww() {
    if (!g_tex_ww_tried) {
        g_tex_ww_tried = true;
        // auto (default): off under Wine (Proton, SteamOS). There the heap is private memory imported as amdgpu userptr BOs, and
        // making those pages read-only invalidates the BOs: the next submission loses the device (#169, Legion Go S and Steam Deck).
        const std::string v = nxrt::config_get_str("nvn", "tex_write_watch", "auto");
        const bool on = v == "auto" ? !nxrt::host_is_wine() : (v == "on" || v == "1" || v == "true");
        NXLOG_I(kTag, "tex_write_watch %s (%s%s)", on ? "on" : "off", v.c_str(),
                v == "auto" ? (nxrt::host_is_wine() ? ", Wine" : ", not Wine") : "");
        if (on && mem::heap_exists())
            g_tex_ww = mem::ww::region_create(mem::kHeapBase, mem::heap_reserved());
    }
    return g_tex_ww;
}

bool tex_ww_wanted(const Texture& t, const TexGpu& g) { return t.cpu_coherent && t.mem && t.storage_size && !g.gpu_written && tex_ww(); }

void tex_ww_arm(const Texture& t, const TexGpu& g) {
    if (!tex_ww_wanted(t, g)) return;
    mem::ww::Region* r = tex_ww();
    const uint64_t a = reinterpret_cast<uint64_t>(t.mem), base = mem::ww::region_base(r);
    if (a < base) return;
    const uint64_t first = (a - base) >> 12, last = (a + t.storage_size - 1 - base) >> 12;
    mem::ww::arm(r, first, last - first + 1);   // false: a page it cannot protect (another view, a file read in progress): not watched
}

// ---- arming off the render thread ([nvn] tex_ww_async, default on) ---------------------------------------------------------------
// arm() is a VirtualQuery walk and a VirtualProtect under the region's exclusive lock. While the game streams an area those calls took the
// render thread 60 to 490 ms a frame (the hitch line's "write-watch arming", inside "uploads"). Here the render thread hashes the storage it
// uploads from and hands the arm to one worker thread. The worker arms first and hashes the storage again: a write after the arm faults and
// is reported as before (tex_poll_cpu_writes); a write between the upload's read and the arm shows as a different hash and is reported the
// same way (textures_cpu_wrote), so the texture is uploaded again. Nothing the CPU writes is missed; the only cost is that write's re-upload.
struct ArmJob {
    Ref<Texture> t;
    uint64_t first = 0, count = 0, hash = 0, seq = 0;
};
struct ArmQueue {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<ArmJob> q;
    std::unordered_map<Texture*, uint64_t> latest;     // newest job of a texture (older ones are skipped)
    uint64_t seq = 0;
    bool started = false;
};
static ArmQueue* g_armq = nullptr;     // never destroyed: the worker runs until the process ends
static std::mutex g_arm_import_mu;     // tex_arm_import_mutex: the worker's arms against pool_lazy's imports
static std::atomic<uint64_t> g_arm_async_jobs{0}, g_arm_async_rewritten{0};
// the hitch log: arms the worker made, and uploads it found written before its arm (uploaded again), during a slow frame
static const bool g_arm_hitch_registered =
    (nxrt::hitch_add_counter("tex_arms_async", []() -> uint64_t { return g_arm_async_jobs.load(std::memory_order_relaxed); }),
     nxrt::hitch_add_counter("tex_rewritten_before_arm", []() -> uint64_t { return g_arm_async_rewritten.load(std::memory_order_relaxed); }), true);

bool tex_ww_async_on() {
    static const bool on = nxrt::config_get_bool("nvn", "tex_ww_async", true);
    return on;
}

static void arm_worker() {
    log_set_thread_name("nvn-ww-arm");
    ArmQueue& aq = *g_armq;
    std::unique_lock<std::mutex> lk(aq.mu);
    for (;;) {
        aq.cv.wait(lk, [&] { return !aq.q.empty(); });
        ArmJob j = std::move(aq.q.front());
        aq.q.pop_front();
        Texture* t = j.t.get();
        auto it = aq.latest.find(t);
        if (it == aq.latest.end() || it->second != j.seq) continue;   // a newer upload queued its own arm
        lk.unlock();
        if (!t->destroyed.load()) {
            {
                std::lock_guard<std::mutex> ilk(g_arm_import_mu);   // not between a lazy import's forget and the import (tex_arm_import_mutex)
                mem::ww::arm(g_tex_ww, j.first, j.count);   // false: a page it cannot protect: not watched (as the synchronous arm)
            }
            uint64_t h = 0;
            const bool same = texprep::hash_storage(*t, h) && h == j.hash;   // unreadable now: reported, as a write would be
            lk.lock();
            const bool newest = aq.latest.count(t) && aq.latest[t] == j.seq;
            if (newest) aq.latest.erase(t);
            lk.unlock();
            if (!same && newest) {      // written since the upload read it (or not readable): upload it again
                g_arm_async_rewritten.fetch_add(1, std::memory_order_relaxed);
                textures_cpu_wrote(t->mem, t->storage_size);
            }
        } else {
            lk.lock();
            if (aq.latest.count(t) && aq.latest[t] == j.seq) aq.latest.erase(t);
            lk.unlock();
        }
        g_arm_async_jobs.fetch_add(1, std::memory_order_relaxed);
        j.t = Ref<Texture>();     // the texture may be destroyed here
        lk.lock();
    }
}
}  // namespace tex

std::mutex& tex_arm_import_mutex() { return g_arm_import_mu; }

namespace tex {
// Render thread, after the storage was read for an upload of content `hash` (have_hash false: unknown). Arms the pages on the worker.
void tex_ww_arm_later(Texture* t, const TexGpu& g, bool have_hash, uint64_t hash) {
    if (!tex_ww_wanted(*t, g)) return;
    if (!have_hash) { tex_ww_arm(*t, g); return; }      // nothing to compare with after the arm (storage not readable): as before
    mem::ww::Region* r = tex_ww();
    const uint64_t a = reinterpret_cast<uint64_t>(t->mem), base = mem::ww::region_base(r);
    if (a < base) return;
    if (!g_armq) g_armq = new ArmQueue();
    ArmQueue& aq = *g_armq;
    std::lock_guard<std::mutex> lk(aq.mu);
    if (!aq.started) {
        aq.started = true;
        std::thread(arm_worker).detach();
    }
    ArmJob j;
    j.t = Ref<Texture>::share(t);
    j.first = (a - base) >> 12;
    j.count = ((a + t->storage_size - 1 - base) >> 12) - j.first + 1;
    j.hash = hash;
    j.seq = ++aq.seq;
    aq.latest[t] = j.seq;
    aq.q.push_back(std::move(j));
    aq.cv.notify_one();
}
}  // namespace tex

// ---- texture interface (gpu::*, internal.h) -----------------------------------------------------------------------------
TexGpu* tex_gpu_attachment(Texture* t) {
    if (!t || t->gpu || !active()) return tex_or_null(t);
    if (texture_registry_find_alias(t)) return tex_or_null(t);   // shares the image (and its scale) of the texture it aliases
    return tex_or_null(t, attachment_scale(*t));
}

TexGpu* tex_gpu_scaled(Texture* t, float scale) { return tex_or_null(t, scale); }

float render_scale() { return render_scale_now(); }

float tex_scale(Texture* t) {
    return (t && t->gpu && !t->gpu->failed) ? t->gpu->scale : 1.0f;
}

VkExtent3D tex_image_extent(Texture* t, uint32_t level) {
    TexGpu* g = tex_or_null(t);
    return g ? level_extent(g->base, level) : tex_extent(t, level);
}

float tex_scale_wanted(Texture* t) {
    TexGpu* g = tex_or_null(t);
    if (!g || g->scale_blocked || g->transcoded || is_block_format(g->format)) return g ? g->scale : 1.0f;
    return attachment_scale(*t);
}

// Remakes the image at another scale (Ryujinx scales a texture when it becomes a render target and blacklists one that cannot be
// scaled). The content is blitted over when the format allows it (otherwise it starts undefined). Going to 1 is for good. The old image
// and its views are destroyed after the frames that use them.
bool tex_rescale(Texture* t, Frame& f, float to, const char* why) {
    TexGpu* g = tex_or_null(t);
    if (!g || g->scale == to) return false;
    if (to != 1.0f && (g->scale_blocked || g->transcoded || is_block_format(g->format))) return false;
    Shape s = shape_of(*t);
    if (to != 1.0f) {
        s.base.width = static_cast<uint32_t>(std::lround(s.base.width * to));
        s.base.height = static_cast<uint32_t>(std::lround(s.base.height * to));
    }
    gfx::Image ni;
    std::string err;
    // the flags the image was made with: base_flags alone lacks EXTENDED_USAGE, and an sRGB image with storage usage was refused at every
    // rescale (once a frame, as the texture kept wanting its scale)
    if (device().create_image(make_desc(s, g->format, g->usage, g->cflags ? g->cflags : base_flags(s, g->format)), &ni, &err) != VK_SUCCESS) {
        NXLOG_W(kTag, "texture %u: cannot remake it at %ux%u (%s): it keeps scale %.3f", t->id, s.base.width, s.base.height, err.c_str(), g->scale);
        if (to == 1.0f) g->scale_blocked = true;
        return false;
    }
    static int logged = 0;
    if (logged++ < 60)
        NXLOG_I(kTag, "texture %u: %s %dx%d now rendered at %ux%u (%s)", t->id, g->fmt ? g->fmt->name : "?", t->b.width, t->b.height, s.base.width, s.base.height, why);
    VkFormatProperties fp = {};
    vkGetPhysicalDeviceFormatProperties(device().physical(), g->format, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const bool can_blit = g->layout != VK_IMAGE_LAYOUT_UNDEFINED && s.samples <= 1 && (fp.optimalTilingFeatures & need) == need &&
                          (g->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (g->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkImageLayout nl = VK_IMAGE_LAYOUT_UNDEFINED;
    if (can_blit) {
        move_layout(f, *g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        image_barrier(f.cb, ni.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, g->aspect, 0, s.levels, 0, s.layers);
        std::vector<VkImageBlit> regions;
        for (uint32_t l = 0; l < s.levels; ++l) {
            const VkExtent3D a = level_extent(g->base, l), b = level_extent(s.base, l);
            VkImageBlit r = {};
            r.srcSubresource = {g->aspect, l, 0, s.layers};
            r.dstSubresource = {g->aspect, l, 0, s.layers};
            r.srcOffsets[1] = {static_cast<int32_t>(a.width), static_cast<int32_t>(a.height), 1};
            r.dstOffsets[1] = {static_cast<int32_t>(b.width), static_cast<int32_t>(b.height), 1};
            regions.push_back(r);
        }
        vkCmdBlitImage(f.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ni.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()),
                       regions.data(), g->depth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        nl = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    gfx::Image old = g->img;
    std::vector<VkImageView> views;
    for (auto& kv : g->views) views.push_back(kv.second);
    g->views.clear();
    g_bind_gen.fetch_add(1, std::memory_order_relaxed);   // the views of the old image go
    defer_destroy([old, views]() mutable {
        for (VkImageView v : views) { vkDestroyImageView(vkdev(), v, nullptr); vk_gauge_image_views().sub(1); }
        device().destroy_image(old);
    });
    g->img = ni;
    g->base = s.base;
    g->layout = nl;
    bump_gen(*g, -1);
    g->scale = to;
    if (to == 1.0f) g->scale_blocked = true;
    return true;
}

// Why the polls run at the start of every replay call (contract): NVN pool memory is shared with the GPU on the console; a CPU write the
// game made before nvnQueueSubmit (and, for a cached pool, nvnMemoryPoolFlushMappedRange) is seen by the commands of that submission. Here
// the GPU reads the game's memory through device-local copies (pool mirror) and uploaded images (textures of coherent pools), so writes since
// the last replay call must invalidate them before this call's commands are recorded. The game writes pool memory between the submissions
// of one frame (uniform/vertex rings, CPU-written textures), so one poll per frame would bind stale copies to later submissions. The
// poll itself stays per call; setup_coalesce only makes it cheaper: an empty poll reads the summary (~220 words for an 8 GiB region)
// instead of every word of the bitmap.
bool setup_coalesce_on() {
    static const bool on = [] {
        const bool v = nxrt::config_get_bool("nvn", "setup_coalesce", true);
        mem::ww::set_summary_scan(v);
        NXLOG_I(kTag, "setup_coalesce %s ([nvn] setup_coalesce): write-watch polls read %s; the mirror walks %s", v ? "on" : "off",
                v ? "the summary bitmap" : "every bitmap word", v ? "the wanted page range" : "every page");
        return v;
    }();
    return on;
}

void tex_poll_cpu_writes() {
    (void)setup_coalesce_on();
    if (!g_tex_ww) return;
    g_tex_ww_pages.clear();
    const uint64_t w0 = ft::g_split_on ? mem::ww::take_stats().words : 0;
    mem::ww::take_written(g_tex_ww, g_tex_ww_pages);
    if (ft::g_split_on) {
        ft::split_count(ft::kCntTexPoll);
        if (g_tex_ww_pages.empty()) ft::split_count(ft::kCntTexPollEmpty);
        ft::split_add(ft::kCntTexPages, g_tex_ww_pages.size());
        ft::split_add(ft::kCntWwWords, mem::ww::take_stats().words - w0);
    }
    if (g_tex_ww_pages.empty()) return;
    std::sort(g_tex_ww_pages.begin(), g_tex_ww_pages.end());
    const uint64_t base = mem::ww::region_base(g_tex_ww);
    for (size_t i = 0; i < g_tex_ww_pages.size();) {
        size_t j = i + 1;
        while (j < g_tex_ww_pages.size() && g_tex_ww_pages[j] <= g_tex_ww_pages[j - 1] + 1) ++j;   // a run of written pages: one call
        textures_cpu_wrote(reinterpret_cast<const void*>(base + (g_tex_ww_pages[i] << 12)), (g_tex_ww_pages[j - 1] - g_tex_ww_pages[i] + 1) << 12);
        i = j;
    }
}

void tex_transition(Texture* t, Frame& f, VkImageLayout layout) {
    TexGpu* g = tex_or_null(t);
    if (g) move_layout(f, *g, layout);
}

VkImageLayout tex_layout(Texture* t) {
    return (t && t->gpu && !t->gpu->failed) ? t->gpu->layout : VK_IMAGE_LAYOUT_UNDEFINED;
}

void tex_sparse_before_copy(Texture* t, uint32_t layer, uint32_t level) {
    TexGpu* g = tex_or_null(t);
    if (g && g->sparse) sparse_bind_for_copy(t, *g, layer, level);
}

void tex_transition_attach(Texture* t, Frame& f, VkImageLayout layout, uint32_t level) {
    TexGpu* g = tex_or_null(t);
    if (g) move_layout(f, *g, layout, static_cast<int>(level));
}

bool tex_current_on_gpu(Texture* t) {
    const TexGpu* g = (t && t->gpu && !t->gpu->failed) ? t->gpu : nullptr;
    return g && !t->cpu_dirty.load() && !g->streamed && (g->gpu_written || g->uploaded);
}

bool tex_has_feedback_loop(Texture* t) {
    const TexGpu* g = (t && t->gpu && !t->gpu->failed) ? t->gpu : nullptr;
    return g && (g->usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT) != 0;
}

// Only images that show a feedback draw get the usage: on some GPUs it costs the colour compression of the image (RADV turns DCC off for
// feedback loop images on older chips), so it is not given to every render target at creation. Same shape, format, scale and flags; every
// level and layer is copied over.
bool tex_enable_feedback_loop(Texture* t, Frame& f) {
    TexGpu* g = tex_or_null(t);
    if (!g) return false;
    if (g->usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT) return true;
    if (g->fb_loop_tried) return false;
    g->fb_loop_tried = true;
    Shape s = shape_of(*t);
    s.base = g->base;
    if (!device().features().attachment_feedback_loop || g->sparse || g->depth || s.samples > 1 || g->type != VK_IMAGE_TYPE_2D ||
        !(g->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || !(g->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return false;
    const VkImageUsageFlags nu = g->usage | VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    gfx::Image ni;
    std::string err;
    if (!device().image_supported(make_desc(s, g->format, nu, g->cflags), &err) ||
        device().create_image(make_desc(s, g->format, nu, g->cflags), &ni, &err) != VK_SUCCESS) {
        NXLOG_W(kTag, "texture %u (%s %ux%u): no feedback loop usage (%s): its feedback draws keep the copy", t->id, g->fmt ? g->fmt->name : "?", g->base.width,
                g->base.height, err.c_str());
        return false;
    }
    static int logged = 0;
    if (logged++ < 40)
        NXLOG_I(kTag, "texture %u: %s %ux%u (%u levels) remade with feedback loop usage (a draw samples it while rendering to it)", t->id, g->fmt ? g->fmt->name : "?",
                g->base.width, g->base.height, g->levels);
    VkImageLayout nl = VK_IMAGE_LAYOUT_UNDEFINED;
    if (g->layout != VK_IMAGE_LAYOUT_UNDEFINED) {
        move_layout(f, *g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        image_barrier(f.cb, ni.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, g->aspect, 0, g->levels, 0, g->layers);
        std::vector<VkImageCopy> regions;
        for (uint32_t l = 0; l < g->levels; ++l) {
            VkImageCopy r = {};
            r.srcSubresource = {g->aspect, l, 0, g->layers};
            r.dstSubresource = {g->aspect, l, 0, g->layers};
            r.extent = level_extent(g->base, l);
            regions.push_back(r);
        }
        vkCmdCopyImage(f.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ni.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()),
                       regions.data());
        nl = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    gfx::Image old = g->img;
    std::vector<VkImageView> views;
    for (auto& kv : g->views) views.push_back(kv.second);
    g->views.clear();
    g_bind_gen.fetch_add(1, std::memory_order_relaxed);   // the views of the old image go (set_reuse memos must not outlive them)
    defer_destroy([old, views]() mutable {
        for (VkImageView v : views) { vkDestroyImageView(vkdev(), v, nullptr); vk_gauge_image_views().sub(1); }
        device().destroy_image(old);
    });
    g->img = ni;
    g->layout = nl;
    g->usage = nu;
    bump_gen(*g, -1);
    return true;
}

uint64_t tex_write_gen(Texture* t, uint32_t level) {
    TexGpu* g = (t && t->gpu && !t->gpu->failed) ? t->gpu : nullptr;
    return g ? g->wgen[std::min(level, kGenLevels - 1)] : 0;
}

void tex_mark_gpu_written(Texture* t, int level) {
    TexGpu* g = tex_or_null(t);
    if (!g) return;
    bump_gen(*g, level);
    g->gpu_written = true;
    t->cpu_dirty.store(false);                   // the GPU data is the newer one until the CPU writes again
}

VkImage tex_image(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g ? g->img.image : VK_NULL_HANDLE;
}

VkFormat tex_vk_format(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g ? g->format : VK_FORMAT_UNDEFINED;
}

bool tex_promoted(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g && g->promoted;
}

// ---- converting transfers: an image whose format is not the NVN one (an HDR-promoted target, a wider fallback) holds the same texels in
// another layout. A buffer copy addresses the buffer in the image's texel size, so a copy straight into or out of it reads or writes past
// the game's bytes (issue #76: RGBA16F for RGBA8 is twice the bytes, VK_ERROR_DEVICE_LOST on NVIDIA). These go through a transient image
// in the other format and a blit, which converts on the GPU. The transient images are made per transfer (rare) and freed after the frame.

namespace tex {
static bool format_has(VkFormat f, VkFormatFeatureFlags need) {
    VkFormatProperties fp = {};
    vkGetPhysicalDeviceFormatProperties(device().physical(), f, &fp);
    return (fp.optimalTilingFeatures & need) == need;
}

// A blit can convert between a and b: colour, not block compressed, not integer (a blit does not convert integers to or from floats).
static bool blit_convertible(VkFormat a, VkFormat b) {
    for (VkFormat f : {a, b})
        if (f == VK_FORMAT_UNDEFINED || is_block_format(f) || vk_format_is_integer(f)) return false;
    return true;
}

static bool make_transient(VkImageType type, VkFormat fmt, VkExtent3D e, uint32_t layers, gfx::Image* out, std::string* why) {
    gfx::ImageDesc d;
    d.type = type;
    d.format = fmt;
    d.width = e.width;
    d.height = e.height;
    d.depth = type == VK_IMAGE_TYPE_3D ? e.depth : 1;
    d.array_layers = type == VK_IMAGE_TYPE_3D ? 1 : layers;
    d.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    d.create_view = false;
    d.name = "nvn converting transfer";
    return device().create_image(d, out, why) == VK_SUCCESS;
}

static void free_transient(gfx::Image img) {
    defer_destroy([img]() mutable { device().destroy_image(img); });
}
}  // namespace tex

bool tex_copy_buffer_converted(Texture* t, Frame& f, VkBuffer buffer, const VkBufferImageCopy& c, bool to_texture, const char** why) {
    TexGpu* g = tex_or_null(t);
    *why = nullptr;
    if (!g || !g->fmt) { *why = "no image"; return false; }
    const VkFormat nf = g->fmt->vk;
    if (g->depth || c.imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) { *why = "depth/stencil"; return false; }
    if (!blit_convertible(g->format, nf)) { *why = "no blit between the formats"; return false; }
    if (shape_of(*t).samples > 1) { *why = "multisampled"; return false; }
    if (!(g->usage & (to_texture ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT))) { *why = "image lacks transfer usage"; return false; }
    // the texture blits to the transient (to a buffer) or from it (from a buffer); the transient is copied to or from the buffer
    const VkFormatFeatureFlags img_need = to_texture ? VK_FORMAT_FEATURE_BLIT_DST_BIT : VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    const VkFormatFeatureFlags tmp_need = to_texture ? (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
                                                     : (VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
    if (!format_has(g->format, img_need) || !format_has(nf, tmp_need)) { *why = "format lacks blit support"; return false; }
    const uint32_t layers = c.imageSubresource.layerCount;
    if (!c.imageExtent.width || !c.imageExtent.height || !c.imageExtent.depth || !layers) { *why = "empty region"; return false; }

    // the region in the image: the NVN texels times the image's scale, clipped to the level
    const VkExtent3D le = level_extent(g->base, c.imageSubresource.mipLevel);
    const float sc = g->scale;
    const auto scaled = [sc](int32_t v, uint32_t lim) { return std::clamp<int32_t>(static_cast<int32_t>(std::lround(v * sc)), 0, static_cast<int32_t>(lim)); };
    const int32_t w = static_cast<int32_t>(c.imageExtent.width), h = static_cast<int32_t>(c.imageExtent.height), dp = static_cast<int32_t>(c.imageExtent.depth);
    VkOffset3D i0 = {scaled(c.imageOffset.x, le.width), scaled(c.imageOffset.y, le.height), c.imageOffset.z};
    VkOffset3D i1 = {scaled(c.imageOffset.x + w, le.width), scaled(c.imageOffset.y + h, le.height), c.imageOffset.z + dp};
    VkOffset3D t1 = {w, h, dp};
    if (g->type != VK_IMAGE_TYPE_3D) { i0.z = 0; i1.z = 1; t1.z = 1; }
    if (i1.x <= i0.x || i1.y <= i0.y) { *why = "region outside the image"; return false; }

    gfx::Image tmp;
    std::string err;
    if (!make_transient(g->type, nf, c.imageExtent, layers, &tmp, &err)) {
        static int logged = 0;
        if (logged++ < 5) NXLOG_W(kTag, "texture %u: no transient %ux%u image for a converting copy: %s", t->id, c.imageExtent.width, c.imageExtent.height, err.c_str());
        *why = "transient image refused";
        return false;
    }
    const VkImageSubresourceLayers ts = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    VkBufferImageCopy tc = c;
    tc.imageSubresource = ts;
    tc.imageOffset = {0, 0, 0};
    // same size: nearest (an exact conversion); a scaled image: linear when the blit's source format filters
    const bool same_size = i1.x - i0.x == w && i1.y - i0.y == h;
    const VkFilter filter = !same_size && format_has(to_texture ? nf : g->format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkImageBlit b = {};
    image_barrier(f.cb, tmp.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
    if (to_texture) {
        vkCmdCopyBufferToImage(f.cb, buffer, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tc);
        image_barrier(f.cb, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
        b.srcSubresource = ts;
        b.srcOffsets[1] = t1;
        b.dstSubresource = c.imageSubresource;
        b.dstOffsets[0] = i0;
        b.dstOffsets[1] = i1;
        vkCmdBlitImage(f.cb, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, filter);
    } else {
        b.srcSubresource = c.imageSubresource;
        b.srcOffsets[0] = i0;
        b.srcOffsets[1] = i1;
        b.dstSubresource = ts;
        b.dstOffsets[1] = t1;
        vkCmdBlitImage(f.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, filter);
        image_barrier(f.cb, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
        vkCmdCopyImageToBuffer(f.cb, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &tc);
    }
    free_transient(tmp);
    static int logged = 0;
    if (logged++ < 20)
        NXLOG_I(kTag, "texture %u: copy %s %s %ux%u (level %u, %u layer(s)) converted through a %s blit (image format %d)", t->id, to_texture ? "buffer ->" : "-> buffer",
                g->fmt->name, c.imageExtent.width, c.imageExtent.height, c.imageSubresource.mipLevel, layers, g->promoted ? "HDR float16" : "fallback",
                static_cast<int>(g->format));
    return true;
}

bool tex_resolve_converted(Texture* s, Texture* d, Frame& f, const VkImageResolve& r, const char** why) {
    TexGpu *gs = tex_or_null(s), *gd = tex_or_null(d);
    *why = nullptr;
    if (!gs || !gd) { *why = "no image"; return false; }
    if (gs->depth || gd->depth) { *why = "depth/stencil"; return false; }
    if (!blit_convertible(gs->format, gd->format)) { *why = "no blit between the formats"; return false; }
    if (shape_of(*d).samples > 1) { *why = "multisampled destination"; return false; }
    if (!(gd->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { *why = "destination lacks transfer usage"; return false; }
    // s resolves into a transient of its own format (a resolve destination needs the colour attachment feature), blitted into d
    if (!format_has(gs->format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !format_has(gd->format, VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        *why = "format lacks blit support";
        return false;
    }
    if (!r.extent.width || !r.extent.height) { *why = "empty region"; return false; }
    gfx::Image tmp;
    std::string err;
    if (!make_transient(VK_IMAGE_TYPE_2D, gs->format, r.extent, 1, &tmp, &err)) {
        static int logged = 0;
        if (logged++ < 5) NXLOG_W(kTag, "texture %u: no transient %ux%u image for a converting resolve: %s", d->id, r.extent.width, r.extent.height, err.c_str());
        *why = "transient image refused";
        return false;
    }
    const VkImageSubresourceLayers ts = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkImageResolve tr = r;
    tr.dstSubresource = ts;
    tr.dstOffset = {0, 0, 0};
    image_barrier(f.cb, tmp.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    vkCmdResolveImage(f.cb, gs->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tr);
    image_barrier(f.cb, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    VkImageBlit b = {};
    b.srcSubresource = ts;
    b.srcOffsets[1] = {static_cast<int32_t>(r.extent.width), static_cast<int32_t>(r.extent.height), 1};
    b.dstSubresource = r.dstSubresource;
    b.dstOffsets[0] = r.dstOffset;
    b.dstOffsets[1] = {r.dstOffset.x + static_cast<int32_t>(r.extent.width), r.dstOffset.y + static_cast<int32_t>(r.extent.height), r.dstOffset.z + 1};
    vkCmdBlitImage(f.cb, tmp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gd->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    free_transient(tmp);
    static int logged = 0;
    if (logged++ < 20)
        NXLOG_I(kTag, "texture %u: downsample of texture %u (%ux%u) converted through a %s blit (format %d -> %d)", d->id, s->id, r.extent.width, r.extent.height,
                gd->promoted ? "HDR float16" : "fallback", static_cast<int>(gs->format), static_cast<int>(gd->format));
    return true;
}

VkImageAspectFlags tex_aspect(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g ? g->aspect : VK_IMAGE_ASPECT_COLOR_BIT;
}

VkExtent3D tex_extent(Texture* t, uint32_t level) { return level_extent(shape_of(*t).base, level); }

uint32_t tex_levels(Texture* t) { return t->levels; }

uint32_t tex_layers(Texture* t) { return t->layers; }

bool tex_is_depth(Texture* t) { return t->fmt && t->fmt->aspect != kAspectColor; }

void tex_destroy(TexGpu* g) {
    if (!g) return;
    if (--g->refs > 0) return;   // another texture object over the same storage still uses the image
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        if (!g_live_set.erase(g)) return;   // already destroyed by tex_shutdown
    }
    g_gauge_textures_live.sub(1);
    for (auto& kv : g->views) { vkDestroyImageView(vkdev(), kv.second, nullptr); vk_gauge_image_views().sub(1); }
    g->views.clear();
    if (g->img) g_live_images.fetch_sub(1);
    device().destroy_image(g->img);
    if (g->rb) device().destroy_buffer(g->rb);   // download_texture's buffer: tex_destroy runs once the GPU is done with the image
    if (g->sparse) {
        for (VkDeviceMemory m : g->sparse->chunks) vkFreeMemory(vkdev(), m, nullptr);
        delete g->sparse;
    }
    delete g;
}

TexStats tex_stats() {
    TexStats s;
    s.images = g_live_images.load();
    s.uploads = counters().texture_uploads.load();
    s.upload_bytes = counters().texture_upload_bytes.load();
    s.transcodes = counters().bc7_transcodes.load();
    s.failed = counters().textures_failed.load();
    return s;
}

// Frees every cached object this module made (samplers, dummy views and images). Render thread, device idle, no texture alive:
// called by the tests before shutdown_for_test (the back end's teardown does not call it yet).
void tex_shutdown() {
    texup::shutdown();                // the cache upgrade threads stop (their work is dropped, never half written)
    texprep::release_staging();       // upload buffers of finished background results belong to the device that is going away
    std::vector<TexGpu*> left;
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        left.assign(g_live_set.begin(), g_live_set.end());
    }
    // Textures that are still alive (the game never finalized them, or a test left them behind) keep their place in the registry. They
    // must not keep a pointer to the image object that is about to be freed: texture_registry_find_alias would hand it to the next texture
    // made over the same memory, and that texture would use freed memory (a barrier on VK_NULL_HANDLE, or a crash).
    texture_registry_detach_gpu();
    for (TexGpu* g : left) { g->refs = 1; tex_destroy(g); }
    for (auto& kv : g_sh.samplers)
        for (auto& e : kv.second) { vkDestroySampler(vkdev(), e.second, nullptr); vk_gauge_samplers().sub(1); }
    g_sh.samplers.clear();
    if (g_sh.dummy_sampler != VK_NULL_HANDLE) { vkDestroySampler(vkdev(), g_sh.dummy_sampler, nullptr); vk_gauge_samplers().sub(1); }
    for (auto& kv : g_sh.storage) {
        if (kv.second.view != VK_NULL_HANDLE) { vkDestroyImageView(vkdev(), kv.second.view, nullptr); vk_gauge_image_views().sub(1); }
        device().destroy_image(kv.second.img);
    }
    for (auto& kv : g_sh.sized) {
        if (kv.second.view != VK_NULL_HANDLE) { vkDestroyImageView(vkdev(), kv.second.view, nullptr); vk_gauge_image_views().sub(1); }
        device().destroy_image(kv.second.img);
    }
    for (Dummy* set : {g_sh.color, g_sh.depth, g_sh.dcmp[0], g_sh.dcmp[1], g_sh.dcmp[2], g_sh.dcmp[3], g_sh.dcmp[4], g_sh.dcmp[5], g_sh.dcmp[6], g_sh.dcmp[7]}) {
        for (int i = 0; i < 6; ++i) {
            if (set[i].view != VK_NULL_HANDLE) { vkDestroyImageView(vkdev(), set[i].view, nullptr); vk_gauge_image_views().sub(1); }
            device().destroy_image(set[i].img);
        }
    }
    g_sh = Shared();
}

namespace tex {
static float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m + 1024), static_cast<int>(e) - 25);
    return s ? -v : v;
}
static float small_float(uint32_t bits, int mant_bits) {   // unsigned 11 / 10 bit float (5 bit exponent)
    const uint32_t e = bits >> mant_bits, m = bits & ((1u << mant_bits) - 1);
    if (e == 0) return std::ldexp(static_cast<float>(m), -14 - mant_bits);
    if (e == 31) return m ? NAN : INFINITY;
    return std::ldexp(1.0f + static_cast<float>(m) / static_cast<float>(1u << mant_bits), static_cast<int>(e) - 15);
}
static uint8_t to8(float v, bool hdr) {
    if (!(v == v)) return 255;
    if (hdr) v = v / (1.0f + v);          // display of a high dynamic range value
    return static_cast<uint8_t>(std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f);
}
}  // namespace tex

// The decode of a dump: texel bytes of a level to RGBA8 (saved as a PNG when path is not empty) plus per-channel statistics.
static bool convert_png_impl(const uint8_t* src, VkFormat f, VkExtent3D e, uint32_t bpp, VkImageAspectFlags aspect, const std::string& path, std::string* stats) {
    std::vector<uint8_t> rgba(static_cast<size_t>(e.width) * e.height * 4, 255);
    double sum[4] = {0, 0, 0, 0};
    float mn[4] = {1e30f, 1e30f, 1e30f, 1e30f}, mx[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
    uint64_t nonzero = 0;
    for (uint32_t i = 0; i < e.width * e.height; ++i) {
        const uint8_t* p = src + static_cast<size_t>(i) * bpp;
        float c[4] = {0, 0, 0, 1};
        bool hdr = false;
        auto u32 = [&](int k) { uint32_t v; std::memcpy(&v, p + 4 * k, 4); return v; };
        auto f32 = [&](int k) { float v; std::memcpy(&v, p + 4 * k, 4); return v; };
        auto u16 = [&](int k) { uint16_t v; std::memcpy(&v, p + 2 * k, 2); return v; };
        switch (f) {
            case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: for (int k = 0; k < 4; ++k) c[k] = p[k] / 255.0f; break;
            case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: c[0] = p[2] / 255.0f; c[1] = p[1] / 255.0f; c[2] = p[0] / 255.0f; c[3] = p[3] / 255.0f; break;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32: { const uint32_t v = u32(0); c[0] = (v & 1023) / 1023.0f; c[1] = ((v >> 10) & 1023) / 1023.0f; c[2] = ((v >> 20) & 1023) / 1023.0f; c[3] = (v >> 30) / 3.0f; break; }
            case VK_FORMAT_R8G8_UNORM: c[0] = p[0] / 255.0f; c[1] = p[1] / 255.0f; c[2] = 0; break;
            case VK_FORMAT_R8_UNORM: c[0] = c[1] = c[2] = p[0] / 255.0f; break;
            case VK_FORMAT_R16_UNORM: c[0] = c[1] = c[2] = u16(0) / 65535.0f; break;
            case VK_FORMAT_R16_SFLOAT: c[0] = c[1] = c[2] = half_to_float(u16(0)); hdr = true; break;
            case VK_FORMAT_R16G16B16A16_SFLOAT: for (int k = 0; k < 4; ++k) c[k] = half_to_float(u16(k)); hdr = true; break;
            case VK_FORMAT_R16G16B16A16_UNORM: for (int k = 0; k < 4; ++k) c[k] = u16(k) / 65535.0f; break;
            case VK_FORMAT_R32_SFLOAT: c[0] = c[1] = c[2] = f32(0); hdr = true; break;
            case VK_FORMAT_R32G32_SFLOAT: c[0] = f32(0); c[1] = f32(1); c[2] = 0; hdr = true; break;
            case VK_FORMAT_R32G32B32A32_SFLOAT: for (int k = 0; k < 4; ++k) c[k] = f32(k); hdr = true; break;
            case VK_FORMAT_B10G11R11_UFLOAT_PACK32: { const uint32_t v = u32(0); c[0] = small_float(v & 2047, 6); c[1] = small_float((v >> 11) & 2047, 6); c[2] = small_float(v >> 22, 5); hdr = true; break; }
            case VK_FORMAT_R32_UINT: { const uint32_t v = u32(0); c[0] = (v & 255) / 255.0f; c[1] = ((v >> 8) & 255) / 255.0f; c[2] = ((v >> 16) & 255) / 255.0f; c[3] = (v >> 24) / 255.0f; break; }
            case VK_FORMAT_R32G32_UINT: { const uint32_t v = u32(0), w2 = u32(1); c[0] = (v & 255) / 255.0f; c[1] = ((v >> 8) & 255) / 255.0f; c[2] = (w2 & 255) / 255.0f; c[3] = ((w2 >> 8) & 255) / 255.0f; break; }
            case VK_FORMAT_R32G32B32A32_UINT: for (int k = 0; k < 4; ++k) c[k] = (u32(k) & 255) / 255.0f; break;
            case VK_FORMAT_D16_UNORM: c[0] = c[1] = c[2] = u16(0) / 65535.0f; break;
            case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: c[0] = c[1] = c[2] = f32(0); break;
            case VK_FORMAT_D24_UNORM_S8_UINT: c[0] = c[1] = c[2] = (u32(0) & 0xffffff) / 16777215.0f; break;
            default: break;
        }
        bool any = false;
        for (int k = 0; k < 4; ++k) {
            if (c[k] == c[k]) {
                sum[k] += c[k];
                mn[k] = std::min(mn[k], c[k]);
                mx[k] = std::max(mx[k], c[k]);
            }
            if (c[k] != 0.0f) any = true;
            rgba[static_cast<size_t>(i) * 4 + k] = (k == 3 && (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) ? 255 : to8(c[k], hdr);
        }
        if (any) ++nonzero;
    }
    if (stats) {
        char b[400];
        const double n = static_cast<double>(e.width) * e.height;
        std::snprintf(b, sizeof b, "min %g %g %g %g max %g %g %g %g mean %g %g %g %g nonzero %.1f%%", mn[0], mn[1], mn[2], mn[3], mx[0], mx[1], mx[2], mx[3],
                      sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, 100.0 * static_cast<double>(nonzero) / n);
        *stats = b;
    }
    return path.empty() || gfx::save_png_rgba(path, rgba.data(), e.width, e.height);
}

// ---- mid-frame snapshots (debug): a copy of a texture recorded into the frame being replayed, written out after the frame
static uint32_t dump_bpp(VkFormat f, VkImageAspectFlags* aspect) {
    *aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return 16;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: return 2;
        case VK_FORMAT_R8_UNORM: return 1;
        case VK_FORMAT_D16_UNORM: *aspect = VK_IMAGE_ASPECT_DEPTH_BIT; return 2;
        case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: *aspect = VK_IMAGE_ASPECT_DEPTH_BIT; return 4;
        default: return 0;
    }
}

namespace tex {
struct Snapshot {
    gfx::Buffer buf;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D e = {0, 0, 1};
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t bpp = 0;
    bool raw = false;                  // a block-compressed image copied as it is (the buffer is written to a file as it is)
};
static std::vector<Snapshot> g_snaps;
}  // namespace tex

int snapshot_record(Texture* t, Frame& fr) {
    TexGpu* g = t ? t->gpu : nullptr;
    if (!g || g->failed || g->layout == VK_IMAGE_LAYOUT_UNDEFINED) return -1;
    Snapshot s;
    s.format = g->format;
    s.bpp = dump_bpp(g->format, &s.aspect);
    if (!s.bpp) return -1;
    const VkExtent3D full = level_extent(g->base, 0);
    const bool is3d = g->type == VK_IMAGE_TYPE_3D;
    const uint32_t slices = is3d ? full.depth : std::max<uint32_t>(1, g->layers);   // a 3D texture's slices or an array's layers, stacked into one tall image
    s.e = {full.width, full.height * slices, 1};
    const uint64_t size = static_cast<uint64_t>(s.e.width) * s.e.height * s.bpp;
    if (device().create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &s.buf, "nvn snapshot") != VK_SUCCESS) return -1;
    const VkImageLayout prev = g->layout;
    move_layout(fr, *g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = {};
    r.imageSubresource = {s.aspect, 0, 0, is3d ? 1u : slices};
    r.imageExtent = {full.width, full.height, is3d ? slices : 1u};
    vkCmdCopyImageToBuffer(fr.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buf.buffer, 1, &r);
    move_layout(fr, *g, prev);
    g_snaps.push_back(std::move(s));
    g_gauge_texture_snapshots.add(1);
    return static_cast<int>(g_snaps.size()) - 1;
}

// A block-compressed image: every level and layer, copied as blocks (for comparing what the GPU holds with trace_dump_prepared's data).
int snapshot_record_raw(Texture* t, Frame& fr) {
    TexGpu* g = t ? t->gpu : nullptr;
    if (!g || g->failed || g->layout == VK_IMAGE_LAYOUT_UNDEFINED || g->depth) return -1;
    uint32_t bytes = 0;
    if (g->format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && g->format <= VK_FORMAT_BC7_SRGB_BLOCK)
        bytes = (g->format <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK || g->format == VK_FORMAT_BC4_UNORM_BLOCK || g->format == VK_FORMAT_BC4_SNORM_BLOCK) ? 8 : 16;
    if (!bytes) return -1;
    // every level, tight, level after level with all layers: the layout prepare_texture produces for block formats
    std::vector<VkBufferImageCopy> regions;
    uint64_t size = 0;
    for (uint32_t l = 0; l < g->levels; ++l) {
        const VkExtent3D e = level_extent(g->base, l);
        VkBufferImageCopy r = {};
        r.bufferOffset = size;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, g->layers};
        r.imageExtent = {e.width, e.height, 1};
        regions.push_back(r);
        size += static_cast<uint64_t>((e.width + 3) / 4) * ((e.height + 3) / 4) * bytes * g->layers;
    }
    Snapshot s;
    s.raw = true;
    s.format = g->format;
    s.e = {static_cast<uint32_t>(size), 1, 1};
    if (device().create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &s.buf, "nvn raw snapshot") != VK_SUCCESS) return -1;
    const VkImageLayout prev = g->layout;
    move_layout(fr, *g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(fr.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buf.buffer, static_cast<uint32_t>(regions.size()), regions.data());
    move_layout(fr, *g, prev);
    g_snaps.push_back(std::move(s));
    g_gauge_texture_snapshots.add(1);
    return static_cast<int>(g_snaps.size()) - 1;
}

void trace_dump_prepared(Texture* t, const std::string& dir) {
    static std::unordered_set<uint32_t> done;
    if (!t || !done.insert(t->id).second) return;
    TexCaps caps;
    caps.bc = device().features().bc;
    caps.astc = device().features().astc_ldr;
    caps.bc7_transcode = true;
    PreparedTexture prep;
    const bool ok = prepare_texture(*t, caps, prep);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string base = dir + "/t" + std::to_string(t->id);
    if (FILE* f = std::fopen((base + ".txt").c_str(), "w")) {
        std::fprintf(f, "ok %d format %d levels %u layers %u block %ux%u bytes %u transcoded %d error %s\n", ok ? 1 : 0, static_cast<int>(prep.format), prep.levels, prep.layers,
                     prep.block_w, prep.block_h, prep.block_bytes, prep.transcoded ? 1 : 0, prep.error.c_str());
        for (const PreparedLevel& l : prep.level)
            std::fprintf(f, "level %u %u %u offset %llu size %llu row_blocks %u\n", l.width, l.height, l.depth, static_cast<unsigned long long>(l.offset),
                         static_cast<unsigned long long>(l.size), l.row_blocks);
        std::fclose(f);
    }
    if (ok)
        if (FILE* f = std::fopen((base + ".bin").c_str(), "wb")) {
            std::fwrite(prep.data.data(), 1, prep.data.size(), f);
            std::fclose(f);
        }
}

bool snapshot_write(int index, const std::string& path, std::string* stats) {
    if (index < 0 || index >= static_cast<int>(g_snaps.size())) return false;
    Snapshot& s = g_snaps[static_cast<size_t>(index)];
    if (!s.buf) return false;
    device().invalidate_buffer(s.buf);
    if (s.raw) {
        bool wrote = false;
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            wrote = std::fwrite(s.buf.mapped, 1, static_cast<size_t>(s.e.width), f) == s.e.width;   // e.width holds the byte count of a raw copy
            std::fclose(f);
        }
        if (stats) *stats = "raw " + std::to_string(s.e.width) + " bytes";
        device().destroy_buffer(s.buf);
        s.buf = gfx::Buffer();
        return wrote;
    }
    const bool ok = convert_png_impl(static_cast<const uint8_t*>(s.buf.mapped), s.format, s.e, s.bpp, s.aspect, path, stats);
    if ((s.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || s.format == VK_FORMAT_R16G16B16A16_UNORM) {   // depth, 16-bit colour: the texel bytes too (<path>.bin), an 8-bit PNG loses the precision
        if (FILE* f = std::fopen((path + ".bin").c_str(), "wb")) {
            std::fwrite(s.buf.mapped, 1, static_cast<size_t>(s.e.width) * s.e.height * s.bpp, f);
            std::fclose(f);
        }
    }
    device().destroy_buffer(s.buf);
    s.buf = gfx::Buffer();
    return ok;
}

bool dump_texture(Texture* t, const std::string& path, std::string* stats) {
    TexGpu* g = t ? t->gpu : nullptr;
    if (!g || g->failed || g->layout == VK_IMAGE_LAYOUT_UNDEFINED) return false;
    const VkFormat f = g->format;
    uint32_t bpp = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            bpp = 4; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: bpp = 8; break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: bpp = 16; break;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: bpp = 2; break;
        case VK_FORMAT_R8_UNORM: bpp = 1; break;
        case VK_FORMAT_D16_UNORM: bpp = 2; aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
        case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: bpp = 4; aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
        case VK_FORMAT_D24_UNORM_S8_UINT: bpp = 4; aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
        default: return false;
    }
    const VkExtent3D e = level_extent(g->base, 0);
    const uint64_t size = static_cast<uint64_t>(e.width) * e.height * bpp;
    finish();
    gfx::Buffer rb;
    run_frame_sync([&](Frame& fr) {
        if (device().create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &rb, "nvn texture dump") != VK_SUCCESS) return;
        const VkImageLayout prev = g->layout;
        move_layout(fr, *g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r = {};
        r.imageSubresource = {aspect, 0, 0, 1};
        r.imageExtent = {e.width, e.height, 1};
        vkCmdCopyImageToBuffer(fr.cb, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &r);
        move_layout(fr, *g, prev);
    });
    if (!rb) return false;
    device().invalidate_buffer(rb);
    const bool ok = convert_png_impl(static_cast<const uint8_t*>(rb.mapped), f, e, bpp, aspect, path, stats);
    device().destroy_buffer(rb);
    return ok;
}

void mark_texture_cpu_dirty(Texture* t) {
    if (!t) return;
    texprep::on_flushed(t);        // before the flag: a reader that sees the flag also sees the new generation
    t->cpu_dirty.store(true);
}

void texture_bound(Texture* t) { texprep::on_bound(t); }
void texture_registered(Texture* t) { texprep::on_registered(t); }

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

    // [nvn] readback_defer (#276): a small texture the game reads again and again (Pokopia: texture 286, 30x16, about once a second) gets
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
    const bool exact_texture = g->format == VK_FORMAT_R8G8B8A8_SRGB && g->base.width == 30 && g->base.height == 16 &&
                               g->base.depth == 1 && L.levels == 1 && L.layers == 1 && L.layout.linear_size <= kDeferMaxBytes;
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
