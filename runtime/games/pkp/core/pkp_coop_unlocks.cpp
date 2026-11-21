// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's shared unlocks (pkp_coop_internal.h): the moves and emotes any member learned, the areas the group
// visited, the buildings it finished and the area gates it opened, unlocked for every member. The game keeps each of these first-time
// lists in the player's own save and sets them on one process: a character's reward talk or an event's callback runs on the talker's
// process alone, an area visit is recorded only when the player is not a Link Play visitor. So the others never learned the move or
// emote, and a member's save never counted the areas the group went to. Each process shares what grew in its lists (UnlockKnown), and
// every member gets the union (a member that is behind never takes one away).
//
// The game, 2.0.1 (modding-docs coop-v2/UNLOCK-LISTS-RE.md). R = save root, `*(main+0x3561930)`; one byte an entry, the bit named:
//   moves     R + 0x6d82f0 + id * 8, id 1..0x3c   m_pokeSkillHistory +4 bit 0 (learned). kMoveLearn(R + 0x108, id, notice) sets it
//                                                with no "already known" test, then (notice) queues the game's "learned a move"
//                                                event for slot 0 and puts the move on the wheel. Ids 0x14..0x18 are taught by the
//                                                player's own first use of an action (0x5a5aa0): never shared.
//   emotes    R + 0x6d84f4 + id, id 1..0x64       m_emoteHistory bit 0. kEmoteLearn(player, id, force, quiet, alt) -> 1 if learned
//                                                now; unless quiet it queues the "learned an emote" event (0xe9c) for the player's
//                                                slot; it adds the emote to the communication wheel either way.
//   areas     R + 0x6d7775 + stage * 0x34         m_stageHistoryVer2 +1 bit 4 (visited), stage 1..5 (the story areas). Written only
//                                                inline by the area load (0x62f7e0), for the current stage and never on a visitor:
//                                                the game has no setter for another stage, so the bit is set here directly. Nothing
//                                                else hangs off it: the shop and building conditions read the bit live, and the
//                                                travel list is rebuilt from it at an area load outside a session (0x62f7e0 returns
//                                                for a visitor before its rebuild). In a session only the host records areas, so only
//                                                the authority's areas are taken.
//   buildings R + 0x73b1d4 + id, id 0..0x7f       m_buildingHistory bit 0 (built once: the building list shows its picture).
//                                                kBuildingSet(id, 1) sets it (0 would clear it); no popup.
//   gates     R + 0x73b1aa + id, id 0..0x1f       m_gateHistory bit 0 (layout::kSaveGateHistory; AREA-GATE-RE.md): an area gate
//                                                opened. The door's first press opens it and records it (0x7b1040: the gate and its
//                                                paired gate through the setter 0x5c059c), a press with the bit set enters. The setter
//                                                refuses while a Link Play session is open, so in a session no gate was ever recorded
//                                                and every press replayed the opening: on_area_gate_record lets it through. A visitor's
//                                                getter (0x5c0504) reads the copy R + 0x1718470 + id (kSaveGateMirror), which no game
//                                                code fills: on a visitor the entry is read there and both bytes are set.
// The wheels (m_pokeSkillWheelHistory, m_communicationWheelHistory) are the player's own layout, filled by the learns above. The
// other first-time lists stay per player (the RE notes: items, cooking, collections, photo frames and filters); the tips' history is shared by
// pkp_coop_tutorials.cpp.
// Rules on a receiver: an entry another member has is set here at a safe moment (in the group's world, can walk, talks to no one),
// kSettle seconds after it arrived and only if its bit is still clear (the game's own path, such as quest effect 2 on every process,
// wins with its own scene). A few moves or emotes waiting: one a safe moment with the game's own event; more (a join's whole lists):
// quietly, kApplyPerTick a frame, with one line in the message log. Areas, buildings and area gates are always quiet, with that line.
// An area gate's door that is already loaded keeps its closed box until the next area load (0x7abe60 builds the open one from the
// bit); its next press enters all the same (0x7b0c50 -> 0x7b0d80).
// Each process reads its lists each frame and sends what grew, and every list every kAllEvery seconds and when it or another member
// joins, through the authority.
#include "core/pkp_coop_internal.h"
#include "core/pkp_strings.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <bitset>
#include <cstdio>
#include <string>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::unlocks {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kMoveLearn = 0x3bcca8;      // (R + 0x108, id, notice)
constexpr uint64_t kEmoteLearn = 0x2f5860;     // (player, id, force, quiet, alt) -> 1 if learned now
constexpr uint64_t kBuildingSet = 0x5c04d4;    // (id, on): bit 0 of the building's byte
constexpr uint64_t kSaveCommon = 0x108;        // R + this: m_saveCommon, the move learn's first argument

constexpr double kAllEvery = 30.0;             // every list again (a member that missed a change)
constexpr double kSettle = 8.0;                // seconds a received entry waits for the game's own path here
constexpr unsigned kLoudMax = 2;               // at most this many moves and emotes waiting: each with the game's own event
constexpr unsigned kApplyPerTick = 16;
constexpr size_t kIds = 0x80;                  // every list's ids fit 0..0x7f

// The setter's `tbnz w0, #0` after its session test (0x207b0): w0 bit 0 the session, w20 the gate id (< 0x20, tested before).
constexpr int kGateSessionReg = 0, kGateIdReg = 20;

enum class Set : uint8_t { Move, Emote, Area, Building, AreaGate };

using S = strings::Str;

struct List {
    uint8_t wire_id;                 // UnlockKnown +2
    Set set;
    uint64_t field;                  // R + field + id * stride: the entry's byte
    uint32_t stride;
    uint8_t bit;
    uint8_t first, last;             // ids first..last
    uint8_t skip_first, skip_last;   // ids never shared (skip_first > skip_last: none)
    bool authority_only;             // taken only from the authority
    bool loud;                       // the game has an event for it
    const char* one;                 // the log's words (NXLOG)
    const char* many;
    strings::Str shared, count_one, count_many;   // the player's line for one with the game's event, and the batch's counts
    uint64_t visitor_field = 0;      // non-zero: on a Link Play visitor the game reads the entry at R + this + id * stride instead
};
constexpr List kLists[] = {
    {1, Set::Move, 0x6d82f0, 8, 0x01, 1, 0x3c, 0x14, 0x18, false, true, "move", "moves", S::CoopSharedMove, S::CoopMovesOne, S::CoopMovesMany},
    {2, Set::Emote, 0x6d84f4, 1, 0x01, 1, 0x64, 1, 0, false, true, "emote", "emotes", S::CoopSharedEmote, S::CoopEmotesOne, S::CoopEmotesMany},
    {3, Set::Area, 0x6d7775, 0x34, 0x10, 1, 5, 1, 0, true, false, "area", "areas", S::CoopSharedArea, S::CoopAreasOne, S::CoopAreasMany},
    {4, Set::Building, 0x73b1d4, 1, 0x01, 0, 0x7f, 1, 0, false, false, "building", "buildings", S::CoopSharedBuilding, S::CoopBuildingsOne, S::CoopBuildingsMany},
    {5, Set::AreaGate, layout::kSaveGateHistory, 1, 0x01, 0, layout::kGateCount - 1, 1, 0, false, false, "area gate", "area gates",
     S::CoopSharedGate, S::CoopGatesOne, S::CoopGatesMany, layout::kSaveGateMirror},
};
constexpr size_t kListCount = sizeof kLists / sizeof kLists[0];
constexpr const List& kGateList = kLists[4];
static_assert(kGateList.set == Set::AreaGate);

struct ListState {
    std::bitset<kIds> seen;      // per id: set at the last read (and sent, once baseline_sent)
    std::bitset<kIds> pending;   // per id: another member has it, not set here yet
    uint8_t origin[kIds] = {};   // per pending id: the member that sent it
    double at[kIds] = {};        // per pending id: when it arrived
    unsigned quiet = 0;          // set quietly in the current batch (its line goes when it is done)
};
struct State {
    ListState lists[kListCount];
    bool have_seen = false;      // the first read of a session only takes the baseline
    bool baseline_sent = false;
    bool send_all = true;
    double all_at = 0.0;
    int batch_origin = -1;       // the node of the batch's first entry
};
State& st() {
    static State s;
    return s;
}

bool shared(const List& l, uint32_t id) { return id >= l.first && id <= l.last && !(id >= l.skip_first && id <= l.skip_last); }

uint64_t save_root() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    if (!r) return 0;
    for (const List& l : kLists) {
        const uint64_t size = static_cast<uint64_t>(l.last) * l.stride + 1;
        if (!guest::ok(r + l.field, size) || (l.visitor_field && !guest::ok(r + l.visitor_field, size))) return 0;
    }
    return r;
}
// Where the game reads the entry here: the visitor's copy on a Link Play visitor (as 0x376c4 picks), else the own byte.
uint64_t entry_at(uint64_t root, const List& l, uint32_t id) {
    const bool visitor = l.visitor_field && (session_bits() & layout::kSessionVisitor);
    return root + (visitor ? l.visitor_field : l.field) + id * l.stride;
}
bool has(uint64_t root, const List& l, uint32_t id) { return guest::rd<uint8_t>(entry_at(root, l, id)) & l.bit; }
void set_bit(uint64_t at, uint8_t bit) { guest::wr<uint8_t>(at, static_cast<uint8_t>(guest::rd<uint8_t>(at) | bit)); }

bool shared_here() { return (session_bits() & layout::kSessionOnline) && link::in_session(); }

// This player is in the group's world (the host, or a member once the visitor bit is set), can walk and talks to no one.
bool safe_now(bool in_game) {
    const bool in_world = role() == CoopRole::Host || (session_bits() & layout::kSessionVisitor);
    return in_world && session::can_walk(in_game) && !talk::talking();
}

// Sends, per list, the ids set since the last read (all: every id set). false: a send failed, and the same ids go at the next read.
bool share(uint64_t root, bool all) {
    State& s = st();
    const bool first = !s.have_seen;
    s.have_seen = true;
    for (size_t k = 0; k < kListCount; ++k) {
        const List& l = kLists[k];
        ListState& ls = s.lists[k];
        std::vector<uint8_t> ids;
        for (uint32_t id = l.first; id <= l.last; ++id) {
            if (!shared(l, id)) continue;
            if (!has(root, l, id)) {
                ls.seen.reset(id);   // a reloaded save may have lost it: sent again when it comes back
                continue;
            }
            if (all || first || !ls.seen.test(id)) ids.push_back(static_cast<uint8_t>(id));
        }
        if (ids.empty()) continue;
        wire::Writer w(link::self());
        w.u8(l.wire_id).u16(static_cast<uint16_t>(ids.size())).bytes(ids.data(), ids.size());
        if (!link::share(wire::Type::UnlockKnown, w.take())) return false;
        for (uint8_t id : ids) {
            if (s.baseline_sent && !first && !ls.seen.test(id)) NXLOG_I(kTag, "Co-op unlocks: %s %u set here, shared", l.one, id);
            ls.seen.set(id);
        }
    }
    s.baseline_sent = true;
    return true;
}

// The batch's line, once nothing more waits: "<name> unlocked 2 moves and 1 area for you too!"
void batch_line() {
    State& s = st();
    size_t lists = 0, done = 0;
    for (const ListState& ls : s.lists) lists += ls.quiet != 0;
    if (!lists) return;
    std::string parts;
    for (size_t k = 0; k < kListCount; ++k) {
        ListState& ls = s.lists[k];
        if (!ls.quiet) continue;
        if (done) parts += strings::get(base(), done + 1 == lists ? S::CoopListAnd : S::CoopListComma);
        parts += ls.quiet == 1 ? strings::get(base(), kLists[k].count_one) : strings::fmt(base(), kLists[k].count_many, ls.quiet);
        ++done;
        ls.quiet = 0;
    }
    notice::post(strings::fmt(base(), S::CoopUnlockedBatch, notice::name_of(static_cast<uint8_t>(s.batch_origin < 0 ? 0 : s.batch_origin)),
                              parts));
    s.batch_origin = -1;
}

// Sets one entry through the game's own setter (loud: with the game's event). false: it was set meanwhile, or no player.
bool set_entry(::Ctx* c, uint64_t root, const List& l, uint32_t id, bool loud) {
    if (has(root, l, id)) return false;   // the game's own path set it meanwhile (the move learn has no test of its own)
    switch (l.set) {
    case Set::Move:
        guest::call(c, base() + kMoveLearn, root + kSaveCommon, id, loud ? 1u : 0u);
        break;
    case Set::Emote: {
        const uint64_t player = local_player();
        if (!player) return false;
        guest::call(c, base() + kEmoteLearn, player, id, 0, loud ? 0u : 1u, 0);
        break;
    }
    case Set::Area:   // no setter for another stage (see the header)
        set_bit(root + l.field + id * l.stride, l.bit);
        break;
    case Set::Building:
        guest::call(c, base() + kBuildingSet, id, 1);
        break;
    case Set::AreaGate:   // the game's setter refuses in a session (see the header); the own byte, and the copy a visitor reads
        set_bit(root + l.field + id * l.stride, l.bit);
        set_bit(entry_at(root, l, id), l.bit);
        break;
    }
    return true;
}

// Sets the entries that settled: one move or emote with the game's event when only a few wait (one a safe moment: the event is
// queued, and the next safe moment comes once it closed), else up to kApplyPerTick quietly. Areas and buildings are always quiet.
void apply(::Ctx* c, uint64_t root) {
    State& s = st();
    const double now = qpc::seconds();
    size_t pending = 0, loud_settled = 0;
    bool settled = false, batching = false;
    for (size_t k = 0; k < kListCount; ++k) {
        const ListState& ls = s.lists[k];
        pending += ls.pending.count();
        batching |= kLists[k].loud && ls.quiet != 0;   // a quiet area or building does not silence a move
        for (uint32_t id = 0; id < kIds; ++id) {
            if (!ls.pending.test(id) || now - ls.at[id] < kSettle) continue;
            settled = true;
            loud_settled += kLists[k].loud;
        }
    }
    if (!pending) {
        batch_line();
        return;
    }
    if (!settled) return;
    const bool loud_ok = loud_settled <= kLoudMax && !batching;
    unsigned budget = kApplyPerTick;
    bool loud_done = false;
    for (size_t k = 0; k < kListCount && budget; ++k) {
        const List& l = kLists[k];
        ListState& ls = s.lists[k];
        const bool loud = l.loud && loud_ok;
        for (uint32_t id = 0; id < kIds && budget; ++id) {
            if (!ls.pending.test(id) || now - ls.at[id] < kSettle) continue;
            if (loud && loud_done) break;   // one event a safe moment
            ls.pending.reset(id);
            if (!set_entry(c, root, l, id, loud)) continue;
            --budget;
            if (s.have_seen) ls.seen.set(id);   // ours now: not sent back as a change
            const bool set_now = has(root, l, id);
            NXLOG_I(kTag, "Co-op unlocks: %s %u from node %u set here%s%s", l.one, id, ls.origin[id], loud ? " (the game's event)" : "",
                    set_now ? "" : ", the game did not set it");
            if (!set_now) continue;
            if (loud) {
                loud_done = true;
                notice::post(strings::fmt(base(), l.shared, notice::name_of(ls.origin[id])));
            } else {
                if (s.batch_origin < 0) s.batch_origin = ls.origin[id];
                ++ls.quiet;
            }
        }
    }
}

}  // namespace

int on_area_gate_record(::Ctx* c) {
    if (!(c->x[kGateSessionReg] & 1)) return 0;   // no session: the game's own write
    const uint32_t id = static_cast<uint32_t>(c->x[kGateIdReg]) & 0xff;
    c->x[kGateSessionReg] = 0;                    // the guard lets the write of the own byte through
    if (const uint64_t root = save_root(); root && id < layout::kGateCount) set_bit(entry_at(root, kGateList, id), kGateList.bit);
    NXLOG_I(kTag, "Co-op unlocks: area gate %u recorded here in the session", id);
    return 0;
}

void on_known_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint8_t list = r.u8();
    const uint16_t n = r.u16();
    if (!r.ok() || !shared_here()) return;
    const uint64_t root = save_root();
    if (!root) return;
    size_t k = 0;
    while (k < kListCount && kLists[k].wire_id != list) ++k;
    if (k == kListCount) return;   // a list of a newer build
    const List& l = kLists[k];
    if (l.authority_only && origin != link::authority()) return;
    ListState& ls = s.lists[k];
    for (uint16_t i = 0; i < n; ++i) {
        const uint8_t id = r.u8();
        if (!r.ok()) return;
        if (!shared(l, id) || has(root, l, id) || ls.pending.test(id)) continue;
        ls.pending.set(id);
        ls.origin[id] = origin;
        ls.at[id] = qpc::seconds();
    }
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    const uint64_t root = save_root();
    if (!root || !shared_here()) return;
    if (c && safe_now(in_game)) apply(c, root);
    const double now = qpc::seconds();
    const bool all = s.send_all || now - s.all_at >= kAllEvery;
    if (!share(root, all) || !all) return;
    s.send_all = false;
    s.all_at = now;
}

void on_session_change() {
    State& s = st();
    for (ListState& ls : s.lists) ls = ListState{};
    s.have_seen = false;
    s.baseline_sent = false;
    s.send_all = true;
    s.batch_origin = -1;
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::unlocks

// The digests' reader (pkp_coop_digest.cpp): every shared list's set ids, as (list << 16) | id.
namespace nxrt::games::pkp::coop::digest {

bool unlocks(Entries& out) {
    const uint64_t root = coop::unlocks::save_root();
    if (!root) return false;
    for (const coop::unlocks::List& l : coop::unlocks::kLists)
        for (uint32_t id = l.first; id <= l.last; ++id)
            if (coop::unlocks::shared(l, id) && coop::unlocks::has(root, l, id)) out.push_back({static_cast<uint32_t>(l.wire_id) << 16 | id, 1});
    return true;
}

}  // namespace nxrt::games::pkp::coop::digest
