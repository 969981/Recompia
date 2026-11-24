// SPDX-License-Identifier: GPL-3.0-or-later
// Split screen: the game runs once per player on one PC (two to four), each process a window in its part of the screen. Whoever starts
// the group (the launcher) tells each process which window it is and how the processes meet (for the game: as the members of one co-op
// save); play mode keeps the window in its part and gives it one controller (src/gfx/play/play_split.cpp).
// After the parent project's split screen (MIT, windows/docs/split-screen.md): the same design, on this runtime's own window and input code.
//
// Configuration ([split], environment NXRT_SPLIT_<KEY>, read once; docs/CONFIG.md):
//   window = 0          1: the first window (it hosts), 2 to 4: another one (it visits); 0: no split screen
//   count = 2           how many windows share the screen: 2, 3 or 4
//   layout = stacked    two windows: stacked = top and bottom halves, each as wide as the screen; side = left and right halves.
//                       Three: the first window has the top half (stacked) or the left half (side), the other two share the rest.
//                       Four: a quarter each, whatever this says
//   displays =          empty: the windows share one screen. Else the display of each window in window order (`1,2`: window 2 has the
//                       second display), numbered as [display] monitor, the same list for every window. A window alone on its display
//                       fills it (borderless fullscreen); windows on one display share it as `layout` says. A display that is not
//                       there counts as the primary one
//   pad = <window>      the controller this window plays, by its place in the order the controllers were connected (1 = the first);
//                       0: none (keyboard and mouse only)
//   mute = false        this window plays no sound
#pragma once

namespace nxrt::split {

int window();   // 0: no split screen, else which window this process is (1 to 4)

}  // namespace nxrt::split
