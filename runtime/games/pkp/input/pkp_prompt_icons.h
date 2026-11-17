// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the icons button prompts are drawn with (pkp_prompt_icons.cpp): the prompt icon pack's pad icons and Kenney's keyboard
// and mouse icons (assets/kbm_icons/<name>.png next to the exe), loaded once and kept, plus the image helpers that put them into the
// game's glyphs (pkp_kbm.cpp) and into the game's art (pkp_baked_prompts.cpp). Not thread-safe: the owner (the kbm feature) holds its lock.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nxrt::games::pkp::prompt_icons {

struct Image {
    bool tried = false;
    std::vector<uint8_t> rgba;    // straight-alpha RGBA8, w x h
    uint32_t w = 0, h = 0;
};

// The icons by name: a pack entry ("xbox/a", "ps/cross": a name with a slash) or a Kenney file name without .png ("keyboard_e").
// A wanted icon ("target") is one name, or "@move:" and four Kenney names (the move keys, forward left back right) drawn as the WASD
// cluster.
class Cache {
public:
    void set_dir(std::string dir) { dir_ = std::move(dir); }
    const std::string& dir() const { return dir_; }
    // The icon `name`, or null when it is missing. Pack icons as the pack draws them (the colourless ones on the game's glyph plate);
    // Kenney icons on that plate (plated) or as Kenney drew them.
    const Image* get(const std::string& name, bool plated = true);
    // Every icon `target` needs is there.
    bool have(const std::string& target);
    // `target` drawn into a w x h glyph (the icon's drawn part filling it, aspect kept), straight-alpha RGBA8. `target` must be had.
    std::vector<uint8_t> pixels(const std::string& target, uint32_t w, uint32_t h);

private:
    std::string dir_;                   // <exe folder>/assets/kbm_icons/
    std::map<std::string, Image> images_;
};

// The icon scaled (bilinear, premultiplied) to w x h.
std::vector<uint8_t> scaled(const Image& im, uint32_t w, uint32_t h);
// The icon's drawn part (its bounding box, without the file's clear margin) scaled to fill 0.96 of w x h, aspect kept, centred.
std::vector<uint8_t> fitted(const Image& im, uint32_t w, uint32_t h);
// 1 for the clear texels reachable from the border through clear texels; the other clear texels are holes in the shape (a key's letter).
std::vector<uint8_t> outside_of(const uint8_t* rgba, int w, int h);
// The icon over a plate of colour `rgb` at `alpha` (0..1): the icon's shape grown by a rim of `rim` (a fraction of its width) and every
// hole inside the shape filled.
void plate(Image& im, const uint8_t rgb[3], float alpha, double rim);

}  // namespace nxrt::games::pkp::prompt_icons
