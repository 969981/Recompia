// SPDX-License-Identifier: GPL-3.0-or-later
// Texture debug aids: PNG conversion, snapshots, texture dumps.
#include "nvn_gpu_texture_internal.h"

namespace nxrt::nvn::be {

namespace gpu {

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

}  // namespace gpu
}  // namespace nxrt::nvn::be
