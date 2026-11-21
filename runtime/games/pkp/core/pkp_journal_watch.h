// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's watchers (pkp_journal.h), one per area, each in its own file pkp_journal_<area>.cpp:
//   events    the event queue: every offer taken or refused (and why), the runners' states, content records and notices
//   talk      talks: the press, the talk the handler chose and from what, the talk conditions a member reads differently
//   roles     the session bits and the host / visitor branches the game takes
//   quests    quest states (and attempts), the quest lists, story lines, step actions and effects
//   items     the bags (in, out, which bag), world pickups, chests and storage boxes opened (where) and their contents
//   travel    area trips and stage loads phase by phase, fades, the session's own ends
//   services  creature work and NPC services, Link Play's request queue, requests, the shop's stock, discovery setters
//   interact  every press on a world object (the object's class and handler)
// A watcher only reads: every hook returns 0, so the game (and co-op after it) runs as before. The journal feature registers every
// watcher's sites before co-op (pkp_game.cpp), so a watcher's entry hook sees each call before co-op's hook may end the chain.
// Unlike co-op's sites (all or none), each journal site is checked on its own: a site whose word differs (another game build) or a
// midhook the lift does not have (a lift older than the site's main.sites line) is skipped alone, logged once, and the rest run.
// game_frame runs on the game thread once a frame, in co-op or not: polls must compare and write only what changed.
#pragma once
#include <cstddef>
#include <cstdint>

#include "nxrt/abi.h"
#include "core/pkp_site_check.h"

namespace nxrt::games::pkp::journal {

struct WatchSite {
    HookSite at;
    void (*fn)(::Ctx* c);   // the watcher's look at the site; the hook returns 0 after it
    const char* name;       // for the "skipped" line
};

struct Watcher {
    const char* name;
    const WatchSite* sites;
    size_t count;
    void (*game_frame)(::Ctx* c, bool in_game) = nullptr;   // polls (game thread, once a frame)
    void (*reset)() = nullptr;                              // the feature was removed: forget the state
};

// main's guest base (0 before install).
uint64_t base();

extern const Watcher kEventsWatcher;
extern const Watcher kTalkWatcher;
extern const Watcher kRolesWatcher;
extern const Watcher kQuestsWatcher;
extern const Watcher kItemsWatcher;
extern const Watcher kTravelWatcher;
extern const Watcher kServicesWatcher;
extern const Watcher kInteractWatcher;

}  // namespace nxrt::games::pkp::journal
