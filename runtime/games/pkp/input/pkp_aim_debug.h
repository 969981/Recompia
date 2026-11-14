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

// The game path that asked: the command target query, the swing tools' preview and live hit shapes, or another caller of the row
// helper (the placement collectors).
enum class Use : uint8_t { Query, SwingPreview, SwingHit, Other, Count };

// A cell the ray walked: before it got near the character, behind the character (past the old two-cell box), tested and empty.
enum class Step : uint8_t { Far, Behind, Tested };

// The ray walk (camera_ray): its start, then each cell, then the hit (none: no hit). The interaction query's walks are not reported.
void ray_begin(const float origin[3], const float dir[3], const float character[3], int feet_row, int reach);
void ray_step(int x, int y, int z, Step s);
void ray_hit(int x, int y, int z, int h);

// The row hook (hook_pitch): the base row, the hit's row offset from it, the band chosen (-1..2, or kUnder: the down extreme, the cell
// under the character; why says which, or why the game's band stayed) and the pitch before and after (after not finite: unchanged).
inline constexpr int kUnder = 100;
void row(Use u, bool hit, int base, int d, int band, const char* why, float pitch_before, float pitch_after);

// The reach check (aim_hit): the hit it looked at (when hit), its row offset d from the base row, and the reason it was rejected (null:
// accepted).
void pick(Use u, bool hit, int x, int y, int z, int h, int d, const char* reject);

// The target query's start cell: the game's (x, y, z) and the cell written (the same: kept).
void query_start(int x, int y, int z, int new_x, int new_y, int new_z);

// A swing shape's position (world x, y, z) before and after the position hook, and what it did.
void swing_pos(Use u, const float before[3], const float after[3], const char* how);

}  // namespace aim_debug
}  // namespace nxrt::games::pkp
