// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: Story co-op's gifts for a visitor (pkp_story_gifts.h).
//
// The gift manager of the 2.0.1 main ([main + 0x3565160]) has one 16-byte entry per gift 1..200 at +gift * 16 (u16 +0 the creature it
// is linked to, u8 +2 the gift); the gift's state is the player's own (save root + 0x6fabd9 + gift * 4: 1 idle, 2 queued, 3 and 4 with
// the creature, 5 received). Its per-frame queue (0x6132b8) leaves an entry whose handle is not a creature's alone, and 0x618304, the
// hand-over, needs that creature.
// - 0x61ccc0(manager, u16* handle): the link, called once when a creature is made (0x295c98). The creature's gift giver id (0x61d068)
//   gives its gifts (table +0x948, row data bytes +2..+0x15); every gift 0x615b90 allows is linked to the handle, and one already
//   queued or with the creature is given to it again (0x6132b8, 0x614860, 0x6145e4).
// - 0x61d068(u16* handle): the creature's gift giver id, 0 for none. Its callers are the link, the unlink when the creature goes
//   (0x61d1ac) and the creature's own "have I a gift" (0x3b5624), nothing else.
// - 0x11435c(u16* handle): 0x61d068's first test and only caller: the handle is a resident creature (0x286de4), the player is not a
//   visitor (session bit 4) and the stage is not 0x11 (0x77ecb0). The visitor test is why a visitor gets no gift in a host's world.
//   The entry hook answers it for a visitor as the game does at home: 0x286de4 and the stage.
// - 0x615b90(manager, entry, state == 3): the gift may be linked now: the gift system is on and the player not busy (the gates of the
//   manager's update), and the gift's stage (gift table +0x1160, row data +0x60) is the stage the player is in.
// A Link Play log showed every handle of a visitor's manager 0 from the moment of joining, and a direct call of 0x61ccc0 linking
// nothing (0x11435c). With the hook the link the game makes when a creature is made works; in case the manager is set up after the
// world's creatures are made, every 30 game frames as a visitor a live creature (the player manager's lists at +0x4550 + id * 8, what
// 0x28cc60 walks; its handle at +0x27fe) with a gift the game would link now but has not is linked with the game's own 0x61ccc0.
// Once linked its entries hold its handle, so it is not linked again; a creatures whose link did not hold is left alone until the
// session changes.
#include "core/pkp_story_gifts.h"
#include "core/pkp_guest.h"
#include "core/pkp_hook_set.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_site_check.h"
#include "core/pkp_tables.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <vector>

#include "nxrt/hooks.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kGiftManager = 0x3565160;
constexpr uint64_t kIsGiver = 0x11435c;                   // (u16* handle) -> bit 0; the entry hook
constexpr uint64_t kIsResident = 0x286de4;                // (u16* handle) -> bit 0
constexpr uint64_t kLink = 0x61ccc0, kMayLink = 0x615b90, kGiverId = 0x61d068;
// pacibsp: the hooked function and the functions this module calls (0x286de4 starts with its null test's adrp).
constexpr HookSite kSites[] = {{kIsGiver, 0xd503237fu, true}, {kLink, 0xd503237fu, true}, {kMayLink, 0xd503237fu, true},
                               {kGiverId, 0xd503237fu, true}, {kIsResident, 0xb0018fe8u, true}};
constexpr uint8_t kStageNoGifts = 0x11;                   // 0x77ecb0
constexpr uint64_t kEntrySize = 0x10, kGiftStates = 0x6fabd9;
constexpr uint32_t kGiftMax = 200;
constexpr uint8_t kStateIdle = 1, kStateHeld = 3, kStateHeldLast = 4;
constexpr uint64_t kGiverTable = 0x948;                   // per gift giver id: row data bytes +2..+0x15, its gifts (0: none)
constexpr uint64_t kGiverFirst = 2, kGiverLast = 0x15;
constexpr uint32_t kGiverMax = 0x1ff;                     // 0x61ccc0's bound
constexpr uint64_t kActorLists = 0x4550, kActorHandle = 0x27fe;
constexpr uint32_t kActorListMax = 0x3ff;
constexpr uint8_t kVisitor = layout::kSessionOnline | layout::kSessionVisitor;
constexpr int kEveryFrames = 30;

struct State {
    uint64_t base = 0;
    HookSet* hooks = nullptr;
    uint32_t hook = 0;
    std::atomic<bool> armed{false};
    std::atomic<bool> checked{false};
    // The game thread's (0x61d068's callers and game_frame run there):
    int frames = 0;
    uint16_t handle = 0;             // 0x61ccc0's and 0x61d068's argument
    uint8_t session = 0;             // the session bits `left` belongs to
    std::vector<uint16_t> left;      // creatures whose link did not hold
    uint32_t linked = 0;
    bool logged_hook = false;
};
State& st() {
    static State s;
    return s;
}

// The session bits and the stage, or false while the game state is not there.
bool session(const State& s, uint8_t* bits, uint8_t* stage) {
    const uint64_t game = guest::rd<uint64_t>(s.base + layout::kGameState);
    if (!guest::ok(game, layout::kStateSession + 1)) return false;
    *bits = guest::rd<uint8_t>(game + layout::kStateSession);
    *stage = guest::rd<uint8_t>(game + layout::kStateStage);
    return true;
}

// 0x11435c(u16* handle), a visitor's: the game's answer without its visitor test.
int hook_is_giver(::Ctx* c, uint64_t, void*) {
    State& s = st();
    uint8_t bits = 0, stage = 0;
    if (!s.armed.load(std::memory_order_acquire) || !session(s, &bits, &stage) || (bits & kVisitor) != kVisitor) return 0;
    const uint64_t resident = guest::call(c, s.base + kIsResident, c->x[0]) & 1;
    c->x[0] = resident && stage != kStageNoGifts ? 1 : 0;
    if (c->x[0] && !s.logged_hook) {
        s.logged_hook = true;
        NXLOG_I(kTag, "Story co-op gifts: the creatures of this world give this visitor its own gifts");
    }
    return 1;   // handled: the function returns x0
}

// A creature's handle as the game checks one (0x6132b8, 0x618304): not bit 11, kind (bits 12-15) 1..4.
bool live_handle(uint16_t h) { return !(h & 0x800) && (h >> 12) - 1u <= 3u; }

// The first live creature of an actor list, or 0.
uint16_t creature_of(const State& s, uint32_t list) {
    const uint64_t players = guest::rd<uint64_t>(s.base + layout::kPlayerManager);
    const uint64_t at = players + kActorLists + list * 8ull;
    if (!players || !guest::ok(at, 8)) return 0;
    const uint64_t head = guest::rd<uint64_t>(at);
    const uint64_t actor = head && guest::ok(head, 8) ? guest::rd<uint64_t>(head) : 0;
    if (!actor || !guest::ok(actor + kActorHandle, 2)) return 0;
    const uint16_t h = guest::rd<uint16_t>(actor + kActorHandle);
    return live_handle(h) ? h : 0;
}

// The creature (s.handle) has a gift the game would link to it now and has not.
bool needs_link(::Ctx* c, State& s, uint64_t manager, uint64_t save) {
    const uint32_t giver = guest::call(c, s.base + kGiverId, reinterpret_cast<uint64_t>(&s.handle)) & 0xffff;
    if (giver - 1 >= kGiverMax) return false;
    const uint64_t row = tables::row(c, s.base, kGiverTable, giver);
    const uint64_t data = row && guest::ok(row + 8, 8) ? guest::rd<uint64_t>(row + 8) : 0;
    if (!data || !guest::ok(data, kGiverLast + 1)) return false;
    for (uint64_t i = kGiverFirst; i <= kGiverLast; ++i) {
        const uint8_t gift = guest::rd<uint8_t>(data + i);
        if (gift - 1u >= kGiftMax) continue;
        const uint64_t entry = manager + gift * kEntrySize;
        const uint8_t state = guest::rd<uint8_t>(save + kGiftStates + gift * 4ull);
        if (guest::rd<uint16_t>(entry) == s.handle || state < kStateIdle || state > kStateHeldLast) continue;
        if (guest::call(c, s.base + kMayLink, manager, entry, state == kStateHeld) & 1) return true;
    }
    return false;
}

void game_frame(::Ctx* c) {
    State& s = st();
    if (!s.armed.load(std::memory_order_acquire) || ++s.frames < kEveryFrames) return;
    s.frames = 0;
    uint8_t bits = 0, stage = 0;
    if (!session(s, &bits, &stage)) return;
    if (bits != s.session) {
        s.session = bits;
        s.left.clear();
    }
    if ((bits & kVisitor) != kVisitor) return;
    const uint64_t manager = guest::rd<uint64_t>(s.base + kGiftManager);
    const uint64_t save = guest::rd<uint64_t>(s.base + layout::kSaveRoot);
    if (!manager || !guest::ok(manager, (kGiftMax + 1) * kEntrySize) || !save || !guest::ok(save + kGiftStates, (kGiftMax + 1) * 4ull)) return;
    for (uint32_t list = 1; list <= kActorListMax; ++list) {
        const uint16_t handle = creature_of(s, list);
        if (!handle || std::find(s.left.begin(), s.left.end(), handle) != s.left.end()) continue;
        s.handle = handle;
        if (!needs_link(c, s, manager, save)) continue;
        guest::call(c, s.base + kLink, manager, reinterpret_cast<uint64_t>(&s.handle));
        if (needs_link(c, s, manager, save)) {
            s.left.push_back(handle);
            NXLOG_W(kTag, "Story co-op gifts: the gifts of the creature 0x%04x (actor list %u) could not be linked to it", handle, list);
        } else if (s.linked++ < 64) {
            NXLOG_I(kTag, "Story co-op gifts: the gifts of the creature 0x%04x (actor list %u) are linked to it in this world", handle, list);
        }
    }
}

// Play mode's frame (main is loaded): the first call checks the hooked function and the functions this module calls, and arms the
// hook or removes it.
void frame(bool, double, play::GameFrame*) {
    State& s = st();
    if (!s.hook || s.checked.exchange(true)) return;
    uint32_t word = 0;
    if (const HookSite* bad = first_bad_site(s.base, kSites, std::size(kSites), &word)) {
        s.hooks->remove(s.hook);
        s.hook = 0;
        NXLOG_W(kTag, "Story co-op gifts: off: main+0x%llx is %08x, expected %08x (another game version)",
                static_cast<unsigned long long>(bad->site), word, bad->word);
        return;
    }
    s.armed.store(true, std::memory_order_release);
    NXLOG_I(kTag, "Story co-op gifts: a visitor gets its own gifts from the creatures of the world it is in (hook at main+0x%llx)",
            static_cast<unsigned long long>(kIsGiver));
}

void install(const FeatureInstall& in) {
    State& s = st();
    if (s.hook || !settings::get_bool(settings::Key::LinkStoryCoop)) return;
    s.base = in.base;
    s.hooks = &in.hooks;
    s.armed.store(false);
    s.checked.store(false);
    s.frames = 0;
    s.session = 0;
    s.left.clear();
    s.linked = 0;
    s.logged_hook = false;
    s.hook = in.hooks.add_entry(in.base + kIsGiver, &hook_is_giver);
}

void remove() {   // the registry has removed the hook
    State& s = st();
    s.armed.store(false);
    s.checked.store(false);
    s.hook = 0;
    s.hooks = nullptr;
}

}  // namespace

const Feature kStoryGiftsFeature = {.name = "story_gifts", .install = &install, .remove = &remove, .frame = &frame, .game_frame = &game_frame};

}  // namespace nxrt::games::pkp
