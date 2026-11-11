// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: a player's render view as the game last built it, captured on the game thread, and world points projected through it to
// the picture (debug views drawn on the overlay's canvas, nxrt/overlay_canvas.h).
//
// The render view is the camera object the follow camera writes each frame (0x7100088aac, pkp_shoulder_camera.cpp):
// [[main+0x3483e18]] + player * 0x260 + 0x49b0, a KTGL camera (pkp_aspect.cpp): eye vec4 +0x10, look-at +0x20, up +0x30, projection rows
// +0xc0, view-projection rows +0x100, vertical FOV (radians) +0x148, aspect +0x14c. Two ways to project, the first that holds:
//   matrix  the view-projection rows (clip = x * row0 + y * row1 + z * row2 + row3, the layout the aspect hook relies on), used when the
//           look-at lands in the middle of the picture through them (they hold this view, the over-the-shoulder offset and the aspect fix)
//   look-at the eye, look-at and up as a right-handed look-at (an assumption, like the shoulder camera's right vector), scaled by the
//           projection's diagonal (or the FOV and aspect fields when it is not sane)
// Up on the picture is taken from the up vector in both, so a flipped clip-space Y does not matter.
#pragma once
#include <cstdint>

namespace nxrt::games::pkp::view_projection {

struct View {
    bool valid = false;
    bool matrix = false;          // projected through the view-projection rows (else the look-at)
    float eye[3] = {}, look[3] = {}, up[3] = {};
    float fov = 0, aspect = 0;    // the fields (radians, width / height)
    float sx = 1, sy = 1;         // the look-at way: the projection's x and y scales
    float vp[16] = {};
    float y_sign = 1;             // the matrix way: +1 when clip-space Y points up on the picture
};

// The game thread. Not valid while the view block is not there (before the world is loaded).
View capture(uint64_t base, int player);

// Clip space: x, y, w (x / w and y / w in -1..1 across the picture, y up; w > 0 in front of the eye).
struct Clip {
    float x = 0, y = 0, w = 0;
};
Clip to_clip(const View& v, const float p[3]);
// Clip space to picture pixels (w x h, top left 0, 0). The caller keeps w above kNear.
void to_picture(const View& v, const Clip& c, float w, float h, float* px, float* py);
inline constexpr float kNear = 1.f;   // world units: points closer to the eye plane than this are clipped

}  // namespace nxrt::games::pkp::view_projection
