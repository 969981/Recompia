// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's followers (pkp_coop_internal.h): a story follower is an escort, a creature that follows the player
// who asked for it until the story releases it; a member's party leave takes no other player's follower; the group's invited
// followers leave when the group moves. RE: modding-docs coop-v2/PARTY-CHECKS-RE.md section 13, coop-v2/quests/QUEST-SCRIPT-RE.md.
//
// The game has two follows. An invited one (0x657a20) is kept by the party manager per session slot: the authority attaches it for
// any slot's player (it does so for every visitor's join request, 0x5519bc) and the game sends each slot's list to every member
// with the leader written on the creature. The story's own (contents command 0xdac mode 3 -> 0x117b18 -> 0x2a1ed4 -> 0x659280 ->
// 0x6599c0) attaches only to the process's own player, in a list no other process sees: with it a member's story follow could only
// be made for the host's player (the creature would not come to the member's work spot), no other process could answer "is it
// following" for it, and the game's own refusals read it for the host alone.
// So in a session the story's follow rides the invited one, and what makes it the story's is co-op's own:
//   The escort record, on the authority: creature data id -> the leading player's session slot and node. Sent whole to every member
//   when it changes (PartyEscorts). The record is the truth: every kRetry seconds, at a safe moment, the authority attaches an
//   escort whose creature is in this stage and does not follow its leader (after an ask, a group move, a rejoin, a hand-over, an
//   attach that failed). The attach is the game's 0x657a20(party manager, &leader id, &creature id, 1, 0), after the game's own leave
//   when another player leads the creature, and after one invited follower of the leader that is no escort has left when the
//   leader's five places are taken (the game's story attach makes room the same way). The attach is then accepted as the
//   authority's party tick accepts an invite (accept()): the story's follow does not ask the creature, as the story's own attach
//   does not, and the tick's ask (0x16570) says no to a creature the story holds. Every escort is matched at every look. Not
//   while the group is on its way to another area (travel::moving): an escort released for the move is attached where it arrives,
//   not before the stage changes.
//   The record in the save (pkp_coop_escort_marks.cpp): the game's own story mark of a creature, in the row the running game
//   reads: the authority's own save, and on a member the copy of the host's rows it reads while visiting (a member's own save is
//   never written). The game's own "is it the story's follower" answers then hold for an escort on every process, and the record
//   is in the save the story is in. A row is named by the creature's save index, read from the live creature (and sent with the
//   record). The authority finds the saved record at its looks: a creature of this stage whose row is marked and that is in no
//   record is an escort of its own player, by default (a mark names no leader, and at a session's opening no other player is
//   there). Nothing is sent before the first look, so no member clears a mark the authority has not read yet.
//   A leader by default (from the save, after its leader left, after the kGone rule) is replaced by the next player whose own
//   story asks for the creature; a leader that asked, or that led the creature by invitation, keeps it.
//   Outside a session the game puts a marked creature in its story list when the creature spawns (0x296474), which is before a
//   session opens: on every process a creature found in that list is taken out by the story's own leave (the invite refuses a
//   creature in it, and no other process sees it). On the authority it is an escort first; the leave clears its mark and the
//   record's next write sets it again.
//   "Follows its leader" is: the creature names the leader and is in the leader's invited list (the lists are emptied at a stage
//   change with no leave, so a creature can name a leader no list knows; the invite asks the lists alone, 0x659f40).
//   0x2a1ed4 (entry hook): the story's attach never runs in a session. On the authority the creature becomes an escort of its own
//   player; on a member the ask goes to the authority (StoryFollow, with the member's slot), which makes it an escort of the asker.
//   A creature that follows a session player already becomes that player's escort, whoever asks: a content whose only test is "the
//   creature follows a player" runs on every process (pkp_coop_party_table.inc), and none of them may take it from its leader.
//   That leader's node is learnt from its own ask; until then only the authority's content can release it.
//   0x659588 (entry hook), the leave every path ends in (the gate, a trip, the release by hand, an AI state change, a talk, a
//   member's request): for a creature in the record it does not run. Not from here: this module's own calls, and the session's
//   own teardown of a player that leaves (0x65a100, return 0x65a278), after which the escort is attached to its new leader.
//   0x659bf8 (entry hook), the story's own leave, called by every scripted release right before the invited leave (0x2a1f44) and by
//   the escort end (0x617d98): the story releases the creature. On the authority it is taken out of the record and leaves; on the
//   member that leads it, the release is sent to the authority (EscortRelease), which does the same. A release on a member that
//   does not lead it changes nothing: a teammate who reaches a scene or a stage late does not end another player's escort.
//   0x2a1f44 (entry hook), the scripted release: it calls 0x659bf8 only for a creature that names a valid leader. An escort that
//   follows nobody at that moment (not attached yet in a stage just entered, its leader's places full) is released there instead,
//   or the next look would attach a creature the story has just placed (a contents `dc3` puts a creature on its spot this way).
//   The game clears the story mark there only for a creature in its story list (0x659d80), which an escort is not; so outside a
//   session this hook clears it in a host's save, or a creature that was an escort when the session closed would come back as a
//   story follower after the story released it.
//   The authority's scheduler also runs records a member's relayed notice queued (pkp_coop_feed.cpp). The first time a record
//   releases a creature that counts, whoever's notice it was: the group's story got there. The same record releasing the same
//   creature again for the notice of a player who does not lead it does not: a teammate that sees an old scene again does not
//   end a later escort.
//   A leader that leaves the session: its escorts become the authority's player's.
//   The group's move to another area (pkp_coop_travel.cpp): every invited follower leaves through the game's own 0x2a1dc0 first, an
//   escort too (the game clears the lists at the stage change, which is no leave: the follow mark in the save would stay), but an
//   escort stays in the record and is attached again where its creature is next seen. The other followers who can come are
//   recorded right before (pkp_coop_carry.cpp) and follow their leaders again in the next area.
// A member's party leave (RequestPartyNPC +0x38 = 0, sent by 0x659588 on a visitor) carries the creature (+0x36) and the leader
// as the member sees it (+0x34), not who asks; the host's handler 0x5519bc detaches it from whoever leads it. The rule: a leave
// only ever takes a creature from the player who asked.
//   0x5519bc (entry hook), on the authority in the session: called only by the receive entry 0x547980 (return 0x547ab8), whose w21
//   is the sender's member index. The asker's player is the session player character (player manager +0x30..+0x48) whose member
//   index (u8 +0x5a38) is that; the leader's is the one whose actor id (+0x27fe) leads the creature. Both found and different:
//   refused, as handled (returns 0, so it is not sent again) and logged. Anything that cannot be told apart is the game's own.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::follow {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kInvite = 0x657a20;      // (party manager, &u16 leader id, &u16 npc id, w3 1, w4 0): the invited follow
constexpr uint64_t kLeave = 0x2a1dc0;       // (w0 0, &u16 npc actor id, w2 0, w3 1) -> the creature leaves whoever leads it (0x659588,
                                            // w3: the "busy" refusal 0x16d40 is not asked), as the game's map move calls it (0xbd784)
constexpr uint64_t kFirstOf = 0x28cc60;     // (player manager, data id) -> the first live resident of that data id, or 0
constexpr uint64_t kActorById = 0x281360;   // (player manager, &u16 actor id) -> the actor, or 0
constexpr uint64_t kDataIdOf = 0x27d848;    // (&u16 actor id) -> u16 the character's data id
constexpr uint64_t kActorId = 0x27fe;       // resident actor + : u16 its actor id
constexpr uint64_t kInParty = 0x1d8;        // actor vt + : bool, it is in a party
constexpr uint64_t kLeaderOf = 0x410;       // actor vt + : its leader's object, or 0
constexpr uint64_t kPartyBlock = 0x35e0;    // leader object + : its party block; +0xc u32 its save index, +0x10 u16 the leader's actor id
constexpr uint64_t kPartyIndex = 0xc, kPartyLeaderId = 0x10;
constexpr uint64_t kPartOf = 0x408;         // actor vt + : the object whose + kPartyBlock the invite and the party tick write (0x657a20)
constexpr uint64_t kPartFollowing = 0x1e;   // party block + : u8, the invite turned the creature's follow on (0x3f0ef0 w1)
constexpr uint64_t kRaise = 0x4b1d54;       // (game state + kRaiseQueue, kind, slot, 0): the host's game event raise
constexpr uint64_t kRaiseQueue = 0x3bb620;
constexpr uint32_t kRaiseParty = 4;         // kind 4: the slot's invited list is sent to the members
constexpr uint64_t kStateLeaving = 0x11;    // game state + : u8, bit 0 set: the party tick raises nothing (0x6564e0)
constexpr uint64_t kResidentLists = 0x4550;   // player manager + this + 8 * data id: the live residents of that data id (0x28cc60)
constexpr size_t kEnded = 64;                 // the releases remembered
constexpr uint64_t kStoryLeave = 0x659bf8;  // (party manager, &u16 leader id, &u16 npc id): out of the story's list, its mark cleared
constexpr uint16_t kDataIdMax = 0x3ff;
constexpr uint64_t kPartyCall = 0x547ab8;   // the receive entry's return from a handler (blr at 0x547ab4)
constexpr int kSenderReg = 21;              // there: w21 = the sender's member index
constexpr uint64_t kMsgNpc = 0x36, kMsgJoin = 0x38, kMsgHome = 0x3a;   // RequestPartyNPC + : u16 creature, u8 join, u8 home-navi
constexpr uint64_t kNetIndex = 0x5a38;      // player character + : u8 its member index (its session slot)
constexpr uint64_t kPlayerSlots = 4;        // player manager + kManagerPlayer + 8 * i: the session's player characters
constexpr uint32_t kIndexMax = 0xff;
constexpr uint64_t kListIds = 0, kListCount = 8;   // a party list: the u16 actor ids, their count
constexpr uint64_t kLeaderPlaces = 5;       // a slot's invited followers at most (0x657a20 refuses above 4 before its attach)
constexpr uint64_t kLeaveNpc = 3;           // 0x659588: x3 = &u16 the creature's actor id
constexpr uint64_t kStoryLeaveNpc = 2;      // 0x659bf8: x2 = &u16 the creature's actor id
constexpr uint64_t kTeardownLeave = 0x65a278;   // 0x659588's return into 0x65a100, the teardown of a leaving player's party
constexpr double kRefusedLineGap = 30.0;
constexpr double kRetry = 2.0;              // the record is compared with the world this often
constexpr double kSendEvery = 1.0;          // the record is sent at most this often
constexpr double kGone = 30.0;              // a leader of unknown node absent this long has left
constexpr size_t kQueue = 8;
constexpr size_t kEscorts = 16;
constexpr unsigned kAskSlotShift = 1;       // StoryFollow's flag byte: bit 0 the attach's flag, bits 1..3 the asker's slot + 1

struct Ask {
    uint16_t data;   // the creature's data id
    uint8_t slot;    // the asker's session slot
};

struct Escort {
    uint16_t data;        // the creature's data id
    uint8_t slot;         // its leader's session slot
    uint8_t node;         // and node (the authority's own for its own player)
    uint16_t index = 0;   // the creature's save index, as read from the live creature (0: not seen yet); names its row in the save
    bool by_default = false;   // the authority: nobody's story asked for this leader (see the header)
    bool told = false;    // the authority: a failed attach was logged
    double gone_at = 0.0; // the authority: since when its leader's player is not here, for a leader whose node is not known
};

struct Refused {
    uint32_t repeats = 0;   // refusals not logged since the last line
    double line_at = 0.0;
};

struct State {
    std::vector<Ask> outgoing;      // a member's asks not sent yet
    std::vector<uint16_t> released; // a member's releases of its own escorts not sent yet (data ids)
    std::vector<Escort> escorts;    // the authority: the record; a member: the authority's last copy
    bool dirty = false;             // the authority: the record changed since it was sent
    bool loaded = false;            // the authority: its first look ran (the saved record of this stage is read; nothing is sent before)
    bool unmarked = false;          // the record's creatures changed since the marks were written (a member: a record arrived)
    bool member = false;            // this process is a member of the session (its marks are in the visiting copy)
    std::vector<uint64_t> ended;    // the authority: the releases its content made, as record (content entry, id) and creature
    double sent_at = -1e9, looked_at = -1e9;
    bool own_call = false;          // this module calls the game's leave: the guard lets it run
    // The authority's refused leaves by (asker, creature, leader): a member's game sends the same leave again at each AI state change
    // of the creature, so a repeat is counted and logged with the next line of its key (at most every kRefusedLineGap).
    std::unordered_map<uint64_t, Refused> refused;
    std::unordered_map<uint64_t, double> kept;   // the guard's log lines by (creature, caller): one per kRefusedLineGap
};
State& st() {
    static State s;
    return s;
}

// 0x2a1f44's test of an id: bit 11 clear and (id >> 12) - 1 <= 3 (a player or character id, not the empty one).
bool valid_id(uint16_t id) { return !(id & 0x800) && static_cast<uint32_t>((id >> 12) - 1) <= 3; }

uint64_t player_manager() { return guest::rd<uint64_t>(base() + layout::kPlayerManager); }

// The actor id of the player this process's copy of the creature (&u16 actor id) follows, or 0 when it follows nobody here.
uint16_t leader_here(::Ctx* c, uint64_t npc) {
    const uint64_t pm = player_manager();
    if (!pm) return 0;
    const uint64_t actor = guest::call(c, base() + kActorById, pm, npc);
    if (!guest::ok(actor, 8)) return 0;
    const uint64_t vt = guest::rd<uint64_t>(actor);
    if (!guest::ok(vt + kLeaderOf, 8) || !(guest::call(c, guest::rd<uint64_t>(vt + kInParty), actor) & 1)) return 0;
    const uint64_t leader = guest::call(c, guest::rd<uint64_t>(vt + kLeaderOf), actor);
    if (!guest::ok(leader + kPartyBlock + kPartyLeaderId, 2)) return 0;
    const uint16_t id = guest::rd<uint16_t>(leader + kPartyBlock + kPartyLeaderId);
    return valid_id(id) ? id : 0;
}

// The session's player characters here: the actor id of the one with member index `index` (0: none), and the member index of the
// one with actor id `actor` (-1: none).
struct Players {
    uint16_t of_index = 0;
    int index_of = -1;
};
Players players(uint32_t index, uint16_t actor) {
    Players p;
    const uint64_t pm = player_manager();
    if (!guest::ok(pm + layout::kManagerPlayer, kPlayerSlots * 8)) return p;
    for (uint64_t i = 0; i < kPlayerSlots; ++i) {
        const uint64_t ch = guest::rd<uint64_t>(pm + layout::kManagerPlayer + i * 8);
        if (!guest::ok(ch + kActorId, 2) || !guest::ok(ch + kNetIndex, 1)) continue;
        const uint16_t id = guest::rd<uint16_t>(ch + kActorId);
        const uint8_t net = guest::rd<uint8_t>(ch + kNetIndex);
        if (net == index && valid_id(id)) p.of_index = id;
        if (id == actor) p.index_of = net;
    }
    return p;
}

// This process's own player's session slot (kIndexMax: no player).
uint8_t own_slot() {
    const uint64_t own = guest::deref(player_manager() + layout::kManagerPlayer);
    return guest::ok(own + kNetIndex, 1) ? guest::rd<uint8_t>(own + kNetIndex) : static_cast<uint8_t>(kIndexMax);
}

Escort* escort_of(uint16_t data) {
    for (Escort& e : st().escorts)
        if (e.data == data) return &e;
    return nullptr;
}

// The game's own leave for the creature, from this module (the guard lets it run).
void leave(::Ctx* c, uint16_t actor) {
    State& s = st();
    const uint64_t sp = guest::scratch(c);
    guest::wr<uint16_t>(sp + 0x10, actor);
    s.own_call = true;
    guest::call(c, base() + kLeave, 0, sp + 0x10, 0, 1, 0, sp);
    s.own_call = false;
}

// The valid ids of one of this process's party lists (at most kLeaderPlaces).
size_t list_ids(uint64_t which, uint16_t* out) {
    const uint64_t pg = guest::deref(base() + layout::kPartyManager);
    const uint64_t list = pg + layout::kPartyLists + which * layout::kPartyListSize;
    if (!pg || !guest::ok(list, layout::kPartyListSize)) return 0;
    const uint64_t ids = guest::rd<uint64_t>(list + kListIds), count = guest::rd<uint64_t>(list + kListCount);
    if (count > kLeaderPlaces || !guest::ok(ids, count * 2)) return 0;
    size_t n = 0;
    for (uint64_t i = 0; i < count; ++i)
        if (const uint16_t id = guest::rd<uint16_t>(ids + i * 2); valid_id(id)) out[n++] = id;
    return n;
}

bool in_list(uint64_t which, uint16_t actor) {
    uint16_t ids[kLeaderPlaces];
    const size_t n = list_ids(which, ids);
    for (size_t i = 0; i < n; ++i)
        if (ids[i] == actor) return true;
    return false;
}

// The record's creatures whose row is known are marked. False while the save is not there.
bool save_marks(State& s) {
    bool done = true;
    for (const Escort& e : s.escorts)
        if (e.index) done = marks::put(e.index, true, s.member) && done;
    if (done) s.unmarked = false;
    return done;
}

// The save index of the creature whose u16 actor id is at npc: the u32 the game's own attach, leave and restore take (0: none).
uint32_t save_index(::Ctx* c, uint64_t npc) {
    const uint64_t pm = player_manager();
    const uint64_t actor = pm ? guest::call(c, base() + kActorById, pm, npc) : 0;
    if (!guest::ok(actor, 8)) return 0;
    const uint64_t vt = guest::rd<uint64_t>(actor);
    if (!guest::ok(vt + kLeaderOf, 8) || !(guest::call(c, guest::rd<uint64_t>(vt + kInParty), actor) & 1)) return 0;
    const uint64_t part = guest::call(c, guest::rd<uint64_t>(vt + kLeaderOf), actor);
    return guest::ok(part + kPartyBlock + kPartyIndex, 4) ? guest::rd<uint32_t>(part + kPartyBlock + kPartyIndex) : 0;
}

// ---- the authority: the record ----

// An escort nobody's story asked for here: this process's own player leads it until a player's story asks.
void default_escort(State& s, uint16_t data, uint32_t index, const char* why) {
    const uint8_t slot = own_slot();
    if (slot >= kPlayerSlots || escort_of(data) || s.escorts.size() >= kEscorts) return;
    Escort e{data, slot, link::self()};
    e.index = static_cast<uint16_t>(index);
    e.by_default = true;
    s.escorts.push_back(e);
    s.dirty = s.unmarked = true;
    NXLOG_I(kTag, "Co-op follow: character %u (save index %u) %s; it is the escort of this process's player (%u) until a player's "
            "story asks for it or the story releases it", data, index, why, slot);
}

// The story's own list is not used in a session: a creature the game's restore put in it before the session is taken out with
// the story's own leave (which clears its mark: the record's creatures are marked again by the next write).
void empty_story_list(::Ctx* c, State& s) {
    uint16_t ids[kLeaderPlaces];
    const size_t n = list_ids(layout::kPartyStoryList, ids);
    const uint64_t pg = guest::deref(base() + layout::kPartyManager);
    const uint16_t own = n ? players(own_slot(), 0).of_index : 0;
    if (!own || !pg) return;
    const uint64_t sp = guest::scratch(c);
    for (size_t i = 0; i < n; ++i) {
        guest::wr<uint16_t>(sp + 0x10, own);
        guest::wr<uint16_t>(sp + 0x14, ids[i]);
        const uint16_t data = static_cast<uint16_t>(guest::call(c, base() + kDataIdOf, sp + 0x14, 0, 0, 0, 0, sp));
        if (!s.member && static_cast<uint16_t>(data - 1) < kDataIdMax)
            default_escort(s, data, save_index(c, sp + 0x14), "was the story's follower before the session");
        guest::wr<uint16_t>(sp + 0x10, own);
        guest::wr<uint16_t>(sp + 0x14, ids[i]);
        s.own_call = true;
        guest::call(c, base() + kStoryLeave, pg, sp + 0x10, sp + 0x14, 0, 0, sp);
        s.own_call = false;
        NXLOG_I(kTag, "Co-op follow: character %u (actor 0x%x) was in this process's story list, restored by the game before the "
                "session; it is taken out%s", data, ids[i], escort_of(data) ? " and follows as an escort" : "");
    }
    s.unmarked = true;
}

// The saved record, as far as its creatures are in this stage: a live creature whose row in the save carries the story mark.
void discover(::Ctx* c, State& s) {
    const uint64_t pm = player_manager();
    if (!guest::ok(pm + kResidentLists, (uint64_t{kDataIdMax} + 1) * 8)) return;
    for (uint16_t data = 1; data <= kDataIdMax; ++data) {
        if (!guest::rd<uint64_t>(pm + kResidentLists + uint64_t{data} * 8) || escort_of(data)) continue;
        const uint64_t actor = guest::call(c, base() + kFirstOf, pm, data);
        const uint32_t index = actor && guest::ok(actor + kActorId, 2) ? save_index(c, actor + kActorId) : 0;
        if (marks::marked(index)) default_escort(s, data, index, "is marked in the save as the story's follower");
    }
    s.loaded = true;
}

// The creature becomes an escort: of the player that leads it here already, else of the asker (slot, node).
void adopt(::Ctx* c, uint16_t data, uint8_t slot, uint8_t node) {
    State& s = st();
    const uint64_t pm = player_manager();
    const uint64_t actor = c && pm ? guest::call(c, base() + kFirstOf, pm, data) : 0;
    Escort* had = escort_of(data);
    if (had && had->by_default) {   // nobody's story chose that leader: the asker's does
        if (had->slot != slot)
            NXLOG_I(kTag, "Co-op follow: character %u followed player %u by default; player %u's story asks for it and leads it now",
                    data, had->slot, slot);
        had->slot = slot;
        had->node = node;
        had->by_default = had->told = false;
        had->gone_at = 0.0;
        s.dirty = true;
        return;
    }
    if (actor && guest::ok(actor + kActorId, 2)) {
        const uint16_t leader = leader_here(c, actor + kActorId);
        const int led_by = leader ? players(kIndexMax + 1, leader).index_of : -1;
        if (led_by >= 0 && led_by != slot) {
            slot = static_cast<uint8_t>(led_by);
            node = led_by == own_slot() ? link::self() : wire::kNobody;
        }
    }
    if (Escort* e = had) {
        if (e->slot == slot && e->node == wire::kNobody && node != wire::kNobody) {
            e->node = node;   // its leader's own ask: the node is known now
            s.dirty = true;
            return;
        }
        if (e->slot == slot) return;
        NXLOG_I(kTag, "Co-op follow: character %u, the escort of player %u, is asked for by player %u; it stays with player %u", data,
                e->slot, slot, e->slot);
        return;   // the story asked again on another process: the first asker leads until the story releases it
    }
    if (s.escorts.size() >= kEscorts) {
        NXLOG_W(kTag, "Co-op follow: character %u is not made an escort: %zu are in the record already", data, s.escorts.size());
        return;
    }
    s.escorts.push_back(Escort{data, slot, node});
    s.dirty = s.unmarked = true;
    NXLOG_I(kTag, "Co-op follow: character %u is the escort of player %u (node %u) until the story releases it", data, slot, node);
}

// The story released the creature (told by `who`): out of the record, and it leaves whoever leads it here.
void release(::Ctx* c, uint16_t data, const char* who) {
    State& s = st();
    for (size_t i = 0; i < s.escorts.size(); ++i) {
        if (s.escorts[i].data != data) continue;
        uint32_t index = s.escorts[i].index;
        s.escorts.erase(s.escorts.begin() + static_cast<std::ptrdiff_t>(i));
        s.dirty = true;
        const uint64_t pm = player_manager();
        const uint64_t actor = pm ? guest::call(c, base() + kFirstOf, pm, data) : 0;
        if (!index && actor && guest::ok(actor + kActorId, 2)) index = save_index(c, actor + kActorId);
        marks::put(index, false, false);
        if (actor && guest::ok(actor + kActorId, 2) && leader_here(c, actor + kActorId)) leave(c, guest::rd<uint16_t>(actor + kActorId));
        NXLOG_I(kTag, "Co-op follow: the story released character %u (%s); it is no escort any more", data, who);
        return;
    }
}

// One invited follower of the slot that is no escort leaves, so that the slot has a free place. False when it has none to give.
bool make_room(::Ctx* c, uint8_t slot) {
    const uint64_t pg = guest::deref(base() + layout::kPartyManager);
    const uint64_t list = pg + layout::kPartyLists + slot * layout::kPartyListSize;
    if (!pg || slot >= layout::kPartySlotLists || !guest::ok(list, layout::kPartyListSize)) return false;
    const uint64_t ids = guest::rd<uint64_t>(list + kListIds), count = guest::rd<uint64_t>(list + kListCount);
    if (count > kLeaderPlaces || !guest::ok(ids, count * 2)) return false;
    uint64_t used = 0;
    uint16_t free_one = 0;
    for (uint64_t i = 0; i < count; ++i) {
        const uint16_t id = guest::rd<uint16_t>(ids + i * 2);
        if (!valid_id(id)) continue;
        ++used;
        if (!free_one && !escort_of(static_cast<uint16_t>(guest::call(c, base() + kDataIdOf, ids + i * 2)))) free_one = id;
    }
    if (used < kLeaderPlaces) return true;
    if (!free_one) return false;
    leave(c, free_one);
    return true;
}

}  // namespace

// The story's follow is not the creature's choice: the invited follower (u16 actor id at npc) is taken as the party tick's accept
// takes it. The online authority's party tick 0x656200 asks 0x16570(&player, &creature, 0, &message, 1) of every invited follower
// whose follow is on and whose leader id is not set yet; on a yes it writes the leader id and raises kind 4 for the slot, on a no it
// turns the follow off and takes the creature out of the list (0x658560, not the leave 0x659588). The story's own attach
// (0x659280) asks nothing, and 0x16570 says no to a creature the story holds: an escort was dropped after every attach and followed
// nobody. Here the yes is written before the tick asks. False when the creature's follow is not on (the invite did not attach it).
bool accept(::Ctx* c, uint64_t npc, uint16_t leader, uint8_t slot) {
    const uint64_t pm = player_manager();
    const uint64_t actor = pm ? guest::call(c, base() + kActorById, pm, npc) : 0;
    if (!guest::ok(actor, 8)) return false;
    const uint64_t vt = guest::rd<uint64_t>(actor);
    if (!guest::ok(vt + kPartOf, 8) || !(guest::call(c, guest::rd<uint64_t>(vt + kInParty), actor) & 1)) return false;
    const uint64_t obj = guest::call(c, guest::rd<uint64_t>(vt + kPartOf), actor);
    const uint64_t part = obj ? obj + kPartyBlock : 0;
    if (!part || !guest::ok(part + kPartFollowing, 1) || !guest::rd<uint8_t>(part + kPartFollowing)) return false;
    guest::wr<uint16_t>(part + kPartyLeaderId, leader);
    const uint64_t gm = guest::deref(base() + layout::kGameState);
    if (gm && guest::ok(gm + kStateLeaving, 1) && !(guest::rd<uint8_t>(gm + kStateLeaving) & 1))
        guest::call(c, base() + kRaise, gm + kRaiseQueue, kRaiseParty, slot, 0);
    return true;
}

uint16_t player_actor(uint8_t slot) { return players(slot, 0).of_index; }

bool is_escort(uint16_t data) { return escort_of(data) != nullptr; }

namespace {

// The escort follows its leader here, or is attached now. False when it should and does not.
bool match(::Ctx* c, Escort& e) {
    const uint64_t pm = player_manager();
    const uint64_t actor = pm ? guest::call(c, base() + kFirstOf, pm, e.data) : 0;
    if (!actor || !guest::ok(actor + kActorId, 2)) return true;   // the creature is not in this stage: nothing to match
    const uint16_t leader = players(e.slot, 0).of_index;
    if (!leader) {   // its leader's player is not here (yet)
        if (e.node != wire::kNobody || own_slot() >= kPlayerSlots) return true;   // a known node's leave is told (on_member_change)
        const double now = qpc::seconds();
        if (e.gone_at == 0.0) e.gone_at = now;
        if (now - e.gone_at < kGone) return true;
        NXLOG_I(kTag, "Co-op follow: character %u's leader, player %u, has not been here for %.0f s; it is this process's player's "
                "escort now", e.data, e.slot, now - e.gone_at);
        e.slot = own_slot();
        e.node = link::self();
        e.by_default = true;
        e.gone_at = 0.0;
        st().dirty = true;
        return false;
    }
    e.gone_at = 0.0;
    const uint64_t npc = actor + kActorId;
    const uint64_t pg = guest::deref(base() + layout::kPartyManager);
    const uint64_t sp = guest::scratch(c);
    if (!e.index) {
        e.index = static_cast<uint16_t>(save_index(c, npc));
        if (e.index) st().dirty = st().unmarked = true;   // its row is known now: marked, and told to the members
    }
    const uint16_t id = guest::rd<uint16_t>(npc);
    const uint16_t now = leader_here(c, npc);
    if (now == leader && in_list(e.slot, id)) return true;
    bool room = true;
    if (!in_list(e.slot, id)) {   // in the list already: the invite would return at once (0x659f40), only the accept is missing
        if (now && now != leader) leave(c, id);
        room = make_room(c, e.slot);
        guest::wr<uint16_t>(sp + 0x10, leader);
        guest::wr<uint16_t>(sp + 0x14, guest::rd<uint16_t>(npc));
        if (pg) guest::call(c, base() + kInvite, pg, sp + 0x10, sp + 0x14, 1, 0, sp);
    }
    if (in_list(e.slot, id)) accept(c, npc, leader, e.slot);
    const bool led = leader_here(c, npc) == leader && in_list(e.slot, id);
    if (led || !e.told)
        NXLOG_I(kTag, "Co-op follow: character %u (actor 0x%x), the escort of player %u: %s", e.data, guest::rd<uint16_t>(npc), e.slot,
                led ? "follows that player now" : room ? "the attach did not take; it is made again" : "that player has no free place");
    e.told = !led;
    return led;
}

void send_record(State& s) {
    wire::Writer w(link::self());
    w.u8(static_cast<uint8_t>(s.escorts.size()));
    for (const Escort& e : s.escorts) w.u16(e.data).u8(e.slot).u8(e.node).u16(e.index);
    link::to_members(wire::Type::PartyEscorts, w.take());
    s.dirty = false;
    s.sent_at = qpc::seconds();
}

// ---- a member: its asks and releases ----

void send() {
    State& s = st();
    size_t sent = 0;
    for (const Ask& a : s.outgoing) {
        const uint8_t flag = static_cast<uint8_t>(1 | (a.slot + 1) << kAskSlotShift);
        if (!link::to_authority(wire::Type::StoryFollow, wire::Writer(link::self()).u8(0).u16(a.data).u8(flag).take())) break;
        NXLOG_I(kTag, "Co-op follow: the story's follow of character %u sent to the authority (this player's slot %u)", a.data, a.slot);
        ++sent;
    }
    s.outgoing.erase(s.outgoing.begin(), s.outgoing.begin() + static_cast<std::ptrdiff_t>(sent));
    sent = 0;
    for (uint16_t data : s.released) {
        if (!link::to_authority(wire::Type::EscortRelease, wire::Writer(link::self()).u16(data).take())) break;
        ++sent;
    }
    s.released.erase(s.released.begin(), s.released.begin() + static_cast<std::ptrdiff_t>(sent));
}

}  // namespace

int on_story_follow(::Ctx* c) {
    if (!link::in_session()) return 0;
    const uint64_t npc = c->x[0];
    if (!guest::ok(npc, 2) || !valid_id(guest::rd<uint16_t>(npc))) return 0;   // 0x659280 returns at once on these: as the game does
    const uint16_t data = static_cast<uint16_t>(guest::call(c, base() + kDataIdOf, npc));
    if (static_cast<uint16_t>(data - 1) >= kDataIdMax) return 0;
    const uint8_t slot = own_slot();
    if (slot >= kPlayerSlots) return 0;
    State& s = st();
    if (link::is_authority()) {
        adopt(c, data, slot, link::self());
        return 1;   // the story's own attach does not run: the record's tick attaches the escort
    }
    bool queued = false;
    for (const Ask& a : s.outgoing) queued = queued || a.data == data;
    if (!queued && s.outgoing.size() < kQueue) s.outgoing.push_back(Ask{data, slot});
    return 1;
}

int on_leave(::Ctx* c) {
    State& s = st();
    if (s.own_call || s.escorts.empty() || !link::in_session()) return 0;
    const uint64_t npc = c->x[kLeaveNpc];
    if (!guest::ok(npc, 2) || !valid_id(guest::rd<uint16_t>(npc))) return 0;
    const uint16_t data = static_cast<uint16_t>(guest::call(c, base() + kDataIdOf, npc));
    const Escort* e = escort_of(data);
    if (!e) return 0;
    const uint64_t from = c->x[30] - base();
    if (from == kTeardownLeave) return 0;
    double& line_at = s.kept[uint64_t{data} << 32 | (from & 0xffffffff)];
    const double now = qpc::seconds();
    if (line_at == 0.0 || now - line_at >= kRefusedLineGap) {
        line_at = now;
        NXLOG_I(kTag, "Co-op follow: character %u stays the escort of player %u: a leave from main+0x%llx is not the story's release",
                data, e->slot, static_cast<unsigned long long>(from));
    }
    c->x[0] = 0;
    return 1;
}

namespace {

// The story released the creature of data id `data` in a session: an escort is released on the authority; on the member that
// leads it the release is sent to the authority.
void story_released(::Ctx* c, uint16_t data) {
    State& s = st();
    const Escort* e = escort_of(data);
    if (!e) return;
    if (link::is_authority()) {
        behalf::Run run;
        const bool in_record = behalf::running(&run);
        const uint64_t key = (uint64_t{run.entry} << 40) ^ (uint64_t{run.id} << 16) ^ data;
        bool again = false;
        for (uint64_t k : s.ended) again = again || k == key;
        if (in_record && again && run.origin >= 0 && e->node != wire::kNobody && run.origin != e->node) {
            NXLOG_I(kTag, "Co-op follow: content entry %u record %u releases character %u again, for node %d's notice; node %u leads "
                    "it, it stays an escort", run.entry, run.id, data, run.origin, e->node);
            return;
        }
        if (in_record && !again && s.ended.size() < kEnded) s.ended.push_back(key);
        release(c, data, "this process's content");
    } else if (e->node == link::self()) {
        bool queued = false;
        for (uint16_t d : s.released) queued = queued || d == data;
        if (!queued && s.released.size() < kQueue) s.released.push_back(data);
    }
}

// The u16 actor id at npc names a creature (1..kDataIdMax): its data id, else 0.
uint16_t data_of(::Ctx* c, uint64_t npc) {
    if (!guest::ok(npc, 2) || !valid_id(guest::rd<uint16_t>(npc))) return 0;
    const uint16_t data = static_cast<uint16_t>(guest::call(c, base() + kDataIdOf, npc));
    return static_cast<uint16_t>(data - 1) < kDataIdMax ? data : 0;
}

}  // namespace

int on_story_leave(::Ctx* c) {
    if (st().own_call) return 0;
    const uint64_t npc = c->x[kStoryLeaveNpc];
    const uint16_t data = data_of(c, npc);
    if (!data) return 0;
    if (!link::in_session()) {
        // The story released it: in a host's save no mark stays behind for a creature that is not in the story's list.
        if (role() == CoopRole::Host) marks::put(save_index(c, npc), false, false);
        return 0;
    }
    story_released(c, data);
    return 0;   // the story's own leave runs on (its list holds no escort)
}

int on_story_release(::Ctx* c) {
    if (st().own_call || !link::in_session()) return 0;
    const uint64_t npc = c->x[0];
    const uint16_t data = data_of(c, npc);
    // A creature with a leader goes on to the story's own leave 0x659bf8, which answers for it (on_story_leave).
    if (data && !leader_here(c, npc)) story_released(c, data);
    return 0;
}

int on_party_request(::Ctx* c) {
    if (!link::in_session() || !link::is_authority() || c->x[30] != base() + kPartyCall) return 0;
    const uint64_t msg = c->x[0];
    if (!guest::ok(msg + kMsgNpc, kMsgHome - kMsgNpc + 1)) return 0;
    if (guest::rd<uint8_t>(msg + kMsgHome) || guest::rd<uint8_t>(msg + kMsgJoin)) return 0;   // a home-navi or a join: the game's
    const uint64_t npc = msg + kMsgNpc;
    const uint16_t leader = leader_here(c, npc);
    if (!leader) return 0;   // it follows nobody here: the handler's own leader test returns
    const uint32_t asker = static_cast<uint32_t>(c->x[kSenderReg]);
    const Players p = players(asker, leader);
    const uint16_t data = static_cast<uint16_t>(guest::call(c, base() + kDataIdOf, npc));
    if (asker > kIndexMax || !p.of_index || p.index_of < 0) {
        NXLOG_I(kTag, "Co-op follow: member %u's party leave of character %u (actor 0x%x, led by player 0x%x): %s; the game's own leave "
                "runs", asker, data, guest::rd<uint16_t>(npc), leader,
                asker > kIndexMax || !p.of_index ? "the asker's player is not here" : "its leader is no player here");
        return 0;
    }
    if (p.of_index == leader) return 0;   // the asker's own follower: the game's leave (an escort is kept at the leave itself)
    c->x[0] = 0;   // handled: the sender does not send it again
    const uint16_t actor = guest::rd<uint16_t>(npc);
    Refused& r = st().refused[uint64_t{asker} << 32 | uint64_t{actor} << 16 | leader];
    const double now = qpc::seconds();
    if (r.line_at != 0.0 && now - r.line_at < kRefusedLineGap) {
        ++r.repeats;
        return 1;
    }
    NXLOG_I(kTag, "Co-op follow: member %u (player 0x%x) asked to take character %u (actor 0x%x) out of the party of member %d (player "
            "0x%x); refused, a leave only takes a creature from the player who asked (%u more refused since the last line)", asker,
            p.of_index, data, actor, p.index_of, leader, r.repeats);
    r.repeats = 0;
    r.line_at = now;
    return 1;
}

void release_for_move(::Ctx* c) {
    if (!link::in_session() || !link::is_authority()) return;
    carry::before_move(c);
    uint16_t ids[party::kInvitedMost];
    const size_t n = party::invited(ids, party::kInvitedMost);
    if (!n) return;
    for (size_t i = 0; i < n; ++i) leave(c, ids[i]);
    NXLOG_I(kTag, "Co-op follow: the group moves to another area; its %zu invited followers were released first (%zu escorts stay in "
            "the record and are attached again on arrival, the others who can come follow again there)", n, st().escorts.size());
}

void on_ask_message(::Ctx* c, int, uint8_t origin, wire::Reader& r) {
    r.u8();   // the asker's stage: the record holds for every stage
    const uint16_t data = r.u16();
    const uint8_t flag = r.u8();
    const uint8_t slot = static_cast<uint8_t>((flag >> kAskSlotShift) - 1);
    if (!r.ok() || !link::is_authority() || static_cast<uint16_t>(data - 1) >= kDataIdMax || slot >= kPlayerSlots) return;
    adopt(c, data, slot, origin);
}

void on_release_message(::Ctx* c, int, uint8_t origin, wire::Reader& r) {
    const uint16_t data = r.u16();
    if (!r.ok() || !link::is_authority() || !c) return;
    const Escort* e = escort_of(data);
    if (e && e->node == origin) release(c, data, "its leader's content");
}

void on_record_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    if (link::is_authority()) return;
    std::vector<Escort> got;
    const uint8_t n = r.u8();
    for (uint8_t i = 0; i < n && r.ok() && i < kEscorts; ++i) {
        const uint16_t data = r.u16();
        const uint8_t slot = r.u8(), node = r.u8();
        got.push_back(Escort{data, slot, node, r.u16()});
    }
    if (!r.ok()) return;
    State& s = st();
    for (const Escort& old : s.escorts) {   // out of the record: its mark in the visiting copy goes
        bool kept = false;
        for (const Escort& e : got) kept = kept || e.index == old.index;
        if (!kept) marks::put(old.index, false, true);
    }
    s.escorts = std::move(got);
    s.unmarked = true;
}

void on_member_change(int node, bool joined) {
    State& s = st();
    if (!link::is_authority()) return;
    s.dirty = true;   // a newcomer needs the record
    if (joined) return;
    for (Escort& e : s.escorts) {
        if (e.node != node) continue;
        NXLOG_I(kTag, "Co-op follow: player %u left; its escort, character %u, is this process's player's now", e.slot, e.data);
        e.slot = own_slot();
        e.node = link::self();
        e.by_default = true;
        e.told = false;
    }
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!c) return;
    if (!link::in_session()) {
        if (s.member)   // the session is over: the visiting copy keeps no mark of ours
            for (const Escort& e : s.escorts) marks::put(e.index, false, true);
        s = State{};
        return;
    }
    s.member = !link::is_authority();
    const double now = qpc::seconds();
    const bool look = now - s.looked_at >= kRetry && session::can_walk(in_game) && !talk::talking() && !travel::moving();
    if (!link::is_authority()) {
        send();
        if (look) {
            s.looked_at = now;
            empty_story_list(c, s);
        }
        if (s.unmarked) save_marks(s);
        return;
    }
    if (look) {
        s.looked_at = now;
        discover(c, s);   // before the take-out below, whose leave clears the mark
        empty_story_list(c, s);
        for (Escort& e : s.escorts)   // every escort at every look: one that cannot be attached does not hold up the others
            if (e.slot < kPlayerSlots) match(c, e);
    }
    if (s.unmarked) save_marks(s);
    if (s.loaded && s.dirty && link::member_count() && now - s.sent_at >= kSendEvery) send_record(s);
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::follow
