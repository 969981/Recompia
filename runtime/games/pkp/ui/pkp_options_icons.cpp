// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options: the icons of our tabs in the tab bar (pkp_options_internal.h). Offsets in main of the 2.0.1a update.
//
// A tab cell (class 0x49) shows its icon in two picture panes: slot 5 (72 x 72) while the tab is selected, slot 6 (48 x 48) while it is
// not. The game's three set them from icon pairs of the icon table (Game 0x88 / 0x87: "icon_itemcategory_41_on" / "..._41", Camera
// 0x8a / 0x89, Sound 0x8e / 0x8d), 72 x 72 sprites of the UI icon sheet in its 2048 x 2048 BC7 atlas. The romfs has no sprites for our
// tabs, so:
//   * our icons (tools/settings_tab_icons.py: Font Awesome Free glyphs in the look of the game's pairs) are painted into a strip of that
//     atlas no sprite uses, (0, 1920) to (1920, 2048): one row of 24 cells of 72 x 72 at a pitch of 80, each icon's selected look and then
//     its other one. The strip is one rectangle aligned to 128 texels, so every level down to 1/32 is painted with it.
//   * each of our cells is set to the Game tab's pair (as before: that leaves the pane on the atlas's page and gives it the animation's
//     colours), then the two panes' texture records get the UVs of the tab's cells in the strip. The records are the panes' own (the set
//     copies the sprite's UVs in) and the animation does not write them, so nothing else changes.
// The atlas is checked by the Game tab's sprite: its texture must be 2048 x 2048 and the sprite at (152, 1348), where the strip was
// found free. Painted once per texture (by its runtime id); a texture the game loads again is painted again at the next bar build.
#include "ui/pkp_options_internal.h"
#include "ui/pkp_options_icons_data.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_sprites.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/tex_patch.h"

namespace nxrt::games::pkp::options_detail::icons {

namespace {

using options_icons::kIconCount;
using options_icons::kIcons;
using options_icons::kIconSize;
static_assert(kIconCount == kTabs, "tools/settings_tab_icons.py bakes one icon per tab of ours");

constexpr uint32_t kGameOn = 0x88, kGameOff = 0x87;   // the Game tab's pair (icon ids)
constexpr uint32_t kOnSlot = 5, kOffSlot = 6;
constexpr uint32_t kAtlasSize = 2048;
constexpr sprites::Rect kGameOnRect = {152, 1348, 72, 72};
constexpr uint32_t kStripX = 0, kStripY = 1920, kStripW = 1920, kStripH = 128;
constexpr uint32_t kPitch = 80, kCellX = 4, kCellY = 28;   // a cell's place in the strip
static_assert(2 * kTabs * kPitch <= kStripW && kCellY + kIconSize <= kStripH, "the icons fit the strip");

struct State {
    uint64_t tex = 0;                 // the NVNtexture painted last
    uint32_t id = 0;                  // its runtime id
    std::vector<uint8_t> saved;       // the strip's blocks before the first paint
    bool warned = false;
} g;

// The cell of icon `index`, selected look or not, in atlas texels.
sprites::Rect cell(int index, bool on) {
    return {kStripX + kCellX + kPitch * (2 * static_cast<uint32_t>(index) + (on ? 0 : 1)), kStripY + kCellY, kIconSize, kIconSize};
}

// The strip's texels: per icon, the selected look (the glyph in its colour, lighter at the top as the game's fills, over the outline
// plate) and the other one (the glyph in white), as tools/settings_tab_icons.py previews them.
std::vector<uint8_t> strip() {
    std::vector<uint8_t> px(static_cast<size_t>(kStripW) * kStripH * 4, 0);
    for (int i = 0; i < kIconCount; ++i) {
        const options_icons::Icon& icon = kIcons[i];
        for (int look = 0; look < 2; ++look) {
            const sprites::Rect r = cell(i, look == 0);
            for (int y = 0; y < kIconSize; ++y) {
                const float k = 1.12f - 0.24f * y / (kIconSize - 1);
                for (int x = 0; x < kIconSize; ++x) {
                    const int m = y * kIconSize + x;
                    uint8_t* o = &px[((static_cast<size_t>(r.y - kStripY) + y) * kStripW + (r.x - kStripX) + x) * 4];
                    const float sa = icon.glyph[m] / 255.f;
                    if (look == 1) {
                        o[0] = o[1] = o[2] = 255;
                        o[3] = icon.glyph[m];
                        continue;
                    }
                    const float da = icon.plate[m] / 255.f, oa = sa + da * (1 - sa);
                    for (int ch = 0; ch < 3; ++ch) {
                        const float fill = std::min(255.f, std::round(icon.rgb[ch] * k));
                        o[ch] = static_cast<uint8_t>(oa > 0 ? std::lround((fill * sa + options_icons::kOutline[ch] * da * (1 - sa)) / oa) : 0);
                    }
                    o[3] = static_cast<uint8_t>(std::lround(oa * 255.f));
                }
            }
        }
    }
    return px;
}

void warn_once(const char* why) {
    if (g.warned) return;
    g.warned = true;
    NXLOG_W(kTag, "options: our tabs keep the Game tab's icon (%s)", why);
}

}  // namespace

bool prepare(::Ctx* c, uint64_t base, uint64_t scratch, uint64_t sp) {
    const uint64_t rec = sprites::icon_sprite(c, base, kGameOn, scratch, sp);
    const uint64_t tex = rec ? sprites::texture_of(rec) : 0;
    gfx::GuestTexInfo ti;
    sprites::Rect r;
    if (!tex || !gfx::guest_texture_info(reinterpret_cast<const void*>(tex), &ti) || !sprites::rect_of(rec, ti.width, ti.height, &r)) {
        warn_once("the icon atlas was not found");
        return false;
    }
    if (ti.width != kAtlasSize || ti.height != kAtlasSize || r.x != kGameOnRect.x || r.y != kGameOnRect.y || r.w != kGameOnRect.w ||
        r.h != kGameOnRect.h) {
        warn_once("the icon atlas is not laid out as expected");
        return false;
    }
    if (tex == g.tex && ti.id == g.id) return true;
    const std::vector<uint8_t> px = strip();
    std::vector<uint8_t> was;
    if (!gfx::texture_patch_bc7(reinterpret_cast<const void*>(tex), kStripX, kStripY, kStripW, kStripH, px.data(), &was)) {
        warn_once("the icon atlas could not be painted");
        return false;
    }
    if (g.saved.empty()) g.saved = std::move(was);
    g.tex = tex;
    g.id = ti.id;
    NXLOG_I(kTag, "options: tab icons painted into texture #%u (%ux%u, %u levels)", ti.id, ti.width, ti.height, ti.levels);
    return true;
}

void set_cell(::Ctx* c, uint64_t base, uint64_t holder, int index, bool painted, uint64_t sp) {
    guest::call(c, base + sprites::kSetIcon, holder, kOnSlot, kGameOn, 0, 0, sp);
    guest::call(c, base + sprites::kSetIcon, holder, kOffSlot, kGameOff, 0, 0, sp);
    if (!painted || index < 0 || index >= kIconCount) return;
    for (const bool on : {true, false}) {
        const uint64_t pane = slot_pane(c, holder, on ? kOnSlot : kOffSlot);
        const uint64_t rec = pane && guest::ok(pane + layout::kPaneTexture, 8) ? guest::rd<uint64_t>(pane + layout::kPaneTexture) : 0;
        if (!rec || !guest::ok(rec + layout::kTextureUv, 32)) continue;
        const sprites::Rect r = cell(index, on);
        const float u0 = static_cast<float>(r.x) / kAtlasSize, u1 = static_cast<float>(r.x + r.w) / kAtlasSize;
        const float v0 = static_cast<float>(r.y) / kAtlasSize, v1 = static_cast<float>(r.y + r.h) / kAtlasSize;
        const float uv[8] = {u0, v0, u1, v0, u0, v1, u1, v1};   // top left, top right, bottom left, bottom right
        for (int k = 0; k < 8; ++k) guest::wr<float>(rec + layout::kTextureUv + 4 * k, uv[k]);
    }
}

// Only into the texture painted last, while it is still that texture (the game may have freed it, and its object made another one).
void restore() {
    gfx::GuestTexInfo ti;
    const void* tex = reinterpret_cast<const void*>(g.tex);
    if (g.tex && !g.saved.empty() && guest::ok(g.tex, 16) && gfx::guest_texture_info(tex, &ti) && ti.id == g.id)
        gfx::texture_put_bc7_blocks(tex, kStripX, kStripY, kStripW, kStripH, g.saved.data(), g.saved.size());
    g = State{};
}

}  // namespace nxrt::games::pkp::options_detail::icons
