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
// The pump steps only quests in its lists (QL = GM+0x3b9328, the object 0x118d44 also keeps in *(main+0x34efd60): C, state 1, count
// +0, u16 ids +4, 40 at most; A, state 2, count +0x60, {u32 id, f32 timer} at +0x64; B, state 3, count +0x1b8, u16 ids +0x1bc). A quest
// is listed only when its prerequisites (5 u16 at the quest row +0x4e, 0x114e40(id, 0..4)) are all closed (0x11af60) and its stage
// (0x11ac38) is this area's (GM+0x18): at the stage start (0x11933c clears, 0x119640(QL, GM+0x18) lists), and in C later only when
// one of its prerequisites closes in this process's own pump (0x11c220(QL, prerequisite), the pump's only caller; it walks the follow-up
// map 0xa58a80, checks the stage, the prerequisites, state 1 and duplicates itself). So:
// - A quest behind whose prerequisites are not closed here is not flagged first: a prerequisite another member is ahead on is moved
//   on before it (quest 11 needs 9, 10 and 44). A quest skipped after a stall is tried again at once when one of its prerequisites
//   closes here.
// - A quest that became listable after its prerequisite closed here through the catch-up, but that was off-stage or not yet at state
//   1 then, is never put in C until the next stage start. When such a quest stalls at state 1 with its stage here and every
//   prerequisite closed, the catch-up calls 0x11c220(QL, p) for each prerequisite p, as the pump would have; a quest still not listed
//   after that (none to call with, or at state 2 / 3 not in A / B) gets the stage start's own rebuild, 0x11933c + 0x119640, at most
//   once per kRebuildGap and once per quest and state. Both only at a moment the pump could run (not busy, 0x1186ec(0)), from the
//   game thread between frames. 0x11933c clears whatever the gate is, but 0x119640 lists only behind its own gate (GM+0x8c bit 0,
//   0x538750() clear, and not GM+0x98 with the player manager's +0x30 vt+0x148 true): the rebuild runs only when that gate is open,
//   and the lists are put back as they were if it left all three empty.
// - Why a stalled quest is not listed (its stage, a prerequisite, a full list) is logged once per reason.
// The safety net (StoryDigest): every kDigestEvery seconds the authority sends every member its whole story (every quest's state and
// the ability bits). A member behind it catches up as above; a member ahead of it sends each quest it is ahead on again, once per
// state (a rise lost on the way is sent once more; one the authority's pump cannot step yet is not sent every digest); ability bits
// are merged both ways. Received states are taken only in the range a quest can have (2..4 shared, 0..4 in a digest). So two saves
// that drifted apart for any reason are brought together within
// seconds, and a member who joins late gets the abilities unlocked before it came.
//
// Effects. A quest step's effect commands go through 0x11b214(cmd), one command each (u8 kind at +0; kinds 1..0x39 act, any other
// returns with nothing done). Every process runs them, since every process runs the pump. Personal kinds (the player's abilities,
// bag, name, profile: 0x01 0x02 0x03 0x19 0x1e 0x1f 0x30), the quest music 0x07, the time of day (0x0a, and 0x09 for zone 0) and the
// story kinds (quest flags and states: 0x04 0x14 0x15 0x27 0x29 0x2d) run on every process; every other kind changes the shared world,
// which the game's own replication brings from the authority to the members, so on a member in the session it returns at once, as an
// unknown kind does.
// 0x07 (0x1222d4): +4 nonzero plays BGM +2 through the BGM player *(main+0x3561910)+0x10 (0xf9774, +6 its flag), 0 stops it
// (0xf99e0); both only play locally and keep the stage's music id in this process's m_contentsBGM (save root +0x4a36e0 + stage).
// 0x0a and 0x09 zone 0 write only this process's clock override, CMNDAT bytes at save root +0x4a4402 (phase), +0x4a4403 (zone),
// +0x4a4404 (fast-forward state), +0x4a4408 (its target, s), +0x4a440c (its rate). The clock tick 0x1f79b8 reads them: a zone 1..7 or a
// fast-forward state 3 (target reached) freezes the clock, and the tick then returns before 0x1fa1e0, the only place a visitor takes
// the host's time (GameReplication kind 0x23: its reader 0x1fb6ec keeps it pending at clock +0x30..+0x3c until then; 0x1fa1e0 takes it
// outright when far off, else runs the clock at 0.5..1.5x until they meet). 0x0a (0x1223a4) starts a fast forward to +2 h : +4 m
// (quests 8 and 9: the opening's evening, 17:40 then 19:00, held there once reached). 0x09 (0x1231f0, zone u8 +2) ends any fast forward
// and applies the zone; zone 0 (quest 17's close: the night is over) is the real clock again through 0x1f9140(clock, 5), which returns
// at once on a visitor and on the authority sets the time and raises kind 0x23, with the game's own fade when no event runs. Zones 1..7
// set a fixed time through 0x1f8f9c, whose tail 0x1fa9c0 ticks world timers: on a member they stay the authority's (no quest step uses
// them; contents action 0x901 calls 0x1231f0 outside this hook). Before, a member skipped both: it never had the evening, and the
// release never cleared an override its own save or contents actions (0x902 / 0x901, 0x12f690) had left, so its clock could stay frozen
// with the host's time pending while the host's turned to day. Run here, a member has the same evening and the same release, and from
// the release on the authority's time takes over (its kind 0x23 at the release, then from its resync once ability 18, granted by the
// same step, lets it run). Replayed steps run them too: only quests 8, 9 and 17 have them, stepped in id order, so a member far behind
// ends released, as the host is.
// The music only for a live step: a step the catch-up replays (a process behind runs every old step through its own pump) plays no
// music, so a member several quests behind neither hears each old step's music nor is left with an old quest's. A member makes every
// step through the catch-up now (it moves no quest by itself, pkp_coop_quests.cpp), so the step that reaches the group's state of its
// quest (the target is the state the step leads to) counts as live and plays; only a step with more of that quest's steps after it is
// a replay.
// The music a member is left with is the authority's, though: a muted step can be the one that ends an earlier step's music
// (Kyogre's: quest 122's close plays track 117, quest 61's start plays 119, quest 110's start stops it), and a member that saved
// while the wrong track played keeps it in its save (m_contentsBGM is CMNDAT, read when the area starts). So the authority sends
// its area's m_contentsBGM with every digest (AreaMusic), and a member in the same area whose own differs plays or stops it with
// 0x1222d4's own calls (0xf9774 / 0xf99e0, which also write m_contentsBGM) once no quest music step has run here for kMusicSettle.
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
// (the catch-up), so each takes the id out of its own set. A clear the catch-up replays (a process behind runs quest 664 long after
// the others passed 680) is not kept or shared: only a live one is. The set lives in memory only and only for the session (emptied
// when this process joins or leaves one); the event command 0x12c35c (value 2 clears) is not watched.
// The game's ability check 0x113f10 makes a visitor read some abilities from the host's join-time record instead of these bits; on a
// co-op member it reads its own (on_ability_check), so the group's abilities count there as they do offline.
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
#include "core/pkp_strings.h"
#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
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
constexpr uint64_t kFlagOf = 0x5d86e4;   // (S, id); set through quests::set_flag (0x5d8774); the state is read from the quest words
constexpr uint64_t kPumpStateWrites[] = {0x11bcc0, 0x11bda8, 0x11c00c};             // return sites of the state setter's callers
constexpr uint32_t kQuestMax = layout::kQuestWordCount;   // ids 1..1000; word of id at save root + layout::quest_word(id), state in its low byte
constexpr uint8_t kListed = 1;                            // a quest no one has started: not sent
constexpr uint8_t kStarted = 2, kClosed = 4;              // the states a quest can be shared at; a received state past kClosed
                                                          // is one no pump step reaches, so the catch-up would flag it for ever
constexpr uint64_t kQuestListGlobal = 0x34efd60;   // GM: *(main + layout::kGameState)
constexpr uint64_t kGmStage = 0x18, kGmQuestLists = 0x3b9328;   // GM + : this area's stage, the quest lists (QL)
constexpr uint64_t kListC = 0x0, kListCIds = 0x4, kListA = 0x60, kListAEntries = 0x64, kListB = 0x1b8, kListBIds = 0x1bc;   // QL +
constexpr uint64_t kListAEntry = 8, kListATimer = 4;   // A's entry {u32 id (low u16), f32 timer} (0x11b600, 0x119640 writes timer 0)
constexpr uint32_t kListCap = 0x28;
constexpr uint64_t kPrereqOf = 0x114e40, kStageOf = 0x11ac38, kUnlockFollowUps = 0x11c220;
constexpr uint64_t kClearLists = 0x11933c, kListStage = 0x119640, kStoryBusy = 0x1186ec;
constexpr unsigned kPrereqs = 5;
constexpr unsigned kPrereqDepth = 4;     // prerequisites of prerequisites looked through, at most
constexpr double kRebuildGap = 60.0;     // the stage start's list rebuild, at most once per this many seconds
constexpr uint64_t kListsSize = 0x258;   // QL + 0 .. : the four lists 0x11933c clears (their counts and entries)
constexpr uint64_t kGmStory = 0x8c, kGmLinkWait = 0x98, kPlayerMgr = 0x3561928, kPmObject = 0x30, kPmBusySlot = 0x148;
constexpr uint64_t kStoryOff = 0x538750;
constexpr uint32_t kFirstTalk = 2;   // a new world: quest 1 closes on entering the first room, quest 2 at the first talk (run P0RE-2)

constexpr uint8_t kEveryProcessEffects[] = {0x01, 0x02, 0x03, 0x19, 0x1e, 0x1f, 0x30,    // personal
                                            0x07,                                         // quest music (local)
                                            0x0a,                                         // time of day (own clock override)
                                            0x04, 0x14, 0x15, 0x27, 0x29, 0x2d};   // story
constexpr uint8_t kQuestMusicEffect = 0x07, kTimeZoneEffect = 0x09;   // 0x09 runs on a member for zone 0 (u8 +2) only
constexpr uint64_t kAreaMusic = 0x4a36e0;              // save root + stage: m_contentsBGM, the area's quest music (0: none)
constexpr uint64_t kBgmPlayer = 0x3561910, kBgmPlayerObj = 0x10;   // 0x1222d4's music player: *(main + kBgmPlayer) + kBgmPlayerObj
constexpr uint64_t kBgmPlay = 0xf9774, kBgmStop = 0xf99e0;          // (player, track u8, flag) / (player), as 0x1222d4 calls them
constexpr double kMusicSettle = 3.0;   // a member takes the authority's music once no quest music step ran here for this long
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

// The abilities a line names (pkp_strings_table.h); any other is "something new".
struct AbilityName {
    uint16_t id;
    strings::Str name;
};
constexpr AbilityName kAbilityNames[] = {{17, strings::Str::CoopAbilityCreatureList}, {76, strings::Str::CoopAbilityCreatureList},
                                         {10, strings::Str::CoopAbilityWater}, {55, strings::Str::CoopAbilityWater},
                                         {101, strings::Str::CoopAbilityWater}};

struct State {
    uint8_t sent[kQuestMax] = {};      // per quest (id - 1): the state last shared from here
    uint8_t target[kQuestMax] = {};    // per quest: the highest state another member has, which this process catches up to
    uint8_t logged[kQuestMax] = {};    // per quest: the target the catch-up was last logged for
    uint8_t resent[kQuestMax] = {};    // per quest: the state sent again because a digest showed the authority behind it; each state
                                       // is sent again once, so a quest the authority cannot step does not resend every digest
    bool send_all = true;              // the next look shares every started quest (joined, or another member joined)
    double stall_until[kQuestMax] = {};   // per quest: passed over by the catch-up until then
    uint8_t stalls[kQuestMax] = {};       // per quest: how often the pump left its flag (the skip doubles each time)
    uint16_t prereq[kQuestMax][kPrereqs] = {};   // per quest: its prerequisites (0x114e40), once read
    bool prereq_read[kQuestMax] = {};
    uint8_t closed_prereqs[kQuestMax] = {};      // per quest: how many of its prerequisites were closed here at the last look
    uint8_t why[kQuestMax] = {};                 // per quest: the "not listed" reason last logged
    double rebuilt_at = -1e9;
    uint8_t rebuilt_for[kQuestMax] = {};         // per quest: the state the rebuild was last made for (once per quest and state)
    uint32_t stepping = 0;                // the quest the catch-up flagged last (id), and its state then
    uint8_t stepping_state = 0;
    double stepping_since = 0.0;
    double digest_at = 0.0, story_line_at = -1e9;
    std::string last_ability_line;
    bool logged_state = false;
    bool logged_check = false;               // the ability check's first member answer was logged
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
    double music_at = -1e9;                  // the last quest music step (0x07) seen here, run or muted
    uint8_t music_flag[256] = {};            // the authority: per area, the play flag of the last 0x07 seen there (+1; 0 = none)
    struct Music {                           // a member: the authority's area music, as its last AreaMusic said
        bool have = false;
        uint8_t stage = 0, track = 0, flag = 1;
        bool applied = false;                // taken here (each new area / track is taken once)
    } music;
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
    if (quests::member_rules()) return;   // a member's quests move only on the authority's states (pkp_coop_quests.cpp)
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

uint64_t quest_lists() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!guest::ok(gm, kGmQuestLists + kListsSize)) return 0;
    const uint64_t ql = gm + kGmQuestLists;
    // The pump's own object (0xb80c0 passes GM+0x3b9328 to 0xba660); 0x118d44 keeps it in the global too.
    return guest::rd<uint64_t>(base() + kQuestListGlobal) == ql ? ql : 0;
}

uint8_t area_stage() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(gm, kGmStage + 1) ? guest::rd<uint8_t>(gm + kGmStage) : 0;
}

// Quest (index i)'s prerequisites as its row has them; read once the row is there (its stage nonzero: a stage-0 row is never listed).
const uint16_t* prereqs_of(::Ctx* c, uint32_t i) {
    State& s = st();
    if (!s.prereq_read[i]) {
        const uint32_t id = i + 1;
        for (unsigned k = 0; k < kPrereqs; ++k) s.prereq[i][k] = static_cast<uint16_t>(guest::call(c, base() + kPrereqOf, id, k));
        s.prereq_read[i] = (guest::call(c, base() + kStageOf, id) & 0xff) != 0;
    }
    return s.prereq[i];
}

bool quest_id(uint16_t id) { return static_cast<uint16_t>(id - 1) < kQuestMax; }

// Quest id is in the list for its state here (C for 1, A for 2, B for 3).
bool listed(uint64_t ql, uint32_t id, uint8_t state) {
    if (state == kListed) {
        const uint32_t n = std::min(guest::rd<uint32_t>(ql + kListC), kListCap);
        for (uint32_t k = 0; k < n; ++k)
            if (guest::rd<uint16_t>(ql + kListCIds + k * 2) == id) return true;
    } else if (state == kStarted) {
        const uint32_t n = std::min(guest::rd<uint32_t>(ql + kListA), kListCap);
        for (uint32_t k = 0; k < n; ++k)
            if ((guest::rd<uint32_t>(ql + kListAEntries + k * 8) & 0xffff) == id) return true;
    } else if (state == kComplete) {
        const uint32_t n = std::min(guest::rd<uint32_t>(ql + kListB), kListCap);
        for (uint32_t k = 0; k < n; ++k)
            if (guest::rd<uint16_t>(ql + kListBIds + k * 2) == id) return true;
    }
    return false;
}

// A prerequisite of quest (index i) that is not closed here and that another member is ahead on, and is not passed over now: the
// one to move on first (looked through up to kPrereqDepth levels). 0 if none.
uint32_t prereq_to_step(::Ctx* c, uint64_t words, uint32_t i, double now, unsigned depth = 0) {
    State& s = st();
    if (depth >= kPrereqDepth) return 0;
    const uint16_t* pre = prereqs_of(c, i);
    for (unsigned k = 0; k < kPrereqs; ++k) {
        if (!quest_id(pre[k])) continue;
        const uint32_t p = pre[k] - 1u;
        if (state_at(words, p) >= kClosed) continue;
        if (const uint32_t deeper = prereq_to_step(c, words, p, now, depth + 1)) return deeper;
        if (s.target[p] > state_at(words, p) && now >= s.stall_until[p]) return p + 1;
    }
    return 0;
}

// Why the stalled quest (index i, at state here) is not stepped; logged once per reason.
enum class Why : uint8_t { None, Listed, Stage, Prereq, Full, Unlisted };
Why why_not(::Ctx* c, uint64_t words, uint64_t ql, uint32_t i, uint8_t state, uint16_t* prereq_open) {
    const uint32_t id = i + 1;
    *prereq_open = 0;
    if (listed(ql, id, state)) return Why::Listed;
    if ((guest::call(c, base() + kStageOf, id) & 0xff) != area_stage()) return Why::Stage;
    const uint16_t* pre = prereqs_of(c, i);
    for (unsigned k = 0; k < kPrereqs; ++k)
        if (quest_id(pre[k]) && state_at(words, pre[k] - 1u) != kClosed) {
            *prereq_open = pre[k];
            return Why::Prereq;
        }
    if (state == kListed && guest::rd<uint32_t>(ql + kListC) >= kListCap) return Why::Full;
    return Why::Unlisted;
}

void log_why(::Ctx* c, uint32_t i, uint8_t state, Why why, uint16_t prereq, uint64_t words) {
    State& s = st();
    if (s.why[i] == static_cast<uint8_t>(why)) return;
    s.why[i] = static_cast<uint8_t>(why);
    const uint32_t id = i + 1;
    switch (why) {
        case Why::Listed:
            NXLOG_I(kTag, "Co-op story: quest %u is listed here at state %u; the pump does not step it yet (its condition, or busy)", id,
                    state);
            break;
        case Why::Stage:
            NXLOG_I(kTag, "Co-op story: quest %u is not listed here: its stage is %u, this area's %u", id,
                    static_cast<unsigned>(guest::call(c, base() + kStageOf, id) & 0xff), area_stage());
            break;
        case Why::Prereq:
            NXLOG_I(kTag, "Co-op story: quest %u is not listed here: its prerequisite %u is at state %u here (%u on the group)", id, prereq,
                    state_at(words, prereq - 1u), st().target[prereq - 1u]);
            break;
        case Why::Full:
            NXLOG_I(kTag, "Co-op story: quest %u is not listed here: the list of listed quests is full (%u)", id, kListCap);
            break;
        case Why::Unlisted:
            NXLOG_I(kTag, "Co-op story: quest %u is not listed here (state %u): its prerequisites closed after this area's quests were listed",
                    id, state);
            break;
        default: break;
    }
}

uint32_t list_counts(uint64_t ql) {
    return guest::rd<uint32_t>(ql + kListC) + guest::rd<uint32_t>(ql + kListA) + guest::rd<uint32_t>(ql + kListB);
}

// 0x119640's own gate: it lists nothing unless the story is on (GM+0x8c bit 0), 0x538750() is clear, and not (GM+0x98 set and the
// player manager's object +0x30 answering true to its vt+0x148).
bool stage_listing_open(::Ctx* c) {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!guest::ok(gm, kGmLinkWait + 8) || !(guest::rd<uint8_t>(gm + kGmStory) & 1)) return false;
    if (guest::call(c, base() + kStoryOff, 0) & 1) return false;
    if (!guest::rd<uint64_t>(gm + kGmLinkWait)) return true;
    const uint64_t pm = guest::rd<uint64_t>(base() + kPlayerMgr);
    const uint64_t obj = pm && guest::ok(pm + kPmObject, 8) ? guest::rd<uint64_t>(pm + kPmObject) : 0;
    if (!obj || !guest::ok(obj, 8)) return true;
    const uint64_t vt = guest::rd<uint64_t>(obj);
    if (!guest::ok(vt + kPmBusySlot, 8)) return false;
    return !(guest::call(c, guest::rd<uint64_t>(vt + kPmBusySlot), obj) & 1);
}

// A stalled quest whose stage is this area's and whose prerequisites are all closed, but which is in no list: put in the list as the
// game does (0x11c220 for each closed prerequisite; the stage start's rebuild when that does not do it). Only when the pump could
// run now. True when it is listed afterwards.
bool list_quest(::Ctx* c, uint64_t words, uint64_t ql, uint32_t i, uint8_t state) {
    State& s = st();
    const uint32_t id = i + 1;
    if (guest::call(c, base() + kStoryBusy, 0) & 1) return false;
    if (state == kListed) {
        const uint16_t* pre = prereqs_of(c, i);
        unsigned called = 0;
        for (unsigned k = 0; k < kPrereqs; ++k) {
            if (!quest_id(pre[k]) || state_at(words, pre[k] - 1u) != kClosed) continue;
            guest::call(c, base() + kUnlockFollowUps, ql, pre[k]);
            ++called;
        }
        if (listed(ql, id, state)) {
            NXLOG_I(kTag, "Co-op story: quest %u listed here through its %u closed prerequisites (0x11c220)", id, called);
            return true;
        }
    }
    const double now = qpc::seconds();
    if (now - s.rebuilt_at < kRebuildGap || s.rebuilt_for[i] == state || !stage_listing_open(c)) return false;
    s.rebuilt_at = now;
    s.rebuilt_for[i] = state;
    uint8_t saved[kListsSize];
    std::memcpy(saved, reinterpret_cast<const void*>(ql), sizeof saved);
    const uint32_t before = list_counts(ql);
    guest::call(c, base() + kClearLists, ql);
    guest::call(c, base() + kListStage, ql, area_stage());
    if (before && !list_counts(ql)) {   // 0x119640 listed nothing after all: the lists as they were
        std::memcpy(reinterpret_cast<void*>(ql), saved, sizeof saved);
        NXLOG_W(kTag, "Co-op story: the quest list rebuild for quest %u listed nothing; the lists were put back", id);
        return false;
    }
    // 0x119640 starts every quest it puts in A with timer 0: a quest that was in A already keeps the timer it had (the catch-up runs
    // on the host too, so every started quest's wait would start over otherwise).
    uint32_t old_a = 0;
    std::memcpy(&old_a, saved + kListA, sizeof old_a);
    old_a = std::min(old_a, kListCap);
    const uint32_t new_a = std::min(guest::rd<uint32_t>(ql + kListA), kListCap);
    unsigned kept = 0;
    for (uint32_t k = 0; k < new_a; ++k) {
        const uint64_t entry = ql + kListAEntries + k * kListAEntry;
        const uint16_t quest = static_cast<uint16_t>(guest::rd<uint32_t>(entry));
        for (uint32_t j = 0; j < old_a; ++j) {
            uint32_t old_id = 0;
            float timer = 0.0f;
            std::memcpy(&old_id, saved + kListAEntries + j * kListAEntry, sizeof old_id);
            if (static_cast<uint16_t>(old_id) != quest) continue;
            std::memcpy(&timer, saved + kListAEntries + j * kListAEntry + kListATimer, sizeof timer);
            guest::wr<float>(entry + kListATimer, timer);
            ++kept;
            break;
        }
    }
    const bool ok = listed(ql, id, state);
    if (kept) NXLOG_I(kTag, "Co-op story: %u started quests kept their timers through the rebuild", kept);
    NXLOG_W(kTag, "Co-op story: this area's quest lists rebuilt as at a stage start, for quest %u at state %u: %s", id, state,
            ok ? "listed now" : "still not listed");
    return ok;
}

uint8_t closed_prereqs(::Ctx* c, uint64_t words, uint32_t i) {
    const uint16_t* pre = prereqs_of(c, i);
    uint8_t closed = 0;
    for (unsigned k = 0; k < kPrereqs; ++k)
        if (quest_id(pre[k]) && state_at(words, pre[k] - 1u) == kClosed) ++closed;
    return closed;
}

// A quest passed over after a stall is tried again at once when one of its prerequisites closed here since it stalled.
void retry_on_prereq_close(::Ctx* c, uint64_t words, double now) {
    State& s = st();
    for (uint32_t i = 0; i < kQuestMax; ++i) {
        if (!s.target[i] || s.stall_until[i] <= now) continue;
        if (closed_prereqs(c, words, i) <= s.closed_prereqs[i]) continue;
        s.stall_until[i] = 0.0;
        s.stalls[i] = 0;
        NXLOG_I(kTag, "Co-op story: a prerequisite of quest %u closed here; it is tried again", i + 1);
    }
}

// Moves the lowest quest that is behind another member's state one pump step on (its flag set in this process's own state), and
// waits for the pump to make that step before it flags another.
void catch_up(::Ctx* c) {
    State& s = st();
    const uint64_t words = quest_words(), story_obj = story();
    if (!words || !story_obj || !story_shared_here()) return;
    const double now = qpc::seconds();
    retry_on_prereq_close(c, words, now);
    for (uint32_t q = 0; q < kQuestMax; ++q) {
        if (!s.target[q] || now < s.stall_until[q]) continue;
        if (state_at(words, q) >= s.target[q]) continue;
        // Its prerequisites first: a quest is listed only once they are closed here. i is the quest moved on now.
        uint32_t i = q;
        if (const uint32_t first = prereq_to_step(c, words, q, now)) i = first - 1;
        const uint8_t state = state_at(words, i);
        const uint32_t id = i + 1;
        if (s.stepping != id || s.stepping_state != state) {   // a new quest, or the pump made the last step
            s.stepping = id;
            s.stepping_state = state;
            s.stepping_since = now;
            if (i != q) NXLOG_I(kTag, "Co-op story: quest %u waits for its prerequisite %u here; that one is moved on first", q + 1, id);
        } else if (now - s.stepping_since > kStall) {
            if (const uint64_t ql = quest_lists()) {
                uint16_t open = 0;
                const Why why = why_not(c, words, ql, i, state, &open);
                log_why(c, i, state, why, open, words);
                if (why == Why::Unlisted && list_quest(c, words, ql, i, state)) {
                    s.stepping_since = now;   // listed now: the pump gets its frames again
                    return;
                }
            }
            const double skip = std::min(kStallSkipMax, kStallSkip * static_cast<double>(1u << std::min<unsigned>(s.stalls[i], 5)));
            s.stall_until[i] = now + skip;
            s.closed_prereqs[i] = closed_prereqs(c, words, i);   // one more closing here ends the skip (retry_on_prereq_close)
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
        if (!quest_flag(c, story_obj, id)) quests::set_flag(c, story_obj, id);
        return;   // one quest at a time
    }
}

// The authority's whole story to every member (StoryDigest).
void send_music();   // with the digest (below, with the other music parts)

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
    send_music();
}

const char* ability_name(uint16_t id) {
    for (const AbilityName& a : kAbilityNames)
        if (a.id == id) return strings::get(base(), a.name);
    return nullptr;
}

// A line naming what another member's player unlocked; the same line twice in a row (the creature list is two ids) is said once.
void ability_line(uint8_t origin, uint16_t id) {
    State& s = st();
    const char* name = ability_name(id);
    const std::string who = notice::name_of(origin);
    const std::string text = name ? strings::fmt(base(), strings::Str::CoopAbilityNamed, who, name)
                                  : strings::fmt(base(), strings::Str::CoopAbilityNew, who);
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

// A pump step the catch-up is replaying here: a quest whose flag is set and whose state is still below another member's. The pump runs
// a step's effects before it writes the state and clears the flag (0x11bd3c, and 0x11b600's close path), so while such a step runs
// the quest is still behind here: every other member made that step already, and is past what it did. Every catch-up step counts,
// the group's latest too (unlike catch_up_step, the music's test): a member steps only through the catch-up, and the authority,
// whose step is the live one, keeps and shares that clear itself (members share no states, so it replays nothing). A member one step
// behind on quest 664 may be in a group long past 680, which gave the ability back; keeping that clear would take it from all.
bool replaying(::Ctx* c) {
    const State& s = st();
    const uint64_t words = quest_words(), story_obj = story();
    if (!words || !story_obj) return false;
    for (uint32_t i = 0; i < kQuestMax; ++i)
        if (s.target[i] > state_at(words, i) && quest_flag(c, story_obj, i + 1)) return true;
    return false;
}

// A clear seen at one of the two clear paths (raw: the id as the command holds it; the game uses its low byte). Only a live clear is
// kept and shared: one a catch-up replays (an old step, quest 664 on a process still behind) is a step the others made long ago,
// and a later step (quest 680) may have given the id back for them; sharing it would take the ability from the whole group.
void note_clear(::Ctx* c, uint16_t raw, const char* by) {
    const uint16_t id = raw & 0xff;
    if (!ability_id(id) || ability_cleared(id)) return;
    if (replaying(c)) {
        NXLOG_I(kTag, "Co-op story: ability %u cleared here (%s) by a step the catch-up replays; not shared", id, by);
        return;
    }
    mark_cleared(id);
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
        journal::Rec(journal::Area::Quest, "state-decision")
            .in("id=%u state=%u by=main+0x%llx", static_cast<unsigned>(c->x[1] & 0xffff), static_cast<unsigned>(c->x[2] & 0xff),
                static_cast<unsigned long long>(from))
            .out("written, not shared: not one of the pump's steps")
            .note("logged once per run")
            .warn();
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
    journal::Rec(journal::Area::Quest, "flag-decision")
        .in("id=%u by=main+0x%llx", id, static_cast<unsigned long long>(c->x[30] - base()))
        .out("set here: the pump steps it; the state it leads to is shared");
    return 0;
}

namespace {

// A pump step the catch-up replays here, for the quest music: a quest whose flag is set and whose state is more
// than one step below another member's. The pump runs a step's effects before it writes the state and clears the flag (0x11bd3c,
// and 0x11b600's close path), so while such a step runs the quest is still at the state the step starts from. A step to the group's
// state of the quest (target == state + 1) is the group's latest: on a member it is the live step (members step only through the
// catch-up), and it plays. The quest the catch-up is stepping decides; a quest passed over earlier keeps its flag, and is looked at
// only when the stepping one is not being stepped (no flag, or moved on), so one stalled quest far behind does not mute every live
// step. The ability clears keep their own, stricter test (replaying()).
bool catch_up_step(::Ctx* c) {
    const State& s = st();
    const uint64_t words = quest_words(), story_obj = story();
    if (!words || !story_obj) return false;
    if (quest_id(static_cast<uint16_t>(s.stepping))) {
        const uint32_t i = s.stepping - 1;
        const uint8_t here = state_at(words, i);
        if (here == s.stepping_state && s.target[i] > here && quest_flag(c, story_obj, s.stepping)) return s.target[i] > here + 1;
    }
    for (uint32_t i = 0; i < kQuestMax; ++i) {
        const uint8_t here = state_at(words, i);
        if (s.target[i] > here + 1 && quest_flag(c, story_obj, i + 1)) return true;
    }
    return false;
}

}  // namespace

namespace {

// The quest music of area `stage` on this process (its m_contentsBGM), or -1 when the save is not readable.
int area_music(uint8_t stage) {
    const uint64_t root = save_root();
    return root && guest::ok(root + kAreaMusic + stage, 1) ? guest::rd<uint8_t>(root + kAreaMusic + stage) : -1;
}

// The authority: its area's quest music, to every member (with the digest).
void send_music() {
    const State& s = st();
    const uint8_t stage = area_stage();
    const int track = area_music(stage);
    if (track < 0) return;
    const uint8_t flag = s.music_flag[stage] ? static_cast<uint8_t>(s.music_flag[stage] - 1) : 1;
    link::to_members(wire::Type::AreaMusic,
                     wire::Writer(link::self()).u8(stage).u8(static_cast<uint8_t>(track)).u8(flag).take());
}

// A member: the authority's area music, once this process's catch-up has made no music step for kMusicSettle.
void take_music(::Ctx* c) {
    State& s = st();
    State::Music& m = s.music;
    if (!m.have || m.applied || link::is_authority() || !link::in_session()) return;
    if (qpc::seconds() - s.music_at < kMusicSettle || m.stage != area_stage()) return;
    const int here = area_music(m.stage);
    if (here < 0) return;
    m.applied = true;
    if (here == m.track) return;
    const uint64_t bgm = guest::rd<uint64_t>(base() + kBgmPlayer);
    if (!bgm) return;
    if (m.track) {
        guest::call(c, base() + kBgmPlay, bgm + kBgmPlayerObj, m.track, m.flag ? 1 : 0);
        NXLOG_I(kTag, "Co-op story: the area's quest music is track %u on the authority (%d here); it plays here", m.track, here);
    } else {
        guest::call(c, base() + kBgmStop, bgm + kBgmPlayerObj);
        NXLOG_I(kTag, "Co-op story: the area has no quest music on the authority (track %d here); it stops here", here);
    }
}

}  // namespace

int on_quest_effect(::Ctx* c) {
    // The game's own session bits, not the channel: a member in the host's world never runs a world effect, even before its channel
    // has learnt the authority.
    if (!guest::ok(c->x[0], 1)) return 0;
    const uint8_t kind = guest::rd<uint8_t>(c->x[0]);
    if (kind < kEffectFirst || kind > kEffectLast) return 0;
    if (kind == kAbilitySet && link::in_session() && guest::ok(c->x[0], 2 + kAbilitySlots * 2))
        for (unsigned k = 0; k < kAbilitySlots; ++k) note_grant(guest::rd<uint16_t>(c->x[0] + 2 + k * 2), "quest effect 0x01");
    const bool member = role() == CoopRole::Visit && (session_bits() & layout::kSessionVisitor);
    if (kind == kQuestMusicEffect) {
        st().music_at = qpc::seconds();
        if (guest::ok(c->x[0], 8)) st().music_flag[area_stage()] = guest::rd<uint16_t>(c->x[0] + 6) ? 2 : 1;
    }
    if (kind == kQuestMusicEffect && link::in_session() && catch_up_step(c)) {
        NXLOG_I(kTag, "Co-op story: effect 0x07 of a step the catch-up replays; its music not played here");
        journal::Rec(journal::Area::Quest, "effect-decision").in("kind=0x07").out("skipped: a step the catch-up replays (old music)");
        return 1;
    }
    bool every_process = false;
    for (uint8_t every : kEveryProcessEffects) every_process |= kind == every;
    every_process |= kind == kTimeZoneEffect && guest::ok(c->x[0], 3) && guest::rd<uint8_t>(c->x[0] + 2) == 0;
    if (!member || every_process) {
        if (link::in_session()) {
            NXLOG_I(kTag, "Co-op story: effect 0x%02x run here", kind);
            journal::Rec(journal::Area::Quest, "effect-decision")
                .in("kind=0x%02x member=%u", kind, member ? 1u : 0u)
                .out("run here: %s",
                     member ? "a personal, music, time of day or story kind (every process)" : "the authority runs every kind");
        }
        return 0;
    }
    NXLOG_I(kTag, "Co-op story: world effect 0x%02x left to the authority", kind);
    journal::Rec(journal::Area::Quest, "effect-decision").in("kind=0x%02x member=1", kind).out("skipped: a world effect, the authority's");
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
}

int on_ability_effect(::Ctx* c) {
    // A clear (+4 value 0) or a set is noted; the effect itself runs as the game has it.
    if (!link::in_session() || !guest::ok(c->x[0], 6)) return 0;
    if (guest::rd<uint16_t>(c->x[0] + 4) == 0) note_clear(c, guest::rd<uint16_t>(c->x[0] + 2), "quest effect 0x19");
    else note_grant(guest::rd<uint16_t>(c->x[0] + 2), "quest effect 0x19");
    return 0;
}

int on_ability_step(::Ctx* c) {
    // 0x10c0d0's own guards: the record *(x1 + 8) is there, its id (+4, low byte) in range; +6 value 0 is the clear, else a set.
    if (!link::in_session() || !guest::ok(c->x[1] + 8, 8)) return 0;
    const uint64_t rec = guest::rd<uint64_t>(c->x[1] + 8);
    if (!rec || !guest::ok(rec, 8)) return 0;
    if (guest::rd<int16_t>(rec + 6) == 0) note_clear(c, guest::rd<uint16_t>(rec + 4), "story step");
    else note_grant(guest::rd<uint16_t>(rec + 4), "story step");
    return 0;
}

int on_ability_check(::Ctx* c) {
    // The ability check 0x113f10(id, member) (121 callers, 0x377cc(id) = (id, 0xff) has 652 more): row flag bit 0 answers 1, offline
    // (GM+0x90 bit 0 clear) or a visitor whose host index 0x27eae0() is above 3 reads this save's own bits (save root + 0x4a26a8),
    // flag bit 1 on Cloud Island answers 1, an explicit member 0..3 reads that member's record (its own bits for this player's index).
    // Member 0xff with flag bit 2 reaches this `tbz w8, #2, 0x114048` (w8 = GM+0x90): set, a visitor reads the host's member record
    // R+0x14c2a40 + host index * 0xb8 + 0x51 + id / 8 (0xb19d8), a snapshot the host builds at session start and sends only at a join,
    // so an ability the group got after the join stayed off there (a teammate's PC menu lacked the Environment Level panel, ability
    // 133). A co-op member's own bits are the group's (the ability share and the digest's merge above): bit 2 cleared, the check takes
    // the own-bits branch, as offline. w8 is reloaded at 0x114048, nothing else reads it. Every other case never reaches this site.
    if (role() != CoopRole::Visit || !link::in_session() || !(c->x[8] & layout::kSessionVisitor)) return 0;
    c->x[8] &= ~static_cast<uint64_t>(layout::kSessionVisitor);
    State& s = st();
    if (!s.logged_check) {
        s.logged_check = true;
        NXLOG_I(kTag, "Co-op story: the ability check (ability %u) answers from this member's own bits, not the host's join-time record",
                static_cast<unsigned>(c->x[19] & 0xff));
    }
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
        notice::post(strings::fmt(base(), strings::Str::CoopStoryStep, notice::name_of(origin)));
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
            notice::post(strings::get(base(), strings::Str::CoopStoryCaughtUp));
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

void on_music_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    const uint8_t stage = r.u8(), track = r.u8(), flag = r.u8();
    if (!r.ok() || link::is_authority()) return;
    State::Music& m = st().music;
    if (m.have && m.stage == stage && m.track == track) return;   // the same as before: taken already, or waiting
    m = State::Music{true, stage, track, flag, false};
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
    take_music(c);
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
    std::memset(s.closed_prereqs, 0, sizeof s.closed_prereqs);
    std::memset(s.why, 0, sizeof s.why);
    std::memset(s.rebuilt_for, 0, sizeof s.rebuilt_for);
    s.stepping = 0;
    s.digest_at = 0.0;
    s.send_all = true;
    s.have_seen = false;
    // The set belongs to this session: joined or left, it starts empty, so a set from another group (or another save) never strips
    // an ability here or there. On a rejoin of the same group the others send theirs again (on_member_joined).
    std::memset(s.cleared, 0, sizeof s.cleared);
    s.cleared_new.clear();
    s.cleared_send_all = false;
    s.music = State::Music{};
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

// The digests' readers (pkp_coop_digest.cpp): quest states and ability bits as the save has them now.
namespace nxrt::games::pkp::coop::digest {

bool quest_states(Entries& out) {
    const uint64_t words = coop::story::quest_words();
    if (!words) return false;
    for (uint32_t i = 0; i < coop::story::kQuestMax; ++i)
        if (const uint8_t v = coop::story::state_at(words, i)) out.push_back({i + 1, v});
    return true;
}

bool abilities(Entries& out) {
    const uint64_t root = coop::story::save_root();
    if (!root) return false;
    for (uint32_t id = 1; id < 0x100; ++id)
        if (coop::story::ability_on(root, static_cast<uint16_t>(id))) out.push_back({id, 1});
    return true;
}

}  // namespace nxrt::games::pkp::coop::digest
