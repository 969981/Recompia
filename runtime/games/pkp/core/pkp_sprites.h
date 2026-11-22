// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the game's UI sprites (main 2.0.1a) and the atlas texture behind one. Used by the prompt glyphs (input/pkp_kbm.cpp) and by
// the icons of our Settings tabs (ui/pkp_options_icons.cpp).
//
// A sprite sheet (0x9125e0 from a u16 sheet id in guest memory) holds 88-byte sprite records: the name (64 bytes), the size at +66 / +68,
// the sprite id at +72 and the quad at +80, whose four corner UVs (u, v floats: top left, top right, bottom left, bottom right) are at
// +24. One sheet can span several textures (sheet 20, the UI icons, spans five). The icon table (data table at +0x10b0, ids 1..500)
// names a sheet and a sprite per icon id; 0xa7c05c(pane holder, slot, icon id, 0, 0) sets a picture pane from it, which copies the
// sprite's UVs into the pane's own texture record (layout::kPaneTexture).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/pkp_guest.h"
#include "core/pkp_tables.h"
#include "nxrt/memory.h"
#include "nxrt/tex_patch.h"

namespace nxrt::games::pkp::sprites {

constexpr uint64_t kSheet = 0x9125e0;             // (u16* sheet id) -> the sprite sheet
constexpr uint64_t kSheetSprite = 0x24ec2c0;      // (sheet, sprite id) -> the 88-byte sprite record, 0 if the sheet has none
constexpr uint64_t kSetIcon = 0xa7c05c;           // (pane holder, slot, icon id, 0, 0): a picture pane shows that icon
constexpr uint64_t kIconTable = 0x10b0;           // data table: icon id -> row, [row + 8] = {u32 sprite id, u16 sheet id}
constexpr uint32_t kIconMax = 500;
constexpr uint64_t kSpriteQuad = 80, kQuadUv = 24;
constexpr uint32_t kGuestMagic = 0x4e585433;      // "NXT3": the runtime's header in the game's NVN objects (+8)

// The game's NVNtexture reachable from a sprite record (BC7 only): a breadth-first walk over the pointers it holds (the runtime's
// header names the texture). 0 if none.
inline uint64_t texture_of(uint64_t root) {
    std::vector<uint64_t> todo{root}, seen;
    for (size_t i = 0; i < todo.size() && i < 256; ++i) {
        const uint64_t node = todo[i];
        if (std::find(seen.begin(), seen.end(), node) != seen.end()) continue;
        seen.push_back(node);
        if (!mem::is_mapped(node, 0x110)) continue;
        for (uint64_t off = 0; off < 0x100; off += 8) {
            if (guest::rd<uint32_t>(node + off + 8) == kGuestMagic) {
                gfx::GuestTexInfo ti;
                if (gfx::guest_texture_info(reinterpret_cast<const void*>(node + off), &ti) && ti.bc7) return node + off;
            }
            const uint64_t p = guest::rd<uint64_t>(node + off);
            if (p >= mem::kGuestBase && p < mem::kLayoutEnd && (p & 7) == 0 && todo.size() < 512) todo.push_back(p);
        }
    }
    return 0;
}

// A sprite's rectangle in texels of a w x h texture, from its quad's UVs. False when the quad cannot be read.
struct Rect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};
inline bool rect_of(uint64_t rec, uint32_t tex_w, uint32_t tex_h, Rect* out) {
    const uint64_t quad = guest::ok(rec + kSpriteQuad, 8) ? guest::rd<uint64_t>(rec + kSpriteQuad) : 0;
    if (!quad || !guest::ok(quad + kQuadUv, 32)) return false;
    float u0 = 1, v0 = 1, u1 = 0, v1 = 0;
    for (int k = 0; k < 4; ++k) {
        const float u = guest::rd<float>(quad + kQuadUv + 8 * k), v = guest::rd<float>(quad + kQuadUv + 4 + 8 * k);
        u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
    }
    out->x = static_cast<uint32_t>(std::lround(u0 * tex_w));
    out->y = static_cast<uint32_t>(std::lround(v0 * tex_h));
    out->w = static_cast<uint32_t>(std::lround(u1 * tex_w)) - out->x;
    out->h = static_cast<uint32_t>(std::lround(v1 * tex_h)) - out->y;
    return true;
}

// The sprite record of icon `id` (the icon table, as 0xa7c05c reads it); 0 while the table or the sheet is not loaded. `scratch` is 16
// bytes of guest memory the call may use (the sheet lookup takes the id by pointer), `sp` a stack pointer below it.
inline uint64_t icon_sprite(::Ctx* c, uint64_t base, uint32_t id, uint64_t scratch, uint64_t sp) {
    if (id - 1 >= kIconMax) return 0;
    const uint64_t row = tables::row(c, base, kIconTable, id);
    const uint64_t icon = row && guest::ok(row + 8, 8) ? guest::rd<uint64_t>(row + 8) : 0;
    if (!icon || !guest::ok(icon, 8)) return 0;
    guest::wr<uint32_t>(scratch, guest::rd<uint16_t>(icon + 4));
    const uint64_t sheet = guest::call(c, base + kSheet, scratch, 0, 0, 0, 0, sp);
    const uint64_t rec = sheet ? guest::call(c, base + kSheetSprite, sheet, guest::rd<uint32_t>(icon), 0, 0, 0, sp) : 0;
    return rec && guest::ok(rec, 88) ? rec : 0;
}

}  // namespace nxrt::games::pkp::sprites
