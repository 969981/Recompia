// SPDX-License-Identifier: GPL-3.0-or-later
// Inventory sort (pkp_sort.cpp): Y or the "(Y) Sort" key guide in the bag / container screen sorts the items under the cursor.
// [pkp] inventory_sort (on), [pkp] inventory_autosort (off).
#pragma once
#include "pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kSortFeature;
bool sort_click();   // main thread: a left click on the Sort key guide (pkp_menu.cpp); the screen sorts at its next frame. False (click
                     // not taken) unless the bag / container screen is up and its key guide shows the Sort entry

}  // namespace nxrt::games::pkp
