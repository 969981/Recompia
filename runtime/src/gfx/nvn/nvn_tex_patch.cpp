// SPDX-License-Identifier: GPL-3.0-or-later
// Texture rectangle patch (nxrt/tex_patch.h): each level the rectangle covers is untiled, the rectangle's blocks are replaced, the level is
// tiled back into the game's storage and the texture is marked CPU-written (the registry's normal path to a re-upload).
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include <nxrt/tex_patch.h>

#include "astc2bc7.h"
#include "nvn_core.h"
#include "nvn_format.h"
#include "nvn_fnv.h"
#include "nvn_texture_layout.h"

namespace nxrt::gfx {
namespace {

using nvn::be::Texture;

std::mutex g_mu;   // one patch at a time (each rewrites whole levels)

Texture* bc7_texture(const void* guest) {
    Texture* t = nvn::be::host_of<Texture>(guest);
    if (!t || t->destroyed.load() || !t->mem || !t->block_linear || !t->fmt) return nullptr;
    if (t->fmt->nvn != NVN_FORMAT_BPTC_UNORM && t->fmt->nvn != NVN_FORMAT_BPTC_UNORM_SRGB) return nullptr;
    return t;
}

// The levels the level-0 rectangle (x, y, w, h) covers whole: the rectangle halves per level and must stay block aligned (4 texels).
uint32_t rect_levels(const Texture* t, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t n = 0;
    for (uint32_t l = 0; l < std::max(t->levels, 1u) && l < 16; ++l) {
        const uint32_t m = (4u << l) - 1;
        if ((x | y | w | h) & m) break;
        ++n;
    }
    return n;
}

// Runs fn(level's blocks as a linear row-major array, blocks per row) on one level and, when `write`, writes the level back.
template <class Fn> bool with_level(Texture* t, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool write, Fn fn) {
    if ((x | y | w | h) & 3u || w == 0 || h == 0) return false;
    const nvn::be::TexLayout tl = nvn::be::compute_texture_layout(t->b);
    if (!tl.valid || !tl.block_linear || level >= tl.layout.levels.size()) return false;
    const tegra::LevelLayout& lv = tl.layout.levels[level];
    if (x + w > lv.width || y + h > lv.height || lv.tiled_offset + lv.tiled_size > t->storage_size) return false;
    std::vector<uint8_t> lin(static_cast<size_t>(lv.linear_slice_size));
    std::lock_guard<std::mutex> lk(g_mu);
    uint8_t* tiled = t->mem + lv.tiled_offset;
    tegra::untile_level(tl.desc, lv, tiled, lin.data());
    fn(lin.data(), static_cast<size_t>(lv.linear_row_stride));
    if (!write) return true;
    tegra::tile_level(tl.desc, lv, lin.data(), tiled);
    return true;
}

void mark_written(Texture* t) {
    t->cpu_dirty.store(true);
    nvn::be::textures_cpu_wrote(t->mem, t->storage_size);
}

// Half size, alpha-weighted 2x2 box filter of straight-alpha RGBA.
std::vector<uint8_t> half(const std::vector<uint8_t>& src, uint32_t w, uint32_t h) {
    const uint32_t hw = w / 2, hh = h / 2;
    std::vector<uint8_t> out(static_cast<size_t>(hw) * hh * 4);
    for (uint32_t y = 0; y < hh; ++y)
        for (uint32_t x = 0; x < hw; ++x) {
            float c[3] = {0, 0, 0}, a = 0;
            for (uint32_t k = 0; k < 4; ++k) {
                const uint8_t* p = &src[(static_cast<size_t>(2 * y + k / 2) * w + 2 * x + k % 2) * 4];
                for (int ch = 0; ch < 3; ++ch) c[ch] += p[ch] * static_cast<float>(p[3]);
                a += p[3];
            }
            uint8_t* o = &out[(static_cast<size_t>(y) * hw + x) * 4];
            for (int ch = 0; ch < 3; ++ch) o[ch] = static_cast<uint8_t>(a > 0 ? std::min(255.f, c[ch] / a + 0.5f) : 0);
            o[3] = static_cast<uint8_t>(a / 4 + 0.5f);
        }
    return out;
}

}  // namespace

bool guest_texture_info(const void* guest, GuestTexInfo* out) {
    const Texture* t = nvn::be::host_of<Texture>(guest);
    if (!t || t->destroyed.load()) return false;
    out->id = t->id;
    out->width = static_cast<uint32_t>(t->b.width);
    out->height = static_cast<uint32_t>(t->b.height);
    out->levels = t->levels;
    out->layers = t->layers;
    out->bc7 = bc7_texture(guest) != nullptr;
    out->srgb = t->fmt && t->fmt->srgb;
    out->format = t->fmt ? static_cast<uint32_t>(t->fmt->nvn) : 0;
    return true;
}

namespace {

bool patch(Texture* t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba, std::vector<uint8_t>* saved) {
    if (!t || !rgba) return false;
    const uint32_t n = rect_levels(t, x, y, w, h);
    if (n == 0) return false;
    std::vector<uint8_t> img(rgba, rgba + static_cast<size_t>(w) * h * 4), old;
    bool ok = true;
    for (uint32_t l = 0; l < n && ok; ++l) {
        const uint32_t lw = w >> l, lh = h >> l, lx = x >> l, ly = y >> l;
        if (l) img = half(img, lw * 2, lh * 2);
        std::vector<uint8_t> enc(static_cast<size_t>(lw / 4) * (lh / 4) * 16);
        a2b_options opt{};
        opt.quality = A2B_DEFAULT_QUALITY;
        opt.threads = 1;
        opt.allow_opaque_mode6 = 0;
        if (a2b_encode_bc7(lw, lh, img.data(), &opt, enc.data(), enc.size(), nullptr) != A2B_OK) return false;
        const size_t row = static_cast<size_t>(lw / 4) * 16;
        ok = with_level(t, l, lx, ly, lw, lh, true, [&](uint8_t* lin, size_t stride) {
            for (uint32_t by = 0; by < lh / 4; ++by) {
                uint8_t* dst = lin + (ly / 4 + by) * stride + static_cast<size_t>(lx / 4) * 16;
                old.insert(old.end(), dst, dst + row);
                std::memcpy(dst, enc.data() + by * row, row);
            }
        });
    }
    if (saved) *saved = std::move(old);
    mark_written(t);
    return ok;
}

bool is_bc7(const Texture& t) {
    return !t.destroyed.load() && t.mem && t.block_linear && t.fmt && (t.fmt->nvn == NVN_FORMAT_BPTC_UNORM || t.fmt->nvn == NVN_FORMAT_BPTC_UNORM_SRGB);
}

bool put_blocks(Texture* t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* blocks, size_t size) {
    if (!t || !blocks) return false;
    const uint32_t n = rect_levels(t, x, y, w, h);
    size_t at = 0;
    bool ok = n != 0;
    for (uint32_t l = 0; l < n && ok; ++l) {
        const uint32_t lw = w >> l, lh = h >> l, lx = x >> l, ly = y >> l;
        const size_t row = static_cast<size_t>(lw / 4) * 16;
        if (at + row * (lh / 4) > size) break;
        ok = with_level(t, l, lx, ly, lw, lh, true, [&](uint8_t* lin, size_t stride) {
            for (uint32_t by = 0; by < lh / 4; ++by, at += row) std::memcpy(lin + (ly / 4 + by) * stride + static_cast<size_t>(lx / 4) * 16, blocks + at, row);
        });
    }
    mark_written(t);
    return ok;
}

// The live BC7 texture with this id, referenced (null when the game freed it).
nvn::be::Ref<Texture> by_id(uint32_t id) {
    auto pred = [](const Texture& t, void* u) { return t.id == *static_cast<const uint32_t*>(u) && is_bc7(t); };
    std::vector<nvn::be::Ref<Texture>> found = nvn::be::texture_registry_collect(pred, &id);
    return found.empty() ? nvn::be::Ref<Texture>() : std::move(found.front());
}

}  // namespace

bool texture_patch_bc7(const void* guest, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba, std::vector<uint8_t>* saved) {
    return patch(bc7_texture(guest), x, y, w, h, rgba, saved);
}

bool texture_put_bc7_blocks(const void* guest, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* blocks, size_t size) {
    return put_blocks(bc7_texture(guest), x, y, w, h, blocks, size);
}

bool texture_patch_bc7_by_id(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba, std::vector<uint8_t>* saved) {
    const nvn::be::Ref<Texture> t = by_id(id);
    return patch(t.get(), x, y, w, h, rgba, saved);
}

bool texture_put_bc7_blocks_by_id(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* blocks, size_t size) {
    const nvn::be::Ref<Texture> t = by_id(id);
    return put_blocks(t.get(), x, y, w, h, blocks, size);
}

size_t texture_registry_count() { return nvn::be::texture_registry_size(); }

std::vector<Bc7Found> texture_find_bc7(const Bc7Match* m, size_t n, std::vector<uint32_t>* checked) {
    struct Want { const Bc7Match* m; size_t n; const std::vector<uint32_t>* skip; } want{m, n, checked};
    auto pred = [](const Texture& t, void* u) {
        const Want& wt = *static_cast<const Want*>(u);
        if (!is_bc7(t)) return false;
        bool size = false;
        for (size_t i = 0; i < wt.n && !size; ++i)
            size = static_cast<uint32_t>(t.b.width) == wt.m[i].width && static_cast<uint32_t>(t.b.height) == wt.m[i].height;
        if (!size) return false;
        if (!(t.prep_flags.load() & 1)) return false;   // not drawn yet: the game may still be filling it (looked at once it was uploaded)
        return !wt.skip || std::find(wt.skip->begin(), wt.skip->end(), t.id) == wt.skip->end();
    };
    std::vector<Bc7Found> out;
    for (const nvn::be::Ref<Texture>& r : nvn::be::texture_registry_collect(pred, &want)) {
        Texture* t = r.get();
        if (checked) checked->push_back(t->id);
        const uint32_t tw = static_cast<uint32_t>(t->b.width), th = static_cast<uint32_t>(t->b.height);
        // level 0 is untiled once; every match of this size is hashed from it
        with_level(t, 0, 0, 0, 4, 4, false, [&](uint8_t* lin, size_t stride) {
            for (size_t i = 0; i < n; ++i) {
                const Bc7Match& mi = m[i];
                if (tw != mi.width || th != mi.height || ((mi.x | mi.y | mi.w | mi.h) & 3u) || mi.x + mi.w > tw || mi.y + mi.h > th) continue;
                uint64_t hash = nvn::be::kFnvBasis;   // FNV-1a 64 of the rectangle's blocks, row by row
                for (uint32_t by = 0; by < mi.h / 4; ++by) {
                    const uint8_t* row = lin + (mi.y / 4 + by) * stride + static_cast<size_t>(mi.x / 4) * 16;
                    for (size_t k = 0; k < static_cast<size_t>(mi.w / 4) * 16; ++k) hash = nvn::be::fnv_step(hash, row[k]);
                }
                if (hash == mi.hash) {
                    out.push_back({t->id, static_cast<uint32_t>(i)});
                    return;
                }
            }
        });
    }
    return out;
}

bool texture_read_bc7_level(const void* guest, uint32_t level, std::vector<uint8_t>* blocks, uint32_t* width, uint32_t* height) {
    Texture* t = bc7_texture(guest);
    if (!t || !blocks) return false;
    const nvn::be::TexLayout tl = nvn::be::compute_texture_layout(t->b);
    if (!tl.valid || !tl.block_linear || level >= tl.layout.levels.size()) return false;
    const tegra::LevelLayout& lv = tl.layout.levels[level];
    const uint32_t lw = lv.width & ~3u, lh = lv.height & ~3u;
    if (!lw || !lh) return false;
    blocks->clear();
    const size_t row = static_cast<size_t>(lw / 4) * 16;
    const bool ok = with_level(t, level, 0, 0, lw, lh, false, [&](uint8_t* lin, size_t stride) {
        for (uint32_t by = 0; by < lh / 4; ++by) blocks->insert(blocks->end(), lin + by * stride, lin + by * stride + row);
    });
    if (width) *width = lw;
    if (height) *height = lh;
    return ok;
}

}  // namespace nxrt::gfx
