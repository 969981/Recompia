// SPDX-License-Identifier: GPL-3.0-or-later
// Internal header of the texture images (nvn_gpu_texture*.cpp, render thread only): TexGpu and the helpers the texture files share.
#pragma once
#include <chrono>
#include <mutex>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/ftrace.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/host_device.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/write_watch.h"
#include "vk_gauges.h"
#include "nvn_dcmp.h"
#include "nvn_enums.h"
#include "nvn_fnv.h"
#include "nvn_format.h"
#include "nvn_format_guard.h"
#include "nvn_gpu_internal.h"
#include "extras/hdr_nvn.h"
#include "nvn_texdata.h"
#include "nvn_texprep.h"
#include "nvn_texup.h"
#include "nvn_texture_layout.h"
#include "tegra_tiling.h"
#include "astc2bc7.h"

namespace nxrt::nvn::be {

constexpr uint32_t kGenLevels = 16;

// ---- readback ([nvn] readback_async / readback_defer, nvn_gpu_texture_readback.cpp) ------------------------------------------------
// The levels and layers one read asks for.
struct ReadbackRange {
    uint32_t level = 0, levels = 0, layer = 0, layers = 0;
    bool operator==(const ReadbackRange&) const = default;
    bool fits(uint32_t level_count, uint32_t layer_count) const {
        return levels && layers && level < level_count && levels <= level_count - level && layer < layer_count && layers <= layer_count - layer;
    }
};
// What a read asked for and the state of the storage it asked about. Two reads with equal stamps want the same bytes of the same image:
// only then may a deferred copy stand in for the read ([nvn] readback_defer). cpu_generation: Texture::prep_gen (a CPU write or an upload
// in between makes the previous read's data wrong); sync_epoch: TexGpu::rb_sync_epoch (a synchronous read on the render thread refreshed
// the storage without rb_mu, so the deferred bookkeeping is stale).
struct ReadbackStamp {
    uint32_t texture = 0, cpu_generation = 0;
    uintptr_t storage = 0;
    uint64_t storage_bytes = 0;
    ReadbackRange range;
    uint64_t sync_epoch = 0;
    bool operator==(const ReadbackStamp&) const = default;
};
// A deferred copy ([nvn] readback_defer): queued by one read, landed by a later one once the GPU has finished it, never waited for. It owns
// its own buffer (the normal readback buffer TexGpu::rb keeps its deduplication state): dropping the last owner retires the buffer through
// the GPU-retirement list, so a copy still in flight on the GPU keeps its memory. The queued record job holds an owner until it has recorded.
struct DeferredReadback {
    gfx::Buffer buffer;
    std::atomic<bool> copied{false};                    // the record job copied into buffer (false: nothing to land)
    uint64_t serial = 0;                                // the submission the copy is in (poll_readback_serial)
    ReadbackStamp stamp;
    std::chrono::steady_clock::time_point requested;
    ~DeferredReadback() {
        if (buffer && gpu::active()) {
            gfx::Buffer old = buffer;
            gpu::defer_destroy([old]() mutable { if (gpu::active()) gpu::device().destroy_buffer(old); });
        }
    }
};

struct TexGpu {
    bool promoted = false;                              // HDR scene pipeline: an 8-bit sRGB render target kept as RGBA16F (src/gfx/extras/hdr_nvn.h)
    gfx::Image img;                                     // the image; created without a view (views are cached below)
    const FormatInfo* fmt = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;              // the image format: the NVN one, its fallback, or BC7 for ASTC
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkExtent3D base = {1, 1, 1};                        // level 0 extent (depth for 3D images)
    uint32_t levels = 1, layers = 1;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags cflags = 0;                      // the create flags the image was made with
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;   // tracked layout (the same for every level and layer)
    bool failed = false;                                // no image: the texture is not usable on the GPU (logged once)
    bool transcoded = false;                            // ASTC stored as BC7
    bool mutable_fmt = false;                           // sRGB and UNORM views of the image are allowed
    bool cube = false;                                  // created with CUBE_COMPATIBLE
    bool depth = false;                                 // depth and/or stencil aspect
    bool uploaded = false;                              // the image holds the storage's content (or the black fallback)
    bool gpu_written = false;                           // rendered by the GPU since the CPU last wrote the storage
    float scale = 1.0f;                                 // resolution scale: base is the NVN size times this ([gfx] render_scale, render targets only)
    bool scale_blocked = false;                         // was scaled, brought back to 1 (bound as a storage image, or paired with an unscaled target)
    std::map<std::array<uint32_t, 12>, VkImageView> views;
    int refs = 1;                                       // texture objects over the same storage share one image (render thread)
    struct SparseState* sparse = nullptr;               // a sparse-residency image (a big texture of a virtual pool), else null
    // CPU-streamed textures (tex_ensure_current): uploads for CPU writes that came at most kStreamGap presents apart, in a row
    uint32_t cpu_streak = 0, cpu_upload_at = 0;
    bool streamed = false;                              // rewritten by the CPU frame after frame: not write-watched, read again once per present
    uint32_t streamed_at = 0;                           // present count of the last read in that mode
    bool untiled = false;                               // the last upload went through gpu_untile (cheap enough to repeat every frame)
    // Write generations per level (kGenLevels; deeper levels share the last slot): a level gets a new, globally unique number whenever it may
    // have been written. A copy of the image (the feedback copies of nvn_replay.cpp) remembers the numbers it copied and copies again only
    // the levels whose number changed. Every write goes through a layout change into a writable layout (move_layout: all levels) or is a
    // render pass (tex_transition_attach / tex_mark_gpu_written with its level), so a copy that leaves the image in TRANSFER_SRC sees each later
    // write. A new image (creation, rescale, feedback loop usage) gets new numbers for every level.
    uint64_t wgen[kGenLevels] = {};
    bool fb_loop_tried = false;                         // tex_enable_feedback_loop ran (once per image)
    bool read_back = false;                             // download_texture copied it into game memory (the CPU reads it): a dispatch that writes
                                                        // it is never skipped ([nvn] async_compute). Render thread (set in download_texture's job)
    // [nvn] readback_async (download_texture). rb: the readback buffer, laid out like the linear image (tegra::Layout offsets), grown to
    // the largest request and never shrunk; freed with the image (tex_destroy). rb_gen[level]: the write generation the level had when the
    // storage last got it (read after the layout restore, which counts as a write); rb_have[level][layer]: the storage holds that layer
    // of it. Both render thread only (the dedupe runs in queue order there). rb_mu: held by a reading thread from the queueing to the
    // tiling, so two guest threads reading the image do not interleave.
    std::mutex rb_mu;
    std::atomic<uint64_t> rb_sync_epoch{0};
    gfx::Buffer rb;
    std::vector<uint64_t> rb_gen;
    std::vector<std::vector<uint8_t>> rb_have;
    // [nvn] readback_defer, under rb_mu. rb_pending: the copy the last read queued, still to land. rb_previous (valid, at): the read whose
    // data the storage holds, and when that data was requested from the GPU. rb_last / rb_last_range: when and what the last read asked for
    // (a read repeats when it asks the same within 2 s).
    std::shared_ptr<DeferredReadback> rb_pending;
    bool rb_previous_valid = false;
    ReadbackStamp rb_previous;
    std::chrono::steady_clock::time_point rb_previous_at{};
    std::chrono::steady_clock::time_point rb_last{};
    ReadbackRange rb_last_range;
};

// A virtual-pool texture too big to back fully (Mario Kart World's terrain virtual texture caches: 8192x8192 arrays of 68 layers, 1.4 to
// 5.8 GB, of which a few hundred MB of tiles are mapped) is a Vulkan sparse-residency image. Host sparse blocks get device memory when a
// mapped guest tile lands on them; the mip tail and the levels whose guest tiles are not whole GOB-block runs are bound at creation.
struct SparseState {
    VkDeviceSize page = 0x10000;                        // bytes of one sparse block (the image's memory alignment)
    uint32_t mem_type = 0;
    VkExtent3D gran = {1, 1, 1};                        // texels of one sparse block
    uint32_t tail_first = 0;                            // first level of the mip tail
    VkDeviceSize tail_size = 0, tail_offset = 0, tail_stride = 0;
    bool single_tail = false;
    std::vector<VkDeviceMemory> chunks;                 // device memory the blocks are suballocated from
    VkDeviceSize chunk_used = 0, chunk_size = 0;
    std::set<uint64_t> bound;                           // (layer, level, block y, block x) of the bound blocks
    TexLayout L;                                        // the guest layout
    std::vector<VkSparseImageMemoryBind> binds;         // waiting for the next flush
    std::vector<VkSparseMemoryBind> opaque;
    uint64_t bytes = 0;                                 // device memory bound
    uint64_t tiles_uploaded = 0;
};

namespace gpu {
// The texture files' own names (they were anonymous namespaces; `using namespace tex` below keeps the lookup the same). Their
// definitions are in nvn_gpu_texture*.cpp; a name only one file uses is static there.
namespace tex {

constexpr const char* kTag = "nvn.tex";
extern std::atomic<uint64_t> g_live_images;
extern nxrt::GaugeCounter g_gauge_texture_snapshots;
bool first_time(const std::string& key);

inline uint64_t div_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

// ---- formats -------------------------------------------------------------------------------------------------------------
inline bool is_block_format(VkFormat f) {
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) ||
           (f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK) ||
           (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
}

inline bool is_srgb(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return true;
        default:
            break;
    }
    if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)
        return ((static_cast<int>(f) - static_cast<int>(VK_FORMAT_ASTC_4x4_UNORM_BLOCK)) & 1) != 0;
    return false;
}

// The sRGB / UNORM partner of a format (the two may view the same image when it is mutable), UNDEFINED when there is none.
inline VkFormat srgb_partner(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
        case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
        case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
        case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
        case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
        case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
        default: break;
    }
    if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
        const int base = static_cast<int>(VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
        return static_cast<VkFormat>(((static_cast<int>(f) - base) ^ 1) + base);   // the ASTC UNORM/SRGB pairs are consecutive
    }
    return VK_FORMAT_UNDEFINED;
}

// The float member of a 32/64/128-bit integer format's class and back: Maxwell moves the raw bits, whatever the shader's declared
// type (the translated shaders declare every sampled value and colour output as float).
inline VkFormat float_alias(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: return VK_FORMAT_R32_SFLOAT;
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_SFLOAT;
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}
inline VkFormat uint_alias(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_SINT: return VK_FORMAT_R32_UINT;
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_UINT;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}
inline VkFormat sint_alias(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: return VK_FORMAT_R32_SINT;
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: return VK_FORMAT_R32G32_SINT;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: return VK_FORMAT_R32G32B32A32_SINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}
inline bool bit_alias_class(VkFormat f) { return float_alias(f) != VK_FORMAT_UNDEFINED || uint_alias(f) != VK_FORMAT_UNDEFINED; }

// Same image family: the format itself, its sRGB / UNORM partner, or a raw-bit alias of the same size, on a mutable image.
inline bool format_view_ok(const TexGpu& g, VkFormat fmt) {
    if (fmt == g.format) return true;
    if (!g.mutable_fmt) return false;
    if (srgb_partner(g.format) == fmt) return true;
    return bit_alias_class(g.format) && (fmt == float_alias(g.format) || fmt == uint_alias(g.format) || fmt == sint_alias(g.format));
}
VkFormat promoted_view_format(const TexGpu& g, VkFormat f);

// The format a colour attachment of this image is rendered through: integer 32-bit formats as their float alias, because the
// translated fragment shaders write float outputs and the console stores the register bits unconverted.
inline VkFormat attachment_format_of(const TexGpu& g, VkFormat requested) {
    const VkFormat f = requested == VK_FORMAT_UNDEFINED ? g.format : requested;
    if (g.mutable_fmt && bit_alias_class(f)) {
        const VkFormat a = float_alias(f);
        if (a != VK_FORMAT_UNDEFINED && format_view_ok(g, a)) return a;
    }
    return f;
}

// ---- shapes --------------------------------------------------------------------------------------------------------------
struct Shape {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkExtent3D base = {1, 1, 1};
    uint32_t levels = 1, layers = 1;
    bool cube = false;
    uint32_t samples = 1;                      // multisampled render target / resolve source
};

inline VkExtent3D level_extent(const VkExtent3D& base, uint32_t level) {
    auto shr = [level](uint32_t v) { return level >= 32 ? 1u : std::max<uint32_t>(1, v >> level); };
    return {shr(base.width), shr(base.height), shr(base.depth)};
}

inline VkImageCreateFlags base_flags(const Shape& s, VkFormat fmt) {
    VkImageCreateFlags f = 0;
    if (s.cube) f |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (s.type == VK_IMAGE_TYPE_3D) f |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;   // 2D views of the slices
    if (srgb_partner(fmt) != VK_FORMAT_UNDEFINED || bit_alias_class(fmt)) f |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    return f;
}
bool create_sparse_texture(Texture* t, TexGpu* g, const Shape& s, std::string* why);
TexGpu* create_texture_gpu(Texture* t, float want_scale);

// The texture's image, created on first use. Null when the texture has no usable image (the failure is counted and logged once).
inline TexGpu* tex_or_null(Texture* t, float want_scale = 1.0f) {
    if (!t || !active()) return nullptr;
    if (t->gpu) return t->gpu->failed ? nullptr : t->gpu;
    if (Ref<Texture> alias = texture_registry_find_alias(t)) {
        TexGpu* ag = alias->gpu;
        if (ag && !ag->failed) {
            // the same bytes under another texture object: one image (the object rendered to and the one that is sampled are not the same
            // object); what the image holds is as current for this object as for the one it was made for
            t->gpu = ag;
            g_bind_gen.fetch_add(1, std::memory_order_relaxed);
            ++ag->refs;
            if (ag->uploaded || ag->gpu_written) t->cpu_dirty.store(false);
            static int logged = 0;
            if (logged++ < 20) NXLOG_I(kTag, "texture %u shares the image of texture %u (the same storage)", t->id, alias->id);
            return ag;
        }
    }
    NXFT_MIN("tex.create", 30000);
    TexGpu* g = create_texture_gpu(t, want_scale);
    return g->failed ? nullptr : g;
}
void move_layout(Frame& f, TexGpu& g, VkImageLayout to, int write_level = -1);
void copy_regions_to_image(Frame& f, TexGpu& g, VkBuffer buf, const std::vector<VkBufferImageCopy>& regions);
void sparse_update(Texture* t, TexGpu& g, Frame& f);

// Dummy views: 1x1 (x1) textures of each view type, black (or depth 1.0), made the first time they are asked for.
struct Dummy {
    gfx::Image img;
    VkImageView view = VK_NULL_HANDLE;
    bool tried = false;
};

struct Shared {
    std::map<uint64_t, std::vector<std::pair<SamplerInfo, VkSampler>>> samplers;
    VkSampler dummy_sampler = VK_NULL_HANDLE;
    Dummy color[6];
    Dummy depth[6];
    std::map<std::pair<int, int>, Dummy> storage;   // (view type, format): GENERAL-layout 1x1 storage images
    DcmpValues dcmp_vals;                           // depth-compare stand-ins by cleared value (dcmp_view)
    Dummy dcmp[kDcmpMaxValues][6];                  // ... per value and view type
    std::unordered_map<uint32_t, std::pair<uint32_t, float>> dcmp_tex;   // texture id -> (prep_gen when read, red of texel 0)
    std::map<std::tuple<int, uint32_t, uint32_t, uint32_t>, Dummy> sized;   // (view type, width, height, layers): dummy_view_sized
};
extern Shared g_sh;
bool tex_ww_wanted(const Texture& t, const TexGpu& g);
void tex_ww_arm(const Texture& t, const TexGpu& g);
bool tex_ww_async_on();
void tex_ww_arm_later(Texture* t, const TexGpu& g, bool have_hash, uint64_t hash);
void sparse_bind_for_copy(Texture* t, TexGpu& g, uint32_t layer, uint32_t level);

}  // namespace tex
using namespace tex;

}  // namespace gpu
}  // namespace nxrt::nvn::be
