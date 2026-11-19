// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's shared story (pkp_coop_internal.h). Every player runs the story; its state is kept equal on every
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
// setters and runs the quest's effect commands. In 2.0.1 the state setter has three callers, all inside those pump steps.
// Whoever moves a quest on moves it on for everyone: every process shares its quests' states (the low byte of each quest word, save
// root + layout::quest_word(id); the DLC ids 801..1000 have their own run, as the game keeps them), all of them when it joins a
// session or another member does, then every state that rose since the last look.
// A process behind a received state catches up through its own pump: it sets the quest's flag in its own state, the pump makes the
// step (state write, effects), and the next frame sets it again until the state is reached. So a step only one player could make (a
// quest active for one of them, a condition met in one process: the grass built by another player, a talk started by another) is
// still made on every process, with that process's own effects and rewards. Sharing flag sets instead (as before) lost every step
// whose receiver was not in exactly the sender's state at that moment.
// The catch-up moves one quest at a time, the lowest id first, and waits for the pump to step it before it flags the next, so a quest
// another quest's effect closes (effect 0x15) has a chance to be closed by that effect before it is pumped itself (id order is not
// known to follow the story's order; it is only the usual case). The pump runs every frame, so a step it will make comes within
// frames: a flag it leaves for kStall seconds (the quest is in none of its lists: another stage, a prerequisite unmet, a cutscene) is
// passed over, for kStallSkip seconds doubled on each stall of that quest up to kStallSkipMax, so quests that cannot move never hold up
// the ones that can. The flag stays set: the pump takes it whenever the quest can move. It runs only while the game's own session is
// up (on a member: in the host's world), the same condition the world effects' skip uses.
// The safety net (StoryDigest): every kDigestEvery seconds the authority sends every member its whole story (every quest's state and
// the ability bits). A member behind it catches up as above; a member ahead of it sends each quest it is ahead on again, once per
// state (a rise lost on the way is sent once more; one the authority's pump cannot step yet is not sent every digest); ability bits
// are merged both ways. Received states are taken only in the range a quest can have (2..4 shared, 0..4 in a digest). So two saves
// that drifted apart for any reason are brought together within
// seconds, and a member who joins late gets the abilities unlocked before it came.
//
// Effects. A quest step's effect commands go through 0x11b214(cmd), one command each (u8 kind at +0; kinds 1..0x39 act, any other
// returns with nothing done). Every process runs them, since every process runs the pump. Personal kinds (the player's abilities,
// bag, name, profile: 0x01 0x02 0x03 0x19 0x1e 0x1f 0x30) and the story kinds (quest flags and states: 0x04 0x14 0x15 0x27 0x29 0x2d)
// run on every process; every other kind changes the shared world, which the game's own
// replication brings from the authority to the members, so on a member in the session it returns at once, as an unknown kind does.
//
// Abilities (unlocks such as the creature list and its menu entries). The bits live at save root + 0x4a26a8 (id 1..0xf9: byte id >> 3, bit
// id & 7) and have four writers: the quest effects 0x01 (0x121bb8) and 0x19 (0x1227fc), event command 0x10c0d0 and script command
// 0x12c35c (the creature list talk sets ids 17 and 76 through one of the latter two). So the bits themselves are
// watched, not a writer: once a frame in the session, every bit that turned on here since the last look is shared as an effect 0x19
// "set" command, and a receiver whose bit is off runs 0x1227fc with it. The first look in a session only takes the baseline; what a
// member had before joining reaches the others through the digest (below), which merges the bits both ways.
// Some steps take an ability away (quest 664's effect 0x19 clears 99): 0x1227fc with its +4 value 0 and the story-line step action
// 0x10c0d0 (record *(x1 + 8)) with its +6 value 0 clear the bit. The union above would bring such a bit back from any process that
// still has it, so a cleared id is kept in a set for the co-op run (entry hooks on both clears, in a session only), shared
// (AbilityCleared) and cleared on every member, and never shared or applied again: not as a bit that turned on, not by the digest's
// merge (a member that still has it clears it instead), not from an ability command. The set is sent again whole when this process
// joins or another member does. A later step can give it back (quest 680 sets 99 again): a set the game makes here (0x1227fc with
// +4 nonzero, 0x10c0d0 with +6 nonzero, a slot of effect 0x01 at the dispatcher) takes the id out of this process's set; co-op's own
// applies never set an id in the set, so every set those hooks see is the game's. Every process runs that step through its own pump
// (the catch-up), so each takes the id out of its own set. It lives in memory only; the event command 0x12c35c (value 2 clears) is not
// watched.
//
// Wild appearances. A wild creature that comes to a habitat once (Bulbasaur to the first tall grass after Squirtle: quest 7 waits
// for it to move in, then its close removes the entry with effect 0x33) is armed in m_sEventPopWildPokemon (stage data +0x972ba0,
// 10 slots) by 0x1240dc(s0 delay, w0 habitat id, w1 creature data id, w2 bit 0 add). Its callers are quest effect 0x33 (0x123008,
// a world effect: the authority's) and event-script command 0xc1e, which the talks run on the process of the player who talks, so
// a member's talk armed the appearance only in that member's own stage data, which no replication carries: the group's world never
// had the creature and the quest waited for ever. A member's call in the session is sent to the authority (WildPop), which makes
// the same call through effect 0x33's handler; the add keeps one slot per creature (0x5f9a3c overwrites the creature's slot), so
// an appearance armed twice is armed once. The member's own call still runs, as before.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::story {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kSpectatorReg = 3, kSpecialReg = 4, kPatternReg = 13;
constexpr uint64_t kAuthorityPattern = 0x7fffffffull;   // bits 0..30: the offline single player's permissions, story on
constexpr uint64_t kMemberPattern = 0x7f7fffffull;      // the same without bit 23: no shop visitor lock

constexpr uint64_t kStory = 0x4a26a0, kAbilities = layout::kSaveAbilities;         // save root +
constexpr uint64_t kFlagOf = 0x5d86e4, kSetFlag = 0x5d8774;   // (S, id[, value]); the state is read from the quest words
constexpr uint64_t kPumpStateWrites[] = {0x11bcc0, 0x11bda8, 0x11c00c};             // return sites of the state setter's callers
constexpr uint32_t kQuestMax = layout::kQuestWordCount;   // ids 1..1000; word of id at save root + layout::quest_word(id), state in its low byte
constexpr uint8_t kListed = 1;                            // a quest no one has started: not sent
constexpr uint8_t kStarted = 2, kClosed = 4;              // the states a quest can be shared at; a received state past kClosed
                                                          // is one no pump step reaches, so the catch-up would flag it for ever
constexpr uint32_t kFirstTalk = 2;   // a new world: quest 1 closes on entering the first room, quest 2 at the first talk (run P0RE-2)

constexpr uint8_t kEveryProcessEffects[] = {0x01, 0x02, 0x03, 0x19, 0x1e, 0x1f, 0x30,    // personal
                                            0x04, 0x14, 0x15, 0x27, 0x29, 0x2d};   // story
constexpr uint8_t kEffectFirst = 0x01, kEffectLast = 0x39;

constexpr uint8_t kAbilitySet = 0x01, kAbilityToggle = 0x19;
constexpr uint64_t kAbilitySetFn = 0x121bb8, kAbilityToggleFn = 0x1227fc;
constexpr unsigned kAbilitySlots = 8;   // command 0x01: u16 ids at +2, +4, .. +0x10
constexpr unsigned kAbilityBytes = wire::kAbilityBytes;
constexpr double kDigestEvery = 5.0;                 // the authority's whole story to every member
constexpr double kStall = 1.0, kStallSkip = 5.0, kStallSkipMax = 120.0;   // a catch-up flag the pump leaves: passed over
constexpr double kNoticeGap = 4.0;                   // at most one "moved the story on" line per this many seconds
constexpr uint8_t kComplete = 3;
constexpr uint64_t kWildPopEffectFn = 0x123008;   // effect 0x33's handler: the command's fields -> the call 0x1240dc
constexpr uint8_t kWildPopEffect = 0x33;
constexpr size_t kWildPopQueue = 16;   // a member's appearances waiting for the channel

// The abilities a line names; any other is "something new".
struct AbilityName {
    uint16_t id;
    const char* name;
};
constexpr AbilityName kAbilityNames[] = {{17, "the creature list"}, {76, "the creature list"}, {10, "Water Gun"}, {55, "Water Gun"}, {101, "Water Gun"}};

struct State {
    uint8_t sent[kQuestMax] = {};      // per quest (id - 1): the state last shared from here
    uint8_t target[kQuestMax] = {};    // per quest: the highest state another member has, which this process catches up to
    uint8_t logged[kQuestMax] = {};    // per quest: the target the catch-up was last logged for
    uint8_t resent[kQuestMax] = {};    // per quest: the state sent again because a digest showed the authority behind it; each state
                                       // is sent again once, so a quest the authority cannot step does not resend every digest
    bool send_all = true;              // the next look shares every started quest (joined, or another member joined)
    double stall_until[kQuestMax] = {};   // per quest: passed over by the catch-up until then
    uint8_t stalls[kQuestMax] = {};       // per quest: how often the pump left its flag (the skip doubles each time)
    uint32_t stepping = 0;                // the quest the catch-up flagged last (id), and its state then
    uint8_t stepping_state = 0;
    double stepping_since = 0.0;
    double digest_at = 0.0, story_line_at = -1e9;
    std::string last_ability_line;
    bool logged_state = false;
    alignas(8) uint8_t command[0x20] = {};   // a received ability command, guest-readable
    uint8_t seen[kAbilityBytes] = {};        // the ability bits at the last look; a bit on now and off here turned on since
    bool have_seen = false;                  // false until the first look of a session (the baseline)
    struct WildPop {
        uint16_t habitat, creature;
        int16_t delay;
        uint8_t add;
    };
    std::vector<WildPop> wild_pops;          // a member's appearances not sent yet (the channel was not up)
    alignas(8) uint8_t pop_command[wire::kEffectSize] = {};   // a received appearance as an effect 0x33 command, guest-readable
    uint8_t cleared[kAbilityBytes] = {};     // the abilities a step took away in this co-op run (bit per id): never given back
    std::vector<uint8_t> cleared_new;        // cleared here and not sent yet
    bool cleared_send_all = false;           // the next send is the whole set (joined, or another member joined)
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return guest::ok(r, kAbilities + kAbilityBytes) ? r : 0;
}
uint64_t story() {
    const uint64_t r = save_root();
    return r ? r + kStory : 0;
}

// The save root, when every quest word (ids 1..800 and the DLC run 801..1000) is readable; read through state_at.
uint64_t quest_words() {
    const uint64_t r = save_root();
    return r && guest::ok(r + layout::kQuestWords, layout::kQuestWordsEnd - layout::kQuestWords) ? r : 0;
}

// Quest (index + 1)'s state, from its word where the game keeps it (0x5d85c0's mapping).
uint8_t state_at(uint64_t words, uint32_t i) { return static_cast<uint8_t>(guest::rd<uint16_t>(words + layout::quest_word(i + 1))); }

bool quest_flag(::Ctx* c, uint64_t s, uint32_t id) { return guest::call(c, base() + kFlagOf, s, id) & 1; }

bool ability_id(uint16_t v) { return static_cast<uint8_t>(v - 1) < 0xf9; }
bool ability_cleared(uint16_t v) { return st().cleared[(v & 0xff) >> 3] & (1u << (v & 7)); }
// Puts the id in the cleared set; true when it was not in it yet.
bool mark_cleared(uint16_t v) {
    uint8_t& byte = st().cleared[(v & 0xff) >> 3];
    const uint8_t bit = static_cast<uint8_t>(1u << (v & 7));
    if (byte & bit) return false;
    byte |= bit;
    return true;
}
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

// Shares the quests whose state rose here since the last look (every started one after a join), as (u16 id, u8 state) pairs.
void share_states() {
    State& s = st();
    const uint64_t words = quest_words();
    if (!words || !link::in_session()) return;
    const bool all = s.send_all;
    s.send_all = false;
    std::vector<uint8_t> pairs;
    uint16_t n = 0;
    for (uint32_t i = 0; i < kQuestMax; ++i) {
        const uint8_t state = state_at(words, i);
        if (state <= kListed || (!all && state <= s.sent[i])) continue;
        s.sent[i] = state;
        if (!all && state <= s.target[i]) continue;   // caught up to another member's state: they have it already
        const uint16_t id = static_cast<uint16_t>(i + 1);
        pairs.push_back(static_cast<uint8_t>(id));
        pairs.push_back(static_cast<uint8_t>(id >> 8));
        pairs.push_back(state);
        ++n;
        if (!all)
            NXLOG_I(kTag, "Co-op story: quest %u %s state %u here, shared", id, s.resent[i] == state ? "still ahead of the authority at" : "reached",
                    state);
    }
    if (!n) return;
    link::share(wire::Type::StoryStates, wire::Writer(link::self()).u16(n).bytes(pairs.data(), pairs.size()).take());
    if (all) NXLOG_I(kTag, "Co-op story: the states of %u started quests shared (a join)", static_cast<unsigned>(n));
}

// The game's own session is up, and on a member it is in the host's world: the condition the world effects' skip uses too.
bool story_shared_here() {
    const uint8_t bits = session_bits();
    if (!(bits & layout::kSessionOnline) || !link::in_session()) return false;
    return role() == CoopRole::Host || (bits & layout::kSessionVisitor);
}

// Moves the lowest quest that is behind another member's state one pump step on (its flag set in this process's own state), and
// waits for the pump to make that step before it flags another.
void catch_up(::Ctx* c) {
    State& s = st();
    const uint64_t words = quest_words(), story_obj = story();
    if (!words || !story_obj || !story_shared_here()) return;
    const double now = qpc::seconds();
    for (uint32_t i = 0; i < kQuestMax; ++i) {
        if (!s.target[i] || now < s.stall_until[i]) continue;
        const uint8_t state = state_at(words, i);
        if (state >= s.target[i]) continue;
        const uint32_t id = i + 1;
        if (s.stepping != id || s.stepping_state != state) {   // a new quest, or the pump made the last step
            s.stepping = id;
            s.stepping_state = state;
            s.stepping_since = now;
        } else if (now - s.stepping_since > kStall) {
            const double skip = std::min(kStallSkipMax, kStallSkip * static_cast<double>(1u << std::min<unsigned>(s.stalls[i], 5)));
            s.stall_until[i] = now + skip;
            s.stepping = 0;
            if (s.stalls[i]++ == 0)
                NXLOG_W(kTag, "Co-op story: quest %u stays at state %u here (the pump does not step it yet; %u wanted); passed over for now",
                        id, state, s.target[i]);
            continue;
        }
        if (s.logged[i] != s.target[i]) {
            s.logged[i] = s.target[i];
            NXLOG_I(kTag, "Co-op story: quest %u catching up here, state %u -> %u", id, state, s.target[i]);
        }
        if (!quest_flag(c, story_obj, id)) guest::call(c, base() + kSetFlag, story_obj, id, 1);
        return;   // one quest at a time
    }
}

// The authority's whole story to every member (StoryDigest).
void send_digest() {
    State& s = st();
    const uint64_t words = quest_words(), root = save_root();
    if (!words || !root || !link::is_authority() || !story_shared_here()) return;
    const double now = qpc::seconds();
    if (now - s.digest_at < kDigestEvery) return;
    s.digest_at = now;
    uint8_t states[kQuestMax];
    for (uint32_t i = 0; i < kQuestMax; ++i) states[i] = state_at(words, i);
    link::to_members(wire::Type::StoryDigest, wire::Writer(link::self())
                                                  .u16(static_cast<uint16_t>(kQuestMax))
                                                  .bytes(states, kQuestMax)
                                                  .bytes(reinterpret_cast<const uint8_t*>(root + kAbilities), kAbilityBytes)
                                                  .take());
}

const char* ability_name(uint16_t id) {
    for (const AbilityName& a : kAbilityNames)
        if (a.id == id) return a.name;
    return nullptr;
}

// A line naming what another member's player unlocked; the same line twice in a row (the creature list is two ids) is said once.
void ability_line(uint8_t origin, uint16_t id) {
    State& s = st();
    const char* name = ability_name(id);
    const std::string who = notice::name_of(origin);
    const std::string text = name ? who + " unlocked " + name + ". You have it too!" : who + " unlocked something new. You have it too!";
    if (text == s.last_ability_line) return;
    s.last_ability_line = text;
    notice::post(text);
}

// The ability bit applied here as received, through the quest effect 0x19 (set): marked seen, so it is not sent back.
void apply_ability(::Ctx* c, uint64_t root, uint16_t id) {
    State& s = st();
    std::memset(s.command, 0, sizeof s.command);
    s.command[0] = kAbilityToggle;
    const uint16_t args[2] = {id, 1};
    std::memcpy(s.command + 2, args, sizeof args);
    guest::call(c, base() + kAbilityToggleFn, reinterpret_cast<uint64_t>(s.command));
    if (s.have_seen && ability_on(root, id)) s.seen[(id & 0xff) >> 3] |= static_cast<uint8_t>(1u << (id & 7));
}

// The ability bit cleared here through the quest effect 0x19 (+4 value 0), as the step that took it away did. The id is already in
// the set, so the clear hook does not take it for a new one.
void clear_ability(::Ctx* c, uint16_t id) {
    State& s = st();
    std::memset(s.command, 0, sizeof s.command);
    s.command[0] = kAbilityToggle;
    const uint16_t args[2] = {id, 0};
    std::memcpy(s.command + 2, args, sizeof args);
    guest::call(c, base() + kAbilityToggleFn, reinterpret_cast<uint64_t>(s.command));
}

// A clear seen at one of the two clear paths (raw: the id as the command holds it; the game uses its low byte).
void note_clear(uint16_t raw, const char* by) {
    const uint16_t id = raw & 0xff;
    if (!ability_id(id) || !mark_cleared(id)) return;
    st().cleared_new.push_back(static_cast<uint8_t>(id));
    NXLOG_I(kTag, "Co-op story: ability %u cleared here (%s); it stays off for the group", id, by);
}

// A set the game made here of an id in the set (a later step gives it back): out of the set, so it is shared and merged again.
void note_grant(uint16_t raw, const char* by) {
    State& s = st();
    const uint16_t id = raw & 0xff;
    if (!ability_id(id) || !ability_cleared(id)) return;
    s.cleared[id >> 3] &= static_cast<uint8_t>(~(1u << (id & 7)));
    s.cleared_new.erase(std::remove(s.cleared_new.begin(), s.cleared_new.end(), static_cast<uint8_t>(id)), s.cleared_new.end());
    NXLOG_I(kTag, "Co-op story: ability %u given back here (%s); shared again", id, by);
}

// The ids cleared here since the last send (the whole set after a join), once the channel can carry them.
void send_cleared() {
    State& s = st();
    if (!link::in_session()) return;
    std::vector<uint8_t> ids;
    if (s.cleared_send_all) {
        for (uint16_t id = 1; id <= 0xf9; ++id)
            if (ability_cleared(id)) ids.push_back(static_cast<uint8_t>(id));
    } else {
        ids = s.cleared_new;
    }
    if (ids.empty()) {
        s.cleared_send_all = false;
        return;
    }
    if (!link::share(wire::Type::AbilityCleared,
                     wire::Writer(link::self()).u16(static_cast<uint16_t>(ids.size())).bytes(ids.data(), ids.size()).take()))
        return;   // kept for the next frame
    s.cleared_new.clear();
    s.cleared_send_all = false;
}

void share_ability(uint16_t id) {
    uint8_t cmd[wire::kEffectSize] = {kAbilityToggle};
    const uint16_t args[2] = {id, 1};
    std::memcpy(cmd + 2, args, sizeof args);
    link::share(wire::Type::Ability, wire::Writer(link::self()).bytes(cmd, sizeof cmd).take());
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
            if (!(turned_on & (1u << bit)) || !ability_id(id) || ability_cleared(id)) continue;
            share_ability(id);
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
    // Only logged: the state it leads to is what is shared (share_states).
    if (!(c->x[2] & 1) || !link::in_session()) return 0;
    const uint64_t story_obj = story();
    const uint32_t id = static_cast<uint32_t>(c->x[1]) & 0xffff;
    if (!story_obj || c->x[0] != story_obj || id - 1 >= kQuestMax || quest_flag(c, story_obj, id)) return 0;
    NXLOG_I(kTag, "Co-op story: quest %u flag set here (from main+0x%llx)", id, static_cast<unsigned long long>(c->x[30] - base()));
    return 0;
}

int on_quest_effect(::Ctx* c) {
    // The game's own session bits, not the channel: a member in the host's world never runs a world effect, even before its channel
    // has learnt the authority.
    if (!guest::ok(c->x[0], 1)) return 0;
    const uint8_t kind = guest::rd<uint8_t>(c->x[0]);
    if (kind < kEffectFirst || kind > kEffectLast) return 0;
    if (kind == kAbilitySet && link::in_session() && guest::ok(c->x[0], 2 + kAbilitySlots * 2))
        for (unsigned k = 0; k < kAbilitySlots; ++k) note_grant(guest::rd<uint16_t>(c->x[0] + 2 + k * 2), "quest effect 0x01");
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

int on_town_action(::Ctx* c) {
    // Step action 6 of a story line (0x10afc0 calls it through table 0x322d658 row 6 and ANDs the returns): the twin of quest effect
    // 0x26. It writes the town record (R+0x4a27cc + idx bit 0, target R+0x4a32e0 + idx = 0); when bit 0 turned on, the character's
    // home stage decides: the current stage runs the house arrival 0x9bdf50 (which refuses on a visitor, so a member gets nothing),
    // another stage sets bit 4 (pending move), which on a member no reader acts on or clears. A member in the host's world leaves it to
    // the authority, as the world effects (the game's own session bits); the authority runs it when it applies the line
    // (pkp_coop_events.cpp apply_lines). Single player and the authority run the game's own code.
    const bool member = role() == CoopRole::Visit && (session_bits() & layout::kSessionVisitor);
    if (!member) return 0;
    if (guest::ok(c->x[1], 0x10)) {
        const uint64_t node = guest::rd<uint64_t>(c->x[1] + 8);
        if (node && guest::ok(node, 8))
            NXLOG_I(kTag, "Co-op story: story-line town register of index %u (on %u) left to the authority",
                    guest::rd<uint16_t>(node + 4), guest::rd<uint16_t>(node + 6));
    }
    c->x[0] = 1;   // the game's own return (every path returns 1): the line steps on as before
    return 1;
int on_ability_effect(::Ctx* c) {
    // A clear (+4 value 0) or a set is noted; the effect itself runs as the game has it.
    if (!link::in_session() || !guest::ok(c->x[0], 6)) return 0;
    if (guest::rd<uint16_t>(c->x[0] + 4) == 0) note_clear(guest::rd<uint16_t>(c->x[0] + 2), "quest effect 0x19");
    else note_grant(guest::rd<uint16_t>(c->x[0] + 2), "quest effect 0x19");
    return 0;
}

int on_ability_step(::Ctx* c) {
    // 0x10c0d0's own guards: the record *(x1 + 8) is there, its id (+4, low byte) in range; +6 value 0 is the clear, else a set.
    if (!link::in_session() || !guest::ok(c->x[1] + 8, 8)) return 0;
    const uint64_t rec = guest::rd<uint64_t>(c->x[1] + 8);
    if (!rec || !guest::ok(rec, 8)) return 0;
    if (guest::rd<int16_t>(rec + 6) == 0) note_clear(guest::rd<uint16_t>(rec + 4), "story step");
    else note_grant(guest::rd<uint16_t>(rec + 4), "story step");
    return 0;
}

int on_wild_pop(::Ctx* c) {
    // A member in the host's world (the game's own session bits, as the world effects' skip): the authority's world is the group's.
    const bool member = role() == CoopRole::Visit && (session_bits() & layout::kSessionVisitor);
    if (!member) return 0;
    const uint16_t habitat = static_cast<uint16_t>(c->x[0]), creature = static_cast<uint16_t>(c->x[1]);
    if (static_cast<uint16_t>(habitat - 1) >= 500 || static_cast<uint16_t>(creature - 1) >= 0x3ff) return 0;   // the game's own bounds
    const float delay = c->v[0].f[0];
    State::WildPop pop{habitat, creature,
                       static_cast<int16_t>(delay > 30000.0f ? 30000 : delay < -30000.0f ? -30000 : static_cast<int>(delay)),
                       static_cast<uint8_t>(c->x[2] & 1)};
    State& s = st();
    if (s.wild_pops.size() < kWildPopQueue) s.wild_pops.push_back(pop);
    NXLOG_I(kTag, "Co-op story: wild appearance of creature %u at habitat %u %s here (from main+0x%llx), sent to the authority",
            creature, habitat, pop.add ? "armed" : "cleared", static_cast<unsigned long long>(c->x[30] - base()));
    return 0;   // the member's own call runs too, as before
}

void on_wild_pop_message(::Ctx* c, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint16_t habitat = r.u16(), creature = r.u16();
    const int16_t delay = static_cast<int16_t>(r.u16());
    const uint8_t add = r.u8();
    if (!r.ok() || static_cast<uint16_t>(habitat - 1) >= 500 || static_cast<uint16_t>(creature - 1) >= 0x3ff || add > 1) return;
    std::memset(s.pop_command, 0, sizeof s.pop_command);
    s.pop_command[0] = kWildPopEffect;
    const uint16_t args[4] = {habitat, creature, static_cast<uint16_t>(delay), add};
    std::memcpy(s.pop_command + 2, args, sizeof args);
    guest::call(c, base() + kWildPopEffectFn, reinterpret_cast<uint64_t>(s.pop_command));
    NXLOG_I(kTag, "Co-op story: wild appearance of creature %u at habitat %u %s for node %u (delay %d s)", creature, habitat,
            add ? "armed" : "cleared", origin, delay);
}

void on_states_message(::Ctx* c, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint16_t n = r.u16();
    const uint64_t words = quest_words();
    unsigned ahead = 0;
    bool completed = false;
    for (uint16_t k = 0; k < n && r.ok(); ++k) {
        const uint16_t id = r.u16();
        const uint8_t state = r.u8();
        if (!r.ok() || id - 1u >= kQuestMax || state < kStarted || state > kClosed || state <= s.target[id - 1]) continue;
        s.target[id - 1] = state;
        const uint8_t here = words ? state_at(words, id - 1u) : 0;
        if (here < state) {
            ++ahead;
            completed |= state >= kComplete;
            if (n <= 8) NXLOG_I(kTag, "Co-op story: quest %u at state %u on node %u, %u here", id, state, origin, here);
        }
    }
    if (n > 8) NXLOG_I(kTag, "Co-op story: %u quest states from node %u, %u ahead of this process", n, origin, ahead);
    const double now = qpc::seconds();
    if (n <= 8 && completed && now - s.story_line_at >= kNoticeGap) {
        s.story_line_at = now;
        notice::post(notice::name_of(origin) + " finished a step of a request. Your story moves on too!");
    }
    catch_up(c);
}

void on_digest_message(::Ctx* c, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint16_t n = r.u16();
    uint8_t states[kQuestMax], bits[kAbilityBytes];
    if (!r.ok() || n != kQuestMax) return;
    r.bytes(states, kQuestMax);
    r.bytes(bits, kAbilityBytes);
    const uint64_t words = quest_words(), root = save_root();
    if (!r.ok() || !words || !root || !story_shared_here()) return;
    unsigned behind = 0, ahead = 0;
    for (uint32_t i = 0; i < kQuestMax; ++i) {
        if (states[i] > kClosed) continue;
        const uint8_t here = state_at(words, i);
        if (states[i] > here && states[i] > s.target[i]) {
            s.target[i] = states[i];
            ++behind;
        }
        // Ahead of the authority: this state is sent once more (share_states), in case the first send was lost. A state the
        // authority still lacks after that is one its pump cannot step yet; sending it again each digest would not change that.
        if (here > states[i] && here > kListed && here > s.resent[i]) {
            s.resent[i] = here;
            s.sent[i] = 0;
            ++ahead;
        }
    }
    unsigned got = 0, gave = 0, dropped = 0;
    for (unsigned byte = 0; byte < kAbilityBytes; ++byte) {
        const uint8_t mine = guest::rd<uint8_t>(root + kAbilities + byte);
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint16_t id = static_cast<uint16_t>(byte * 8 + bit);
            if (!ability_id(id)) continue;
            const bool there = bits[byte] & (1u << bit), here = mine & (1u << bit);
            if (ability_cleared(id)) {   // taken away in this run: never given back, and off here too
                if (here && !there) {
                    clear_ability(c, id);
                    ++dropped;
                }
                continue;
            }
            if (there && !here) {
                apply_ability(c, root, id);
                ++got;
            } else if (here && !there) {
                share_ability(id);
                ++gave;
            }
        }
    }
    if (behind || ahead || got || gave || dropped)
        NXLOG_I(kTag, "Co-op story: the authority's story (node %u): %u quests behind it here, %u ahead (sent again); abilities %u taken, "
                "%u sent, %u cleared here (taken away in this run)", origin, behind, ahead, got, gave, dropped);
    if (behind || got) {
        const double now = qpc::seconds();
        if (now - s.story_line_at >= kNoticeGap) {
            s.story_line_at = now;
            notice::post("Caught up with your group's story.");
        }
    }
    catch_up(c);
}

void on_cleared_message(::Ctx* c, int, uint8_t origin, wire::Reader& r) {
    const uint16_t n = r.u16();
    const uint64_t root = save_root();
    unsigned turned_off = 0;
    for (uint16_t k = 0; k < n && r.ok(); ++k) {
        const uint8_t id = r.u8();
        if (!r.ok() || !ability_id(id)) continue;
        mark_cleared(id);
        if (root && ability_on(root, id)) {
            clear_ability(c, id);
            ++turned_off;
            NXLOG_I(kTag, "Co-op story: ability %u was taken away on node %u; cleared here too", id, origin);
        }
    }
    if (n && !turned_off) NXLOG_I(kTag, "Co-op story: %u cleared abilities from node %u, all already off here", n, origin);
}

void on_ability_message(::Ctx* c, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    std::memset(s.command, 0, sizeof s.command);
    r.bytes(s.command, wire::kEffectSize);
    const uint64_t root = save_root();
    if (!r.ok() || !root || (s.command[0] != kAbilitySet && s.command[0] != kAbilityToggle)) return;
    uint16_t ids[kAbilitySlots];
    std::memcpy(ids, s.command + 2, sizeof ids);
    // An id taken away in this run is never applied again: a set command's slot is emptied (id 0 fails the game's own range test),
    // a toggle that sets it is dropped.
    if (s.command[0] == kAbilityToggle) {
        if (ids[1] != 0 && ability_cleared(ids[0])) return;
    } else {
        for (uint16_t& id : ids)
            if (ability_id(id) && ability_cleared(id)) id = 0;
        std::memcpy(s.command + 2, ids, sizeof ids);
    }
    if (!ability_changes(root, s.command)) return;
    bool off_before[kAbilitySlots] = {};
    for (unsigned k = 0; k < kAbilitySlots; ++k) off_before[k] = ability_id(ids[k]) && !ability_on(root, ids[k]);
    guest::call(c, base() + (s.command[0] == kAbilitySet ? kAbilitySetFn : kAbilityToggleFn), reinterpret_cast<uint64_t>(s.command));
    // Received: only the bits this command turned on are marked seen (not sent back); a bit that turned on here the same frame still is.
    const unsigned slots = s.command[0] == kAbilityToggle ? 1 : kAbilitySlots;
    for (unsigned k = 0; k < slots; ++k) {
        if (!off_before[k] || !ability_on(root, ids[k])) continue;
        if (s.have_seen) s.seen[(ids[k] & 0xff) >> 3] |= static_cast<uint8_t>(1u << (ids[k] & 7));
        ability_line(origin, ids[k]);
    }
    NXLOG_I(kTag, "Co-op story: ability command 0x%02x from node %u applied here", s.command[0], origin);
}

// A member's wild appearances to the authority, once the channel can carry them.
static void send_wild_pops() {
    State& s = st();
    if (s.wild_pops.empty() || !link::in_session() || link::is_authority()) return;
    size_t sent = 0;
    for (const State::WildPop& p : s.wild_pops) {
        if (!link::to_authority(wire::Type::WildPop, wire::Writer(link::self())
                                                    .u16(p.habitat)
                                                    .u16(p.creature)
                                                    .u16(static_cast<uint16_t>(p.delay))
                                                    .u8(p.add)
                                                    .take()))
            break;   // kept for the next frame, in order
        ++sent;
    }
    s.wild_pops.erase(s.wild_pops.begin(), s.wild_pops.begin() + static_cast<std::ptrdiff_t>(sent));
}

void tick(::Ctx* c) {
    send_wild_pops();
    send_cleared();
    catch_up(c);
    share_states();
    share_new_abilities();
    send_digest();
}

void on_session_change() {
    State& s = st();
    std::memset(s.target, 0, sizeof s.target);
    std::memset(s.logged, 0, sizeof s.logged);
    std::memset(s.resent, 0, sizeof s.resent);
    std::fill(std::begin(s.stall_until), std::end(s.stall_until), 0.0);
    std::memset(s.stalls, 0, sizeof s.stalls);
    s.stepping = 0;
    s.digest_at = 0.0;
    s.send_all = true;
    s.have_seen = false;
    s.cleared_send_all = true;   // the set is kept: an ability taken away stays away for the co-op run
}

void on_member_joined() {
    st().send_all = true;
    st().cleared_send_all = true;
}

bool world_started() {
    const uint64_t words = quest_words();
    const uint32_t i = kFirstTalk - 1;
    return st().target[i] >= kClosed || (words && state_at(words, i) >= kClosed);
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::story
