// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's bag records (pkp_coop_internal.h; RE: modding-docs coop-v2 BAG-RECORDS-PLAN.md, P0-RE.md P0RE-11,
// REJOIN-BAG-2-RE.md). Vanilla Link Play treats a visitor's bag as the host's to keep and hand back, and a copy that drifts from the
// real bag came back at a rejoin, a drop or a crash: an empty bag, or another session's. In co-op the bag is ours instead (PROJECT.md
// 6.7): one record per member, which is the truth, written into whichever bag the game uses; the game's own writers that fight it are
// switched off (pkp_coop_bag.cpp).
//
// The game, 2.0.1a:
//   The bag is 130 slots of 16 bytes (layout::kBagWholeSlots): +0 the slot vtable (main + layout::kItemSlotVtable, a pointer into this
//   process's main, never copied), +8 the payload (pkp_items.h: packed item word, count; only items::kMoved of it is copied, as the
//   game's own copies do, so each slot keeps its bit 31 and padding). There are two: the offline bag (save common
//   + layout::kBagOffline) and the online one (+ layout::kBagOnline). The selectors 0x2ecc04 / 0x2e70e0 take the online bag on stage
//   0x11 (Cloud Island), or when IsOnline (0x207b0), not GM+0x11 bit 0 (layout::kStateOwnBag) and the visitor test (0x376c4); the
//   offline bag otherwise. (Their third case, the local second player, is a split-screen window of its own here: its own process.)
//   While an item is held, the bag screen works on a temp copy (layout::kItemTempFlag) and copies it back over the bag at the end;
//   while it is open at all, its panels show the array as they last read it. After a whole-bag write the game refreshes the HUD's
//   item bar (layout::kItemBarRefresh); so does every write here.
//   The host also keeps a working copy of each visitor's bag (save root + layout::kBagMirror, by member table index) that its request
//   checks read and change (station crafts, pickups). At a join (0x575644, join state 3 -> 4) the game fills it from its per-user
//   table and sets its valid flag, which later stores it back there.
// The record:
//   - per member key: a sequence number and the 130 payloads. A file per key, <save dir>/bags/<member key>.bin ([fs] save_dir is
//     the co-op folder, launcher coop_save.rs): "CBAG", u8 format, u64 sequence, 130 x 16 bytes (the first 8 of each 0), written
//     whole (temp file + rename) at most every kWriteEvery while it changed, and at session end. Every process keeps its own player's
//     record there; the authority also keeps every member's (a later host, the newest copy, has at least its own).
//   - when two copies meet, the higher sequence wins.
// Every process, the authority's own bag too, in a session:
//   - apply: the record is written into the bag the selector picks now, at session up, when the authority's copy is newer, and when
//     the selection may change (the visitor test, GM+0x11 bit 0, the stage, the player's character made again at a load's end). The
//     game's own loaded bag is never taken over a record. No record anywhere (a first co-op session): the bag as it is becomes the
//     first record, once the selection has held for kGrace. A member does neither before its visitor test is up (until then the
//     selection is its own island's bag, not the one it plays the session with). An apply waits while the bag / container screen
//     is open, so no panel shows a bag that is no longer there.
//   - take: once a frame the selected bag is compared with the record; a change becomes the record, sequence + 1, and a member sends
//     it to the authority (BagRecord, at most every kSendEvery).
//   - a member's hold: its game takes items out of the bag before it asks the host (a container store moves them while the screen is
//     open and asks at its close, kind 0x1a; a drop from the bag screen, kind 0x1d), and the host's request check takes them out of
//     its mirror, refusing when they are not there (0x21eb58). So a member takes nothing while the bag / container screen is open or
//     a Link Play request is unanswered or kept to be sent later (linkqueue::unanswered / kept; a dropped answer or a record that
//     never goes stops the hold after kAnswerWait): the record and
//     the mirror fed from it still hold the items when the host checks. A refused request writes the record back into the bag
//     (pkp_coop_bag.cpp), as the game's own reset did with the mirror. Without the hold the mirror lacks the items, the host
//     refuses every store and drop, and the items are in neither the bag nor the box.
//     Not every such path has a screen we know: a building kit's deposit (kind 0x1a, sent by 0x85b430 through 0x214974) takes the
//     items out of the bag and asks about 0.2 s later, and its screen is neither the bag nor the container one. The host's check
//     (0x221d00) is a balance: the request's bag plus the kit's new contents, less the mirror and the kit's old contents, must leave
//     nothing, so a mirror that already has the move (a deposit's loss, a take-back's gain) refuses it. So a member also takes a
//     change only once the bag has stayed unchanged for kSettleWait (each further change, a second deposit, starts the wait again;
//     after kSettleMax a stillness of kSettleQuiet is enough): a request sent in that time is unanswered by then, and the hold above
//     runs on until its answer.
//   - safeguard: within kGrace of an apply or a selection change (a load, an area or session change: where the game's own wipes
//     ran, and any not mapped would), a change that loses anything is not taken: the record is written back into the bag, and the
//     refusal logged (an emptied bag and more than kMaxKindsGone item kinds gone are named in the line). In play every change is
//     taken, a store-all too.
//     Losses are counted per item id over the whole bag, so a sort or a stack merge (slots emptied, nothing lost) is taken.
//   - write-backs and gains (the rule): a write-back (an apply, a refused request's restore, a refused wipe) writes the whole record
//     over the bag, so what the bag holds beyond the record is discarded. That is the point for a wipe, a stale array and a refused
//     request (its own moves, both ways, are undone exactly: a container session also moves items INTO the bag, from its temp list),
//     but a real gain made after the record's content was last taken (a hold, a pending apply, before the authority's answer) would
//     vanish with it. A bag diff cannot tell the two apart, so a real gain is credited at its source: a module that adds to this
//     player's bag brackets the add with gain_begin / gain_end, and the bag's gain between the two (per slot) goes into the record at
//     once (sequence + 1): into the same slot when the record still agreed with the bag there, else onto a record stack of the same
//     packed word that has room, else into the same slot if the record has it empty, else into the record's lowest empty slot below
//     one in use (a live slot; slots past the bag's size are never used); what fits nowhere is logged. A loss inside the bracket is
//     never credited, so a credit only adds what the game just added: nothing is duplicated, and the write-back carries the gain once.
//     Before the authority's answer a credit goes into the record without a new sequence and is put again on top of the authority's
//     copy if that one wins. Every write-back logs how many slots it changed, how many credited gains it carried and how many items
//     the bag held beyond the record (discarded), so tester logs show if a gain the game made on its own (a pickup's answer during a
//     hold that ends in a refusal) is ever lost.
//   - a member sends its record (sequence 0 when it has none) at session up, again every kHelloEvery until the authority answered;
//     the authority keeps the newer copy and answers the first record of each member's session (and any older than its copy) with a
//     BagRestore. Until that answer a member takes nothing, so a stale bag never outbids the authority's copy.
// The authority's mirror of each visitor:
//   - a record carries its player's user id, which names its row in the member table, so its mirror index. The game's fill at the
//     join is replaced: the mirror gets the record (when there is none yet, the mirror stays as the game cleared it until the first
//     arrives), the valid flag stays clear (nothing stores the mirror back), and the game's fill from its table does not run (it
//     also pushed what did not fit into the host's PC). Every newer record of that member is fed into its mirror again.
// The module's files: this one (the apply, the take, the safeguard, the messages); pkp_coop_bags_store.cpp (the records' files);
// pkp_coop_bags_credit.cpp (a source's gain); they share pkp_coop_bags_internal.h.
#include "core/pkp_coop_bags_internal.h"
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_items.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::bags {

namespace {

constexpr const char* kTag = "pkp";


constexpr double kSendEvery = 1.0;    // seconds between two BagRecords of a member
constexpr double kHelloEvery = 5.0;   // seconds between two first BagRecords while the authority has not answered
constexpr double kGrace = 3.0;        // seconds after an apply or a selection change in which no loss is taken
constexpr uint32_t kMaxKindsGone = 8;
constexpr double kRefuseLogEvery = 1.0;
constexpr double kNotReadyLog = 15.0; // seconds in a session with the player in the world but the bag never ready: logged once
constexpr double kScreenGap = 0.3;    // seconds without a bag / container screen frame: the screen is closed
constexpr double kAnswerWait = 5.0;   // seconds a member's take waits for an unanswered request (the answer comes in well under 1 s)
constexpr double kSettleWait = 2.0;   // seconds a member's bag must stay unchanged before a change is taken (a request follows its move in ~0.2 s)
constexpr double kSettleMax = 10.0;   // seconds after which a change is taken while the bag keeps changing, once it has been still kSettleQuiet
constexpr double kSettleQuiet = 0.5;  // seconds (longer than the move-to-request gap)

}  // namespace

State& st() {
    static State s;
    return s;
}

Key own_key() {
    Key k;
    std::memcpy(k.data(), member_key_bytes(), k.size());
    return k;
}

std::string hex(const Key& k) {
    std::string out;
    char b[3];
    for (uint8_t v : k) {
        std::snprintf(b, sizeof b, "%02x", v);
        out += b;
    }
    return out;
}

// Slots in use and items in them.
std::pair<uint32_t, uint32_t> totals(const Record& r) {
    uint32_t slots = 0, count = 0;
    for (uint64_t p : r.slot)
        if (items::id_of(p)) ++slots, count += items::count_of(p);
    return {slots, count};
}

// ---- the bag ----

// The bag the game's selector picks now (0 while there is none), with what it read.
Selection select(::Ctx* c) {
    Selection sel;
    sel.player = local_player() != 0;
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!guest::ok(r + layout::kSaveCommon, layout::kBagOffline + kBagBytes) || !guest::ok(gm, layout::kStateSession + 1)) return sel;
    sel.stage = guest::rd<uint8_t>(gm + layout::kStateStage);
    sel.own_bag = guest::rd<uint8_t>(gm + layout::kStateOwnBag) & 1;
    sel.visitor = guest::call(c, base() + layout::kOnline, 0) & 1;
    sel.online = sel.stage == layout::kStageCloudIsland ||
                 ((guest::call(c, base() + layout::kIsOnline, 0) & 1) && !sel.own_bag && sel.visitor);
    sel.bag = r + layout::kSaveCommon + (sel.online ? layout::kBagOnline : layout::kBagOffline);
    return sel;
}

// The bag may be read and written now: the player's character exists, no item is held in the bag screen (its temp copy would be
// copied back over ours), and every slot holds the slot vtable (the bag is made; *bad_slot the first that does not). Play mode's
// in_game is not asked: it is also false while a menu is open (no ProcessInput), and a craft or a box move there must be taken.
bool ready(const Selection& sel, size_t* bad_slot) {
    *bad_slot = kSlots;
    if (!sel.bag || !sel.player || guest::rd<uint8_t>(base() + layout::kItemTempFlag)) return false;
    for (size_t i = 0; i < kSlots; ++i)
        if (guest::rd<uint64_t>(sel.bag + i * items::kSlotSize) != base() + layout::kItemSlotVtable) return *bad_slot = i, false;
    return true;
}

// A record holds what a copy moves (items::kMoved) only, so a bag and its record compare equal right after a write.
void read_bag(uint64_t bag, Record* out) {
    for (size_t i = 0; i < kSlots; ++i)
        out->slot[i] = items::moved_of(guest::rd<uint64_t>(bag + i * items::kSlotSize + items::kSlotPayload));
}

bool screen_open() { return qpc::seconds() - st().screen_at < kScreenGap; }

namespace {

void write_bag(uint64_t bag, const Record& r) {
    for (size_t i = 0; i < kSlots; ++i) {
        const uint64_t at = bag + i * items::kSlotSize + items::kSlotPayload;
        guest::wr<uint64_t>(at, items::moved_into(guest::rd<uint64_t>(at), r.slot[i]));
    }
}

// The record into this player's bag, and the HUD's item bar told (slot 0: a co-op process plays one player).
void put(::Ctx* c, uint64_t bag, const Record& r) {
    write_bag(bag, r);
    guest::call(c, base() + layout::kItemBarRefresh, 0, 0, 0);
}

// A member's hold (the header): its take waits while the bag / container screen is open or a request is unanswered.
bool held(double now) {
    State& s = st();
    if (link::is_authority()) return false;
    if (screen_open()) return true;
    if (!linkqueue::unanswered() && !linkqueue::kept()) {   // a kept record's request (a kit deposit, for one) is still to come
        s.unanswered_at = -1.0;
        s.wait_logged = false;
        return false;
    }
    if (s.unanswered_at < 0) s.unanswered_at = now;
    if (now - s.unanswered_at < kAnswerWait) return true;
    if (!s.wait_logged) {
        s.wait_logged = true;
        NXLOG_W(kTag, "Co-op bags: a Link Play request has no answer (or is still kept) after %.0f s; the bag's changes are taken again", kAnswerWait);
    }
    return false;
}

// The authority: the mirror index of the member with this user id (its row in the member table), or -1.
int mirror_index(const UserId& user) {
    if (user == UserId{}) return -1;
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t own = guest::deref(guest::deref(base() + layout::kOwnMemberIndexRef));
    if (!guest::ok(r + layout::kBagMirror, layout::kMemberCount * kBagBytes) || !guest::ok(own + layout::kOwnMirrorIndex, 1)) return -1;
    const uint8_t own_idx = guest::rd<uint8_t>(own + layout::kOwnMirrorIndex);
    for (uint32_t i = 0; i < layout::kMemberCount; ++i) {
        UserId row;
        const uint64_t at = r + layout::kMemberTable + i * layout::kMemberSize + layout::kMemberUserId;
        if (i != own_idx && guest::read(at, row.data(), row.size()) && row == user) return static_cast<int>(i);
    }
    return -1;
}

// The authority: a member's record into its mirror (each slot's vtable and unmoved bits kept). The valid flag is left as it is
// (clear: the game's fill that sets it does not run in co-op).
void feed_mirror(int idx, const Record& r) {
    const uint64_t root = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    write_bag(root + layout::kBagMirror + static_cast<uint64_t>(idx) * kBagBytes, r);
}

// What a change from `was` to `now` loses: whether any item's total fell, and how many item kinds have nothing left.
struct Loss {
    bool any = false;
    uint32_t kinds_gone = 0;
};
Loss loss_of(const Record& was, const Record& now) {
    std::map<uint32_t, int64_t> have;   // item id -> count before minus count after
    for (uint64_t p : was.slot)
        if (const uint32_t id = items::id_of(p)) have[id] += items::count_of(p);
    std::map<uint32_t, int64_t> before = have;
    for (uint64_t p : now.slot)
        if (const uint32_t id = items::id_of(p)) have[id] -= items::count_of(p);
    Loss l;
    for (const auto& [id, left] : have) {
        if (left <= 0) continue;
        l.any = true;
        if (left == before[id]) ++l.kinds_gone;
    }
    return l;
}

// What a write-back of `r` over `bag` changes: slots, and items the bag held beyond the record (discarded).
struct Overwrite {
    uint32_t slots = 0, discarded = 0;
};
Overwrite overwrite_of(const Record& bag, const Record& r) {
    Overwrite o;
    std::map<uint64_t, int64_t> beyond;   // packed word -> bag count minus record count
    for (size_t i = 0; i < kSlots; ++i) {
        if (bag.slot[i] != r.slot[i]) ++o.slots;
        if (items::id_of(bag.slot[i])) beyond[word_of(bag.slot[i])] += items::count_of(bag.slot[i]);
        if (items::id_of(r.slot[i])) beyond[word_of(r.slot[i])] -= items::count_of(r.slot[i]);
    }
    for (const auto& [w, n] : beyond)
        if (n > 0) o.discarded += static_cast<uint32_t>(n);
    return o;
}

// ---- the messages ----

std::vector<uint8_t> slots_payload(wire::Writer& w, const Record& r) {
    const uint8_t zero[items::kSlotPayload] = {};
    for (uint64_t p : r.slot) w.bytes(zero, sizeof zero).u64(p);
    return w.take();
}

bool read_slots(wire::Reader& r, Record* out) {
    for (uint64_t& p : out->slot) {
        r.u64();   // the vtable's place: 0 on the wire
        p = items::moved_of(r.u64());
    }
    return r.ok();
}

void send_record() {
    State& s = st();
    wire::Writer w(link::self());
    UserId user{};
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    if (guest::ok(r + layout::kOwnUserId, user.size())) guest::read(r + layout::kOwnUserId, user.data(), user.size());
    w.bytes(member_key_bytes(), kMemberKeyBytes).u64(s.own.rec.seq).bytes(user.data(), user.size());
    if (link::to_authority(wire::Type::BagRecord, slots_payload(w, s.own.rec))) {
        s.send = false;
        s.sent_at = qpc::seconds();
    }
}

void send_restore(int node, const Key& k, const Record& r) {
    wire::Writer w(link::self());
    w.bytes(k.data(), k.size()).u8(r.seq ? 1 : 0).u64(r.seq);
    link::to_node(node, wire::Type::BagRestore, slots_payload(w, r));
}

// The authority: a member's record changed (or its user id became known): its mirror, if the member is in the member table, gets it.
// Latest wins: the host's own request checks change the mirror in between, and the member's next record carries the same change;
// the member's hold (the header) keeps a record that lacks what an unanswered request takes from reaching the mirror first.
void on_member_record(const Key& k, const Kept& kept, int node) {
    const int idx = kept.rec.seq ? mirror_index(kept.user) : -1;
    if (idx >= 0) feed_mirror(idx, kept.rec);
    NXLOG_D(kTag, "Co-op bags: node %d's record %llu stored (%s), mirror %d", node, static_cast<unsigned long long>(kept.rec.seq),
            hex(k).c_str(), idx);
}

// ---- the tick's steps ----

void session_up() {
    State& s = st();
    Kept& o = own();
    s.active = true;
    s.answered = link::is_authority();
    s.hello_at = -1e9;
    s.send = false;
    s.apply = o.rec.seq != 0;
    s.grace_until = qpc::seconds() + kGrace;
    s.looked = false;
    s.up_at = qpc::seconds();
    s.was_ready = false;
    s.restore = false;
    s.unanswered_at = -1.0;
    s.change_at = s.change_first = -1.0;
    s.gain_open = false;
    s.credited = 0;
    s.early.clear();
    NXLOG_I(kTag, "Co-op bags: in a session as %s; this player's record %llu", s.answered ? "the authority" : "a member",
            static_cast<unsigned long long>(o.rec.seq));
}

void session_down() {
    State& s = st();
    flush(true);
    s.active = false;
    s.greeted = 0;
    NXLOG_I(kTag, "Co-op bags: out of the session; the records are written");
}

void take_or_refuse(::Ctx* c, const Selection& sel, double now) {
    State& s = st();
    Kept& o = s.own;
    Record bag;
    read_bag(sel.bag, &bag);
    if (o.rec.seq == 0) {   // no record anywhere: the bag as it is becomes the first one, once the selection has settled
        if (now < s.grace_until) return;
        bag.seq = 1;
        o.rec = bag;
        o.dirty = true;
        s.send = !link::is_authority();
        const auto [slots, count] = totals(o.rec);
        NXLOG_I(kTag, "Co-op bags: no record of this player anywhere; its bag becomes the first one (%u slots, %u items)", slots, count);
        return;
    }
    if (bag.slot == o.rec.slot) {
        s.credited = 0;   // the record holds the bag: nothing is pending
        s.change_at = s.change_first = -1.0;
        return;
    }
    const Loss loss = loss_of(o.rec, bag);
    const auto [was_slots, was_count] = totals(o.rec);
    const auto [now_slots, now_count] = totals(bag);
    // Only inside the grace window: the game's whole-bag wipes (switched off in pkp_coop_bag.cpp; this catches one not mapped) run
    // at a load, an area or session change or a selection change, and the apply right before it puts the record back. In play, a
    // bulk loss is the player's own (a store-all into a box), and writing the record back then would copy the items.
    const char* why = nullptr;
    if (now >= s.grace_until) {
    } else if (was_slots && !now_slots) why = "it empties the bag right after a load, an area change or a session change";
    else if (loss.kinds_gone > kMaxKindsGone) why = "too many item kinds are gone right after a load, an area change or a session change";
    else if (loss.any) why = "it loses items right after a load, an area change or a session change";
    if (why) {
        const Overwrite ow = overwrite_of(bag, o.rec);
        put(c, sel.bag, o.rec);
        if (now - s.refused_log_at >= kRefuseLogEvery) {
            s.refused_log_at = now;
            NXLOG_W(kTag, "Co-op bags: a change of the bag is refused (%s): %u slots / %u items before, %u / %u after, %u kinds gone; "
                          "the record is written back: %u slots changed, %u credited gains kept, %u items beyond the record discarded",
                    why, was_slots, was_count, now_slots, now_count, loss.kinds_gone, ow.slots, s.credited, ow.discarded);
        }
        s.credited = 0;
        return;
    }
    // A member's change waits (the header's hold): the request that may follow it must find the host's mirror as the bag was before.
    if (!link::is_authority()) {
        if (s.change_first < 0) s.change_first = now;
        if (s.change_at < 0 || bag.slot != s.change_bag.slot) {   // every further change starts the wait again (deposits in a row)
            s.change_at = now;
            s.change_bag = bag;
        }
        const double quiet = now - s.change_at;
        if (quiet < kSettleWait && (now - s.change_first < kSettleMax || quiet < kSettleQuiet)) return;
    }
    s.change_at = s.change_first = -1.0;
    bag.seq = o.rec.seq + 1;
    o.rec = bag;
    o.dirty = true;
    s.send = !link::is_authority();
    s.credited = 0;
}

}  // namespace

void tick(::Ctx* c) {
    State& s = st();
    const bool in = c && link::in_session();
    if (in != s.active) {
        if (in) session_up();
        else session_down();
    }
    if (!in) return;
    const double now = qpc::seconds();
    const Selection sel = select(c);
    if (!s.looked || !(sel == s.seen)) {
        if (s.looked) {   // the first look of a session is session_up's
            s.apply = s.own.rec.seq != 0;
            s.grace_until = now + kGrace;
        }
        s.looked = true;
        s.seen = sel;
    }
    if (!s.answered && now - s.hello_at >= kHelloEvery) {
        s.hello_at = now;
        send_record();
    }
    // A member's bag is the online one once the host's world has loaded (the visitor test rises 10 to 20 s after the join,
    // pkp_coop_session.cpp); before that its selection is its own island's, which is neither applied to nor taken from.
    const bool settled = link::is_authority() || sel.visitor;
    size_t bad_slot = kSlots;
    const bool can = settled && ready(sel, &bad_slot);
    if (can) s.was_ready = true;
    else if (!s.was_ready && bad_slot < kSlots && now - s.up_at > kNotReadyLog) {
        s.was_ready = true;
        NXLOG_W(kTag, "Co-op bags: the %s bag is not usable (slot %zu holds no slot vtable); its record is neither applied nor taken",
                sel.online ? "online" : "offline", bad_slot);
    }
    // An apply (or a refused request's write-back) waits while the bag / container screen is open, and the take with it. It writes
    // the whole record: a change made meanwhile is the write-back's to overwrite (the record it brings is the newer one, or the bag
    // from before the refused request), except a source's gain, which is in the record already (the header's rule).
    // No grace after a write-back: it is play, and a loss right after it is the player's own.
    if (can && (s.apply || s.restore) && !screen_open()) {
        Record was;
        read_bag(sel.bag, &was);
        const Overwrite ow = overwrite_of(was, s.own.rec);
        put(c, sel.bag, s.own.rec);
        const auto [slots, count] = totals(s.own.rec);
        if (s.apply) {
            s.grace_until = now + kGrace;
            NXLOG_I(kTag, "Co-op bags: record %llu written into the %s bag (%u slots, %u items): %u slots changed, %u credited gains kept, "
                          "%u items beyond the record discarded", static_cast<unsigned long long>(s.own.rec.seq),
                    sel.online ? "online" : "offline", slots, count, ow.slots, s.credited, ow.discarded);
        } else {
            NXLOG_I(kTag, "Co-op bags: the host refused a request; record %llu (the bag before it) is written back (%u slots, %u items): "
                          "%u slots changed, %u credited gains kept, %u items beyond the record discarded",
                    static_cast<unsigned long long>(s.own.rec.seq), slots, count, ow.slots, s.credited, ow.discarded);
        }
        s.credited = 0;
        s.apply = s.restore = false;
    } else if (can && !s.apply && !s.restore && s.answered && !held(now)) {
        take_or_refuse(c, sel, now);
    }
    if (s.send && s.answered && !link::is_authority() && now - s.sent_at >= kSendEvery) send_record();
    flush(false);
}

void on_member_joined(int node) {
    if (node >= 0 && node < 32) st().greeted &= ~(1u << node);
}

void on_request_refused() {
    if (st().active && !link::is_authority()) st().restore = true;
}

int on_mirror_fill(::Ctx* c) {
    if (role() != CoopRole::Host) return 0;
    const int idx = static_cast<int>(c->x[0] & 0xff);
    for (const auto& [k, kept] : st().members) {
        if (!kept.rec.seq || mirror_index(kept.user) != idx) continue;
        feed_mirror(idx, kept.rec);
        const auto [slots, count] = totals(kept.rec);
        NXLOG_I(kTag, "Co-op bags: member %d joins; the host's copy of its bag is its record %llu (%u slots, %u items)", idx,
                static_cast<unsigned long long>(kept.rec.seq), slots, count);
        return 1;
    }
    NXLOG_I(kTag, "Co-op bags: member %d joins before its record is here; the host's copy of its bag waits for it", idx);
    return 1;   // the game's fill from its per-user table does not run in co-op
}

int on_bag_screen(::Ctx* c) {
    const uint64_t screen = c->x[0];
    if (guest::ok(screen, 8) && guest::rd<uint64_t>(screen) == base() + layout::kBagScreenVtable) st().screen_at = qpc::seconds();
    return 0;
}

void on_record_message(::Ctx*, int from, uint8_t, wire::Reader& r) {
    Key k;
    r.bytes(k.data(), k.size());
    Record got;
    got.seq = r.u64();
    UserId user;
    r.bytes(user.data(), user.size());
    if (!read_slots(r, &got) || !link::is_authority() || from < 0 || from >= 32) return;
    if (k == own_key()) {
        NXLOG_W(kTag, "Co-op bags: node %d sent a record under this player's own key; it is not kept", from);
        return;
    }
    State& s = st();
    Kept& m = member(k);
    const bool first = !(s.greeted & (1u << from));
    const bool newer = got.seq > m.rec.seq, new_user = user != m.user;
    m.user = user;
    for (auto& [other, kept] : s.members)
        if (kept.node == from) kept.node = -1;   // a node belongs to one member at a time
    m.node = from;
    if (newer) {
        m.rec = got;
        m.dirty = true;
    }
    if (newer || new_user) on_member_record(k, m, from);
    if (first || got.seq < m.rec.seq) {
        s.greeted |= 1u << from;
        send_restore(from, k, m.rec);
        if (first) {
            const auto [slots, count] = totals(m.rec);
            NXLOG_I(kTag, "Co-op bags: node %d joined with record %llu; it gets record %llu (%u slots, %u items)", from,
                    static_cast<unsigned long long>(got.seq), static_cast<unsigned long long>(m.rec.seq), slots, count);
        }
    }
}

void on_restore_message(::Ctx* c, int, uint8_t, wire::Reader& r) {
    Key k;
    r.bytes(k.data(), k.size());
    const uint8_t have = r.u8();
    Record got;
    got.seq = r.u64();
    if (!read_slots(r, &got) || link::is_authority() || k != own_key()) return;
    State& s = st();
    Kept& o = own();
    s.answered = true;
    const bool won = have && got.seq > o.rec.seq;   // the authority's copy replaces ours (an apply may also be pending for another reason)
    if (won) {
        o.rec = got;
        o.dirty = true;
        s.apply = true;
        NXLOG_I(kTag, "Co-op bags: the authority's copy of this player's bag is newer (record %llu); it is written into the bag",
                static_cast<unsigned long long>(got.seq));
    } else if (o.rec.seq > got.seq) {
        s.send = true;   // the authority's copy is older: ours goes to it
    }
    if (s.early.empty()) return;
    // Gains credited before this answer: in our copy already; put again on top of the authority's when that one won.
    uint32_t lost = 0;
    if (won) {
        const uint32_t max = c ? max_stack(c) : 0xff;
        for (const Credit& g : s.early)
            if (!place(o.rec, g, max)) lost += g.n;
    }
    ++o.rec.seq;
    o.dirty = true;
    s.send = true;
    NXLOG_I(kTag, "Co-op bags: %zu gains made before the authority's answer are kept in record %llu (%u items fit nowhere)",
            s.early.size(), static_cast<unsigned long long>(o.rec.seq), lost);
    s.early.clear();
}

size_t records(View* out, size_t most) {
    State& s = st();
    size_t n = 0;
    if (!link::in_session() || !link::is_authority()) return 0;
    if (s.own.rec.seq && n < most) out[n++] = View{link::self(), s.own.rec.seq, s.own.rec.slot.data()};
    for (const auto& [k, kept] : s.members)
        if (kept.node >= 0 && kept.rec.seq && n < most) out[n++] = View{kept.node, kept.rec.seq, kept.rec.slot.data()};
    return n;
}

void reset() {
    if (st().loaded) flush(true);
    st() = State{};
}

}  // namespace nxrt::games::pkp::coop::bags
