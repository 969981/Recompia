// SPDX-License-Identifier: GPL-3.0-or-later
// Image views, dummy images, samplers and texel buffer views of the texture images.
#include "nvn_gpu_texture_internal.h"

namespace nxrt::nvn::be {

namespace gpu {

namespace tex {
// ---- views ----------------------------------------------------------------------------------------------------------------
// The view with these fields, created once and cached on the image.
static VkImageView view_for(TexGpu& g, VkImageViewType type, uint32_t base_level, uint32_t level_count, uint32_t base_layer, uint32_t layer_count,
                     VkFormat fmt, VkImageAspectFlags aspect, const VkComponentMapping& swz) {
    const std::array<uint32_t, 12> key = {static_cast<uint32_t>(type), base_level, level_count, base_layer, layer_count,
                                          static_cast<uint32_t>(fmt), static_cast<uint32_t>(aspect), static_cast<uint32_t>(swz.r),
                                          static_cast<uint32_t>(swz.g), static_cast<uint32_t>(swz.b), static_cast<uint32_t>(swz.a), 0u};
    auto it = g.views.find(key);
    if (it != g.views.end()) return it->second;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g.img.image;
    vi.viewType = type;
    vi.format = fmt;
    vi.components = swz;
    vi.subresourceRange = {aspect, base_level, level_count, base_layer, layer_count};
    // A view takes the usages of its image; a format of the view that cannot do one of them (an sRGB view cannot be stored to) restricts it.
    VkImageViewUsageCreateInfo uc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    {
        // VK_EXT_attachment_feedback_loop_layout: a view used as an attachment in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL must carry
        // ATTACHMENT_FEEDBACK_LOOP_BIT_EXT in its usage (VUID-VkRenderingAttachmentInfo-imageView-10780), so a restricted usage keeps it
        // whenever the image has it. It has no format feature of its own: the image already passed its creation-time check (colour
        // attachment + sampled), and the view's own SAMPLED/COLOR_ATTACHMENT bits are still format-checked below.
        constexpr VkImageUsageFlags kViewUsages = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                  VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
        VkFormatProperties fp = {};
        vkGetPhysicalDeviceFormatProperties(device().physical(), fmt, &fp);
        VkImageUsageFlags u = g.usage & kViewUsages;
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) u &= ~VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) u &= ~VK_IMAGE_USAGE_STORAGE_BIT;
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) u &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (u != (g.usage & kViewUsages) && !(aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
            uc.usage = u;
            vi.pNext = &uc;
        }
    }
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(vkdev(), &vi, nullptr, &view) != VK_SUCCESS) {
        NXLOG_W(kTag, "vkCreateImageView failed for a view of format %d", static_cast<int>(fmt));
        return VK_NULL_HANDLE;
    }
    g.views.emplace(key, view);
    vk_gauge_image_views().add(1);
    return view;
}

static VkImageView bad_view(const Texture& t, const char* what) {
    if (first_time("view " + std::to_string(t.id) + " " + what))
        NXLOG_W(kTag, "texture %u: view not possible (%s): the binding gets the dummy view", t.id, what);
    return VK_NULL_HANDLE;
}

static const VkComponentMapping kIdentity = {};
constexpr VkImageViewType kDummyTypes[6] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                            VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, VK_IMAGE_VIEW_TYPE_3D};

static int dummy_index(VkImageViewType type) {
    for (int i = 0; i < 6; ++i)
        if (kDummyTypes[i] == type) return i;
    return 1;   // anything else: a 2D dummy
}
Shared g_sh;

static bool make_dummy(Dummy& d, VkImageViewType type, bool depth, float depth_value = 1.0f, uint32_t width = 1, uint32_t height = 1, uint32_t layers2d = 1) {
    d.tried = true;
    const VkFormat fmt = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    gfx::ImageDesc id;
    id.format = fmt;
    id.width = width;
    id.height = height;
    if (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY) id.array_layers = layers2d;
    id.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    id.create_view = false;
    id.name = depth ? "nvn dummy depth" : "nvn dummy";
    switch (type) {
        case VK_IMAGE_VIEW_TYPE_1D: id.type = VK_IMAGE_TYPE_1D; break;
        case VK_IMAGE_VIEW_TYPE_3D: id.type = VK_IMAGE_TYPE_3D; break;
        case VK_IMAGE_VIEW_TYPE_CUBE:
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
            id.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            id.array_layers = 6;
            break;
        default: break;
    }
    std::string why;
    if (device().create_image(id, &d.img, &why) != VK_SUCCESS) {
        NXLOG_W(kTag, "dummy view image failed: %s", why.c_str());
        return false;
    }
    const VkImageAspectFlags asp = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    const uint32_t layers = id.array_layers;
    const VkImage img = d.img.image;
    const bool ok = device().submit_once([&](VkCommandBuffer cmd) {
        image_barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, asp, 0, 1, 0, layers);
        const VkImageSubresourceRange range = {asp, 0, 1, 0, layers};
        if (depth) {
            VkClearDepthStencilValue v = {depth_value, 0};
            vkCmdClearDepthStencilImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        } else {
            VkClearColorValue c = {};
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
        }
        image_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, asp, 0, 1, 0, layers);
    });
    if (!ok) {
        NXLOG_W(kTag, "dummy view clear failed");
        device().destroy_image(d.img);
        return false;
    }
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = type;
    vi.format = fmt;
    vi.subresourceRange = {asp, 0, 1, 0, layers};
    if (vkCreateImageView(vkdev(), &vi, nullptr, &d.view) != VK_SUCCESS) {
        d.view = VK_NULL_HANDLE;
        device().destroy_image(d.img);
        return false;
    }
    vk_gauge_image_views().add(1);
    return true;
}

// ---- samplers -------------------------------------------------------------------------------------------------------------
static uint64_t sampler_hash(const SamplerInfo& s) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
        h ^= h >> 29;
    };
    auto bits = [](float f) {
        uint32_t u = 0;
        std::memcpy(&u, &f, 4);
        return static_cast<uint64_t>(u);
    };
    mix(static_cast<uint32_t>(s.min_filter));
    mix(static_cast<uint32_t>(s.mag_filter));
    mix(static_cast<uint32_t>(s.wrap_s));
    mix(static_cast<uint32_t>(s.wrap_t));
    mix(static_cast<uint32_t>(s.wrap_r));
    mix(bits(s.lod_min));
    mix(bits(s.lod_max));
    mix(bits(s.lod_bias));
    mix(bits(s.lod_snap));
    mix(static_cast<uint32_t>(s.compare_mode));
    mix(static_cast<uint32_t>(s.compare_func));
    for (float c : s.border_color) mix(bits(c));
    mix(static_cast<uint32_t>(s.border_kind));
    mix(bits(s.max_anisotropy));
    mix(static_cast<uint32_t>(s.reduction));
    return h;
}

static VkSamplerAddressMode wrap_mode(int nvn_wrap) {
    const VkSamplerAddressMode m = vk_address_mode(nvn_wrap);
    if (m == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE && !device().features().sampler_mirror_clamp_to_edge)
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    return m;
}

// Without custom border colours: the nearest of transparent black, opaque black and opaque white.
static VkBorderColor standard_border(const SamplerInfo& s) {
    float c[4];
    for (int k = 0; k < 4; ++k) {
        if (s.border_kind == 0) {
            c[k] = s.border_color[k];
        } else {
            uint32_t u = 0;
            std::memcpy(&u, &s.border_color[k], 4);
            c[k] = s.border_kind == 1 ? static_cast<float>(static_cast<int32_t>(u)) : static_cast<float>(u);
        }
    }
    const float cand[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
    int best = 0;
    float best_d = 1e30f;
    for (int i = 0; i < 3; ++i) {
        float d = 0;
        for (int k = 0; k < 4; ++k) d += (c[k] - cand[i][k]) * (c[k] - cand[i][k]);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (s.border_kind == 0) {
        static const VkBorderColor f[3] = {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
        return f[best];
    }
    static const VkBorderColor i[3] = {VK_BORDER_COLOR_INT_TRANSPARENT_BLACK, VK_BORDER_COLOR_INT_OPAQUE_BLACK, VK_BORDER_COLOR_INT_OPAQUE_WHITE};
    return i[best];
}

// Null when the device refuses the sampler (the caller then uses the dummy sampler).
static VkSampler create_sampler(const SamplerInfo& s) {
    const gfx::DeviceFeatures& ft = device().features();
    const bool mips = min_filter_uses_mips(s.min_filter);
    VkSamplerCustomBorderColorCreateInfoEXT cbc = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = vk_mag_filter(s.mag_filter);
    ci.minFilter = vk_min_filter(s.min_filter);
    ci.mipmapMode = mips ? vk_mipmap_mode(s.min_filter) : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap_mode(s.wrap_s);
    ci.addressModeV = wrap_mode(s.wrap_t);
    ci.addressModeW = wrap_mode(s.wrap_r);
    ci.mipLodBias = s.lod_bias;
    ci.minLod = mips ? s.lod_min : 0.0f;
    ci.maxLod = mips ? s.lod_max : 0.0f;
    if (s.max_anisotropy > 1.0f && ft.sampler_anisotropy) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(s.max_anisotropy, device().properties().limits.maxSamplerAnisotropy);
    }
    if (s.compare_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE) {
        ci.compareEnable = VK_TRUE;
        ci.compareOp = vk_compare_op(s.compare_func);
    }
    if (s.reduction != NVN_SAMPLER_REDUCTION_AVERAGE)
        note_unsupported("sampler reduction (min/max filtering): not enabled on the device, ignored");
    if (ft.custom_border_color) {
        // The border words are stored as the game gave them: float bits, or int / uint bits (border_kind).
        if (s.border_kind == 0) {
            ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            std::memcpy(cbc.customBorderColor.float32, s.border_color, sizeof(float) * 4);
        } else {
            ci.borderColor = VK_BORDER_COLOR_INT_CUSTOM_EXT;
            std::memcpy(cbc.customBorderColor.uint32, s.border_color, sizeof(uint32_t) * 4);
        }
        cbc.format = VK_FORMAT_UNDEFINED;
        ci.pNext = &cbc;
    } else {
        ci.borderColor = standard_border(s);
    }
    VkSampler out = VK_NULL_HANDLE;
    if (vkCreateSampler(vkdev(), &ci, nullptr, &out) != VK_SUCCESS) {
        NXLOG_W(kTag, "vkCreateSampler failed: the dummy sampler is used");
        return VK_NULL_HANDLE;
    }
    vk_gauge_samplers().add(1);
    return out;
}
}  // namespace tex

TexGpu* tex_gpu(Texture* t) { return tex_or_null(t); }

VkImageView tex_sampled_view(Texture* t, const ViewRequest& req) {
    TexGpu* g = tex_or_null(t);
    if (!g) return VK_NULL_HANDLE;
    const bool is3d = g->type == VK_IMAGE_TYPE_3D;
    const uint32_t total_layers = is3d ? g->base.depth : g->layers;   // 3D images: the slices are the layers of 2D views
    const uint32_t bl = req.base_level;
    if (bl >= g->levels) return bad_view(*t, "base level out of range");
    const uint32_t nl = req.level_count ? req.level_count : g->levels - bl;
    if (nl == 0 || bl + nl > g->levels) return bad_view(*t, "level range out of range");
    const uint32_t bly = req.base_layer;
    uint32_t nly = 1;
    switch (req.type) {
        case VK_IMAGE_VIEW_TYPE_2D:
            // A 2D sampler over an array texture (or a texture view of several layers) reads the first layer of the range
            // (inferred for a 2D TEX over an array header; before, the binding got the black dummy).
            if (bly >= total_layers) return bad_view(*t, "2D view of a layer out of range");
            break;
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
            nly = req.layer_count ? req.layer_count : total_layers - bly;
            if (nly == 0 || bly + nly > total_layers) return bad_view(*t, "2D array layer range out of range");
            break;
        case VK_IMAGE_VIEW_TYPE_CUBE:
            if (!g->cube || is3d || bly % 6 != 0 || bly + 6 > g->layers) return bad_view(*t, "cube needs a cube-compatible image of 6 layers");
            nly = 6;
            break;
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
            nly = req.layer_count ? req.layer_count : g->layers - bly;
            if (!g->cube || is3d || nly == 0 || nly % 6 != 0 || bly % 6 != 0 || bly + nly > g->layers)
                return bad_view(*t, "cube array needs a cube-compatible image with a multiple of 6 layers");
            break;
        case VK_IMAGE_VIEW_TYPE_3D:
            if (!is3d || bly != 0) return bad_view(*t, "3D view of an image that is not 3D");
            break;
        default:
            return bad_view(*t, "1D views are not supported (the image is 2D with height 1)");
    }
    VkFormat fmt = req.format == VK_FORMAT_UNDEFINED ? g->format : promoted_view_format(*g, req.format);
    if (req.component != 0 && g->mutable_fmt && fmt == g->format) {
        // The shader's sampled type decides how the bits read: float shaders over an integer texture (and the reverse) see the raw bits.
        const VkFormat a = req.component == 1 ? float_alias(fmt) : req.component == 2 ? sint_alias(fmt) : uint_alias(fmt);
        if (a != VK_FORMAT_UNDEFINED && format_view_ok(*g, a)) fmt = a;
    }
    if (!format_view_ok(*g, fmt)) {
        char what[160];
        snprintf(what, sizeof what, "view format %d is not compatible with the image's %d (NVN texture format %d, image mutable %d)", static_cast<int>(fmt),
                 static_cast<int>(g->format), static_cast<int>(t->b.format), g->mutable_fmt ? 1 : 0);
        return bad_view(*t, what);
    }
    VkImageAspectFlags asp = g->aspect;
    if (g->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) asp = req.depth_aspect ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    return view_for(*g, req.type, bl, nl, bly, nly, fmt, asp, req.swizzle);
}

VkImageView tex_attachment_view(Texture* t, uint32_t level, uint32_t layer, uint32_t layer_count, VkFormat view_format) {
    TexGpu* g = tex_or_null(t);
    if (!g) return VK_NULL_HANDLE;
    if (level >= g->levels) return bad_view(*t, "attachment level out of range");
    const uint32_t total_layers = g->type == VK_IMAGE_TYPE_3D ? g->base.depth : g->layers;
    const uint32_t n = layer_count > 1 ? layer_count : 1;
    if (layer >= total_layers || layer + n > total_layers) return bad_view(*t, "attachment layers out of range");
    const VkFormat fmt = attachment_format_of(*g, promoted_view_format(*g, view_format));
    if (!format_view_ok(*g, fmt)) return bad_view(*t, "attachment format is not compatible with the image");
    const VkImageViewType type = n > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    return view_for(*g, type, level, 1, layer, n, fmt, g->aspect, kIdentity);
}

VkFormat tex_attachment_format(Texture* t, VkFormat view_format) {
    TexGpu* g = tex_or_null(t);
    return g ? attachment_format_of(*g, promoted_view_format(*g, view_format)) : VK_FORMAT_UNDEFINED;
}

bool tex_has_storage(Texture* t) {
    TexGpu* g = tex_or_null(t);
    return g && (g->usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
}

VkFormat tex_storage_format(Texture* t, VkFormat requested) {
    TexGpu* g = tex_or_null(t);
    const VkFormat f = requested != VK_FORMAT_UNDEFINED ? requested : g ? g->format : VK_FORMAT_UNDEFINED;
    if (!g || !g->mutable_fmt || !is_srgb(f) || srgb_partner(f) == VK_FORMAT_UNDEFINED) return requested;
    return srgb_partner(f);
}

VkImageLayout tex_sampled_layout(Texture* t) {
    return (t && t->fmt && t->fmt->aspect != kAspectColor) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkImageView tex_present_view(Texture* t) {
    TexGpu* g = tex_or_null(t);
    if (!g) return VK_NULL_HANDLE;
    // The UNORM alias of an sRGB image; the plain view when the image cannot be viewed as UNORM.
    const VkFormat fmt = (g->mutable_fmt && is_srgb(g->format)) ? srgb_partner(g->format) : g->format;
    return view_for(*g, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1, fmt, g->aspect, kIdentity);
}

VkImageView dummy_view(VkImageViewType type, bool depth_compare) {
    const int idx = dummy_index(type);
    Dummy& d = (depth_compare ? g_sh.depth : g_sh.color)[idx];
    if (!d.tried) make_dummy(d, kDummyTypes[idx], depth_compare);
    return d.view;
}

// The red value of texel 0 of a colour texture, read once from its storage in the game's memory (no GPU work: texel (0, 0) is the first bytes in the
// block-linear and the pitch-linear layout) and read again only when the CPU wrote the texture since. 1.0 (lit, what the stand-in was before) when the
// texture has no CPU storage (a render target), is compressed, or has a format dcmp_red does not decode.
static float dcmp_red_of(Texture* t) {
    const uint32_t gen = t->prep_gen.load(std::memory_order_acquire);
    const auto it = g_sh.dcmp_tex.find(t->id);
    if (it != g_sh.dcmp_tex.end() && it->second.first == gen) return it->second.second;
    float v = 1.0f;
    uint8_t px[16] = {};
    const FormatInfo* f = t->fmt;
    bool have = false;
    if (f && !f->compressed && f->bytes <= sizeof px) {
        if (t->mem && t->storage_size >= f->bytes) { std::memcpy(px, t->mem, f->bytes); have = true; }
        else if (t->vpool) have = texture_read_virtual(*t, 0, f->bytes, px);
    }
    bool ok = false;
    if (have) v = dcmp_red(f->vk, px, &ok);
    if (!ok) v = 1.0f;
    if (g_sh.dcmp_tex.size() > 4096) g_sh.dcmp_tex.clear();
    g_sh.dcmp_tex[t->id] = {gen, v};
    NXLOG_I(kTag, "depth comparison on a colour texture %u (%s %dx%d): compared with its red channel, %.3f%s", t->id, f ? f->name : "?", t->b.width, t->b.height, v,
            ok ? "" : " (format not read: 1.0)");
    return v;
}

VkImageView dcmp_view(Texture* t, VkImageViewType type) {
    const int slot = g_sh.dcmp_vals.slot(dcmp_red_of(t));
    const int idx = dummy_index(type);
    Dummy& d = g_sh.dcmp[slot][idx];
    if (!d.tried) make_dummy(d, kDummyTypes[idx], true, g_sh.dcmp_vals.v[slot]);
    return d.view;
}

VkImageView dummy_view_sized(VkImageViewType type, uint32_t width, uint32_t height, uint32_t layers) {
    if (type != VK_IMAGE_VIEW_TYPE_2D && type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) return VK_NULL_HANDLE;
    const auto& lim = device().properties().limits;
    if (!width || !height || width > lim.maxImageDimension2D || height > lim.maxImageDimension2D) return VK_NULL_HANDLE;
    if (type == VK_IMAGE_VIEW_TYPE_2D) layers = 1;
    if (!layers || layers > lim.maxImageArrayLayers) return VK_NULL_HANDLE;
    Dummy& d = g_sh.sized[{static_cast<int>(type), width, height, layers}];
    if (!d.tried) make_dummy(d, type, false, 1.0f, width, height, layers);
    return d.view;
}

VkImageView dummy_storage_view(VkImageViewType type, VkFormat format) {
    if (format == VK_FORMAT_UNDEFINED) format = VK_FORMAT_R8G8B8A8_UNORM;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    const std::pair<int, int> key = {static_cast<int>(type), static_cast<int>(format)};
    auto& d = g_sh.storage[key];
    if (d.tried) return d.view;
    d.tried = true;
    gfx::ImageDesc id;
    id.format = format;
    id.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    id.create_view = false;
    id.name = "nvn dummy storage";
    if (type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY) id.type = VK_IMAGE_TYPE_1D;
    if (type == VK_IMAGE_VIEW_TYPE_3D) id.type = VK_IMAGE_TYPE_3D;
    if (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY) id.array_layers = 6;
    std::string why;
    if (device().create_image(id, &d.img, &why) != VK_SUCCESS) {
        NXLOG_W(kTag, "dummy storage image failed: %s", why.c_str());
        return d.view;
    }
    const VkImage img = d.img.image;
    const uint32_t layers = id.array_layers;
    const bool ok = device().submit_once([&](VkCommandBuffer cmd) {
        image_barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        VkClearColorValue c = {};
        vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
        image_barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
    });
    if (!ok) {
        device().destroy_image(d.img);
        return d.view;
    }
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = type;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    if (vkCreateImageView(vkdev(), &vi, nullptr, &d.view) != VK_SUCCESS) {
        d.view = VK_NULL_HANDLE;
        device().destroy_image(d.img);
    } else {
        vk_gauge_image_views().add(1);
    }
    return d.view;
}

VkSampler dummy_sampler() {
    if (g_sh.dummy_sampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = VK_FILTER_NEAREST;
        ci.minFilter = VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        if (vkCreateSampler(vkdev(), &ci, nullptr, &g_sh.dummy_sampler) != VK_SUCCESS) g_sh.dummy_sampler = VK_NULL_HANDLE;
        else vk_gauge_samplers().add(1);
    }
    return g_sh.dummy_sampler;
}

// The sampler as the display settings want it (nxrt/gfx_tuning.h): more anisotropy on filtering samplers, a LOD bias on every sampler
// that samples colour. Nearest-filtered samplers, shadow comparison samplers and clamped single-level samplers are left as the game set them.
SamplerInfo tuned_sampler(const SamplerInfo& in) {
    SamplerInfo s = in;
    const int aniso = gfx::tuning().anisotropy.load(std::memory_order_relaxed);
    const float bias = gfx::tuning().lod_bias.load(std::memory_order_relaxed);
    if (s.compare_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE) return s;
    const bool mips = min_filter_uses_mips(s.min_filter);
    if (aniso > 1 && mips && vk_min_filter(s.min_filter) == VK_FILTER_LINEAR && vk_mag_filter(s.mag_filter) == VK_FILTER_LINEAR)
        s.max_anisotropy = std::max(s.max_anisotropy, static_cast<float>(aniso));
    if (bias != 0.0f && mips) s.lod_bias += bias;
    return s;
}

VkSampler sampler_for(const SamplerInfo& game_sampler) {
    const SamplerInfo s = tuned_sampler(game_sampler);
    auto& bucket = g_sh.samplers[sampler_hash(s)];
    for (const auto& e : bucket)
        if (e.first == s) return e.second;
    const VkSampler v = create_sampler(s);
    if (v == VK_NULL_HANDLE) return dummy_sampler();
    bucket.emplace_back(s, v);
    return v;
}

VkSampler sampler_for(const SamplerInfo& s, VkFormat image_format) {
    SamplerInfo l = s;
    float aniso = l.max_anisotropy;
    if (format_guard().limit_sampler(image_format, &l.min_filter, &l.mag_filter, &aniso)) l.max_anisotropy = aniso;
    return sampler_for(l);
}

VkSampler sampler_for(const SamplerInfo& s, Texture* sampled) {
    const TexGpu* g = sampled ? tex_gpu(sampled) : nullptr;
    return g ? sampler_for(s, g->format) : sampler_for(s);
}

// ---- BUFFER-target textures: texel buffer views -------------------------------------------------------------------------------

namespace tex {
struct BufViewKey {
    VkBuffer buffer;
    VkDeviceSize offset, range;
    VkFormat format;
    bool operator<(const BufViewKey& o) const {
        if (buffer != o.buffer) return buffer < o.buffer;
        if (offset != o.offset) return offset < o.offset;
        if (range != o.range) return range < o.range;
        return format < o.format;
    }
};
static std::mutex g_bufview_mu;
static std::map<BufViewKey, VkBufferView> g_bufviews;

static bool buffer_format_ok(VkFormat f, bool storage) {
    static std::mutex mu;
    static std::map<VkFormat, VkFormatFeatureFlags> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(f);
    if (it == cache.end()) {
        VkFormatProperties p = {};
        vkGetPhysicalDeviceFormatProperties(device().physical(), f, &p);
        it = cache.emplace(f, p.bufferFeatures).first;
    }
    return (it->second & (storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)) != 0;
}
}  // namespace tex

VkBufferView make_buffer_view(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range, VkFormat format) {
    std::lock_guard<std::mutex> lk(g_bufview_mu);
    const BufViewKey key{buffer, offset, range, format};
    if (auto it = g_bufviews.find(key); it != g_bufviews.end()) return it->second;
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    ci.buffer = buffer;
    ci.format = format;
    ci.offset = offset;
    ci.range = range;
    VkBufferView v = VK_NULL_HANDLE;
    if (vkCreateBufferView(vkdev(), &ci, nullptr, &v) != VK_SUCCESS) v = VK_NULL_HANDLE;
    g_bufviews.emplace(key, v);   // a failure is remembered too (null), so it is not retried every draw
    return v;
}

void forget_buffer_views(VkBuffer buffer) {
    std::vector<VkBufferView> gone;
    {
        std::lock_guard<std::mutex> lk(g_bufview_mu);
        for (auto it = g_bufviews.lower_bound(BufViewKey{buffer, 0, 0, VK_FORMAT_UNDEFINED}); it != g_bufviews.end() && it->first.buffer == buffer;) {
            if (it->second) gone.push_back(it->second);
            it = g_bufviews.erase(it);
        }
    }
    if (!gone.empty())
        defer_destroy([gone]() { for (VkBufferView v : gone) vkDestroyBufferView(vkdev(), v, nullptr); });
}

VkBufferView tex_buffer_view(Texture* t, const TextureViewInfo* view, int component, VkFormat shader_format, bool storage,
                             uint64_t* gpu_address, uint64_t* bytes, VkFormat* format) {
    if (!t || !t->fmt || t->b.width <= 0) return VK_NULL_HANDLE;
    // A buffer fetch reads the texture's memory as a run of texels, whatever target the storage was built with (a texture view of BUFFER target,
    // or a 2D/3D texture the game left bound to a samplerBuffer slot): the run is the BUFFER texture's width, else the whole storage.
    const bool buffer_target = t->b.target == NVN_TEXTURE_TARGET_BUFFER;
    const FormatInfo* fi = t->fmt;
    if (view && view->has_format)
        if (const FormatInfo* vf = format_info(view->format)) fi = vf;
    VkFormat fmt = storage && shader_format != VK_FORMAT_UNDEFINED ? shader_format : fi->vk;
    if (fmt == VK_FORMAT_UNDEFINED) return VK_NULL_HANDLE;
    if (component != 0) {
        const VkFormat a = component == 1 ? float_alias(fmt) : component == 2 ? sint_alias(fmt) : uint_alias(fmt);
        if (a != VK_FORMAT_UNDEFINED) fmt = a;
    }
    if (!buffer_format_ok(fmt, storage)) return VK_NULL_HANDLE;
    uint64_t addr = 0;
    if (t->vpool) addr = t->vpool->gpu_base + t->voffset;
    else if (t->mem) addr = reinterpret_cast<uint64_t>(t->mem);
    if (!addr) return VK_NULL_HANDLE;
    const VkPhysicalDeviceLimits& lim = device().properties().limits;
    if (!fi->bytes) return VK_NULL_HANDLE;
    uint64_t texels = static_cast<uint64_t>(t->b.width);
    if (!buffer_target) {
        texels = t->storage_size / fi->bytes;
        if (!texels) texels = static_cast<uint64_t>(t->b.width) * static_cast<uint64_t>(std::max(1, t->b.height)) * static_cast<uint64_t>(std::max(1, t->b.depth));
    }
    uint64_t n = std::min<uint64_t>(texels, lim.maxTexelBufferElements);
    const uint64_t size = n * fi->bytes;
    BufRef br = resolve_buffer(addr, size);
    if (!br.ok) return VK_NULL_HANDLE;
    const VkDeviceSize align = std::max<VkDeviceSize>(1, lim.minTexelBufferOffsetAlignment);
    if (br.offset % align) return VK_NULL_HANDLE;
    const VkDeviceSize range = std::min<VkDeviceSize>(size, br.avail) / fi->bytes * fi->bytes;
    if (!range) return VK_NULL_HANDLE;
    if (gpu_address) *gpu_address = addr;
    if (bytes) *bytes = range;
    if (format) *format = fmt;
    return make_buffer_view(br.buffer, br.offset, range, fmt);
}

VkBufferView dummy_buffer_view(VkFormat format, bool storage) {
    if (format == VK_FORMAT_UNDEFINED || !buffer_format_ok(format, storage)) format = VK_FORMAT_R32_SFLOAT;
    return make_buffer_view(dummy_buffer(), 0, 65536 - 32, format);   // the last 32 bytes are the unset-attribute defaults
}

void buffer_views_forget(VkBuffer buffer) {
    std::lock_guard<std::mutex> lk(g_bufview_mu);
    for (auto it = g_bufviews.lower_bound(BufViewKey{buffer, 0, 0, VK_FORMAT_UNDEFINED}); it != g_bufviews.end() && it->first.buffer == buffer;) {
        if (it->second) vkDestroyBufferView(vkdev(), it->second, nullptr);
        it = g_bufviews.erase(it);
    }
}

}  // namespace gpu
}  // namespace nxrt::nvn::be
