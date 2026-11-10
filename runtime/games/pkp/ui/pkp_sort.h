// SPDX-License-Identifier: GPL-3.0-or-later
// Inventory sort (pkp_sort.cpp): Y or the "(Y) Sort" key guide in the bag / container screen sorts the items under the cursor.
// [pkp] inventory_sort (on), [pkp] inventory_autosort (off).
#pragma once
#include "core/pkp_feature.h"

namespace nxrt::games::pkp {

extern const Feature kSortFeature;   // a key guide client (pkp_key_guide.h): the Sort entry and its click

}  // namespace nxrt::games::pkp
