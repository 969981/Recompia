// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the hooks one feature module registered (nxrt/hooks.h), owned by the feature registry (pkp_feature.h). A module adds
// its hooks through the set it is given at install; the registry removes every hook of the set when play mode stops, so removal is
// complete whatever the module registered. A module that drops one of its hooks while it runs (Link Play's arming) removes it here.
// Main thread only (install, remove and play mode's frame).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "nxrt/hooks.h"

namespace nxrt::games::pkp {

class HookSet {
public:
    // As nxrt::hook_add_*; the id (0 for a null function, which is not kept).
    uint32_t add_inline(uint64_t addr, InlineHookFn fn, void* user = nullptr) { return keep(hook_add_inline(addr, fn, user)); }
    uint32_t add_mid(uint64_t addr, SiteHookFn fn, void* user = nullptr) { return keep(hook_add_mid(addr, fn, user)); }
    uint32_t add_entry(uint64_t addr, SiteHookFn fn, void* user = nullptr) { return keep(hook_add_entry(addr, fn, user)); }

    // Removes one hook of this set; false when the set does not hold it.
    bool remove(uint32_t id) {
        const auto it = std::find(ids_.begin(), ids_.end(), id);
        if (it == ids_.end()) return false;
        ids_.erase(it);
        return hook_remove(id);
    }
    // Removes every hook of the set, the last added first.
    void clear() {
        for (auto it = ids_.rbegin(); it != ids_.rend(); ++it) hook_remove(*it);
        ids_.clear();
    }
    size_t size() const { return ids_.size(); }

private:
    uint32_t keep(uint32_t id) {
        if (id) ids_.push_back(id);
        return id;
    }
    std::vector<uint32_t> ids_;
};

}  // namespace nxrt::games::pkp
