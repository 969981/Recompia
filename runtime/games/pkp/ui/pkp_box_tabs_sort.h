// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs (pkp_box_tabs.cpp): the game's sort over the whole box. The game sorts one container; with tabs that left every tab
// sorted for itself, with gaps at its end. The tabs' slots are copied into one run, the game's own merge and sort run on it, and the
// result goes back over the tabs in L / R order, the box itself first.
#pragma once
#include <cstdint>

#include "nxrt/abi.h"
#include "ui/pkp_box_tabs_storage.h"

namespace nxrt::games::pkp::box_sort {

// false: nothing was changed (a slot the sort would not keep as it is).
bool sort(::Ctx* c, uint64_t base, const box_tabs::BoxStorage& storage, const box_tabs::Tabs& tabs);

}  // namespace nxrt::games::pkp::box_sort
