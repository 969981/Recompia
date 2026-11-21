// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the co-op input lock (pkp_coop_internal.h). Three parts of co-op freeze the local player's input: the name prompt
// (pkp_coop_catchup.cpp), the wait for every member (pkp_coop_gate.cpp) and the wait for a dropped one (pkp_coop_drop.cpp). All use the game's own lock, bit layout::kInputLockBit of
// the byte at character + layout::kInputLock. The bit is one flag, so a holder that cleared it would free a character another still
// wants held: each holder keeps its own claim here, and the bit is written clear only when no claim is left on that character.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

namespace nxrt::games::pkp::coop::lock {

namespace {

constexpr size_t kHolders = 3;

struct State {
    uint64_t claim[kHolders] = {};   // the character each holder's claim is on; 0: none
};
State& st() {
    static State s;
    return s;
}

void write_bit(uint64_t chara, bool on) {
    if (!guest::ok(chara + layout::kInputLock, 1)) return;
    const uint8_t v = guest::rd<uint8_t>(chara + layout::kInputLock);
    guest::wr<uint8_t>(chara + layout::kInputLock, on ? (v | layout::kInputLockBit) : (v & ~layout::kInputLockBit));
}

bool claimed(uint64_t chara) {
    for (uint64_t c : st().claim)
        if (c == chara) return true;
    return false;
}

}  // namespace

void hold(Holder who, uint64_t chara) {
    if (!chara) return;
    uint64_t& mine = st().claim[static_cast<size_t>(who)];
    if (mine && mine != chara) release(who);   // the character changed under the claim (a world load): the old one is let go
    mine = chara;
    write_bit(chara, true);   // on every call, so a caller may repeat the claim
}

void release(Holder who) {
    uint64_t& mine = st().claim[static_cast<size_t>(who)];
    const uint64_t chara = mine;
    mine = 0;
    if (chara && !claimed(chara)) write_bit(chara, false);
}

}  // namespace nxrt::games::pkp::coop::lock
