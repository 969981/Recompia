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
// root + kQuestWords), all of them when it joins a session or another member does, then every state that rose since the last look.
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
// member had before joining reaches the others through the digest (below), which merges the bits both ways. A bit turned off is not
// shared (no writer clears one in the story; the saves seen only grow).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
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
constexpr uint32_t kQuestMax = layout::kQuestWordCount;   // ids 1..1000; word of id at kQuestWords + (id - 1) * 2, state in its low byte
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

uint64_t quest_words() {
    const uint64_t r = save_root();
    return r && guest::ok(r + layout::kQuestWords, kQuestMax * 2) ? r + layout::kQuestWords : 0;
}

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
        const uint8_t state = static_cast<uint8_t>(guest::rd<uint16_t>(words + i * 2));
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

uint8_t state_at(uint64_t words, uint32_t i) { return static_cast<uint8_t>(guest::rd<uint16_t>(words + i * 2)); }

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
            if (!(turned_on & (1u << bit)) || !ability_id(id)) continue;
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
        const uint8_t here = words ? static_cast<uint8_t>(guest::rd<uint16_t>(words + (id - 1u) * 2)) : 0;
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
    unsigned got = 0, gave = 0;
    for (unsigned byte = 0; byte < kAbilityBytes; ++byte) {
        const uint8_t mine = guest::rd<uint8_t>(root + kAbilities + byte);
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint16_t id = static_cast<uint16_t>(byte * 8 + bit);
            if (!ability_id(id)) continue;
            const bool there = bits[byte] & (1u << bit), here = mine & (1u << bit);
            if (there && !here) {
                apply_ability(c, root, id);
                ++got;
            } else if (here && !there) {
                share_ability(id);
                ++gave;
            }
        }
    }
    if (behind || ahead || got || gave)
        NXLOG_I(kTag, "Co-op story: the authority's story (node %u): %u quests behind it here, %u ahead (sent again); abilities %u taken, "
                "%u sent", origin, behind, ahead, got, gave);
    if (behind || got) {
        const double now = qpc::seconds();
        if (now - s.story_line_at >= kNoticeGap) {
            s.story_line_at = now;
            notice::post("Caught up with your group's story.");
        }
    }
    catch_up(c);
}

void on_ability_message(::Ctx* c, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    std::memset(s.command, 0, sizeof s.command);
    r.bytes(s.command, wire::kEffectSize);
    const uint64_t root = save_root();
    if (!r.ok() || !root || (s.command[0] != kAbilitySet && s.command[0] != kAbilityToggle) || !ability_changes(root, s.command)) return;
    uint16_t ids[kAbilitySlots];
    std::memcpy(ids, s.command + 2, sizeof ids);
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

void tick(::Ctx* c) {
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
}

void on_member_joined() { st().send_all = true; }

bool world_started() {
    const uint64_t words = quest_words();
    const uint32_t i = kFirstTalk - 1;
    return st().target[i] >= kClosed || (words && state_at(words, i) >= kClosed);
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::story
