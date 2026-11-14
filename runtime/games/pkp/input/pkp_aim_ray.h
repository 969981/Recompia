// SPDX-License-Identifier: GPL-3.0-or-later
// Camera aim's one ray (pkp_aim.cpp): what the camera looks at near the local player's character, cast once per game frame and view and
// read by every camera aim hook, the way the game's own Joy-Con 2 mouse mode casts its pointer ray once a frame (0x710031935c) and its
// consumers read the result from the character (+0xaa30 hit cell, +0xaa36 the empty cell beside it, +0xaa50 the hit cell's centre).
//
// The ray runs from the view's eye along its direction through the cell grid (100 world units a cell). Cells before the ray comes near the
// character are skipped (the camera is behind it); near means within the box of max(2, 1 + extra) cells of the character's column
// (Chebyshev), and past two cells only cells in front of the character count. The first solid cell in the box is the ray's cell. It is the
// aim's target (hit) when it is within 1 + extra cells of the character's column and its row within -(1 + extra) .. max(2, 1 + extra) rows
// of the feet row (the game's own row bands reach -1 .. +2). The adjacent cell is the cell the ray was in right before: the target's face
// the ray came in through, so placing there puts the item on the side the player looks at. It counts only when it is not solid.
#pragma once
#include <cstdint>

namespace nxrt::games::pkp::aim_ray {

struct View {
    float eye[3] = {};
    float dir[3] = {};   // not normalised
};

struct Cell {
    int x = 0, y = 0, z = 0;
};

struct Aim {
    bool ray = false;          // the ray met a solid cell in the box (cell, h)
    bool hit = false;          // and it is in reach: the target
    Cell cell;
    int h = 0;                 // cells from the character's column (Chebyshev)
    bool adjacent = false;     // hit, and the cell before it is in the world and not solid
    Cell side;
    const char* reject = nullptr;   // why the ray's cell is not the target (null: hit, or no cell)
};

// install: main's base and [pkp] camera_aim_reach (extra cells).
void configure(uint64_t base, int extra);
// The game thread, once a frame: the next cast sees the world again.
void next_frame();

// The aim of view v for the character ch: cast on the first ask of a frame for that view and character position, the same answer after.
// The cast is reported to the debug view (pkp_aim_debug.h).
const Aim& cast(const View& v, uint64_t ch);

int feet_row(uint64_t ch);
// World centre of cell c.
void centre(const Cell& c, float out[3]);

}  // namespace nxrt::games::pkp::aim_ray
