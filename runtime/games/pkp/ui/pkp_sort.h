// SPDX-License-Identifier: GPL-3.0-or-later
// Inventory sort (pkp_sort.cpp): a click on "Hold to Sort" in the bag / container screen runs the game's own sort of the panel under the cursor.
// [pkp] inventory_sort (on), [pkp] inventory_autosort (off).
#pragma once
#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kSortFeature;   // a key guide client (pkp_key_guide.h): the click on Hold to Sort

}  // namespace nxrt::games::pkp
