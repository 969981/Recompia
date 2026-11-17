// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, button prompts drawn into the game's art (pkp_baked_prompts.h).
//
// Some Switch A glyphs are not the UI atlas's sprite (pkp_kbm.cpp overwrites that one) but part of a bigger picture the game loads from its
// files, so they kept the Switch letter while every other prompt showed the pad's or the keyboard's (#730, #733). Each such picture is
// found by the content of its BC7 blocks around the glyph (gfx::Bc7Match: texture size, rectangle, FNV-1a 64 of the rectangle's blocks;
// nxrt/tex_patch.h texture_find_bc7) and that rectangle is drawn again with the icon in the picture's own look. The blocks that were there
// are kept, so a change of prompt style (or back to switch) draws the new icon or puts the game's pixels back. The rectangles hold the
// glyph and nothing else of the picture but what the look redraws (the title's soft shadow, the badge's orange pill).
//
// The pictures (romfs motor_asset_re/KIDS_PLATFORM_OUNCE, 2.0.1a; textures untiled with the Switch 2 GOB order, 16-GOB blocks):
//   * the title logo, sprite 8000_title_press_btn: a 1024 x 1024 texture per language with the A disc 48 x 48 at y 631. English and Japanese
//     share the rectangle (their logos differ, the PRESS (A) BUTTON line does not). The rectangle starts past the last letter left of the
//     disc. No other BC7 texture of the archive carries that disc.
//   * the challenge list's "(A) Complete!" badge, sprite 0500_base_chare_reportable (248 x 60 at 2732, 1768 of texture 0, 4096 x 2048, of
//     the 0500 UI atlas): the A is a 31 px white disc in a dark orange ring on the pill's rounded end. A search of every BC7 texture of the
//     UI archive (0x00011c54) for that A disc, at four scales, found it nowhere else; other letters (B, X, Y) were looked for by sprite
//     name only.
#include "input/pkp_baked_prompts.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "nxrt/log.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";

enum class Look {
    Title,   // white, the symbol cut out, over a soft dark shadow of the whole shape, on a clear background
    Badge,   // outlined in the ring's dark orange (the symbol filled with it) on the pill's rounded end
};

struct Art {
    const char* name;            // logs
    gfx::Bc7Match match;
    Look look;
    uint32_t gx, gy, gs;         // the glyph's box in the rectangle (gs x gs)
};

constexpr Art kArts[] = {
    {"title (English, Japanese)", {1024, 1024, 252, 620, 72, 72, 0xc400174b48564fa1ull}, Look::Title, 10, 11, 48},
    {"title (French)", {1024, 1024, 484, 620, 72, 72, 0x49700b84d2689625ull}, Look::Title, 12, 11, 48},
    {"title (German)", {1024, 1024, 396, 620, 72, 72, 0x272e8cde344353c6ull}, Look::Title, 12, 11, 48},
    {"title (Italian)", {1024, 1024, 388, 620, 72, 72, 0x74981fc10c41b360ull}, Look::Title, 9, 11, 48},
    {"title (Spanish)", {1024, 1024, 372, 620, 72, 72, 0xbef3dac8bffe1914ull}, Look::Title, 9, 11, 48},
    {"challenge badge", {4096, 2048, 2732, 1772, 56, 48, 0x5d93f8d8ae5efb9bull}, Look::Badge, 11, 7, 35},
};
constexpr size_t kArtCount = sizeof(kArts) / sizeof(kArts[0]);

const std::vector<gfx::Bc7Match>& matches() {
    static const std::vector<gfx::Bc7Match> m = [] {
        std::vector<gfx::Bc7Match> v;
        for (const Art& a : kArts) v.push_back(a.match);
        return v;
    }();
    return m;
}

// Over `dst` (straight alpha), `src` with alpha `sa` (0..1).
void blend(uint8_t* dst, const float src[3], float sa) {
    const float da = dst[3] / 255.f, oa = sa + da * (1 - sa);
    for (int ch = 0; ch < 3; ++ch) dst[ch] = static_cast<uint8_t>(oa > 0 ? std::lround((src[ch] * sa + dst[ch] * da * (1 - sa)) / oa) : 0);
    dst[3] = static_cast<uint8_t>(std::lround(oa * 255.f));
}

// The title look: the icon white with its symbol cut out (Kenney's keys as drawn; the pack's icons as they are), over a soft shadow of the
// whole shape (the cut-out shows the shadow, like the art's A), like the art's own A.
std::vector<uint8_t> title_pixels(prompt_icons::Cache& icons, const Art& a, const std::string& icon) {
    const prompt_icons::Image* im = icons.get(icon, false);
    if (!im) return {};
    const int W = static_cast<int>(a.match.w), H = static_cast<int>(a.match.h), g = static_cast<int>(a.gs);
    const int gx = static_cast<int>(a.gx), gy = static_cast<int>(a.gy);
    const std::vector<uint8_t> glyph = prompt_icons::fitted(*im, a.gs, a.gs);
    const std::vector<uint8_t> outside = prompt_icons::outside_of(glyph.data(), g, g);
    std::vector<float> shadow(static_cast<size_t>(W) * H, 0.f), tmp(shadow.size());
    for (int y = 0; y < g; ++y)
        for (int x = 0; x < g; ++x)
            shadow[static_cast<size_t>(y + gy) * W + x + gx] = outside[static_cast<size_t>(y) * g + x] ? glyph[(static_cast<size_t>(y) * g + x) * 4 + 3] / 255.f : 1.f;
    for (int pass = 0; pass < 2; ++pass)   // two box blurs of radius 3 in x and y: about the art's shadow (6 texels)
        for (int dir = 0; dir < 2; ++dir) {
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    float sum = 0;
                    for (int k = -3; k <= 3; ++k) {
                        const int sx = dir ? x : x + k, sy = dir ? y + k : y;
                        if (sx >= 0 && sy >= 0 && sx < W && sy < H) sum += shadow[static_cast<size_t>(sy) * W + sx];
                    }
                    tmp[static_cast<size_t>(y) * W + x] = sum / 7.f;
                }
            shadow.swap(tmp);
        }
    std::vector<uint8_t> out(static_cast<size_t>(W) * H * 4, 0);
    const float dark[3] = {12.f, 12.f, 12.f};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t* o = &out[(static_cast<size_t>(y) * W + x) * 4];
            blend(o, dark, std::min(1.f, shadow[static_cast<size_t>(y) * W + x] * 0.4f));   // the art's shadow: near black, up to ~0.3 alpha
            const int lx = x - gx, ly = y - gy;
            if (lx < 0 || ly < 0 || lx >= g || ly >= g) continue;
            const uint8_t* sp = &glyph[(static_cast<size_t>(ly) * g + lx) * 4];
            const float c[3] = {static_cast<float>(sp[0]), static_cast<float>(sp[1]), static_cast<float>(sp[2])};
            blend(o, c, sp[3] / 255.f);
        }
    return out;
}

// The badge look: the pill's rounded end as the art draws it (a cap of radius 21 centred at (28, 24) of the rectangle, edges at y 3 and
// 45, orange from (255, 139, 38) at the top to (255, 133, 25) at the bottom: on average within 1 level of the art's texels outside the
// disc), and the icon over it outlined in the ring's dark orange with its symbol filled in that colour, like the art's white disc with its
// orange A.
std::vector<uint8_t> badge_pixels(prompt_icons::Cache& icons, const Art& a, const std::string& icon) {
    const prompt_icons::Image* im = icons.get(icon, false);
    if (!im) return {};
    constexpr float kTop = 3.f, kBottom = 45.f, kCx = 28.f, kCy = 24.f, kR = 21.f;
    constexpr float kTopRgb[3] = {255.f, 139.f, 38.f}, kBottomRgb[3] = {255.f, 133.f, 25.f};
    constexpr uint8_t kRing[3] = {224, 89, 7};
    const int W = static_cast<int>(a.match.w), H = static_cast<int>(a.match.h), g = static_cast<int>(a.gs);
    // plate() grows the rim inside the image only: a clear border of the rim's width first, or an icon drawn to its file's edge (the
    // pack's) gets its ring cut flat there
    constexpr double kRim = 4.0 / 64;
    const uint32_t pad = static_cast<uint32_t>(std::ceil(im->w * kRim)) + 1;
    prompt_icons::Image ringed;
    ringed.w = im->w + 2 * pad;
    ringed.h = im->h + 2 * pad;
    ringed.rgba.assign(static_cast<size_t>(ringed.w) * ringed.h * 4, 0);
    for (uint32_t y = 0; y < im->h; ++y)
        std::copy_n(&im->rgba[static_cast<size_t>(y) * im->w * 4], static_cast<size_t>(im->w) * 4,
                    &ringed.rgba[(static_cast<size_t>(y + pad) * ringed.w + pad) * 4]);
    prompt_icons::plate(ringed, kRing, 1.f, kRim * im->w / ringed.w);   // the rim as a fraction of the padded width: the same texels
    const std::vector<uint8_t> glyph = prompt_icons::fitted(ringed, a.gs, a.gs);
    std::vector<uint8_t> out(static_cast<size_t>(W) * H * 4, 0);
    for (int y = 0; y < H; ++y) {
        const float t = std::clamp((y - kTop) / (kBottom - kTop - 1.f), 0.f, 1.f);
        float rgb[3];
        for (int ch = 0; ch < 3; ++ch) rgb[ch] = kTopRgb[ch] + (kBottomRgb[ch] - kTopRgb[ch]) * t;
        for (int x = 0; x < W; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float cover = px < kCx ? kR - std::hypot(px - kCx, py - kCy) + 0.5f : std::min(py - kTop, kBottom - py) + 0.5f;
            uint8_t* o = &out[(static_cast<size_t>(y) * W + x) * 4];
            blend(o, rgb, std::clamp(cover, 0.f, 1.f));
            const int lx = x - static_cast<int>(a.gx), ly = y - static_cast<int>(a.gy);
            if (lx < 0 || ly < 0 || lx >= g || ly >= g) continue;
            const uint8_t* sp = &glyph[(static_cast<size_t>(ly) * g + lx) * 4];
            const float c[3] = {static_cast<float>(sp[0]), static_cast<float>(sp[1]), static_cast<float>(sp[2])};
            blend(o, c, sp[3] / 255.f);
        }
    }
    return out;
}

}  // namespace

const std::vector<uint8_t>& BakedPrompts::pixels(prompt_icons::Cache& icons, uint32_t art, const std::string& icon) {
    for (const Pixels& p : pixels_)
        if (p.art == art && p.icon == icon) return p.rgba;
    const Art& a = kArts[art];
    pixels_.push_back({art, icon, a.look == Look::Title ? title_pixels(icons, a, icon) : badge_pixels(icons, a, icon)});
    return pixels_.back().rgba;
}

void BakedPrompts::scan(uint32_t frame, bool active, int style) {
    if (!active || frame % 60 != 0) return;
    // #280: the scan walks every registered texture under the registry lock. It backs off while nothing changes: the interval doubles
    // (60 .. 600 frames) after two scans in a row that found nothing new, and goes back to 60 as soon as the registry changed (a screen
    // loads textures) or the prompt style did.
    const size_t reg = gfx::texture_registry_count();
    if (reg != registry_ || style != style_) {
        registry_ = reg;
        style_ = style;
        every_ = 60;
        empty_ = 0;
        last_ = 0;
    }
    if (last_ != 0 && frame - last_ < every_) return;
    const size_t checked_before = checked_.size();
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<gfx::Bc7Found> found = gfx::texture_find_bc7(matches().data(), kArtCount, &checked_);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    last_ = frame;
    if (!found.empty() || checked_.size() != checked_before) {
        empty_ = 0;
        every_ = 60;
    } else if (++empty_ >= 2) {
        every_ = std::min<uint32_t>(every_ * 2, 600);
    }
    if (ms > 2.0 && slow_logged_ < 20) {
        ++slow_logged_;
        NXLOG_I(kTag, "kbm: art scan took %.2f ms over %zu textures (next in %u game frames)", ms, registry_, every_);
    }
    for (const gfx::Bc7Found& f : found) {
        found_.push_back({f.id, f.match, std::string(), {}});
        NXLOG_I(kTag, "kbm: %s art found (texture #%u)", kArts[f.match].name, f.id);
    }
}

void BakedPrompts::sync(prompt_icons::Cache& icons, const std::string& icon) {
    const std::string want = !icon.empty() && icons.have(icon) ? icon : std::string();
    for (size_t i = 0; i < found_.size();) {
        Found& f = found_[i];
        if (f.shown == want) {
            ++i;
            continue;
        }
        const gfx::Bc7Match& m = kArts[f.art].match;
        const std::vector<uint8_t>* px = want.empty() ? nullptr : &pixels(icons, f.art, want);
        if (px && px->empty()) {   // the look's own icon file is missing: the art keeps what it shows
            ++i;
            continue;
        }
        bool ok = true;
        if (!f.shown.empty()) {
            ok = gfx::texture_put_bc7_blocks_by_id(f.tex, m.x, m.y, m.w, m.h, f.saved.data(), f.saved.size());
            f.shown.clear();
        }
        if (ok && px) {
            ok = gfx::texture_patch_bc7_by_id(f.tex, m.x, m.y, m.w, m.h, px->data(), &f.saved);
            if (ok) {
                f.shown = want;
                // the title's line keeps its old start: test scripts wait for "kbm: title screen art: PRESS" to know the title is up
                if (kArts[f.art].look == Look::Title)
                    NXLOG_I(kTag, "kbm: title screen art: PRESS (A) BUTTON now shows %s (%s, texture #%u)", want.c_str(), kArts[f.art].name, f.tex);
                else
                    NXLOG_I(kTag, "kbm: %s art (texture #%u): the A now shows %s", kArts[f.art].name, f.tex, want.c_str());
            }
        }
        if (ok) {
            ++i;
            continue;
        }
        // the game freed it (a screen that closed; it is found again when the screen loads it again)
        found_.erase(found_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

}  // namespace nxrt::games::pkp
