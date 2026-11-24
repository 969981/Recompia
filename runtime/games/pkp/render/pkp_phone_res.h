// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: a phone's one resolution setting ([pkp] resolution, percent of the screen per side; pkp_render_size.cpp). The game's
// targets are made once at start (the ceiling, never a setting of its own: the whole screen, or on the low-memory profile the start
// percent) and a change picks the scene's part of them through the game's own levels, so it never remakes a target. A value above the
// ceiling waits for the next start.
#pragma once

namespace nxrt::games::pkp {

constexpr int kPhoneResMin = 15;      // lowest percent of the screen per side
int phone_res_ceiling();              // the highest percent this start renders (the targets' size); 100 before the game set it up
int phone_res_max();                  // the highest percent at all: the screen, or its presented share ([gfx] present_scale)
int phone_res_setting();              // [pkp] resolution as set, clamped to [kPhoneResMin, phone_res_max()]
int phone_res_value();                // the percent in force: the setting clamped to the ceiling
void phone_res_pixels(int pct, int* w, int* h);   // the 3D picture's size in pixels at that percent of the screen

}  // namespace nxrt::games::pkp
