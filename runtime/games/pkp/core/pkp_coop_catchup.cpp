// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's catch-up (pkp_coop_internal.h): what one player's talk gives that player alone in the game, but
// every member needs, is given to the others too. The talk lock (pkp_coop_talk.cpp) means only one player runs a character's event;
// the shared story then moves every process on, but two of the first quest's steps act on the talker's process only:
//
// Name. Event-script command 0x44 (0x43d3e0) asks the player's name and writes it. Once this player's name slot has changed after it,
// every other member is asked for a name of their own (NamePrompt; a member whose slot is already set is not asked). The command runs
// in a talk the character starts too (the first quest's name talk follows the creature list pickup unasked), so it is watched in any event,
// not only in a talk this player holds: the name entry only ever runs on the process whose player is asked. The prompt is the
// game's own name keyboard, asynchronous so the game thread never waits:
//   0x11314b0(6, buf, 0x19, opts) starts it (1: started); 0x11316d0() busy; 0x11316e0() answered with OK; 0x1131680() ends it.
//   opts, 32 bytes: +0 header (char*), +0x18 u16 min length 1, +0x1a u16 max length (12 for languages 1..5, else 6), +0x1c 1, +0x1f 1.
//   The header is the game's own text, from 0x248338(text): +0 u32 kind 0x25, +8 u8 text id 2, +0x10 arguments, +0x18 count 0,
//   +0x20 capacity 6; 6 arguments of 0x88 bytes follow (+0 u32 -1).
// While it is up the character takes no input (the input lock, pkp_coop_lock.cpp). An OK answer is written as command 0x44
// writes it, for the game's language L (u32 at [main+0x36ca680]) in the save root R:
//   R+0x741799 + L*0x19: 0x19 bytes cleared, then 0x1122b50(dst, 0x19, name); 0x1122b50(R+0x74187c + L*0x20, 0x19, name);
//   u32 R+0x741898 + L*0x20 = L; and the autosave is asked for as the command asks for it: game state + 0x3bb5e8 bit 0 set and
//   u32 +0x3bb5e0 (the autosave's step) not 1..5: bit 2 cleared and float +0x3bb5e4 = max(itself, 570).
//
// Items. Event-script command 0xbea (0x12c734(cmd): u16 item at +4 (1..0x11f6), u32 count at +8, given to the local player) is given
// to every other member too (Items), through the same handler. Like the name it counts in any event, a talk the player holds or one a
// character starts: an event runs on the process of the player it is with only (control is taken from that player alone), so a
// give is never run on two processes and shared twice.
// A character's reward list (0x3bbbe0: the next of up to 10 rewards in its species row; the progress byte is in the world's
// character record) gives its item through 0x3bcbc0(comp, &u16 item, &u16 count): 0x3bcdc0(0, &actor id, item, count, 0xff, 0),
// into this process's bag with the item-get scene. It runs where post-talk action 4 runs: a member's talk only writes the action
// into the talk-end record (callback slot 69) and the host runs it (0x2197a0 @0x21c8f4), so the host's player got the member's
// reward and the member nothing. Its item is shared like a script give; its moves, emotes and recipes are shared already
// (pkp_coop_unlocks.cpp, pkp_coop_recipes.cpp). RE: modding-docs coop-v2/VISITOR-SERVICES-RE.md section 10.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <cstring>
#include <deque>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::catchup {

namespace {

constexpr const char* kTag = "pkp";

// The name
constexpr uint64_t kNameSlot = 0x741799, kNameSlotSize = 0x19;   // save root + L * 0x19
constexpr uint64_t kNameCopy = 0x74187c, kNameLang = 0x741898, kNameCopyStride = 0x20;
constexpr uint64_t kCopyText = 0x1122b50;                         // (dst, size, src)
constexpr uint64_t kKeyboardStart = 0x11314b0, kKeyboardBusy = 0x11316d0, kKeyboardOk = 0x11316e0, kKeyboardEnd = 0x1131680;
constexpr uint32_t kKeyboardName = 6;
constexpr uint64_t kTextOf = 0x248338;
constexpr uint32_t kTextKind = 0x25, kTextName = 2, kArgNone = 0xffffffffu;
constexpr size_t kTextArgs = 0x28, kArgSize = 0x88, kArgCount = 6;
constexpr uint64_t kAutosaveStep = 0x3bb5e0, kAutosaveDelay = 0x3bb5e4, kAutosaveFlags = 0x3bb5e8;   // game state +
constexpr float kAutosaveSoon = 570.0f;
constexpr double kWatchName = 600.0;   // how long after command 0x44 this player's name slot is watched

// The items
constexpr uint64_t kGiveItems = 0x12c734;
constexpr uint32_t kGiveCommand = 0xbea;
constexpr uint16_t kItemMax = 0x11f6;
constexpr uint32_t kItemsPerMessage = 0xff;   // Items carries a u8 count: a larger give goes as several messages
constexpr uint32_t kGiveMax = 8 * kItemsPerMessage;   // a script give past this is taken as garbage and cut to it

enum class Prompt { None, Wanted, Open };

struct State {
    int remote = 0;   // > 0 while a received give runs: not shared again
    // the talker
    bool watching = false;
    double watch_since = 0.0;
    uint8_t name_before[kNameSlotSize] = {};
    // the other members
    Prompt prompt = Prompt::None;
    std::deque<std::pair<uint16_t, uint32_t>> gives;   // item, count; one give split over several messages is merged again
    alignas(16) uint8_t name[0x40] = {};
    alignas(16) uint8_t opts[0x20] = {};
    alignas(16) uint8_t text[kTextArgs + kArgCount * kArgSize] = {};
    alignas(16) uint8_t command[0x10] = {};
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return guest::ok(r, kNameLang + 9 * kNameCopyStride) ? r : 0;
}

uint32_t language() {
    const uint64_t p = guest::rd<uint64_t>(base() + layout::kLanguage);
    const uint32_t l = guest::ok(p, 4) ? guest::rd<uint32_t>(p) : 1;
    return l < 9 ? l : 1;
}

uint64_t name_slot(uint64_t root, uint32_t lang) { return root + kNameSlot + lang * kNameSlotSize; }

// The game's own header text of the name keyboard; 0 if none.
uint64_t name_header(::Ctx* c) {
    State& s = st();
    std::memset(s.text, 0, sizeof s.text);
    const uint64_t text = reinterpret_cast<uint64_t>(s.text);
    guest::wr<uint32_t>(text, kTextKind);
    guest::wr<uint8_t>(text + 8, kTextName);
    guest::wr<uint64_t>(text + 0x10, text + kTextArgs);
    guest::wr<uint64_t>(text + 0x20, kArgCount);
    for (size_t i = 0; i < kArgCount; ++i) guest::wr<uint32_t>(text + kTextArgs + i * kArgSize, kArgNone);
    return guest::call(c, base() + kTextOf, text);
}

bool open_prompt(::Ctx* c, uint64_t me) {
    State& s = st();
    const uint32_t lang = language();
    std::memset(s.name, 0, sizeof s.name);
    std::memset(s.opts, 0, sizeof s.opts);
    const uint64_t opts = reinterpret_cast<uint64_t>(s.opts);
    guest::wr<uint64_t>(opts, name_header(c));
    guest::wr<uint16_t>(opts + 0x18, 1);
    guest::wr<uint16_t>(opts + 0x1a, lang >= 1 && lang <= 5 ? 12 : 6);
    guest::wr<uint8_t>(opts + 0x1c, 1);
    guest::wr<uint8_t>(opts + 0x1f, 1);
    if (!(guest::call(c, base() + kKeyboardStart, kKeyboardName, reinterpret_cast<uint64_t>(s.name), kNameSlotSize, opts) & 1))
        return false;
    lock::hold(lock::Holder::Name, me);
    return true;
}

// An OK answer: the three writes of command 0x44, and its autosave request.
void write_name(::Ctx* c) {
    State& s = st();
    const uint64_t root = save_root();
    if (!root || !s.name[0]) return;
    const uint32_t lang = language();
    const uint64_t name = reinterpret_cast<uint64_t>(s.name);
    const uint64_t slot = name_slot(root, lang);
    std::memset(reinterpret_cast<void*>(slot), 0, kNameSlotSize);
    guest::call(c, base() + kCopyText, slot, kNameSlotSize, name);
    guest::call(c, base() + kCopyText, root + kNameCopy + lang * kNameCopyStride, kNameSlotSize, name);
    guest::wr<uint32_t>(root + kNameLang + lang * kNameCopyStride, lang);
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    if (guest::ok(game + kAutosaveStep, 0x10)) {
        const uint8_t flags = guest::rd<uint8_t>(game + kAutosaveFlags);
        const uint32_t step = guest::rd<uint32_t>(game + kAutosaveStep);
        if ((flags & 1) && (step < 1 || step > 5)) {
            guest::wr<uint8_t>(game + kAutosaveFlags, flags & ~uint8_t{4});
            guest::wr<float>(game + kAutosaveDelay, std::max(guest::rd<float>(game + kAutosaveDelay), kAutosaveSoon));
        }
    }
    NXLOG_I(kTag, "Co-op catch-up: this player's name is set");
}

void tick_prompt(::Ctx* c, bool in_game) {
    State& s = st();
    if (s.prompt == Prompt::Wanted) {
        const uint64_t me = local_player();
        if (!in_game || !me || (guest::call_n<0>(c, base() + kKeyboardBusy, {}) & 1)) return;
        if (open_prompt(c, me)) {
            s.prompt = Prompt::Open;
            NXLOG_I(kTag, "Co-op catch-up: asking this player's name");
        }
        return;
    }
    if (s.prompt != Prompt::Open || (guest::call_n<0>(c, base() + kKeyboardBusy, {}) & 1)) return;
    const bool ok = guest::call_n<0>(c, base() + kKeyboardOk, {}) & 1;
    guest::call_n<0>(c, base() + kKeyboardEnd, {});
    lock::release(lock::Holder::Name);
    if (ok) {
        write_name(c);
        s.prompt = Prompt::None;
    } else {
        s.prompt = Prompt::Wanted;   // the name is needed: asked again
    }
}

void tick_watch() {
    State& s = st();
    if (!s.watching) return;
    const uint64_t root = save_root();
    if (!root || qpc::seconds() - s.watch_since > kWatchName) {
        s.watching = false;
        return;
    }
    const uint8_t* now = reinterpret_cast<const uint8_t*>(name_slot(root, language()));
    if (!now[0] || std::memcmp(now, s.name_before, kNameSlotSize) == 0) return;
    s.watching = false;
    link::share(wire::Type::NamePrompt, wire::Writer(link::self()).take());
    NXLOG_I(kTag, "Co-op catch-up: name entered here; the other members are asked theirs");
}

void tick_gives(::Ctx* c) {
    State& s = st();
    if (s.gives.empty() || !local_player()) return;
    while (!s.gives.empty()) {
        const auto [item, count] = s.gives.front();
        s.gives.pop_front();
        std::memset(s.command, 0, sizeof s.command);
        const uint64_t cmd = reinterpret_cast<uint64_t>(s.command);
        guest::wr<uint32_t>(cmd, kGiveCommand);
        guest::wr<uint16_t>(cmd + 4, item);
        guest::wr<uint32_t>(cmd + 8, count);
        ++s.remote;
        guest::call(c, base() + kGiveItems, cmd);
        --s.remote;
        NXLOG_I(kTag, "Co-op catch-up: item %u x%u given here too", item, count);
    }
}

}  // namespace

// A give that ran on this process for its own player (a script's, or a reward list's): every other member gets it too.
void share_give(uint16_t item, uint32_t count) {
    if (st().remote || purchases::replaying() || !link::in_session()) return;
    if (item < 1 || item > kItemMax || !count) return;
    if (count > kGiveMax) NXLOG_W(kTag, "Co-op catch-up: item %u x%u given here; shared as x%u", item, count, kGiveMax);
    const uint32_t total = std::min(count, kGiveMax);
    for (uint32_t left = total; left;) {
        const uint32_t part = std::min(left, kItemsPerMessage);
        link::share(wire::Type::Items, wire::Writer(link::self()).u16(item).u8(static_cast<uint8_t>(part)).take());
        left -= part;
    }
    NXLOG_I(kTag, "Co-op catch-up: item %u x%u given here, shared", item, total);
}

int on_name_entry(::Ctx*) {
    State& s = st();
    const uint64_t root = save_root();
    if (!link::in_session() || !root) return 0;
    std::memcpy(s.name_before, reinterpret_cast<const void*>(name_slot(root, language())), kNameSlotSize);
    s.watching = true;
    s.watch_since = qpc::seconds();
    return 0;
}

int on_script_items(::Ctx* c) {
    if (!guest::ok(c->x[0], 0xc)) return 0;
    share_give(guest::rd<uint16_t>(c->x[0] + 4), guest::rd<uint32_t>(c->x[0] + 8));
    return 0;
}

int on_reward_item(::Ctx* c) {
    if (!guest::ok(c->x[1], 2) || !guest::ok(c->x[2], 2)) return 0;
    share_give(guest::rd<uint16_t>(c->x[1]), guest::rd<uint16_t>(c->x[2]));
    return 0;
}

void on_name_message(::Ctx*, int, uint8_t, wire::Reader&) {
    State& s = st();
    const uint64_t root = save_root();
    if (s.prompt != Prompt::None) return;
    if (root && guest::rd<uint8_t>(name_slot(root, language()))) {
        NXLOG_I(kTag, "Co-op catch-up: a name was entered; this player has one already");
        return;
    }
    s.prompt = Prompt::Wanted;
}

void on_items_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    const uint16_t item = r.u16();
    const uint8_t count = r.u8();
    if (!r.ok() || item < 1 || item > kItemMax || !count) return;
    auto& gives = st().gives;
    if (!gives.empty() && gives.back().first == item) {   // the next part of one give (a count past kItemsPerMessage): one line
        gives.back().second += count;
        return;
    }
    gives.emplace_back(item, count);
    notice::post(notice::name_of(origin) + " was given items in a talk. You got them too!");
}

std::string player_name() {
    const uint64_t root = save_root();
    if (!root) return {};
    const char* name = reinterpret_cast<const char*>(name_slot(root, language()));
    return std::string(name, strnlen(name, kNameSlotSize));
}

void tick(::Ctx* c, bool in_game) {
    tick_watch();
    tick_prompt(c, in_game);
    tick_gives(c);
}

void reset() {
    State& s = st();
    lock::release(lock::Holder::Name);
    s = State{};
}

}  // namespace nxrt::games::pkp::coop::catchup
