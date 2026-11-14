// SPDX-License-Identifier: GPL-3.0-or-later
// Camera aim's debug view, [pkp] camera_aim_debug (off by default, live): camera aim (pkp_aim.cpp) reports what it did on the game thread
// through the calls below, and this module draws it over the picture on the overlay (nxrt/overlay_canvas.h), projected through the
// game's own render view (render/pkp_view_projection.h): the ray, the cells it walked, the cell it hit, the cell the target query was
// moved to, and a text panel with the numbers and the reasons.
//
// Camera aim makes these calls only while on() is true (one relaxed load: with the key off its hooks cost what they did). Each call
// replaces the last report of its kind; every report carries the game frame it came from, so the panel shows how old it is. Any thread
// (a mutex guards the record).
#pragma once
#include <cstdint>

#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kAimDebugFeature;

namespace aim_debug {

bool on();

// The swing tools' preview and live hit shapes (their position reports).
enum class Use : uint8_t { SwingPreview, SwingHit, Count };

// A cell the ray walked: before it got near the character, behind the character (past the old two-cell box), tested and empty.
enum class Step : uint8_t { Far, Behind, Tested };

// The ray walk (pkp_aim_ray.cpp, once a frame): its start, then each cell, then the result: the solid cell it met (ray), its cells from
// the character's column h and rows from the feet row d, the adjacent cell (adjacent: in the world and empty) and why the cell is not the
// target (null: it is).
void ray_begin(const float origin[3], const float dir[3], const float character[3], int feet_row, int box);
void ray_step(int x, int y, int z, Step s);
void ray_result(bool ray, int x, int y, int z, int h, int d, bool adjacent, int ax, int ay, int az, const char* reject);

// The target query's start cell: the game's (x, y, z) and the cell written at its join (the same: kept).
void query_start(int x, int y, int z, int new_x, int new_y, int new_z);

// A swing shape's position (world x, y, z) before and after the position hook, and what it did.
void swing_pos(Use u, const float before[3], const float after[3], const char* how);

}  // namespace aim_debug
}  // namespace nxrt::games::pkp
