// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's followers (pkp_coop_internal.h): a member's party leave takes no other player's follower, and a story content that makes a creature follow "this player" runs,
// on a member, on the authority instead, as the authority's own content does it. RE: modding-docs coop-v2/quests/QUEST-SCRIPT-RE.md
// (contents actions), coop-v2/P0-RE.md (RequestPartyNPC on a member); the fire starter creature is #7099 records 0x20d / 0x23b.
//
// A content record's command 0xdac (data id, mode) is 0x12c940 -> 0x117b18(data id, mode, 0, 1, 0). Mode 3 is "follow this
// process's player": with its w4 clear (0x12c940 passes 0) it calls 0x2a1ed4(&npc actor id, 1), which takes the leader from the
// local player ([0x3561928] + 0x30, its u16 actor id at +0x27fe) and calls 0x659280(party manager, &leader, &npc, flag). 0x659280
// first takes the creature out of the party it is in through 0x659588 (w5 = 1), then attaches it to the leader locally (0x6599c0
// accepts only a local leader, vt+0x1e0; party list manager +0x170) and marks its record +0xa (own R+0x23c3d0, or the visitor's
// copy R+0x16fefd8). On a visitor (every co-op member) 0x659588 does not detach: it sends RequestPartyNPC with +0x38 = 0 (a leave)
// and returns, and the host's handler 0x5519bc reads that form as "take the creature from its current leader" (+0x34, the asking
// player, is not read). So a member's story follow took the creature away from the host's player and attached it in the member's
// memory only, while the host runs the creature's AI: nobody led it and it wandered (tester report: the fire starter creature
// "refused to follow the host or the players to light the campfire and just wandered around"; #7099 record 0x20d starts it,
// record 0x23b repeats it every 30 s, record 0x20f ends it with `dac 2f 1` once the creature is in the campfire's box).
// 0x2a1ed4's only caller is 0x117b18, and 0x117b18's only mode-3 caller is 0x12c940 (0x11a694, contents 0xdf2, passes mode 1), so
// every call of it is a content's follow. The game's other attach, 0x404e34 -> 0x657a20 (the visitor's join request), fills other
// lists (manager +0x8.. per player slot, at most 10, with a home write 0x2c6bc0 offline) and is not this follow.
//   0x2a1ed4 (entry hook), on a member in the session: the game's call does not run (returns 1); the creature's data id is sent
//   to the authority (StoryFollow). On the authority, at a safe moment in the same stage (it can walk, no talk of its own), its copy
//   of the creature (0x28cc60(player manager, data id)): when it already follows a player there (vt+0x1d8, then vt+0x410's party
//   block +0x35e0 +0x10, the leader test 0x2a1f44 makes), nothing (the authority's own content or another member's ask came
//   first); otherwise the game's own 0x2a1ed4(&its actor id, flag): the creature follows the authority's player, exactly the call
//   the authority's own content makes, so the content's later records (0x20f's `dac 2f 1`) end it as the game does. An attach that
//   leaves the creature with no leader (0x659280 returns silently when the player cannot lead) is made again every 2 s, up to 8 times
//   and inside the ask's 60 s window. The leader is always the authority's own player: 0x6599c0 attaches only to a local player
//   (vt+0x1e0), and a member's player on the authority is not one. RE: coop-v2/role-census/build/a14-follow.md.
// A member's party leave (RequestPartyNPC +0x38 = 0, sent by 0x659588 on a visitor instead of a local detach: from an AI state
// change 0x2b5e08 at 0x2b5f64, the player action 0x2df220 at 0x2df510, the escort end 0x617d98, ...) carries the creature (+0x36)
// and the leader as the member sees it (+0x34), not who asks. The host's handler 0x5519bc (0x551a7c..0x551b38) reads the
// creature's leader in its own world and detaches it from that player, so a teammate's routine leave took a creature that followed
// the host or another teammate (story followers that stopped following). The rule: a leave only ever takes a creature from the
// player who asked.
//   0x5519bc (entry hook), on the authority in the session: called only by the receive entry 0x547980 (slot 16, return 0x547ab8),
//   whose w21 is the sender's KTGL member id - 1 (0x5479fc), the game's member index (SESSION-RE). The asker's player is the
//   session player character (player manager +0x30..+0x48) whose member index (u8 +0x5a38, vt+0x4a8 0x66725c) is that; the
//   leader's is the one whose actor id (+0x27fe) leads the creature (leader_here). Both found and different: refused, as handled
//   (returns 0, so it is not sent again) and logged. Anything that cannot be told apart (no leader here, an asker or a leader that
//   is no session player) is the game's own: the handler runs.
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
constexpr uint64_t kAttach = 0x2a1ed4;      // (&u16 npc actor id, w1 flag): attach to the local player (0x659280)
constexpr uint64_t kFirstOf = 0x28cc60;     // (player manager, data id) -> the first live resident of that data id, or 0
constexpr uint64_t kActorById = 0x281360;   // (player manager, &u16 actor id) -> the actor, or 0
constexpr uint64_t kDataIdOf = 0x27d848;    // (&u16 actor id) -> u16 the character's data id
constexpr uint64_t kActorId = 0x27fe;       // resident actor + : u16 its actor id
constexpr uint64_t kInParty = 0x1d8;        // actor vt + : bool, it is in a party
constexpr uint64_t kLeaderOf = 0x410;       // actor vt + : its leader's object, or 0
constexpr uint64_t kPartyBlock = 0x35e0;    // leader object + : its party block; +0x10 u16 the leader's actor id
constexpr uint64_t kPartyLeaderId = 0x10;
constexpr uint16_t kDataIdMax = 0x3ff;
constexpr uint64_t kPartyCall = 0x547ab8;   // the receive entry's return from a handler (blr at 0x547ab4)
constexpr int kSenderReg = 21;              // there: w21 = the sender's member index
constexpr uint64_t kMsgNpc = 0x36, kMsgJoin = 0x38, kMsgHome = 0x3a;   // RequestPartyNPC + : u16 creature, u8 join, u8 home-navi
constexpr uint64_t kNetIndex = 0x5a38;      // player character + : u8 its member index
constexpr uint64_t kPlayerSlots = 4;        // player manager + kManagerPlayer + 8 * i: the session's player characters
constexpr uint32_t kIndexMax = 0xff;
constexpr double kRefusedLineGap = 30.0;
constexpr double kDrop = 60.0;              // an ask the authority found no safe moment for
constexpr double kRetry = 2.0;              // an attach that left the creature with no leader is made again after this long
constexpr uint8_t kTries = 8;
constexpr size_t kQueue = 8;

struct Ask {
    uint8_t stage;
    uint16_t data;   // the creature's data id
    uint8_t flag;    // 0x2a1ed4's w1 & 1
    double at;
    uint8_t origin;
    uint8_t tries = 0;     // attaches made for this ask that left the creature with no leader
    double retry_at = 0;   // not before this time
};

struct Refused {
    uint32_t repeats = 0;   // refusals not logged since the last line
    double line_at = 0.0;
};

struct State {
    std::vector<Ask> outgoing;   // a member's asks not sent yet
    std::vector<Ask> incoming;   // the authority's received asks not made yet
    // The authority's refused leaves by (asker, creature, leader): a member's game sends the same leave again at each AI state change
    // of the creature, so a repeat is counted and logged with the next line of its key (at most every kRefusedLineGap).
    std::unordered_map<uint64_t, Refused> refused;
};
State& st() {
    static State s;
    return s;
}

uint8_t stage() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(gm, layout::kStateStage + 1) ? guest::rd<uint8_t>(gm + layout::kStateStage) : 0;
}

// 0x2a1f44's test of an id: bit 11 clear and (id >> 12) - 1 <= 3 (a player or character id, not the empty one).
bool valid_id(uint16_t id) { return !(id & 0x800) && static_cast<uint32_t>((id >> 12) - 1) <= 3; }

// The actor id of the player this process's copy of the creature (&u16 actor id) follows, or 0 when it follows nobody here.
uint16_t leader_here(::Ctx* c, uint64_t npc) {
    const uint64_t pm = guest::rd<uint64_t>(base() + layout::kPlayerManager);
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
    const uint64_t pm = guest::rd<uint64_t>(base() + layout::kPlayerManager);
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

void send() {
    State& s = st();
    size_t sent = 0;
    for (const Ask& a : s.outgoing) {
        if (!link::to_authority(wire::Type::StoryFollow, wire::Writer(link::self()).u8(a.stage).u16(a.data).u8(a.flag).take())) break;
        NXLOG_I(kTag, "Co-op follow: the story's follow of character %u (stage %u) sent to the authority", a.data, a.stage);
        ++sent;
    }
    s.outgoing.erase(s.outgoing.begin(), s.outgoing.begin() + static_cast<std::ptrdiff_t>(sent));
}

// The authority: the game's own attach for its copy of the creature, unless it follows a player already. True when the ask is settled
// (the creature follows a player, or is not here); false when the attach left it with no leader (the game's attach returns without
// a word when the authority's player cannot lead at that moment), so the ask is made again.
bool make(::Ctx* c, const Ask& a) {
    const uint64_t pm = guest::rd<uint64_t>(base() + layout::kPlayerManager);
    const uint64_t actor = pm ? guest::call(c, base() + kFirstOf, pm, a.data) : 0;
    if (!actor || !guest::ok(actor + kActorId, 2)) {
        NXLOG_I(kTag, "Co-op follow: node %u's follow of character %u dropped (no such character here)", a.origin, a.data);
        return true;
    }
    const uint64_t npc = actor + kActorId;
    if (const uint16_t leader = leader_here(c, npc)) {
        NXLOG_I(kTag, "Co-op follow: node %u's follow of character %u (actor 0x%x): it follows player 0x%x already, left as it is",
                a.origin, a.data, guest::rd<uint16_t>(npc), leader);
        return true;
    }
    guest::call(c, base() + kAttach, npc, a.flag);
    const uint16_t now = leader_here(c, npc);
    NXLOG_I(kTag, "Co-op follow: node %u's follow of character %u (actor 0x%x) made here (try %u); it follows player 0x%x", a.origin,
            a.data, guest::rd<uint16_t>(npc), a.tries + 1, now);
    return now != 0;
}

void apply(::Ctx* c, bool in_game) {
    State& s = st();
    const uint8_t here = stage();
    const double t = qpc::seconds();
    for (size_t i = 0; i < s.incoming.size();) {
        const Ask& a = s.incoming[i];
        if (a.stage != here || t - a.at >= kDrop) {
            NXLOG_I(kTag, "Co-op follow: node %u's follow of character %u dropped (%s)", a.origin, a.data,
                    a.stage != here ? "another stage here" : "no safe moment");
            s.incoming.erase(s.incoming.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    if (s.incoming.empty() || !session::can_walk(in_game) || talk::talking()) return;
    size_t i = 0;
    while (i < s.incoming.size() && s.incoming[i].retry_at > t) ++i;
    if (i == s.incoming.size()) return;
    Ask a = s.incoming[i];
    s.incoming.erase(s.incoming.begin() + static_cast<std::ptrdiff_t>(i));
    if (make(c, a)) return;
    if (++a.tries >= kTries) {
        NXLOG_I(kTag, "Co-op follow: node %u's follow of character %u dropped (the creature still follows nobody after %u tries)",
                a.origin, a.data, a.tries);
        return;
    }
    a.retry_at = t + kRetry;
    s.incoming.push_back(a);
}

}  // namespace

int on_story_follow(::Ctx* c) {
    if (!quests::member_rules()) return 0;
    const uint64_t npc = c->x[0];
    if (!guest::ok(npc, 2) || !valid_id(guest::rd<uint16_t>(npc))) return 0;   // 0x659280 returns at once on these: as the game does
    const uint16_t data = static_cast<uint16_t>(guest::call(c, base() + kDataIdOf, npc));
    if (static_cast<uint16_t>(data - 1) >= kDataIdMax) return 0;
    State& s = st();
    bool queued = false;
    for (const Ask& a : s.outgoing) queued = queued || a.data == data;
    if (!queued && s.outgoing.size() < kQueue)
        s.outgoing.push_back(Ask{stage(), data, static_cast<uint8_t>(c->x[1] & 1), qpc::seconds(), link::self()});
    NXLOG_I(kTag, "Co-op follow: the story asked character %u (actor 0x%x) to follow this player; left to the authority", data,
            guest::rd<uint16_t>(npc));
    return 1;   // the game's local-only attach and its leave request do not run
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
    if (p.of_index == leader) return 0;   // the asker's own follower: the game's leave
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

void on_ask_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint8_t at = r.u8();
    const uint16_t data = r.u16();
    const uint8_t flag = r.u8();
    if (!r.ok() || static_cast<uint16_t>(data - 1) >= kDataIdMax || s.incoming.size() >= kQueue) return;
    for (const Ask& a : s.incoming)
        if (a.data == data) return;   // one ask per creature waits
    NXLOG_I(kTag, "Co-op follow: node %u asks the authority to make character %u follow (stage %u)", origin, data, at);
    s.incoming.push_back(Ask{at, data, static_cast<uint8_t>(flag & 1), qpc::seconds(), origin});
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!c) return;
    if (!link::in_session()) {
        s = State{};
        return;
    }
    if (link::is_authority()) {
        s.outgoing.clear();
        apply(c, in_game);
    } else {
        s.incoming.clear();
        send();
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::follow
