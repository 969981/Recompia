// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's interaction watcher (pkp_journal_watch.h): every interaction the player starts with a world object.
// The player's interaction claim 0x31347c (P0-RE "after a talk is claimed") asks the object (vtable slot 4) and then calls its
// interaction handler, vtable slot 5, with kind 4 = press (0x313ea0..0x313eb4: x0 the object, w1 4, x2 the player). One midhook at
// that call (0x313eb4, `blr x8`) sees every press on every kind of object: doors and gates (slot 5 0x7b0420), passages, boxes, signs.
// The object is named by its vtable (main-relative, the class) and its handler; the detail lines come from the area's own watcher
// (j.travel for gates, j.box for boxes, j.talk for characters). Objects that are not placed have no position: +0x18 / +0x1c are
// printed raw and read as the position only for placed objects (base vtable 0x32643d0, AREA-GATE-RE).
#include <cinttypes>

#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
#include "core/pkp_journal_watch.h"

namespace nxrt::games::pkp::journal {

namespace {

constexpr uint64_t kPosXZ = 0x18, kPosY = 0x1c;   // placed object: u32 packed position, u16 height (AREA-GATE-RE door row)

uint64_t main_rel(uint64_t a) { return a >= base() ? a - base() : a; }

// 0x313eb4: x0 the object, x8 its slot 5 handler, w1 the kind (4), x2 the player; w20 the slot 4 answer (0x313e8c).
void on_interact(::Ctx* c) {
    const uint64_t obj = c->x[0];
    const uint64_t vt = guest::ok(obj, 8) ? guest::rd<uint64_t>(obj) : 0;
    Rec rec(Area::Interact, "press");
    rec.in("object=0x%" PRIx64 " class vt=0x%" PRIx64 " handler=0x%" PRIx64 " kind=%u player=0x%" PRIx64 " slot4=%u", obj, main_rel(vt),
           main_rel(c->x[8]), static_cast<uint32_t>(c->x[1]), c->x[2], static_cast<uint32_t>(c->x[20] & 1));
    if (guest::ok(obj + kPosXZ, 6))
        rec.in("+0x18=0x%08x +0x1c=%u (the position of a placed object)", guest::rd<uint32_t>(obj + kPosXZ), guest::rd<uint16_t>(obj + kPosY));
    rec.out("the object's interaction handler runs");
    rec.step();
}

constexpr WatchSite kSites[] = {
    {{0x313eb4, 0xd63f0100u, false}, &on_interact, "interaction handler call 0x313eb4"},   // blr x8
};

}  // namespace

const Watcher kInteractWatcher = {"interact", kSites, sizeof kSites / sizeof kSites[0]};

}  // namespace nxrt::games::pkp::journal
