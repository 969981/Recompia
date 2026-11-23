// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs: (+) Store All / Retrieve All over every tab of the box (pkp_box_tabs_store.h). Main 2.0.1a. The transfer's status
// (screen +0x170) after one pass: 1 everything that could go went, 7 nothing to move, 0x0a full (nothing moved), 0x0c some moved and some
// is left over, 0x1b (matching only) no item matched; into the bag with the handy bag unlocked 0x09 / 0x0b / 0x1a for the last three.
// The game shows the message for the status it finds after the call.
#include "ui/pkp_box_tabs_store.h"
#include "core/pkp_guest.h"

#include <algorithm>

#include "nxrt/log.h"

namespace nxrt::games::pkp::box_store {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kTransfer = 0xff6f80;   // (screen, from kind, to kind, matching only) -> bit 0: something moved; status at +0x170
constexpr uint64_t kRefresh = 0xff12e8;    // (screen): paints the grids and the detail
constexpr uint64_t kId = 0x154, kPanelId = 0xd4, kStatus = 0x170;
constexpr uint32_t kMoved = 1, kNothing = 7, kFull = 0x0a, kPartly = 0x0c, kNoneMatch = 0x1b;
constexpr uint8_t kBoxPanel = 1;

bool g_running = false;
const Sweep* g_whole_box = nullptr;   // the second matching round: what any tab holds counts as held

using guest::call;
using guest::ok;
using guest::rd;
using guest::wr;

void route(const Sweep& s, uint16_t id) {
    wr<uint16_t>(s.screen + kId, id);
    wr<uint16_t>(s.panel + kPanelId, id);
}

// The status with the handy bag's variants folded into the plain ones; `handy` remembers that one was seen.
uint32_t plain(uint32_t status, bool& handy) {
    if (status != kFull - 1 && status != kPartly - 1 && status != kNoneMatch - 1) return status;
    handy = true;
    return status + 1;
}

struct Pass { bool moved; uint32_t status; };
struct Result {
    bool moved = false, blocked = false, none_match = false, handy = false;
};

Pass pass(::Ctx* c, uint64_t base, const Sweep& s, int n, Result& r) {
    const uint16_t id = s.tabs->ids[(s.shown + n) % s.tabs->count];
    route(s, id);
    wr<uint32_t>(s.screen + kStatus, 0);
    const bool moved = call(c, base + kTransfer, s.screen, s.source, s.target, s.matching ? 1 : 0) & 1;
    const uint32_t status = plain(rd<uint32_t>(s.screen + kStatus), r.handy);
    if (s.log)
        NXLOG_I(kTag, "storage box tabs: %s container %u%s: moved %d, status 0x%x", s.store ? "store all into" : "retrieve all from", id,
                g_whole_box ? " (whole box)" : "", moved, status);
    r.moved |= moved;
    return {moved, status};
}

// Into the box: room that ran short in one tab is made good by a later tab that takes the rest.
void store_round(::Ctx* c, uint64_t base, const Sweep& s, Result& r) {
    r.blocked = false;
    for (int n = 0; n < s.tabs->count; ++n) {
        const Pass p = pass(c, base, s, n, r);
        const bool short_of_room = p.status == kFull || p.status == kPartly;
        // Each tab by its own items (the first matching round): any tab that ran short calls for the second round.
        r.blocked = short_of_room || (r.blocked && s.matching && !g_whole_box);
        if (p.status == kNoneMatch) r.none_match = true;
        // Nothing left that could go. A tab's own matching items say nothing about the next tab's.
        if (p.status == kNothing || ((!s.matching || g_whole_box) && !short_of_room)) break;
    }
}

}  // namespace

bool running() {
    return g_running;
}

int hook_count_matching(::Ctx* c, uint64_t, void*) {
    const Sweep* s = g_whole_box;
    if (!s || (c->x[1] & 0xff) != kBoxPanel || !ok(c->x[2], items::kSlotSize)) return 0;
    const uint32_t id = items::id_of(rd<uint32_t>(c->x[2] + items::kSlotPayload));
    c->x[0] = std::min<uint32_t>(s->storage->count_of(*s->tabs, id), 0xffff);   // the caller tests the low half
    return 1;
}

bool sweep(::Ctx* c, uint64_t base, const Sweep& s) {
    const uint16_t shown = s.tabs->ids[s.shown];
    Result r;
    g_running = true;
    if (s.store) {
        store_round(c, base, s, r);
        if (s.matching && r.blocked) {
            g_whole_box = &s;
            store_round(c, base, s, r);
            g_whole_box = nullptr;
        }
    } else {
        // Out of the box: every tab is asked, as an empty tab says nothing about the next one and a full bag still takes what merges.
        for (int n = 0; n < s.tabs->count; ++n) {
            const uint32_t status = pass(c, base, s, n, r).status;
            r.blocked |= status == kFull || status == kPartly;
            r.none_match |= status == kNoneMatch;
        }
    }
    route(s, shown);
    g_running = false;
    call(c, base + kRefresh, s.screen);

    // One message for the whole box, as the game would give it for one container.
    uint32_t status = kNothing;
    if (r.blocked) status = (r.moved ? kPartly : kFull) - (r.handy ? 1 : 0);
    else if (r.moved) status = kMoved;
    else if (r.none_match) status = kNoneMatch - (r.handy ? 1 : 0);
    wr<uint32_t>(s.screen + kStatus, status);
    return r.moved;
}

}  // namespace nxrt::games::pkp::box_store
