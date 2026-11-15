// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: no Dream Island trip from a story area's Link Play session (pkp_link_trips.h).
//
// The 2.0.1 main's chain: Drifloon's talk script starts poll kind 2 and waits for it; the poll's end writes each player's result (the
// byte "this player goes") into the script context, and one of three event callbacks reads it:
// - 0x49cc40, 0x49d040, 0x49d7c0: online (`bl IsOnline`), each loads the byte into w9 (`ldrb w9, [..]`) and branches on it two
//   instructions later (`cbz w9`): zero queues the game's "staying" event, non-zero the departure and the trip callback 0x49d468. The
//   midhooks sit between the load and the branch and clear w9.
// - 0x9a50ac: the request for the island (it writes the island's state: seed, theme, who goes; the host then starts the move). Also
//   reached from the network when a visitor asks. The entry hook ends the call.
#include "core/pkp_link_trips.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_site_check.h"

#include <atomic>

#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/notices.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";
constexpr double kNoticeSeconds = 6.0;
constexpr uint8_t kStoryStageFirst = 1, kStoryStageLast = 10;
constexpr uint64_t kGoesReg = 9;

enum class SiteKind : uintptr_t { Result, Request };
constexpr HookSite kSites[] = {
    {0x49ccfc, 0x794002a8u, false},   // ldrh w8, [x21] (0x49cc40)
    {0x49d274, 0x794002a8u, false},   // ldrh w8, [x21] (0x49d040)
    {0x49d894, 0x79400288u, false},   // ldrh w8, [x20] (0x49d7c0)
    {0x9a50ac, 0xd503237fu, true},    // pacibsp
};
constexpr size_t kSiteCount = sizeof kSites / sizeof kSites[0];

struct State {
    uint64_t base = 0;
    HookSet* hooks = nullptr;
    uint32_t ids[kSiteCount] = {};   // all 0: off or not installed
    std::atomic<bool> armed{false};
    std::atomic<bool> checked{false};
    double told_at = -1e9;           // the game thread's
    bool logged = false;
};
State& st() {
    static State s;
    return s;
}

bool blocked_here(const State& s) {
    const uint64_t game = guest::rd<uint64_t>(s.base + layout::kGameState);
    if (!guest::ok(game, layout::kStateSession + 1)) return false;
    return (guest::rd<uint8_t>(game + layout::kStateSession) & layout::kSessionOnline) &&
           link_trip_blocked(guest::rd<uint8_t>(game + layout::kStateStage));
}

void tell(State& s) {
    const double now = qpc::seconds();
    if (now - s.told_at > kNoticeSeconds) {
        s.told_at = now;
        notices::post("pkp.link_trips", "Link Play: a Dream Island can't be visited from this area during a session", kNoticeSeconds);
    }
    if (!s.logged) {
        s.logged = true;
        NXLOG_I(kTag, "Link Play: a Dream Island trip from a story area's session was stopped (it is offered in Palette Town and on Cloud Islands)");
    }
}

int hook(::Ctx* c, uint64_t, void* user) {
    State& s = st();
    if (!s.armed.load(std::memory_order_acquire) || !blocked_here(s)) return 0;
    if (static_cast<SiteKind>(reinterpret_cast<uintptr_t>(user)) == SiteKind::Request) {
        tell(s);
        return 1;
    }
    if (c->x[kGoesReg] & 0xff) tell(s);
    c->x[kGoesReg] = 0;
    return 0;
}

void remove_hooks(State& s) {
    for (uint32_t& h : s.ids) {
        if (h && s.hooks) s.hooks->remove(h);
        h = 0;
    }
}

void install(const FeatureInstall& in) {
    State& s = st();
    if (s.ids[0]) return;
    if (!settings::get_bool(settings::Key::LinkInteractEverywhere) && !settings::get_bool(settings::Key::LinkStoryCoop)) return;
    s.base = in.base;
    s.hooks = &in.hooks;
    s.armed.store(false);
    s.checked.store(false);
    s.logged = false;
    for (size_t i = 0; i < kSiteCount; ++i) {
        void* kind = reinterpret_cast<void*>(static_cast<uintptr_t>(kSites[i].entry ? SiteKind::Request : SiteKind::Result));
        s.ids[i] = kSites[i].entry ? in.hooks.add_entry(in.base + kSites[i].site, &hook, kind) : in.hooks.add_mid(in.base + kSites[i].site, &hook, kind);
    }
}

// Play mode's frame (main is loaded): the first call checks the sites and arms the hooks or removes them.
void frame(bool, double, play::GameFrame*) {
    State& s = st();
    if (!s.ids[0] || s.checked.exchange(true)) return;
    uint32_t word = 0;
    if (const HookSite* bad = first_bad_site(s.base, kSites, kSiteCount, &word)) {
        remove_hooks(s);
        NXLOG_W(kTag, "Link Play trips: off: main+0x%llx is not the expected hook site (word %08x, expected %08x)",
                static_cast<unsigned long long>(bad->site), word, bad->word);
        return;
    }
    s.armed.store(true, std::memory_order_release);
    NXLOG_I(kTag, "Link Play trips: a session in a story area has no Dream Island trip");
}

void remove() {   // the registry has removed the hooks
    State& s = st();
    s.armed.store(false);
    s.checked.store(false);
    for (uint32_t& h : s.ids) h = 0;
    s.hooks = nullptr;
}

}  // namespace

bool link_trip_blocked(uint8_t stage) { return stage >= kStoryStageFirst && stage <= kStoryStageLast; }

const Feature kLinkTripsFeature = {.name = "link_trips", .install = &install, .remove = &remove, .frame = &frame};

}  // namespace nxrt::games::pkp
