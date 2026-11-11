// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the per-player view offset (pkp_view_offset.h).
#include "core/pkp_view_offset.h"

#include <atomic>

namespace nxrt::games::pkp::view_offset {
namespace {

struct Slot {
    std::atomic<float> v[3] = {};
};
Slot g_slots[kPlayers];

}  // namespace

void set(int player, float x, float y, float z) {
    if (player < 0 || player >= kPlayers) return;
    Slot& s = g_slots[player];
    s.v[0].store(x, std::memory_order_relaxed);
    s.v[1].store(y, std::memory_order_relaxed);
    s.v[2].store(z, std::memory_order_relaxed);
}

Offset get(int player) {
    if (player < 0 || player >= kPlayers) return {};
    const Slot& s = g_slots[player];
    return {s.v[0].load(std::memory_order_relaxed), s.v[1].load(std::memory_order_relaxed), s.v[2].load(std::memory_order_relaxed)};
}

}  // namespace nxrt::games::pkp::view_offset
