// SPDX-License-Identifier: GPL-3.0-or-later
// Crafting search (pkp_craft.cpp): a "(Y) Search" key guide in the crafting screen; Y or a click asks for a word on the game's keyboard and the
// recipe list keeps the recipes whose item name holds it. [pkp] craft_search (on).
#pragma once
#include "pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kCraftFeature;
bool craft_click();   // main thread: a left click on the Search key guide (pkp_menu.cpp); the screen asks at its next frame. False (click not
                      // taken) unless the crafting screen is up and its key guide shows the Search entry

}  // namespace nxrt::games::pkp
