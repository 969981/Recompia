// SPDX-License-Identifier: GPL-3.0-or-later
// Crafting search (pkp_craft.cpp): a "(Y) Search" key guide in the crafting screen; Y or a click asks for a word on the game's keyboard and the
// recipe list keeps the recipes whose item name holds it. [pkp] craft_search (on).
#pragma once
#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kCraftFeature;   // a key guide client (pkp_key_guide.h): the Search entry and its click

}  // namespace nxrt::games::pkp
