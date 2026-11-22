// SPDX-License-Identifier: GPL-3.0-or-later
// Storage box tabs: (+) Store All over every tab of the box (pkp_box_tabs_store.h). Main 2.0.1a. The transfer's status (screen +0x170)
// after one pass: 1 everything that could go went, 7 nothing to store, 0x0a full (nothing moved), 0x0c some moved and some is left over,
// 0x1b (matching only) the container holds none of the bag's items. The game shows the message for the status it finds after the call.
#include "ui/pkp_box_tabs_store.h"
#include "core/pkp_guest.h"

#include "nxrt/log.h"

namespace nxrt::games::pkp::box_store {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kTransfer = 0xff6f80;   // (screen, bag kind, container kind, matching only) -> bit 0: something moved; status at +0x170
constexpr uint64_t kRefresh = 0xff12e8;    // (screen): paints the grids and the detail
constexpr uint64_t kId = 0x154, kPanelId = 0xd4, kStatus = 0x170;
constexpr uint32_t kStored = 1, kNothing = 7, kFull = 0x0a, kPartly = 0x0c, kNoneMatch = 0x1b;

bool g_running = false;

using guest::call;
using guest::rd;
using guest::wr;

void route(const Sweep& s, uint16_t id) {
    wr<uint16_t>(s.screen + kId, id);
    wr<uint16_t>(s.panel + kPanelId, id);
}

}  // namespace

bool running() {
    return g_running;
}

bool sweep(::Ctx* c, uint64_t base, const Sweep& s) {
    const uint16_t shown = s.ids[s.shown];
    bool moved = false, short_of_room = false, full = false;
    uint32_t status = kNothing;
    g_running = true;
    for (int n = 0; n < s.count; ++n) {
        const uint16_t id = s.ids[(s.shown + n) % s.count];
        route(s, id);
        wr<uint32_t>(s.screen + kStatus, 0);
        const bool pass = call(c, base + kTransfer, s.screen, s.source, s.target, s.matching ? 1 : 0) & 1;
        status = rd<uint32_t>(s.screen + kStatus);
        if (s.log) NXLOG_I(kTag, "storage box tabs: store all into container %u: moved %d, status 0x%x", id, pass, status);
        // Room ran short after the last pass that moved something: a later tab that takes the rest clears it again.
        if (pass) short_of_room = status == kPartly;
        else if (status == kFull || status == kPartly) short_of_room = true;
        full |= status == kFull;
        moved |= pass;
        if (status == kNothing || (!s.matching && status == kStored)) break;   // the bag has nothing more to store
    }
    route(s, shown);
    g_running = false;
    call(c, base + kRefresh, s.screen);

    // One message for the whole box, as the game would give it for one container.
    if (moved) status = short_of_room ? kPartly : kStored;
    else if (full) status = kFull;
    else if (status != kNothing) status = s.matching ? kNoneMatch : kFull;
    wr<uint32_t>(s.screen + kStatus, status);
    return moved;
}

}  // namespace nxrt::games::pkp::box_store
