// SPDX-License-Identifier: GPL-3.0-or-later
// Inventory sort (pkp_sort.cpp): Y in the bag / container screen sorts the items under the cursor. [pkp] inventory_sort (on),
// [pkp] inventory_autosort (off).
#pragma once
#include <cstdint>

namespace nxrt::games::pkp {

void sort_install(uint64_t base);

}  // namespace nxrt::games::pkp
