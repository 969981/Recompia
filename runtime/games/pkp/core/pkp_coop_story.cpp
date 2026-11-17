// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: seamless co-op's shared story (pkp_coop_internal.h). Every player runs the story; its state is kept equal on every
// process by sharing what the quest pump cannot derive on its own.
//
// Permissions. The permission mask (GameMgr+0x8c, bits 0..31; +0x90 the session bits) is built by 0xb0f60 on hosting, on every stage
// entry and when the session ends. Its first instruction after the frame, 0xb0f74 (`mov x19, x0`), still has the arguments in x1..x4
// (online, host, Spectator, special): Spectator and special are cleared, so every stage type takes its normal pattern. In the story-area
// online block, at 0xb1030 (`movk x13, #1, lsl #32`), x13 holds the pattern's bits 0..31 and the movk adds bit 32: it becomes bits 0..30,
// the offline single player's set (story on), on the authority; the members get the same without bit 23, the shop's visitor lock. The
// role bits 33 / 34 stay as the game sets them: the game's own replication splits on them. Link Play's Interact everywhere hooks 0xb1030
// too and runs first (pkp_game.cpp), so this value is the one kept.
//
// Quests. m_story S = save root + 0x4a26a0: per quest id 1..1000 a state (read 0x5d85c0, written 0x5d865c: 1 listed, 2 started,
// 3 complete, 4 closed) and a flag (read 0x5d86e4, written 0x5d8774: the next step's condition is met). The quest pump (0x11b600, every
// frame) moves a quest on its flag alone: flag -> start / complete / close, which writes the state and clears the flag through the two
// setters and runs the quest's effect commands. In 2.0.1 the state setter has three callers, all inside those pump steps, and every flag
// clear is one of theirs; every other flag write sets it (condition checker, scripts, events). So only flag sets are shared: each
// process's own pump then makes the same state writes and runs the same effects, and a state never arrives ahead of its flag.
// A set is an edge whose meaning depends on the state it was made in, so a message carries that state and a receiver sets the flag only
// in the same state with its flag clear. A receiver still behind waits for its pump; a receiver past it (it did that step itself, which
// is normal: every process checks the conditions) drops the message.
//
// Effects. A quest step's effect commands go through 0x11b214(cmd), one command each (u8 kind at +0; kinds 1..0x39 act, any other
// returns with nothing done). Every process runs them, since every process runs the pump. Personal kinds (the player's abilities,
// bag, name, profile: 0x01 0x02 0x03 0x19 0x1e 0x1f 0x30) and the story kinds (quest flags and states: 0x04 0x14 0x15 0x27 0x29 0x2d)
// run on every process; every other kind changes the shared world, which the game's own
// replication brings from the authority to the members, so on a member in the session it returns at once, as an unknown kind does.
//
// Abilities (unlocks such as the Pokedex and its menu entries). The bits live at save root + 0x4a26a8 (id 1..0xf9: byte id >> 3, bit
// id & 7) and have four writers: the quest effects 0x01 (0x121bb8) and 0x19 (0x1227fc), event command 0x10c0d0 and script command
// 0x12c35c (the Pokedex talk sets ids 17 and 76 through one of the latter two). So the bits themselves are
// watched, not a writer: once a frame in the session, every bit that turned on here since the last look is shared as an effect 0x19
// "set" command, and a receiver whose bit is off runs 0x1227fc with it. The first look in a session only takes the baseline: what a
// member already had before joining is not sent. A bit turned off is not shared (no writer clears one in the story; the saves seen only
// grow).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstring>
#include <deque>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::story {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kSpectatorReg = 3, kSpecialReg = 4, kPatternReg = 13;
constexpr uint64_t kAuthorityPattern = 0x7fffffffull;   // bits 0..30: the offline single player's permissions, story on
constexpr uint64_t kMemberPattern = 0x7f7fffffull;      // the same without bit 23: no shop visitor lock

constexpr uint64_t kStory = 0x4a26a0, kAbilities = 0x4a26a8;                       // save root +
constexpr uint64_t kStateOf = 0x5d85c0, kFlagOf = 0x5d86e4, kSetFlag = 0x5d8774;   // (S, id[, value])
constexpr uint64_t kPumpStateWrites[] = {0x11bcc0, 0x11bda8, 0x11c00c};             // return sites of the state setter's callers
constexpr uint32_t kQuestMax = 1000;
constexpr double kWaitForPump = 10.0;   // a flag whose state the receiver has not reached is kept this long

constexpr uint8_t kEveryProcessEffects[] = {0x01, 0x02, 0x03, 0x19, 0x1e, 0x1f, 0x30,    // personal
                                            0x04, 0x14, 0x15, 0x27, 0x29, 0x2d};   // story
constexpr uint8_t kEffectFirst = 0x01, kEffectLast = 0x39;

constexpr uint8_t kAbilitySet = 0x01, kAbilityToggle = 0x19;
constexpr uint64_t kAbilitySetFn = 0x121bb8, kAbilityToggleFn = 0x1227fc;
constexpr unsigned kAbilitySlots = 8;   // command 0x01: u16 ids at +2, +4, .. +0x10
constexpr unsigned kAbilityBytes = 0x20;   // ids 0..0xff, one bit each

struct Flag {
    uint16_t quest;
    uint8_t state;
    double since;
};

struct State {
    int remote = 0;                    // > 0 while a received change is applied: the hooks do not share it again
    std::deque<Flag> flags;            // received flag sets, in the authority's order
    bool logged_state = false;
    alignas(8) uint8_t command[0x20] = {};   // a received ability command, guest-readable
    uint8_t seen[kAbilityBytes] = {};        // the ability bits at the last look; a bit on now and off here turned on since
    bool have_seen = false;                  // false until the first look of a session (the baseline)
};
State& st() {
    static State s;
    return s;
}

struct Remote {
    Remote() { ++st().remote; }
    ~Remote() { --st().remote; }
};

uint64_t save_root() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return guest::ok(r, kAbilities + kAbilityBytes) ? r : 0;
}
uint64_t story() {
    const uint64_t r = save_root();
    return r ? r + kStory : 0;
}

uint8_t quest_state(::Ctx* c, uint64_t s, uint32_t id) { return static_cast<uint8_t>(guest::call(c, base() + kStateOf, s, id)); }
bool quest_flag(::Ctx* c, uint64_t s, uint32_t id) { return guest::call(c, base() + kFlagOf, s, id) & 1; }

bool ability_id(uint16_t v) { return static_cast<uint8_t>(v - 1) < 0xf9; }
bool ability_on(uint64_t root, uint16_t v) {
    return guest::rd<uint8_t>(root + kAbilities + ((v & 0xff) >> 3)) & (1u << (v & 7));
}
// The command would change an ability bit on this process.
bool ability_changes(uint64_t root, const uint8_t* cmd) {
    uint16_t v[kAbilitySlots];
    std::memcpy(v, cmd + 2, sizeof v);
    if (cmd[0] == kAbilityToggle) return ability_id(v[0]) && ability_on(root, v[0]) != (v[1] != 0);
    for (uint16_t id : v)
        if (ability_id(id) && !ability_on(root, id)) return true;
    return false;
}

// Applies the received flag sets whose state this process has reached; per quest, in order.
void apply_flags(::Ctx* c) {
    State& s = st();
    const uint64_t story_obj = story();
    if (!story_obj || s.flags.empty()) return;
    const double now = qpc::seconds();
    bool blocked[kQuestMax + 1] = {};
    for (auto it = s.flags.begin(); it != s.flags.end();) {
        if (blocked[it->quest]) {
            ++it;
            continue;
        }
        const uint8_t state = quest_state(c, story_obj, it->quest);
        if (state == it->state) {
            if (!quest_flag(c, story_obj, it->quest)) {
                Remote remote;
                guest::call(c, base() + kSetFlag, story_obj, it->quest, 1);
                NXLOG_I(kTag, "Co-op story: quest %u flag from another member set here (state %u)", it->quest, state);
            }
        } else if (state < it->state && now - it->since < kWaitForPump) {
            blocked[it->quest] = true;
            ++it;
            continue;
        } else {
            NXLOG_I(kTag, "Co-op story: quest %u flag of state %u DROPPED, this process is at state %u%s", it->quest, it->state, state,
                    state < it->state ? " (waited too long)" : "");
        }
        it = s.flags.erase(it);
    }
}

// Shares every ability bit that turned on here since the last look, as an effect 0x19 "set" command (u16 id at +2, u16 1 at +4).
void share_new_abilities() {
    State& s = st();
    const uint64_t root = save_root();
    if (!root || !link::in_session()) return;
    uint8_t now[kAbilityBytes];
    std::memcpy(now, reinterpret_cast<const void*>(root + kAbilities), sizeof now);
    if (!s.have_seen) {
        std::memcpy(s.seen, now, sizeof now);
        s.have_seen = true;
        return;
    }
    for (unsigned byte = 0; byte < kAbilityBytes; ++byte) {
        const uint8_t turned_on = now[byte] & ~s.seen[byte];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint16_t id = static_cast<uint16_t>(byte * 8 + bit);
            if (!(turned_on & (1u << bit)) || !ability_id(id)) continue;
            uint8_t cmd[wire::kEffectSize] = {kAbilityToggle};
            const uint16_t args[2] = {id, 1};
            std::memcpy(cmd + 2, args, sizeof args);
            link::share(wire::Type::Ability, wire::Writer(link::self()).bytes(cmd, sizeof cmd).take());
            NXLOG_I(kTag, "Co-op story: ability %u turned on here, shared", id);
        }
    }
    std::memcpy(s.seen, now, sizeof now);
}

}  // namespace

int on_mask_start(::Ctx* c) {
    c->x[kSpectatorReg] = 0;
    c->x[kSpecialReg] = 0;
    return 0;
}

int on_mask_pattern(::Ctx* c) {
    c->x[kPatternReg] = role() == CoopRole::Host ? kAuthorityPattern : kMemberPattern;
    return 0;
}

int on_quest_state(::Ctx* c) {
    State& s = st();
    const uint64_t from = c->x[30] - base();
    for (uint64_t site : kPumpStateWrites)
        if (from == site) return 0;
    if (!s.logged_state && c->x[0] == story()) {
        s.logged_state = true;
        NXLOG_W(kTag, "Co-op story: quest %u's state was written outside the quest pump (from main+0x%llx); it is not shared",
                static_cast<unsigned>(c->x[1] & 0xffff), static_cast<unsigned long long>(from));
    }
    return 0;
}

int on_quest_flag(::Ctx* c) {
    State& s = st();
    if (s.remote || !(c->x[2] & 1) || !link::in_session()) return 0;
    const uint64_t story_obj = story();
    const uint32_t id = static_cast<uint32_t>(c->x[1]) & 0xffff;
    if (!story_obj || c->x[0] != story_obj || id - 1 >= kQuestMax || quest_flag(c, story_obj, id)) return 0;
    const uint8_t state = quest_state(c, story_obj, id);
    link::share(wire::Type::StoryFlag, wire::Writer(link::self()).u16(static_cast<uint16_t>(id)).u8(state).take());
    NXLOG_I(kTag, "Co-op story: quest %u flag set here (state %u), shared", id, state);
    return 0;
}

int on_quest_effect(::Ctx* c) {
    // The game's own session bits, not the channel: a member in the host's world never runs a world effect, even before its channel
    // has learnt the authority.
    if (!guest::ok(c->x[0], 1)) return 0;
    const uint8_t kind = guest::rd<uint8_t>(c->x[0]);
    if (kind < kEffectFirst || kind > kEffectLast) return 0;
    const bool member = role() == CoopRole::Visit && (session_bits() & layout::kSessionVisitor);
    bool every_process = false;
    for (uint8_t every : kEveryProcessEffects) every_process |= kind == every;
    if (!member || every_process) {
        if (link::in_session()) NXLOG_I(kTag, "Co-op story: effect 0x%02x run here", kind);
        return 0;
    }
    NXLOG_I(kTag, "Co-op story: world effect 0x%02x left to the authority", kind);
    return 1;   // x0 is still the command, as on the game's own "nothing to do" return
}

void on_flag_message(::Ctx* c, int, uint8_t, wire::Reader& r) {
    const uint16_t quest = r.u16();
    const uint8_t state = r.u8();
    if (!r.ok() || quest - 1u >= kQuestMax) return;
    if (const uint64_t story_obj = story())
        NXLOG_I(kTag, "Co-op story: quest %u flag received (state %u there, %u here)", quest, state, quest_state(c, story_obj, quest));
    st().flags.push_back({quest, state, qpc::seconds()});
    apply_flags(c);
}

void on_ability_message(::Ctx* c, int, uint8_t, wire::Reader& r) {
    State& s = st();
    std::memset(s.command, 0, sizeof s.command);
    r.bytes(s.command, wire::kEffectSize);
    const uint64_t root = save_root();
    if (!r.ok() || !root || (s.command[0] != kAbilitySet && s.command[0] != kAbilityToggle) || !ability_changes(root, s.command)) return;
    guest::call(c, base() + (s.command[0] == kAbilitySet ? kAbilitySetFn : kAbilityToggleFn), reinterpret_cast<uint64_t>(s.command));
    if (s.have_seen) std::memcpy(s.seen, reinterpret_cast<const void*>(root + kAbilities), kAbilityBytes);   // received: not sent back
    NXLOG_I(kTag, "Co-op story: ability command 0x%02x from another member applied here", s.command[0]);
}

void tick(::Ctx* c) {
    apply_flags(c);
    share_new_abilities();
}

void on_session_change() {
    st().flags.clear();
    st().have_seen = false;
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::story
