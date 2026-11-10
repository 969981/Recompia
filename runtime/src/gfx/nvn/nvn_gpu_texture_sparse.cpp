// SPDX-License-Identifier: GPL-3.0-or-later
// Sparse texture images: tile binding on demand, sparse creation, per-frame uploads of the touched tiles.
#include "nvn_gpu_texture_internal.h"

namespace nxrt::nvn::be {

namespace gpu {

namespace tex {
// ---- sparse residency images --------------------------------------------------------------------------------------------------
constexpr uint64_t kGuestTile = 0x10000;   // NVN sparse tile: 64 KiB of storage

// A level whose 64 KiB pieces of storage are each a whole run of GOB blocks of one block row, so that a piece is a rectangle of texels.
static bool sparse_level_chunked(const SparseState& sp, uint32_t level) {
    const TexLayout& L = sp.L;
    if (level >= L.layout.levels.size() || level >= sp.tail_first) return false;
    const tegra::LevelLayout& lv = L.layout.levels[level];
    const uint64_t bpb = L.desc.bytes_per_block, row = static_cast<uint64_t>(lv.blocks_x) * bpb;
    const uint64_t block_bytes = 512ull * lv.gob_blocks_y;
    if (row % 64 || kGuestTile % block_bytes || lv.gob_blocks_z != 1) return false;
    const uint64_t per = kGuestTile / block_bytes, gob_cols = row / 64;
    return gob_cols % per == 0 && lv.blocks_y % (8ull * lv.gob_blocks_y) == 0 && lv.tiled_offset % kGuestTile == 0 && lv.tiled_size % kGuestTile == 0 &&
           L.layout.tiled_layer_stride % kGuestTile == 0;
}

static bool sparse_alloc(SparseState& sp, VkDeviceSize bytes, VkDeviceMemory* mem, VkDeviceSize* off) {
    bytes = (bytes + sp.page - 1) / sp.page * sp.page;
    if (sp.chunks.empty() || sp.chunk_used + bytes > sp.chunk_size) {
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = std::max<VkDeviceSize>(sp.chunk_size, bytes);
        ai.memoryTypeIndex = sp.mem_type;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (vkAllocateMemory(vkdev(), &ai, nullptr, &m) != VK_SUCCESS) return false;
        sp.chunks.push_back(m);
        sp.chunk_used = 0;
    }
    *mem = sp.chunks.back();
    *off = sp.chunk_used;
    sp.chunk_used += bytes;
    sp.bytes += bytes;
    return true;
}

// Binds the block (bx, by) of (layer, level) when it is not bound yet.
static bool sparse_bind_block(TexGpu& g, uint32_t layer, uint32_t level, uint32_t bx, uint32_t by) {
    SparseState& sp = *g.sparse;
    const uint64_t key = (static_cast<uint64_t>(layer) << 48) | (static_cast<uint64_t>(level) << 40) | (static_cast<uint64_t>(by) << 20) | bx;
    if (sp.bound.count(key)) return true;
    VkDeviceMemory m;
    VkDeviceSize off;
    if (!sparse_alloc(sp, sp.page, &m, &off)) return false;
    const uint32_t lw = std::max(1u, g.base.width >> level), lh = std::max(1u, g.base.height >> level);
    VkSparseImageMemoryBind b = {};
    b.subresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer};
    b.offset = {static_cast<int32_t>(bx * sp.gran.width), static_cast<int32_t>(by * sp.gran.height), 0};
    b.extent = {std::min(sp.gran.width, lw - bx * sp.gran.width), std::min(sp.gran.height, lh - by * sp.gran.height), 1};
    b.memory = m;
    b.memoryOffset = off;
    sp.binds.push_back(b);
    sp.bound.insert(key);
    return true;
}

// Binds the texel rectangle of (layer, level).
static bool sparse_bind_rect(TexGpu& g, uint32_t layer, uint32_t level, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    const SparseState& sp = *g.sparse;
    for (uint32_t by = y0 / sp.gran.height; by <= (y0 + h - 1) / sp.gran.height; ++by)
        for (uint32_t bx = x0 / sp.gran.width; bx <= (x0 + w - 1) / sp.gran.width; ++bx)
            if (!sparse_bind_block(g, layer, level, bx, by)) return false;
    return true;
}

// Hands the waiting binds to the queue and waits for them (binds are rare: new blocks only).
static bool sparse_flush_binds(TexGpu& g) {
    SparseState& sp = *g.sparse;
    if (sp.binds.empty() && sp.opaque.empty()) return true;
    VkSparseImageMemoryBindInfo ib = {g.img.image, static_cast<uint32_t>(sp.binds.size()), sp.binds.data()};
    VkSparseImageOpaqueMemoryBindInfo ob = {g.img.image, static_cast<uint32_t>(sp.opaque.size()), sp.opaque.data()};
    VkBindSparseInfo bi = {VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    if (!sp.binds.empty()) { bi.imageBindCount = 1; bi.pImageBinds = &ib; }
    if (!sp.opaque.empty()) { bi.imageOpaqueBindCount = 1; bi.pImageOpaqueBinds = &ob; }
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(vkdev(), &fi, nullptr, &fence) != VK_SUCCESS) return false;
    if (on_render_thread()) submit_recorded();     // binds come after the submissions recorded before them, as without a batch
    submit_drain();                                // [nvn] submit_thread: ... and after the worker has put them on the queue
    VkResult r;
    {
        std::lock_guard<std::mutex> lk(device().queue_mutex());
        r = vkQueueBindSparse(device().queue(), 1, &bi, fence);
    }
    if (r == VK_SUCCESS) r = vkWaitForFences(vkdev(), 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(vkdev(), fence, nullptr);
    sp.binds.clear();
    sp.opaque.clear();
    if (r != VK_SUCCESS) NXLOG_E(kTag, "vkQueueBindSparse failed (%d)", static_cast<int>(r));
    return r == VK_SUCCESS;
}

bool create_sparse_texture(Texture* t, TexGpu* g, const Shape& s, std::string* why) {
    const gfx::DeviceFeatures& ft = device().features();
    const bool to_bc7 = g->fmt->astc && !ft.astc_ldr;
    const VkFormat fmt = to_bc7 ? (g->fmt->srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK) : g->fmt->vk;
    if (fmt == VK_FORMAT_UNDEFINED) { *why = "no Vulkan format"; return false; }
    auto sp = std::make_unique<SparseState>();
    sp->L = compute_texture_layout(t->b);
    if (!sp->L.valid || !sp->L.block_linear || sp->L.layout.levels.size() < s.levels) { *why = "no block-linear layout"; return false; }

    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = base_flags(s, fmt) | VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {s.base.width, s.base.height, 1};
    ci.mipLevels = s.levels;
    ci.arrayLayers = s.layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    VkResult r = vkCreateImage(vkdev(), &ci, nullptr, &img);
    if (r != VK_SUCCESS) { *why = "vkCreateImage " + std::to_string(r); return false; }

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vkdev(), img, &mr);
    uint32_t n = 0;
    vkGetImageSparseMemoryRequirements(vkdev(), img, &n, nullptr);
    std::vector<VkSparseImageMemoryRequirements> sr(n);
    vkGetImageSparseMemoryRequirements(vkdev(), img, &n, sr.data());
    const VkSparseImageMemoryRequirements* req = nullptr;
    for (const auto& q : sr)
        if (q.formatProperties.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) req = &q;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(device().physical(), &mp);
    int type = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type < 0; ++i)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = static_cast<int>(i);
    if (!req || type < 0 || (req->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT)) {
        vkDestroyImage(vkdev(), img, nullptr);
        *why = !req ? "no colour sparse requirements" : type < 0 ? "no device-local memory type" : "non-standard sparse block size";
        return false;
    }
    sp->page = mr.alignment;
    sp->mem_type = static_cast<uint32_t>(type);
    sp->chunk_size = std::max<VkDeviceSize>(64ull << 20, sp->page);
    sp->gran = req->formatProperties.imageGranularity;
    sp->tail_first = std::min(req->imageMipTailFirstLod, s.levels);
    sp->tail_size = req->imageMipTailSize;
    sp->tail_offset = req->imageMipTailOffset;
    sp->tail_stride = req->imageMipTailStride;
    sp->single_tail = (req->formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0;

    g->img.image = img;
    g->img.format = fmt;
    g->img.extent = ci.extent;
    g->img.mip_levels = s.levels;
    g->img.array_layers = s.layers;
    g->format = fmt;
    g->transcoded = to_bc7;
    g->mutable_fmt = srgb_partner(fmt) != VK_FORMAT_UNDEFINED || bit_alias_class(fmt);
    g->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    g->usage = ci.usage;
    g->sparse = sp.release();
    SparseState& st = *g->sparse;

    // always resident: the mip tail and the levels that are not cut into whole-rectangle guest tiles
    bool ok = true;
    if (st.tail_first < s.levels && st.tail_size) {
        const uint32_t tails = st.single_tail ? 1 : s.layers;
        for (uint32_t l = 0; l < tails && ok; ++l) {
            VkDeviceMemory m;
            VkDeviceSize off;
            ok = sparse_alloc(st, st.tail_size, &m, &off);
            if (ok) st.opaque.push_back({st.tail_offset + l * st.tail_stride, st.tail_size, m, off, 0});
        }
    }
    for (uint32_t lvl = 0; lvl < st.tail_first && ok; ++lvl) {
        if (sparse_level_chunked(st, lvl)) continue;
        const uint32_t lw = std::max(1u, s.base.width >> lvl), lh = std::max(1u, s.base.height >> lvl);
        for (uint32_t l = 0; l < s.layers && ok; ++l) ok = sparse_bind_rect(*g, l, lvl, 0, 0, lw, lh);
    }
    if (ok) ok = sparse_flush_binds(*g);
    std::string chunked;
    for (uint32_t lvl = 0; lvl < st.tail_first; ++lvl) chunked += sparse_level_chunked(st, lvl) ? "T" : "w";
    {
        std::string lay;
        char buf[96];
        for (uint32_t lvl = 0; lvl < s.levels && lvl < st.L.layout.levels.size(); ++lvl) {
            const tegra::LevelLayout& lv = st.L.layout.levels[lvl];
            snprintf(buf, sizeof buf, " L%u@%llx+%llx gob%u %ux%u", lvl, static_cast<unsigned long long>(lv.tiled_offset), static_cast<unsigned long long>(lv.tiled_size),
                     lv.gob_blocks_y, lv.blocks_x, lv.blocks_y);
            lay += buf;
        }
        NXLOG_I(kTag, "texture %u: guest layout: layer stride %llx, storage %llx,%s", t->id, static_cast<unsigned long long>(st.L.layout.tiled_layer_stride),
                static_cast<unsigned long long>(t->storage_size), lay.c_str());
    }
    NXLOG_I(kTag, "texture %u: %s %ux%u x%u layers, %u levels as a sparse-residency image (block %ux%u, %llu KB; tail from level %u, %llu KB%s; "
            "levels %s: T by guest tile, w whole); %llu MB bound at creation%s", t->id, g->fmt->name, s.base.width, s.base.height, s.layers, s.levels,
            st.gran.width, st.gran.height, static_cast<unsigned long long>(st.page >> 10), st.tail_first, static_cast<unsigned long long>(st.tail_size >> 10),
            st.single_tail ? " single" : " per layer", chunked.c_str(), static_cast<unsigned long long>(st.bytes >> 20), ok ? "" : " (FAILED)");
    if (!ok) { *why = "binding the always-resident levels failed"; return false; }
    g_live_images.fetch_add(1);
    counters().textures_created.fetch_add(1);
    return true;
}

// ---- sparse residency: uploads --------------------------------------------------------------------------------------------------
// The dirty ranges of a sparse texture's storage are uploaded tile by tile: a 64 KiB guest tile of a tiled level is a rectangle of texels
// (a run of GOB blocks of one block row); its host blocks are bound, it is untiled (and transcoded to BC7 for ASTC) and copied in. Levels
// that are not tiled that way (small levels and the host mip tail) are uploaded whole per layer. At most [nvn] sparse_tiles_per_frame
// tiles go per call; the rest stays dirty for the next frame.
void sparse_update(Texture* t, TexGpu& g, Frame& f) {
    SparseState& sp = *g.sparse;
    const TexLayout& L = sp.L;
    const uint64_t stride = L.layout.tiled_layer_stride;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    {
        std::lock_guard<std::mutex> lk(t->sparse_mu);
        if (t->sparse_all) {
            t->sparse_all = false;
            t->sparse_dirty.clear();
            ranges.emplace_back(0, t->storage_size);
        } else {
            ranges.swap(t->sparse_dirty);
        }
    }
    if (ranges.empty()) return;
    if (g.gpu_written) return;   // the GPU fills it (copies into the cache): the pool bytes are not the content, its tiles are bound by the copies
    std::sort(ranges.begin(), ranges.end());

    // the tiles and whole (layer, level) pieces the ranges touch
    std::set<uint64_t> tiles;                           // storage offsets of guest tiles
    std::set<std::pair<uint32_t, uint32_t>> whole;      // (layer, level)
    for (const auto& r : ranges) {
        uint64_t a = r.first, b = std::min(r.second, t->storage_size);
        while (a < b) {
            const uint32_t layer = static_cast<uint32_t>(a / stride);
            if (layer >= g.layers) break;
            const uint64_t in = a - static_cast<uint64_t>(layer) * stride;
            uint32_t level = 0;
            while (level + 1 < g.levels && in >= L.layout.levels[level + 1].tiled_offset) ++level;
            const tegra::LevelLayout& lv = L.layout.levels[level];
            const uint64_t lvl_end = static_cast<uint64_t>(layer) * stride + lv.tiled_offset + lv.tiled_size;
            if (a >= lvl_end) {   // padding after the level: the next level, or the next layer
                a = level + 1 < g.levels ? static_cast<uint64_t>(layer) * stride + L.layout.levels[level + 1].tiled_offset : static_cast<uint64_t>(layer + 1) * stride;
                continue;
            }
            if (sparse_level_chunked(sp, level)) {
                const uint64_t tile = a / kGuestTile * kGuestTile;
                tiles.insert(tile);
                a = tile + kGuestTile;
            } else {
                whole.insert({layer, level});
                a = lvl_end;
            }
        }
    }

    static const int64_t max_tiles = std::max<int64_t>(1, config_get_int("nvn", "sparse_tiles_per_frame", 256));
    std::vector<std::pair<uint64_t, uint64_t>> later;
    a2b_options opt{};
    opt.quality = static_cast<int>(config_get_int("nvn", "sparse_bc7_quality", 1));
    opt.threads = 0;
    a2b_input in{};
    in.format = g.fmt->srgb ? A2B_ASTC_SRGB : A2B_ASTC_UNORM;
    in.block_w = L.desc.block_w;
    in.block_h = L.desc.block_h;
    const uint32_t bw = L.desc.block_w, bh = L.desc.block_h, bpb = L.desc.bytes_per_block;

    struct Up { UploadAlloc st; VkBufferImageCopy r; };
    std::vector<Up> ups;
    std::vector<uint8_t> raw(kGuestTile), lin, out;
    uint64_t staged = 0;
    static const uint64_t max_staged = static_cast<uint64_t>(std::max<int64_t>(1, config_get_int("nvn", "sparse_upload_mb", 48))) << 20;
    // Untiles (and transcodes) a block-linear piece of w x h texels (whole blocks) with block height gby; the result goes to a staging allocation.
    auto convert = [&](const uint8_t* tiled, uint32_t w, uint32_t h, uint32_t gby, uint32_t level, uint32_t layer, uint32_t x0, uint32_t y0,
                       uint32_t cw, uint32_t ch) -> bool {
        tegra::TextureDesc d = L.desc;
        d.width = w;
        d.height = h;
        d.depth = 1;
        d.levels = 1;
        d.layers = 1;
        d.gob_blocks_y = gby;
        d.gob_blocks_z = 1;
        d.linear_stride_align = 1;
        const tegra::Layout dl = tegra::compute_layout(d);
        if (!dl.valid || dl.levels.empty()) return false;
        lin.assign(static_cast<size_t>(dl.linear_size), 0);
        tegra::untile_level(d, dl.levels[0], tiled, lin.data());
        const uint8_t* data = lin.data();
        uint64_t bytes = lin.size();
        if (g.transcoded) {
            in.width = w;
            in.height = h;
            out.resize(static_cast<size_t>(a2b_output_size(&in)));
            a2b_format of;
            if (a2b_transcode(&in, lin.data(), lin.size(), &opt, &of, out.data(), out.size(), nullptr) != A2B_OK) return false;
            data = out.data();
            bytes = out.size();
        }
        staged += bytes;
        UploadAlloc st = f.staging(bytes);
        if (!st) return false;
        std::memcpy(st.ptr, data, static_cast<size_t>(bytes));
        VkBufferImageCopy r = {};
        r.bufferOffset = st.offset;
        r.bufferRowLength = w;
        r.bufferImageHeight = h;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
        r.imageOffset = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), 0};
        r.imageExtent = {cw, ch, 1};
        ups.push_back({st, r});
        return true;
    };

    // A physical tile mapped at many places of the cache is the game's filler for unused slots (nothing samples them through the page
    // tables): those places stay non-resident (they read as zero) instead of each getting memory and an upload.
    std::vector<const uint8_t*> ptrs;
    texture_virtual_tile_ptrs(*t, ptrs);
    std::unordered_map<const uint8_t*, uint32_t> uses;
    for (const uint8_t* p : ptrs)
        if (p) ++uses[p];
    static const uint32_t filler_min = static_cast<uint32_t>(std::max<int64_t>(2, config_get_int("nvn", "sparse_filler_min", 4)));
    uint64_t skipped_filler = 0, skipped_zero = 0;

    int64_t done = 0;
    uint64_t skipped_unmapped = 0, failed = 0;
    for (uint64_t off : tiles) {
        {
            const size_t ti = static_cast<size_t>(off / kGuestTile);
            const uint8_t* p = ti < ptrs.size() ? ptrs[ti] : nullptr;
            if (!p) { ++skipped_unmapped; continue; }   // not mapped: nothing to read
            if (uses[p] >= filler_min) { ++skipped_filler; continue; }
        }
        if (done >= max_tiles || staged >= max_staged) { later.emplace_back(off, off + kGuestTile); continue; }
        const uint32_t layer = static_cast<uint32_t>(off / stride);
        const uint64_t in_layer = off - static_cast<uint64_t>(layer) * stride;
        uint32_t level = 0;
        while (level + 1 < g.levels && in_layer >= L.layout.levels[level + 1].tiled_offset) ++level;
        const tegra::LevelLayout& lv = L.layout.levels[level];
        if (!texture_read_virtual(*t, off, kGuestTile, raw.data())) { ++skipped_unmapped; continue; }
        {   // a mapped tile the game has not filled yet is zeros (an ASTC error block: magenta once decoded): leave it unbound
            const uint64_t* q = reinterpret_cast<const uint64_t*>(raw.data());
            bool zero = true;
            for (size_t i = 0; i < kGuestTile / 8 && zero; ++i) zero = q[i] == 0;
            if (zero) {
                ++skipped_zero;
                continue;
            }
        }
        // the tile's rectangle: tiles run along a block row of the level
        const uint64_t block_bytes = 512ull * lv.gob_blocks_y, per = kGuestTile / block_bytes;
        const uint64_t gob_cols = static_cast<uint64_t>(lv.blocks_x) * bpb / 64;
        const uint64_t first = (in_layer - lv.tiled_offset) / block_bytes;
        const uint64_t brow = first / gob_cols, bcol = first % gob_cols;
        const uint32_t nbx = static_cast<uint32_t>(per * 64 / bpb), nby = 8u * lv.gob_blocks_y;
        const uint32_t x0 = static_cast<uint32_t>(bcol * 64 / bpb) * bw, y0 = static_cast<uint32_t>(brow) * nby * bh;
        const uint32_t w = nbx * bw, h = nby * bh;
        const uint32_t lw = std::max(1u, g.base.width >> level), lh = std::max(1u, g.base.height >> level);
        if (x0 >= lw || y0 >= lh) continue;
        const uint32_t cw = std::min(w, lw - x0), ch = std::min(h, lh - y0);
        if (!sparse_bind_rect(g, layer, level, x0, y0, cw, ch) || !convert(raw.data(), w, h, lv.gob_blocks_y, level, layer, x0, y0, cw, ch)) {
            ++failed;
            continue;
        }
        ++done;
    }
    std::vector<uint8_t> lvbuf;
    for (const auto& wl : whole) {
        const uint32_t layer = wl.first, level = wl.second;
        const tegra::LevelLayout& lv = L.layout.levels[level];
        lvbuf.assign(static_cast<size_t>(lv.tiled_size), 0);
        texture_read_virtual(*t, static_cast<uint64_t>(layer) * stride + lv.tiled_offset, lv.tiled_size, lvbuf.data());   // unmapped parts are zeros
        const uint32_t w = lv.blocks_x * bw, h = lv.blocks_y * bh;
        const uint32_t lw = std::max(1u, g.base.width >> level), lh = std::max(1u, g.base.height >> level);
        if (!convert(lvbuf.data(), w, h, lv.gob_blocks_y, level, layer, 0, 0, std::min(w, lw), std::min(h, lh))) ++failed;
    }
    if (!sparse_flush_binds(g)) return;
    if (!ups.empty()) {
        move_layout(f, g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (const Up& u : ups) vkCmdCopyBufferToImage(f.cb, u.st.buffer, g.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &u.r);
        move_layout(f, g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else if (g.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        move_layout(f, g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    sp.tiles_uploaded += static_cast<uint64_t>(done);
    g.uploaded = true;
    if (!later.empty()) {
        std::lock_guard<std::mutex> lk(t->sparse_mu);
        t->sparse_dirty.insert(t->sparse_dirty.end(), later.begin(), later.end());
        t->cpu_dirty.store(true);
    }
    static int logged = 0;
    if (logged++ < 40 || failed || logged % 50 == 0) {
        NXLOG_I(kTag, "texture %u: sparse upload: %lld tiles (%llu total), %zu whole levels, %llu unmapped, %llu zero and %llu filler skipped, %llu failed, %zu left; %llu MB bound",
                t->id, static_cast<long long>(done), static_cast<unsigned long long>(sp.tiles_uploaded), whole.size(),
                static_cast<unsigned long long>(skipped_unmapped), static_cast<unsigned long long>(skipped_zero), static_cast<unsigned long long>(skipped_filler), static_cast<unsigned long long>(failed), later.size(),
                static_cast<unsigned long long>(sp.bytes >> 20));
    }
}

// Before a GPU copy into level `level` of layer `layer` of a sparse texture: binds the host blocks of the guest tiles the game mapped
// there (a copy lands only on mapped tiles; on unbound blocks it is dropped, as on the console's unmapped pages).
void sparse_bind_for_copy(Texture* t, TexGpu& g, uint32_t layer, uint32_t level) {
    SparseState& sp = *g.sparse;
    const TexLayout& L = sp.L;
    if (layer >= g.layers || level >= g.levels || !sparse_level_chunked(sp, level)) return;   // other levels are bound at creation
    std::vector<const uint8_t*> ptrs;
    texture_virtual_tile_ptrs(*t, ptrs);
    const tegra::LevelLayout& lv = L.layout.levels[level];
    const uint32_t bw = L.desc.block_w, bh = L.desc.block_h, bpb = L.desc.bytes_per_block;
    const uint64_t base = static_cast<uint64_t>(layer) * L.layout.tiled_layer_stride + lv.tiled_offset;
    const uint64_t block_bytes = 512ull * lv.gob_blocks_y, per = kGuestTile / block_bytes;
    const uint64_t gob_cols = static_cast<uint64_t>(lv.blocks_x) * bpb / 64;
    const uint32_t nbx = static_cast<uint32_t>(per * 64 / bpb), nby = 8u * lv.gob_blocks_y;
    const uint32_t lw = std::max(1u, g.base.width >> level), lh = std::max(1u, g.base.height >> level);
    uint32_t mapped = 0;
    for (uint64_t off = base; off < base + lv.tiled_size; off += kGuestTile) {
        const size_t ti = static_cast<size_t>(off / kGuestTile);
        if (ti >= ptrs.size() || !ptrs[ti]) continue;
        const uint64_t first = (off - base) / block_bytes;
        const uint32_t x0 = static_cast<uint32_t>((first % gob_cols) * 64 / bpb) * bw, y0 = static_cast<uint32_t>(first / gob_cols) * nby * bh;
        if (x0 >= lw || y0 >= lh) continue;
        sparse_bind_rect(g, layer, level, x0, y0, std::min(nbx * bw, lw - x0), std::min(nby * bh, lh - y0));
        ++mapped;
    }
    sparse_flush_binds(g);
    static int logged = 0;
    if (logged < 12) {
        ++logged;
        NXLOG_I(kTag, "texture %u: copy into layer %u level %u: %u mapped tiles of %llu bound (%zu tile pointers); %llu MB bound", t->id, layer, level, mapped,
                static_cast<unsigned long long>(lv.tiled_size / kGuestTile), ptrs.size(), static_cast<unsigned long long>(sp.bytes >> 20));
    }
}
}  // namespace tex

}  // namespace gpu
}  // namespace nxrt::nvn::be
