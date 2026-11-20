// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal feature (pkp_journal_feature.h): installs the journal's crash section and every watcher's sites
// (pkp_journal_watch.h), checks each site once main is loaded and runs the watchers' polls once a frame. Part of running the game
// (a port() row): always on, the log level ([log] level) is the player's switch.
#include "core/pkp_journal_feature.h"

#include <atomic>
#include <vector>

#include "core/pkp_journal.h"
#include "core/pkp_journal_watch.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp {

namespace {

using journal::Watcher;
using journal::WatchSite;

constexpr const Watcher* kWatchers[] = {
    &journal::kEventsWatcher, &journal::kTalkWatcher,  &journal::kRolesWatcher,    &journal::kQuestsWatcher,
    &journal::kItemsWatcher,  &journal::kTravelWatcher, &journal::kServicesWatcher, &journal::kInteractWatcher,
};

struct Armed {
    const WatchSite* site;
    std::atomic<bool> on{false};
};

struct State {
    uint64_t base = 0;
    std::vector<Armed*> sites;       // one per registered site (the hook's user), never moved while hooked
    std::atomic<bool> checked{false};
    std::atomic<bool> in_game{false};   // play mode's, from its frame (main thread) for the game-frame polls
};
State& st() {
    static State s;
    return s;
}

int hook(::Ctx* c, uint64_t, void* user) {
    const Armed* a = static_cast<const Armed*>(user);
    if (a->on.load(std::memory_order_acquire)) a->site->fn(c);
    return 0;
}

void install(const FeatureInstall& in) {
    State& s = st();
    journal::install();
    if (!s.sites.empty()) return;
    s.base = in.base;
    for (const Watcher* w : kWatchers)
        for (size_t i = 0; i < w->count; ++i) {
            Armed* a = new Armed;
            a->site = &w->sites[i];
            s.sites.push_back(a);
            const uint64_t at = in.base + a->site->at.site;
            if (a->site->at.entry) in.hooks.add_entry(at, &hook, a);
            else in.hooks.add_mid(at, &hook, a);
        }
    NXLOG_I("pkp", "Journal: %zu sites of %zu watchers, checked once the game runs", s.sites.size(), sizeof kWatchers / sizeof kWatchers[0]);
}

// Play mode's frame: the first call checks every site on its own and arms the ones that hold their word.
void frame(bool in_game, double, play::GameFrame*) {
    State& s = st();
    s.in_game.store(in_game, std::memory_order_relaxed);
    if (s.sites.empty() || s.checked.exchange(true)) return;
    size_t on = 0;
    for (Armed* a : s.sites) {
        uint32_t word = 0;
        if (first_bad_site(s.base, &a->site->at, 1, &word)) {
            NXLOG_W("pkp", "Journal: site %s (main+0x%llx) skipped: word %08x, expected %08x%s", a->site->name,
                    static_cast<unsigned long long>(a->site->at.site), word, a->site->at.word,
                    a->site->at.entry || word != a->site->at.word ? "" : " (the lift has no hook site there: Set up again)");
            continue;
        }
        a->on.store(true, std::memory_order_release);
        ++on;
    }
    NXLOG_I("pkp", "Journal: armed %zu of %zu sites", on, s.sites.size());
}

void game_frame(::Ctx* c) {
    State& s = st();
    if (!s.checked.load(std::memory_order_acquire)) return;
    const bool in_game = s.in_game.load(std::memory_order_relaxed);
    for (const Watcher* w : kWatchers)
        if (w->game_frame) w->game_frame(c, in_game);
}

void remove() {   // the registry has removed the hooks
    State& s = st();
    for (Armed* a : s.sites) delete a;
    s.sites.clear();
    s.checked.store(false);
    s.in_game.store(false);
    s.base = 0;
    for (const Watcher* w : kWatchers)
        if (w->reset) w->reset();
}

}  // namespace

namespace journal {
uint64_t base() { return st().base; }
}  // namespace journal

const Feature kJournalFeature = {
    .name = "journal",
    .install = &install,
    .remove = &remove,
    .frame = &frame,
    .game_frame = &game_frame,
};

}  // namespace nxrt::games::pkp
