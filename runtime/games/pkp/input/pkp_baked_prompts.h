// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer, button prompts drawn into the game's art (pkp_baked_prompts.cpp) rather than set as glyphs of its UI atlas: the title
// logo's "PRESS (A) BUTTON" (one logo texture per language) and the challenge list's "(A) Complete!" badge. Part of the kbm feature
// (pkp_kbm.cpp), which owns one of these and calls it from its game-frame callback; the game thread is the only caller.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "input/pkp_prompt_icons.h"
#include "nxrt/tex_patch.h"

namespace nxrt::games::pkp {

class BakedPrompts {
public:
    // Looks for art textures the game loaded since the last look, at most every 60 game frames and less often while nothing changes
    // (#280: it reads texture storage under the registry lock). `active`: a prompt style other than switch shows (nothing to draw
    // otherwise); `style` changing resets the back-off.
    void scan(uint32_t frame, bool active, int style);
    // Draws `icon` (a prompt_icons name, "" = the game's own pixels) over the A of every art found, in that art's look. The caller holds
    // the lock of `icons`.
    void sync(prompt_icons::Cache& icons, const std::string& icon);

private:
    struct Found {
        uint32_t tex = 0;              // gfx::Bc7Found::id
        uint32_t art = 0;              // index into the art table
        std::string shown;             // the icon written there now, "" = the game's own pixels
        std::vector<uint8_t> saved;    // the art's blocks under it
    };
    struct Pixels {
        uint32_t art = 0;
        std::string icon;
        std::vector<uint8_t> rgba;
    };
    const std::vector<uint8_t>& pixels(prompt_icons::Cache& icons, uint32_t art, const std::string& icon);

    std::vector<Found> found_;                // arts found (scan) and what they show (sync)
    std::vector<uint32_t> checked_;           // textures looked at
    std::vector<Pixels> pixels_;              // drawn icons, per art and icon
    uint32_t every_ = 60, last_ = 0, empty_ = 0, slow_logged_ = 0;
    size_t registry_ = ~size_t{0};            // registry size at the last scan
    int style_ = -1;
};

}  // namespace nxrt::games::pkp
