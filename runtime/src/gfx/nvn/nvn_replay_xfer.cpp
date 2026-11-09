// SPDX-License-Identifier: GPL-3.0-or-later
// Clears and transfers of the replay: colour/depth/texture clears, buffer-texture and texture copies, downsample, GPU-ordered memory writes.
#include "nvn_replay_internal.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
MaskClear g_mask;
bool g_mask_ready = false, g_mask_ok = false;

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
            g_cmd.bound_pipeline = VK_NULL_HANDLE;
            g_cmd.dirty = kAllDirty;
            g_cmd.topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;   // binding a pipeline without these dynamic states invalidates them
            g_cmd.restart_valid = false;
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
            g_cmd.bound_pipeline = VK_NULL_HANDLE;
            g_cmd.dirty = kAllDirty;
            g_cmd.topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;   // binding a pipeline without these dynamic states invalidates them
            g_cmd.restart_valid = false;
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
    if (tex_scale(s) != tex_scale(d)) log_once(kOnceDownsampleScale, "Downsample between a scaled and an unscaled texture: only the common part is resolved");
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

}  // namespace replay

}  // namespace nxrt::nvn::be::gpu
