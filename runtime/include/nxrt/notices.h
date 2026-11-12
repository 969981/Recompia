// SPDX-License-Identifier: GPL-3.0-or-later
// Short messages over the game's picture: the overlay draws the ones that are up (overlay_hud.cpp). Each comes from a source (a mod's id, a
// part of the runtime) and a newer one from the same source replaces its earlier one. Its time runs from the first frame that shows it, so a
// message posted while the game starts up is still seen. Any thread.
#pragma once
#include <string>
#include <vector>

namespace nxrt::notices {

void post(const std::string& source, const std::string& text, double seconds);

struct Notice {
    std::string source, text;
};
// The messages to draw now, oldest first, at steady-clock time now_s (seconds); expired ones are dropped. The overlay's draw calls it.
std::vector<Notice> shown(double now_s);
bool any();   // a message is up or waiting to be shown (cheap: the overlay asks every frame)

}  // namespace nxrt::notices
