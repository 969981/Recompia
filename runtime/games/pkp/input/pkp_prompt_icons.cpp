// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the icons button prompts are drawn with (pkp_prompt_icons.h).
#include "input/pkp_prompt_icons.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "prompts.h"

namespace nxrt::games::pkp::prompt_icons {

namespace {

constexpr const char* kTag = "pkp";

// The game's button glyphs (atlas #493, "0000_key_1_a" ...) are a white cap with a dark rim and a dark letter, so they read on the white
// dialogue boxes, the blue tutorial boxes and the 3D scene alike. Kenney's keys are a white cap with the letter cut out (transparent), which
// vanishes on a white box. The icon is put on the game's look: a plate of the game's rim colour under it (the icon's shape grown by a rim of
// 3/64 of its size, and every hole inside the shape, the letter, filled) and the icon itself on top.
constexpr uint8_t kGlyphPlate[3] = {51, 51, 51};   // the game's rim and letter: grey 51 at alpha 191
constexpr float kGlyphPlateAlpha = 191.f / 255.f;
constexpr double kGlyphRim = 3.0 / 64;

// A pack icon without colour of its own: the d-pad, the triggers and shoulders and the d-pad's direction icons are white or grey, which
// vanishes on the pale move wheel and the dialogue boxes (#230). The face buttons (A, B, cross, circle...) are saturated and read as they are.
bool colourless(const Image& im) {
    size_t opaque = 0, coloured = 0;
    for (size_t i = 0; i + 3 < im.rgba.size(); i += 4) {
        if (im.rgba[i + 3] <= 128) continue;
        ++opaque;
        const uint8_t hi = std::max({im.rgba[i], im.rgba[i + 1], im.rgba[i + 2]}), lo = std::min({im.rgba[i], im.rgba[i + 1], im.rgba[i + 2]});
        if (hi - lo > 60) ++coloured;
    }
    return opaque > 0 && coloured * 4 < opaque;
}

std::vector<std::string> parts_of(const std::string& target) {
    if (target.compare(0, 6, "@move:") != 0) return {target};
    std::vector<std::string> v;
    size_t p = 6;
    for (size_t q; (q = target.find(',', p)) != std::string::npos; p = q + 1) v.push_back(target.substr(p, q - p));
    v.push_back(target.substr(p));
    return v.size() == 4 ? v : std::vector<std::string>{};
}

}  // namespace

std::vector<uint8_t> outside_of(const uint8_t* rgba, int w, int h) {
    auto a = [&](int x, int y) { return rgba[(static_cast<size_t>(y) * w + x) * 4 + 3]; };
    std::vector<uint8_t> outside(static_cast<size_t>(w) * h, 0);
    std::vector<int> todo;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if ((x == 0 || y == 0 || x == w - 1 || y == h - 1) && a(x, y) < 128) outside[static_cast<size_t>(y) * w + x] = 1, todo.push_back(y * w + x);
    while (!todo.empty()) {
        const int i = todo.back(), x = i % w, y = i / w;
        todo.pop_back();
        const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (const auto& n : nb) {
            if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h) continue;
            const size_t j = static_cast<size_t>(n[1]) * w + n[0];
            if (!outside[j] && a(n[0], n[1]) < 128) outside[j] = 1, todo.push_back(static_cast<int>(j));
        }
    }
    return outside;
}

void plate(Image& im, const uint8_t rgb[3], float alpha, double rim) {
    const int w = static_cast<int>(im.w), h = static_cast<int>(im.h);
    auto a = [&](int x, int y) { return im.rgba[(static_cast<size_t>(y) * w + x) * 4 + 3]; };
    const std::vector<uint8_t> outside = outside_of(im.rgba.data(), w, h);
    const int r = std::max(1, static_cast<int>(std::lround(w * rim)));
    std::vector<uint8_t> out(im.rgba.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float pa = outside[static_cast<size_t>(y) * w + x] ? 0.f : 255.f;   // plate alpha: holes full, the rest grown from the shape
            for (int dy = -r; dy <= r && pa < 255.f; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                    const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                    if (d > r + 0.5f) continue;
                    pa = std::max(pa, a(sx, sy) * std::clamp(r + 0.5f - d, 0.f, 1.f));   // soft edge on the rim
                }
            const uint8_t* s = &im.rgba[(static_cast<size_t>(y) * w + x) * 4];
            uint8_t* o = &out[(static_cast<size_t>(y) * w + x) * 4];
            const float sa = s[3] / 255.f, da = pa / 255.f * alpha, oa = sa + da * (1 - sa);
            for (int ch = 0; ch < 3; ++ch) o[ch] = static_cast<uint8_t>(oa > 0 ? std::lround((s[ch] * sa + rgb[ch] * da * (1 - sa)) / oa) : 0);
            o[3] = static_cast<uint8_t>(std::lround(oa * 255.f));
        }
    im.rgba = std::move(out);
}

const Image* Cache::get(const std::string& name, bool plated) {
    const bool from_pack = name.find('/') != std::string::npos;
    Image& im = images_[from_pack ? "pack:" + name : (plated ? name : "raw:" + name)];
    if (!im.tried) {
        im.tried = true;
        if (from_pack) {
            const play::IconPack* pack = play::prompts_pack();
            const uint8_t* px = pack ? pack->find(name) : nullptr;
            if (px && pack->width() >= 8 && pack->height() >= 8) {
                im.w = pack->width();
                im.h = pack->height();
                im.rgba.assign(px, px + static_cast<size_t>(im.w) * im.h * 4);
                if (colourless(im)) plate(im, kGlyphPlate, kGlyphPlateAlpha, kGlyphRim);
                NXLOG_I(kTag, "prompt icon %s: %ux%u from the pack", name.c_str(), im.w, im.h);
            } else im.rgba.clear();
        } else if (!gfx::load_png_rgba(dir_ + name + ".png", &im.rgba, &im.w, &im.h) || im.w < 8 || im.h < 8) im.rgba.clear();
        else {
            if (plated) plate(im, kGlyphPlate, kGlyphPlateAlpha, kGlyphRim);
            NXLOG_I(kTag, "kbm icon %s.png: %ux%u", name.c_str(), im.w, im.h);
        }
    }
    return im.rgba.empty() ? nullptr : &im;
}

bool Cache::have(const std::string& target) {
    const std::vector<std::string> parts = parts_of(target);
    if (parts.empty()) return false;
    for (const std::string& n : parts)
        if (n.empty() || !get(n)) return false;
    return true;
}

std::vector<uint8_t> Cache::pixels(const std::string& target, uint32_t w, uint32_t h) {
    const std::vector<std::string> parts = parts_of(target);
    if (parts.size() == 1) return fitted(*get(parts[0]), w, h);
    // forward on top in the middle, left / back / right below it. The icons carry a transparent margin (the key is about 3/4 of the
    // icon), so they are drawn at 0.45 of the glyph and 0.31 apart: the keys nearly touch and fill the glyph (the margins overlap or are cut).
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
    const int k = static_cast<int>(w * 0.45f), step = static_cast<int>(w * 0.31f), cx = static_cast<int>(w) / 2, cy = static_cast<int>(h) / 2;
    const int ctr[4][2] = {{cx, cy - step / 2}, {cx - step, cy + step / 2}, {cx, cy + step / 2}, {cx + step, cy + step / 2}};
    for (int i = 0; i < 4; ++i) {
        const std::vector<uint8_t> key = scaled(*get(parts[i]), static_cast<uint32_t>(k), static_cast<uint32_t>(k));
        for (int y = 0; y < k; ++y)
            for (int x = 0; x < k; ++x) {
                const int ox = ctr[i][0] - k / 2 + x, oy = ctr[i][1] - k / 2 + y;
                if (ox < 0 || oy < 0 || ox >= static_cast<int>(w) || oy >= static_cast<int>(h)) continue;
                const uint8_t* src = &key[(static_cast<size_t>(y) * k + x) * 4];
                uint8_t* dst = &out[(static_cast<size_t>(oy) * w + ox) * 4];
                const float sa = src[3] / 255.f, da = dst[3] / 255.f, oa = sa + da * (1 - sa);
                for (int ch = 0; ch < 3; ++ch)
                    dst[ch] = static_cast<uint8_t>(oa > 0 ? std::lround((src[ch] * sa + dst[ch] * da * (1 - sa)) / oa) : 0);
                dst[3] = static_cast<uint8_t>(std::lround(oa * 255.f));
            }
    }
    return out;
}

std::vector<uint8_t> scaled(const Image& im, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const float fx = std::clamp((x + 0.5f) * im.w / w - 0.5f, 0.f, im.w - 1.f), fy = std::clamp((y + 0.5f) * im.h / h - 0.5f, 0.f, im.h - 1.f);
            const uint32_t x0 = static_cast<uint32_t>(fx), y0 = static_cast<uint32_t>(fy);
            const uint32_t x1 = std::min(x0 + 1, im.w - 1), y1 = std::min(y0 + 1, im.h - 1);
            const float ax = fx - x0, ay = fy - y0;
            // premultiplied, so clear texels (black) do not darken the edges
            auto p = [&](uint32_t px, uint32_t py, int ch) {
                const uint8_t* q = &im.rgba[(static_cast<size_t>(py) * im.w + px) * 4];
                return ch == 3 ? static_cast<float>(q[3]) : q[ch] * (q[3] / 255.f);
            };
            auto lerp = [&](int ch) { return (p(x0, y0, ch) * (1 - ax) + p(x1, y0, ch) * ax) * (1 - ay) + (p(x0, y1, ch) * (1 - ax) + p(x1, y1, ch) * ax) * ay; };
            const float alpha = lerp(3);
            uint8_t* o = &out[(static_cast<size_t>(y) * w + x) * 4];
            for (int ch = 0; ch < 3; ++ch) o[ch] = static_cast<uint8_t>(alpha > 0 ? std::lround(std::clamp(lerp(ch) * 255.f / alpha, 0.f, 255.f)) : 0);
            o[3] = static_cast<uint8_t>(std::lround(std::clamp(alpha, 0.f, 255.f)));
        }
    return out;
}

// The game's glyphs fill their sprite, Kenney's keys sit in a margin (and wide keys are half as tall), so they would show smaller than the
// game's without the crop.
std::vector<uint8_t> fitted(const Image& im, uint32_t w, uint32_t h) {
    uint32_t x0 = im.w, y0 = im.h, x1 = 0, y1 = 0;
    for (uint32_t y = 0; y < im.h; ++y)
        for (uint32_t x = 0; x < im.w; ++x)
            if (im.rgba[(static_cast<size_t>(y) * im.w + x) * 4 + 3] > 8) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
    if (x1 <= x0 || y1 <= y0) return scaled(im, w, h);
    Image crop;
    crop.w = x1 - x0;
    crop.h = y1 - y0;
    crop.rgba.resize(static_cast<size_t>(crop.w) * crop.h * 4);
    for (uint32_t y = 0; y < crop.h; ++y)
        std::memcpy(&crop.rgba[static_cast<size_t>(y) * crop.w * 4], &im.rgba[(static_cast<size_t>(y0 + y) * im.w + x0) * 4], static_cast<size_t>(crop.w) * 4);
    const float k = std::min(w * 0.96f / crop.w, h * 0.96f / crop.h);
    const uint32_t sw = std::max(1u, static_cast<uint32_t>(std::lround(crop.w * k))), sh = std::max(1u, static_cast<uint32_t>(std::lround(crop.h * k)));
    const std::vector<uint8_t> small = scaled(crop, sw, sh);
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
    const uint32_t ox = (w - std::min(w, sw)) / 2, oy = (h - std::min(h, sh)) / 2;
    for (uint32_t y = 0; y < sh && oy + y < h; ++y)
        std::memcpy(&out[(static_cast<size_t>(oy + y) * w + ox) * 4], &small[static_cast<size_t>(y) * sw * 4], static_cast<size_t>(std::min(sw, w - ox)) * 4);
    return out;
}

}  // namespace nxrt::games::pkp::prompt_icons
