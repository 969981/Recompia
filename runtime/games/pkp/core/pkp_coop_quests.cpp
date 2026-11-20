// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's quest authority (pkp_coop_internal.h). In a session the authority alone moves the group's quests on;
// a member asks it to, for the steps only the member's own player can make.
//
// Why. Every process runs the quest pump's condition passes on its own player, bag and copy of the world, and every state a process
// reached was shared and taken on by all (pkp_coop_story.cpp). So a member whose own save was further on (its world copy, its own
// records) closed the group's quest, and two processes stepping one quest raced. Now, in a session only, a member never moves a
// quest by itself: it takes the authority's states (the share and the digest, as before) and asks for the steps its player made.
//
// The flag setter 0x5d8774(S, id, value) is the only way a quest moves on: the pump steps a quest on its flag alone. Its direct
// callers (REA xrefs, the lift's argument setup), by return address:
//   0x119c0c  0x119920's C pass (state 1): the start condition (0x11e1bc(id, 0, 1, cond)) holds, or the quest has none     sets
//   0x119d94  0x119920's A pass (state 2): the list A timer passed its start kind's seconds                               sets
//   0x119f38  0x119920's B pass (state 3): the close condition (0x11e1bc(id, 0, 0, cond)) holds, or the quest has none     sets
//   0x11d644, 0x11d744  0x11d4a0: an event record (talk or scene end, types 2 / 0xd / 0x12 / 0x1c) matches a C / B quest   sets
//   0x115ae0  0x11598c: the list A quests tied to a request                                                               sets
//   0x1fbad8  0x1fb880: the new day (it skips a visitor)                                                                  sets
//   0x11bcd4, 0x11bdbc, 0x11c024  the pump's own steps 0x11bc60 / 0x11bd3c / 0x11bf9c                                     clear
// No quest effect sets one: 0x04 (0x11b270) and 0x14 (0x11b330) write their own bytes, 0x15 (0x1226c0) closes through 0x11bf9c,
// which clears. 0x5d8774 returns nothing and 0x11d4a0's result does not depend on it, so a set skipped at its entry changes
// nothing else in the caller.
//
// On a member in the session (the same condition the catch-up runs under), every set is skipped, except the catch-up's own
// (set_flag): the pump then steps only what the authority's states bring. A set that stands for the member's own player is asked
// of the authority instead (QuestFlagAsk: the quest and the member's state of it):
// - the C and B passes when the condition is one that reads the member's own player or records (F4): 1 / 0x13 the unique request
//   records, 5 the player stands in a box (0x117838: the player manager's +0x30 actor's position in 100-unit blocks, so not a clock;
//   tested a0..a2 <= pos < a3..a5), 9 (save root +0x73b264), 0xe (save root +0x7416dc), 0x1a (a bit at save root +0x98), 0x11 the
//   bag holds an item. Any other type and a quest with no condition are the authority's own pass's to decide: 6 / 8 / 0x10 test a
//   character's position in a box (0x121690 on 0x28cc60's character, 0x12159c), 3 / 4 / 7 / 0xa / 0xb / 0xc / 0x14 / 0x1b read
//   characters and the world, 0x15 the contents scheduler;
// - the event matches and 0x11598c (the event, the request, happened to this player);
// - not the A timer (the authority's own timer runs) and not the new day (the authority's own).
// The C and B passes call the setter each frame while the condition holds, so a quest is asked again every kAskEvery seconds at most.
// On the authority, an ask is applied when its own state of the quest is the member's (a flag means "the next step of this state")
// and the quest is in that state's list (QL = GM+0x3b9328, as 0x119920 reads it: C ids +4, A {id, timer} +0x64, B ids +0x1bc, 40 at
// most): its flag is set and its own pump makes the step, whose new state reaches every member through the share and the digest.
// An ask the authority cannot apply yet (its state is the member's, but the quest is not listed) waits for the session, retried
// each frame, until it is applied or the authority's state of the quest moves on (another way made that step): a talk or a scene
// asks once and cannot be made again, so an ask given up on would leave the quest where it is. One slot per quest (the authority
// is at one state of it, and only an ask for that state waits), so the waiting asks are bounded by the quest count and a new one
// never finds the list full. One for a state the authority is past or not at is dropped (a member ahead of the authority does not
// move the group's quest). On a member, an ask waits in one slot per quest too until the channel carries it: it is not dropped
// while the member is in the session, whatever the game's session bits say that frame. Single player, and a process outside a
// session, run every set as the game does.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
#include "core/pkp_layout.h"

#include <cstddef>
#include <cstring>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::quests {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kStory = 0x4a26a0;                         // save root + : m_story
constexpr uint64_t kStateOf = 0x5d85c0, kFlagOf = 0x5d86e4, kSetFlag = 0x5d8774;   // (S, id[, value])
constexpr uint64_t kConditionOf = 0x11e1bc;                   // (id, 0, set: 1 start / 0 close, out[16]): out +0 the type
constexpr uint64_t kGameMgr = 0x3561710, kGmQuestLists = 0x3b9328;
constexpr uint64_t kListC = 0x0, kListCIds = 0x4, kListA = 0x60, kListAEntries = 0x64, kListB = 0x1b8, kListBIds = 0x1bc;
constexpr uint64_t kListsEnd = 0x258;
constexpr uint32_t kListCap = 0x28;
constexpr uint32_t kQuestMax = 1000;
constexpr uint8_t kListed = 1, kStarted = 2, kComplete = 3;

// The setter's return addresses (main +), by what the set stands for.
constexpr uint64_t kStartPass = 0x119c0c, kTimerPass = 0x119d94, kClosePass = 0x119f38;
constexpr uint64_t kNewDay = 0x1fbad8;   // the event matches and 0x11598c are asked, as any other caller

// Condition types that read the member's own player or records.
constexpr uint8_t kPlayerTypes[] = {0x01, 0x05, 0x09, 0x0e, 0x11, 0x13, 0x1a};

constexpr double kAskEvery = 2.0;      // a polled condition that still holds: asked again at most this often

struct Ask {
    uint16_t id;
    uint8_t state;
};

struct State {
    bool own_set = false;                 // the catch-up's own set is running (set_flag)
    std::vector<Ask> asks;                // a member's asks not sent yet, one per quest (at most kQuestMax)
    double asked_at[kQuestMax] = {};      // per quest (id - 1): when it was last asked, and for which state
    uint8_t asked_state[kQuestMax] = {};
    uint8_t logged_state[kQuestMax] = {}; // per quest: the state a skipped set was last logged for
    uint8_t pending[kQuestMax] = {};      // the authority, per quest: the state of a waiting ask (0: none), and whose it is
    uint8_t pending_origin[kQuestMax] = {};
    uint32_t pending_count = 0;
    uint8_t refused_state[kQuestMax] = {};   // the authority, per quest: the asked state last logged as not applied (state + 1)
    alignas(8) uint8_t cond[0x20] = {};   // 0x11e1bc's out, guest-readable
};
State& st() {
    static State s;
    return s;
}

uint64_t story() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return guest::ok(r, kStory + 8) ? r + kStory : 0;
}

uint64_t quest_lists() {
    const uint64_t gm = guest::rd<uint64_t>(base() + kGameMgr);
    return guest::ok(gm, kGmQuestLists + kListsEnd) ? gm + kGmQuestLists : 0;
}

bool quest_id(uint32_t id) { return id - 1u < kQuestMax; }

uint8_t state_of(::Ctx* c, uint64_t s, uint32_t id) { return static_cast<uint8_t>(guest::call(c, base() + kStateOf, s, id)); }

// Quest id is in the list for its state (C for 1, A for 2, B for 3).
bool listed(uint64_t ql, uint32_t id, uint8_t state) {
    if (state == kListed) {
        const uint32_t n = guest::rd<uint32_t>(ql + kListC);
        for (uint32_t k = 0; k < n && k < kListCap; ++k)
            if (guest::rd<uint16_t>(ql + kListCIds + k * 2) == id) return true;
    } else if (state == kStarted) {
        const uint32_t n = guest::rd<uint32_t>(ql + kListA);
        for (uint32_t k = 0; k < n && k < kListCap; ++k)
            if ((guest::rd<uint32_t>(ql + kListAEntries + k * 8) & 0xffff) == id) return true;
    } else if (state == kComplete) {
        const uint32_t n = guest::rd<uint32_t>(ql + kListB);
        for (uint32_t k = 0; k < n && k < kListCap; ++k)
            if (guest::rd<uint16_t>(ql + kListBIds + k * 2) == id) return true;
    }
    return false;
}

// The condition type of quest id's start (set 1) or close (set 0) condition; 0 for none.
uint8_t condition_type(::Ctx* c, uint32_t id, uint32_t set) {
    State& s = st();
    std::memset(s.cond, 0, sizeof s.cond);
    guest::call(c, base() + kConditionOf, id, 0, set, reinterpret_cast<uint64_t>(s.cond));
    return s.cond[0];
}

bool player_type(uint8_t type) {
    for (uint8_t t : kPlayerTypes)
        if (t == type) return true;
    return false;
}

enum class Kind : uint8_t { Ask, Skip };

// What a set from this return address (main +) stands for on a member.
Kind kind_of(::Ctx* c, uint64_t from, uint32_t id, uint8_t* type) {
    *type = 0;
    if (from == kStartPass || from == kClosePass) {
        *type = condition_type(c, id, from == kStartPass ? 1 : 0);
        return player_type(*type) ? Kind::Ask : Kind::Skip;
    }
    if (from == kTimerPass || from == kNewDay) return Kind::Skip;
    return Kind::Ask;   // 0x11d4a0, 0x11598c, or a caller not known: the authority checks its state and lists
}

// Sets the flag on the authority when the quest is in its state's list; false when it is not listed (yet).
bool apply(::Ctx* c, uint32_t id, uint8_t state, uint8_t origin) {
    const uint64_t s = story(), ql = quest_lists();
    if (!s || !ql || !listed(ql, id, state)) return false;
    if (guest::call(c, base() + kFlagOf, s, id) & 1) return true;   // set already: the pump steps it
    set_flag(c, s, id);
    NXLOG_I(kTag, "Co-op quests: quest %u at state %u moved on here for node %u (its ask)", id, state, origin);
    journal::Rec(journal::Area::Quest, "ask-answer").in("node=%u id=%u state=%u", origin, id, state).out("flag set here, the pump steps it");
    return true;
}

// A member's asks to the authority, once the channel carries them. Kept while the member is in the session (a stage move can clear
// the game's session bits for a few frames); the session change empties them (on_session_change).
void send_asks() {
    State& s = st();
    if (s.asks.empty() || !link::in_session()) return;
    if (link::is_authority()) {
        s.asks.clear();
        return;
    }
    size_t sent = 0;
    for (const Ask& a : s.asks) {
        if (!link::to_authority(wire::Type::QuestFlagAsk, wire::Writer(link::self()).u16(a.id).u8(a.state).take())) break;
        ++sent;
    }
    s.asks.erase(s.asks.begin(), s.asks.begin() + static_cast<std::ptrdiff_t>(sent));
}

void clear_pending() {
    State& s = st();
    std::memset(s.pending, 0, sizeof s.pending);
    std::memset(s.pending_origin, 0, sizeof s.pending_origin);
    s.pending_count = 0;
}

// The authority's waiting asks, each frame: applied once the quest is listed; done once the quest's state here moved on.
void run_pending(::Ctx* c) {
    State& s = st();
    if (!s.pending_count) return;
    if (!link::is_authority() || !link::in_session()) {
        clear_pending();
        return;
    }
    const uint64_t story_obj = story();
    if (!story_obj) return;   // the save is not there this frame (a load): kept
    for (uint32_t i = 0; i < kQuestMax; ++i) {
        const uint8_t want = s.pending[i];
        if (!want) continue;
        const uint32_t id = i + 1;
        const uint8_t here = state_of(c, story_obj, id);
        bool done = here != want;   // stepped meanwhile (by its own pass, or another ask)
        if (done) {
            NXLOG_I(kTag, "Co-op quests: node %u's waiting ask for quest %u at state %u ends: it is at %u here", s.pending_origin[i], id,
                    want, here);
            journal::Rec(journal::Area::Quest, "ask-answer")
                .in("node=%u id=%u state=%u (waiting)", s.pending_origin[i], id, want)
                .out("dropped: at state %u here (another way made that step)", here);
        } else {
            done = apply(c, id, want, s.pending_origin[i]);
        }
        if (!done) continue;
        s.pending[i] = 0;
        --s.pending_count;
    }
}

}  // namespace

bool member_rules() {
    if (role() != CoopRole::Visit || !(session_bits() & layout::kSessionOnline) || !(session_bits() & layout::kSessionVisitor)) return false;
    return link::in_session() && !link::is_authority();
}

void set_flag(::Ctx* c, uint64_t s, uint32_t id) {
    State& own = st();
    own.own_set = true;
    guest::call(c, base() + kSetFlag, s, id, 1);
    own.own_set = false;
}

int on_quest_flag(::Ctx* c) {
    State& s = st();
    if (s.own_set || !(c->x[2] & 1) || !member_rules()) return 0;
    const uint64_t story_obj = story();
    const uint32_t id = static_cast<uint32_t>(c->x[1]) & 0xffff;
    if (!story_obj || c->x[0] != story_obj || !quest_id(id)) return 0;
    const uint64_t from = c->x[30] - base();
    const uint8_t state = state_of(c, story_obj, id);
    uint8_t type = 0;
    const Kind kind = kind_of(c, from, id, &type);
    const uint32_t i = id - 1;
    if (kind == Kind::Skip) {
        if (s.logged_state[i] != state) {
            s.logged_state[i] = state;
            NXLOG_I(kTag, "Co-op quests: quest %u's step at state %u (from main+0x%llx, condition type %u) left to the authority", id, state,
                    static_cast<unsigned long long>(from), type);
            journal::Rec(journal::Area::Quest, "flag-decision")
                .in("id=%u state=%u by=main+0x%llx condition type=0x%02x", id, state, static_cast<unsigned long long>(from), type)
                .out("skipped: %s", from == kTimerPass ? "the list A timer is the authority's"
                                    : from == kNewDay  ? "the new day is the authority's"
                                                       : "the condition reads the shared world (or there is none): the authority's pass decides")
                .note("logged once per state");
        }
        return 1;   // the set does not run: the pump steps this quest only on the authority's states
    }
    const double now = qpc::seconds();
    const bool again = s.asked_state[i] == state && now - s.asked_at[i] < kAskEvery;
    if (!again) {
        if (s.asked_state[i] != state) {
            NXLOG_I(kTag, "Co-op quests: quest %u's step at state %u made here (from main+0x%llx, condition type %u), asked of the authority",
                    id, state, static_cast<unsigned long long>(from), type);
            journal::Rec(journal::Area::Quest, "flag-decision")
                .in("id=%u state=%u by=main+0x%llx condition type=0x%02x", id, state, static_cast<unsigned long long>(from), type)
                .out("asked of the authority (QuestFlagAsk), the set itself skipped here")
                .note("logged once per state; asked again every %.0f s while it holds", kAskEvery);
        }
        s.asked_at[i] = now;
        s.asked_state[i] = state;
        bool queued = false;   // one per quest: a quest not sent yet is asked once, at its latest state
        for (Ask& a : s.asks)
            if (a.id == id) {
                a.state = state;
                queued = true;
            }
        if (!queued) s.asks.push_back({static_cast<uint16_t>(id), state});
    }
    return 1;
}

void on_ask_message(::Ctx* c, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint16_t id = r.u16();
    const uint8_t state = r.u8();
    const uint64_t story_obj = story();
    if (!r.ok() || !quest_id(id) || state < kListed || state > kComplete || !story_obj) return;
    const uint8_t here = state_of(c, story_obj, id);
    if (here != state) {
        if (s.refused_state[id - 1] != state + 1) {   // a held condition asks again every few seconds: logged once per state
            s.refused_state[id - 1] = static_cast<uint8_t>(state + 1);
            NXLOG_I(kTag, "Co-op quests: node %u asked for quest %u's step at state %u; it is at %u here, not applied", origin, id, state,
                    here);
            journal::Rec(journal::Area::Quest, "ask-answer")
                .in("node=%u id=%u state=%u", origin, id, state)
                .out("state %u here", here)
                .expect("state %u here", state)
                .refused("the authority is %s that state", here > state ? "past" : "not yet at");
        }
        return;
    }
    if (apply(c, id, state, origin)) return;
    uint8_t& slot = s.pending[id - 1];
    if (slot == state) return;   // waiting already
    if (!slot) ++s.pending_count;
    slot = state;   // the authority is at this state of the quest now: any other waiting state of it was done already
    s.pending_origin[id - 1] = origin;
    NXLOG_I(kTag, "Co-op quests: node %u's ask for quest %u at state %u waits: the quest is not listed here yet (kept for the session)",
            origin, id, state);
    journal::Rec(journal::Area::Quest, "ask-answer")
        .in("node=%u id=%u state=%u", origin, id, state)
        .out("waits: not in the state's list here yet (retried each frame for the session)");
}

void tick(::Ctx* c) {
    send_asks();
    run_pending(c);
}

void on_session_change() {
    State& s = st();
    s.asks.clear();
    clear_pending();
    std::memset(s.asked_state, 0, sizeof s.asked_state);
    std::memset(s.logged_state, 0, sizeof s.logged_state);
    std::memset(s.refused_state, 0, sizeof s.refused_state);
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::quests
