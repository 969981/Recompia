// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, a member's deposit into the story's materials drop, stepped once for the group on the authority
// (pkp_coop_internal.h; RE: modding-docs coop-v2/role-census/build/materials-deposit.md, use-start.md, owner.md).
//
// The game, 2.0.1a. The drop is class 0x326dc98 (placed kind 0x1d9, container type 0x11: one container record, index 677, local id 0,
// slots 1295..1297 of the common store), ten stages in gimmick +0x4a (table #1108, slot 0x144 of the data-table manager):
//   - the use 0x7e77a4(g, player selector 0): the player gates (0x2fdd00 / 0x274248), then online: free (0x768f48) -> the host takes
//     the owner byte and goes on, a member sends RequestGimmickOwnership and returns; not free -> a one-slot owner (+0x20) equal to this
//     process's index goes on. On into 0x7e78c4: the press posts to the contents (0x7659c8; false: pkp_coop_owner.cpp gives the object
//     back at 0x7e78f4), the row needs a category in 1..0x1f, then +0x50 = 0.5f (the open timer), 0x7e7f20(g, 1), 0x74fb80(0xf, 1, ..).
//   - its slot 22 0x7e633c: the timer runs out -> +0x50 = 0, 0x7e6680(g, 1): the container record of type +0x4b at the object's cell
//     (save root + 0xa0ee28; on a Link Play visitor the join snapshot 0x571460 wrote the host's records there) -> the screen
//     0xa94b38(stage, type, local id) -> +0x4c = opened. No record: +0x4c stays 0 and nothing more runs (the block stays open).
//     With +0x4c set and the screen shut -> the close 0x7e67c0: +0x4c = 0, 0x7e7f20(g, 0), 0x74fb80(0xf, 0, ..), the record lookup
//     again (w20 = local id), the stage's row (x21), then at 0x7e6938 0xa94bbc (the screen's confirm flag, manager +0x5a) and at
//     0x7e693c `tbz w0` to the return; then 0x78d050 (the check), the scenes of row +0 and +4 (0x2e6ce0, the local player's queue),
//     +0x4a + 1 and R+0x73b264 (only while that byte is not 0 and the record's cell is the object's), every valid slot of the slice
//     cleared (0x5b2f34, own store), the next row's +6 scene.
//   - the check 0x78d050(stage, type, id): each category +8..+0xa in 1..0x1f needs 0x78d320 >= its count +0xc..+0xe; the last of
//     them 0x14 also needs flag 0x91 (0x377cc). 0x78d320: the #514 rows of that category (u16 item, u8 category; 101 rows) summed with
//     0x3a54a0 over the slice; none: category 0x13 -> 0x5b8e34(R+0x1718bb0) bit 0, 1 -> u32 R+0x73b260 (electricity), else 0.
//   - the slice: 0x5b1c84(holder, type, id, temp, &first, &count) is the one resolver every reader and writer of a container's slots
//     goes through (the check 0x3a54a0, the screens' lists 0xe6dc2c -> 0x5bc618, the moves 0x2ee774, the staging 0xe70bc0 / 0xe70dc0 /
//     0xe709c0). A common-store type with temp set (callers pass IsVisitor) resolves to the visitor's temp list R+0x170f658, else the
//     own common store R+0x13d5970; the close's clear 0x5b2f34 always takes the own store. So a visitor's screen and check would work
//     on the temp list, which nothing of this class fills or saves, and its close clears another store.
//
// In a session (a member, never the authority; single player and plain Link Play are untouched):
//   - the store: at 0x5b1c84's entry a type 0x11 slice asked with temp set resolves to the own store (x3 = 0), so the screen, its
//     check and our clear all work on the member's own common store (its save, CMNDAT; nothing from the host is written there). The
//     member sees and pays only its own items.
//   - the use: pkp_coop_owner.cpp hands this class's grant here (take_grant) instead of giving it back. At the next frame the use
//     0x7e77a4(g, 0) runs for the owner; no timer after it (a gate, a content took the press) -> given back. The open that finds no
//     record (+0x50 back at 0 with +0x4c clear) -> the close's own first steps (0x7e7f20(g, 0), 0x74fb80(0xf, 0, ..)), given back.
//     The open's timer is cleared too, so no screen opens later for an object no longer held. Once the screen has opened and shut,
//     given back.
//   - the close: midhook at 0x7e693c, w0 = the confirm flag (x19 the gimmick, w20 the local id). A confirm on a member does not go on
//     (x0 = 0: no check, scenes, step, record write or clear here). The slice's valid slots are read (own store) and the authority is
//     asked (DepositAsk: cell, the stage this drop shows, the items), resent every kResend with the same id until an answer comes.
//     While an ask waits, this member's next grant of the drop is given back (the items it named stay put).
//   - the answer (DepositState for this member's key and ask id): accepted -> the named slots lose exactly the named items (a slot
//     that no longer holds them is left alone and logged), the drop shows the answer's stage, and the stage's scenes (row +0, +4,
//     the next row's +6) are queued for this player at a safe moment (can walk, no talk), as the close queues them for the confirming
//     player: one process plays them, so a give in them is shared once (pkp_coop_catchup.cpp); each is queued again every
//     kSceneEvery until its played bit is set (the queue drops a scene it cannot take), and the played bits reach the others through
//     pkp_coop_events.cpp. Refused -> the items stay in the
//     drop for the member to take back, the drop shows the authority's stage, and a line says why. No answer within kGiveUp in the
//     session -> the same as a refusal (the items stay; logged).
//   - the save: the own store is CMNDAT, which a visitor writes only at its autosave, while the bag record is on disk within
//     seconds. So after every screen close (items may have gone from the bag into the store) and after an accept's clear, the game
//     save is asked for as the take-out asks it (takeout::owe_save): a quit in between can neither lose the items nor bring paid
//     ones back for longer than the save takes.
//   - the stage: the authority's DepositState with no verdict (every kStateEvery and when its drop's stage changed) sets this
//     member's drop at that cell to the authority's stage, while no use or ask of this member's is on it. The save's record follows
//     through pkp_coop_records.cpp (record 6), so quest condition 9 reads the group's stage on every process.
// On the authority, once a frame while its player can walk (in its world, no load): each ask whose (member key, ask id) was answered before gets the same answer again (a lost
// answer is repeated, never run again). Otherwise its drop at the cell (kind 0x1d9, class 0x326dc98): none -> refused; its own
// screen opening or up -> the ask waits; its stage not the ask's -> refused (so a resend or a second member's ask for a stage already
// passed never steps twice); the stage's row as 0x78d050 reads it, counted from the asked items as 0x78d320 counts them, with the
// electricity, the R+0x1718bb0 test and flag 0x91 read on the authority (the group's) -> refused when not met. Accepted: +0x4a + 1
// and R+0x73b264 as the close writes them; no scene and no clear here (the items are the member's, in the member's store).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_strings.h"
#include "core/pkp_tables.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::deposit {

namespace {

constexpr const char* kTag = "pkp";
constexpr int kGimmickReg = 19, kRecordReg = 20, kAnswerReg = 0;   // at 0x7e693c
constexpr int kTypeReg = 1, kTempReg = 3;                          // at 0x5b1c84's entry
constexpr uint64_t kKindOf = 0x50, kOwnerOf = 0x2c8;   // gimmick vtable +: its kind (u16), the owner of a slot (g, slot)
constexpr uint64_t kCellLo = 0x18, kCellHi = 0x1c;  // gimmick +: u32 x | y << 16, u16 z
constexpr uint64_t kCommonStore = 0x1050;           // holder +: the common store (R+0x13d5970)
constexpr uint64_t kSlotItem = 8, kSlotCount = 0xc, kSlotBytes = 16;
constexpr uint32_t kItemMask = 0x1fff, kItemMax = 0x11f6;   // a valid slot: item 1..0x11f6 (the close's test)
constexpr uint32_t kSlotKeep = 0x80000000u;          // 0x5b2f34 keeps only bit 31 of the item word
constexpr uint32_t kSceneMax = 0xbb7;
constexpr uint32_t kRowCategory = 8, kRowCount = 0xc, kRowScene = 0, kRowExtra = 4, kRowNext = 6;
constexpr uint32_t kItemRows = 101, kCatElectric = 1, kCatTest = 0x13, kCatFlagged = 0x14, kFlag = 0x91;
constexpr size_t kMaxItems = 3;
constexpr double kResend = 2.0;      // a member's ask, again while no answer came
constexpr double kGiveUp = 120.0;    // a member's ask with no answer in the session for this long: its items stay, the ask is dropped
constexpr double kOpenWait = 3.0;    // the use's open timer is 0.5 s: no screen after this long is a failed open
constexpr double kStateEvery = 3.0;  // the authority's stage message
constexpr double kLookEvery = 0.5;   // the authority looks at its drop's stage this often
constexpr double kAskDrop = 60.0;    // the authority: an ask that found no moment for this long is dropped (the member resends)
constexpr double kSceneEvery = 3.0;  // a paid scene not played yet is queued again this often (the queue drops one it cannot take)
constexpr double kSceneGiveUp = 300.0;
constexpr size_t kRemembered = 64, kIncoming = 16;

enum class Verdict : uint8_t { None = 0, Accepted = 1, Refused = 2 };
enum class Why : uint8_t { None = 0, Stage = 1, Needs = 2, NoDrop = 3 };

struct Item {
    uint16_t slot = 0, item = 0;
    uint8_t count = 0;
};

struct Ask {
    uint8_t key[kMemberKeyBytes] = {};
    uint64_t id = 0;
    uint32_t lo = 0;
    uint16_t hi = 0;
    uint8_t stage = 0;
    uint8_t n = 0;
    Item items[kMaxItems];
    int node = -1;        // the authority: who sent it
    double at = 0, next = 0;
};

struct Answer {
    uint8_t key[kMemberKeyBytes] = {};
    uint64_t id = 0;
    uint32_t lo = 0;
    uint16_t hi = 0;
    uint8_t stage = 0;
    Verdict verdict = Verdict::None;
    Why why = Why::None;
};

enum class Use : uint8_t { None, Granted, Opening, Open };

struct State {
    // A member: the use this module runs, the ask waiting for its answer, the scenes to play.
    Use use = Use::None;
    uint32_t lo = 0;
    uint16_t hi = 0;
    uint64_t g = 0;
    double use_at = 0;
    bool asking = false;
    Ask ask;
    double ask_in_session = 0;   // seconds the ask has waited in a session
    double last_tick = 0;
    uint32_t nonce = 0, counter = 0;
    struct Scene {
        uint16_t id;
        double at, next;
    };
    std::vector<Scene> scenes;
    bool have_state = false;     // the authority's latest stage message
    uint32_t state_lo = 0;
    uint16_t state_hi = 0;
    uint8_t state_stage = 0;
    bool busy_told = false;
    // The authority.
    std::deque<Ask> incoming;
    std::deque<Answer> answered;
    double state_at = -1e9, looked_at = -1e9;
    int sent_stage = -1;
    uint32_t known_lo = 0;
    uint16_t known_hi = 0;
    bool known = false;
    // Guest-visible scratch for the game's out arguments (guest addresses are host addresses).
    alignas(8) uint32_t first = 0, count = 0;
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() { return guest::rd<uint64_t>(base() + layout::kSaveRoot); }

bool member() { return link::in_session() && !link::is_authority(); }

uint32_t pack(uint32_t lo, uint16_t hi) { return (lo & 0xfff) | ((lo >> 16) & 0xff) << 12 | static_cast<uint32_t>(hi & 0xfff) << 20; }

// The drop at a cell (the placed-object lookup the grant reply uses, 0x753200), checked by class and kind; 0 when there is none.
uint64_t drop_at(::Ctx* c, uint32_t lo, uint16_t hi) {
    const uint64_t map = guest::rd<uint64_t>(base() + layout::kPlacedObjects);
    if (!guest::ok(map, 8)) return 0;
    const uint64_t g = guest::call(c, base() + layout::kPlacedObjectAt, map, lo & 0xfff, (lo >> 16) & 0xff, hi & 0xfff);
    if (!guest::ok(g, layout::kDropTimer + 4) || guest::deref(g) != base() + layout::kDropClass) return 0;
    const uint64_t kind_of = guest::deref(guest::deref(g) + kKindOf);
    return kind_of && static_cast<uint16_t>(guest::call(c, kind_of, g)) == layout::kDropKind ? g : 0;
}

uint8_t own_index(::Ctx* c) { return static_cast<uint8_t>(guest::call(c, base() + layout::kOwnIndexOf, 0)); }

bool owned_here(::Ctx* c, uint64_t g) {
    const uint64_t owner_of = guest::deref(guest::deref(g) + kOwnerOf);
    return owner_of && static_cast<uint8_t>(guest::call(c, owner_of, g, 0)) == own_index(c);
}

void give_back(::Ctx* c, uint32_t lo, uint16_t hi) { owner::give_back_cell(c, layout::kDropKind, pack(lo, hi)); }

// The own common store's base and the type-0x11 slice of record `id` (0x5b1c84 with temp 0); 0 when the save is not there.
uint64_t slice(::Ctx* c, uint32_t id, uint32_t* first, uint32_t* count) {
    const uint64_t root = save_root();
    if (!guest::ok(root + layout::kStoreHolder, kCommonStore + 8)) return 0;
    State& s = st();
    s.first = s.count = 0;
    const uint64_t store = guest::call_n<6>(c, base() + layout::kStoreSlice,
                                            {root + layout::kStoreHolder, layout::kDropType, id, 0, reinterpret_cast<uint64_t>(&s.first),
                                             reinterpret_cast<uint64_t>(&s.count)});
    *first = s.first;
    *count = s.count;
    return guest::ok(store + (static_cast<uint64_t>(s.first) + s.count) * kSlotBytes, 1) ? store : 0;
}

// A row of #1108 (the stage's) as the close reads it (holder +8; 0 when the table or the row is not there).
uint64_t stage_row(::Ctx* c, uint8_t stage) {
    const uint64_t holder = tables::row(c, base(), layout::kDropStagesTable, stage);
    const uint64_t data = holder && guest::ok(holder + 8, 8) ? guest::rd<uint64_t>(holder + 8) : 0;
    return guest::ok(data, 0x10) ? data : 0;
}

// ---- the authority's check: 0x78d050 / 0x78d320 on the asked items ----

uint32_t category_count(::Ctx* c, uint8_t cat, const Ask& a) {
    bool listed = false;
    uint32_t have = 0;
    for (uint32_t i = 0; i < kItemRows; ++i) {
        const uint64_t holder = tables::row(c, base(), layout::kDropItemsTable, i);
        const uint64_t row = holder && guest::ok(holder + 8, 8) ? guest::rd<uint64_t>(holder + 8) : 0;
        if (!guest::ok(row, 4) || guest::rd<uint8_t>(row + 2) != cat) continue;
        const uint16_t item = guest::rd<uint16_t>(row);
        if (item < 1 || item > kItemMax) continue;
        listed = true;
        for (uint8_t k = 0; k < a.n; ++k)
            if (a.items[k].item == item) have += a.items[k].count;
    }
    if (listed) return have;
    const uint64_t root = save_root();
    if (cat == kCatTest) return guest::call(c, base() + layout::kDropTest, root + layout::kDropTestArg) & 1;
    if (cat == kCatElectric) return guest::rd<uint32_t>(root + layout::kSaveDrop + 8);
    return 0;
}

bool needs_met(::Ctx* c, const Ask& a) {
    if (a.stage < 1 || a.stage > 0x1f) return false;
    const uint64_t row = stage_row(c, a.stage);
    if (!row) return false;
    bool any = false, flagged = false;
    for (uint32_t k = 0; k < 3; ++k) {
        const uint8_t cat = guest::rd<uint8_t>(row + kRowCategory + k);
        if (cat < 1 || cat > 0x1f) continue;
        any = true;
        if (category_count(c, cat, a) < guest::rd<uint8_t>(row + kRowCount + k)) return false;
        flagged = cat == kCatFlagged;
    }
    if (!any) return false;   // a stage with no category is the last one: the use never opens it (0x7e7994)
    return !flagged || (guest::call(c, base() + layout::kFlagTest, kFlag) & 1);
}

void send_answer(const Answer& v, int node) {
    wire::Writer w(link::self());
    w.u32(v.lo).u16(v.hi).u8(v.stage).u8(static_cast<uint8_t>(v.verdict)).u8(static_cast<uint8_t>(v.why)).bytes(v.key, kMemberKeyBytes).u64(v.id);
    if (node < 0) link::to_members(wire::Type::DepositState, w.take());
    else link::to_node(node, wire::Type::DepositState, w.take());
}

// The authority: one ask, now. False: not now (its own screen on the drop), it waits.
bool decide(::Ctx* c, const Ask& a) {
    State& s = st();
    for (const Answer& v : s.answered)
        if (v.id == a.id && !std::memcmp(v.key, a.key, kMemberKeyBytes)) {
            send_answer(v, a.node);   // a lost answer: the same answer again, nothing runs again
            return true;
        }
    Answer v;
    std::memcpy(v.key, a.key, kMemberKeyBytes);
    v.id = a.id;
    v.lo = a.lo;
    v.hi = a.hi;
    const uint64_t g = drop_at(c, a.lo, a.hi);
    if (g && (guest::rd<uint8_t>(g + layout::kDropScreen) || guest::rd<float>(g + layout::kDropTimer) > 0.0f)) return false;
    s.known = true;
    s.known_lo = a.lo;
    s.known_hi = a.hi;
    v.stage = g ? guest::rd<uint8_t>(g + layout::kDropStage) : 0;
    if (!g) v.why = Why::NoDrop;
    else if (v.stage != a.stage) v.why = Why::Stage;
    else if (!needs_met(c, a)) v.why = Why::Needs;
    if (v.why == Why::None) {
        // As the close: +0x4a + 1, and the record's stage while that byte is set and the record's cell is the drop's (0x7e6a44..0x7e6a88).
        v.stage = static_cast<uint8_t>(a.stage + 1);
        guest::wr<uint8_t>(g + layout::kDropStage, v.stage);
        const uint64_t rec = save_root() + layout::kSaveDrop;
        if (guest::rd<uint8_t>(rec + 0xc) && guest::rd<uint32_t>(rec) == a.lo && guest::rd<uint16_t>(rec + 4) == a.hi)
            guest::wr<uint16_t>(rec + 0xc, v.stage);
        v.verdict = Verdict::Accepted;
    } else {
        v.verdict = Verdict::Refused;
    }
    NXLOG_I(kTag, "Co-op deposit: node %d's deposit (ask %llx) for stage %u at (%u,%u,%u): %s (the drop here is at stage %u)", a.node,
            static_cast<unsigned long long>(a.id), a.stage, a.lo & 0xfff, (a.lo >> 16) & 0xff, a.hi & 0xfff,
            v.verdict == Verdict::Accepted ? "accepted, the stage moved on"
            : v.why == Why::Stage           ? "refused, the stage differs"
            : v.why == Why::Needs           ? "refused, the stage's needs are not met"
                                            : "refused, no drop at that cell here",
            v.stage);
    if (s.answered.size() >= kRemembered) s.answered.pop_front();
    s.answered.push_back(v);
    send_answer(v, a.node);
    return true;
}

void authority_tick(::Ctx* c, bool in_game) {
    State& s = st();
    const double now = qpc::seconds();
    if (!session::can_walk(in_game)) return;   // in its world, no load: the asks and the stage message wait
    while (!s.incoming.empty()) {
        Ask& a = s.incoming.front();
        if (!decide(c, a)) {
            if (now - a.at < kAskDrop) break;   // its own screen is on the drop: the next frame
            NXLOG_W(kTag, "Co-op deposit: node %d's ask %llx found no moment; dropped (the member asks again)", a.node,
                    static_cast<unsigned long long>(a.id));
        }
        s.incoming.pop_front();
    }
    // The drop's stage to every member: from the save record's cell, else the cell of the last ask.
    const uint64_t rec = save_root() + layout::kSaveDrop;
    uint32_t lo = s.known_lo;
    uint16_t hi = s.known_hi;
    if (guest::ok(rec, layout::kDropRecordBytes) && (guest::rd<uint32_t>(rec) || guest::rd<uint16_t>(rec + 4))) {
        lo = guest::rd<uint32_t>(rec);
        hi = guest::rd<uint16_t>(rec + 4);
    } else if (!s.known) {
        return;
    }
    if (!link::member_count() || now - s.looked_at < kLookEvery) return;
    s.looked_at = now;
    const uint64_t g = drop_at(c, lo, hi);
    if (!g) return;
    const uint8_t stage = guest::rd<uint8_t>(g + layout::kDropStage);
    if (stage == s.sent_stage && now - s.state_at < kStateEvery) return;
    s.sent_stage = stage;
    s.state_at = now;
    Answer v;
    v.lo = lo;
    v.hi = hi;
    v.stage = stage;
    send_answer(v, -1);
}

// ---- a member ----

// The close's first steps (0x7e67c0 up to 0x7e6824), for an open that found nothing; the timer too, so no open comes later.
void shut(::Ctx* c, uint64_t g) {
    guest::wr<float>(g + layout::kDropTimer, 0.0f);
    guest::wr<uint8_t>(g + layout::kDropScreen, 0);
    guest::call(c, base() + layout::kDropBlock, g, 0);
    guest::call(c, base() + layout::kPlayerEvent, 0xf, 0, guest::rd<uint64_t>(base() + layout::kDropEventPtr), 0, 0);
}

void end_use(::Ctx* c, const char* why) {
    State& s = st();
    const uint64_t g = drop_at(c, s.lo, s.hi);
    if (g && owned_here(c, g)) give_back(c, s.lo, s.hi);
    if (why) NXLOG_I(kTag, "Co-op deposit: this member's use of the drop at (%u,%u,%u) ended (%s); it is given back", s.lo & 0xfff,
                     (s.lo >> 16) & 0xff, s.hi & 0xfff, why);
    s.use = Use::None;
    s.g = 0;
}

void use_tick(::Ctx* c) {
    State& s = st();
    if (s.use == Use::None) return;
    const uint64_t g = drop_at(c, s.lo, s.hi);
    if (!g || g != s.g) {   // the object went (a stage change)
        s.use = Use::None;
        s.g = 0;
        if (member()) give_back(c, s.lo, s.hi);
        return;
    }
    const double now = qpc::seconds();
    switch (s.use) {
        case Use::Granted:
            if (!member() || !owned_here(c, g)) {
                s.use = Use::None;
                s.g = 0;
                return;
            }
            guest::call(c, base() + layout::kDropUse, g, 0);
            if (guest::rd<float>(g + layout::kDropTimer) > 0.0f || guest::rd<uint8_t>(g + layout::kDropScreen)) {
                s.use = Use::Opening;
                s.use_at = now;
                NXLOG_I(kTag, "Co-op deposit: this member's use of the drop at (%u,%u,%u) started (stage %u)", s.lo & 0xfff,
                        (s.lo >> 16) & 0xff, s.hi & 0xfff, guest::rd<uint8_t>(g + layout::kDropStage));
            } else {
                end_use(c, "the use did not start");
            }
            return;
        case Use::Opening:
            if (guest::rd<uint8_t>(g + layout::kDropScreen)) {
                s.use = Use::Open;
            } else if (guest::rd<float>(g + layout::kDropTimer) <= 0.0f || now - s.use_at > kOpenWait) {
                shut(c, g);
                end_use(c, "the open found no container record at the cell");
            }
            return;
        case Use::Open:
            if (!guest::rd<uint8_t>(g + layout::kDropScreen)) {   // the close ran: the screen may have moved items into the own store
                takeout::owe_save();
                end_use(c, nullptr);
            }
            return;
        case Use::None: return;
    }
}

void set_stage(::Ctx* c, uint32_t lo, uint16_t hi, uint8_t stage) {
    const uint64_t g = drop_at(c, lo, hi);
    if (g && guest::rd<uint8_t>(g + layout::kDropStage) != stage) guest::wr<uint8_t>(g + layout::kDropStage, stage);
}

void queue_scenes(::Ctx* c, uint8_t stage) {
    State& s = st();
    const uint64_t row = stage_row(c, stage), next = stage_row(c, static_cast<uint8_t>(stage + 1));
    const uint16_t list[] = {row ? guest::rd<uint16_t>(row + kRowScene) : uint16_t{0}, row ? guest::rd<uint16_t>(row + kRowExtra) : uint16_t{0},
                             next ? guest::rd<uint16_t>(next + kRowNext) : uint16_t{0}};
    const double now = qpc::seconds();
    for (uint16_t scene : list)
        if (scene >= 1 && scene <= kSceneMax) s.scenes.push_back(State::Scene{scene, now, now});
}

bool scene_played(uint16_t scene) {
    const uint64_t at = save_root() + layout::kPlayedScenes + (scene >> 3);
    return !guest::ok(at, 1) || (guest::rd<uint8_t>(at) & (1u << (scene & 7)));
}

// The close's scene call (0x7e69a0..0x7e69cc), selector 0, with its stack arguments on the scratch stack (free at the frame tick).
void play_scene(::Ctx* c, uint16_t scene) {
    const uint64_t sp = (c->x[31] - 0x200) & ~uint64_t{0xf};
    guest::wr<uint64_t>(sp, 0);
    guest::wr<uint64_t>(sp + 8, 0);
    guest::wr<uint64_t>(sp + 0x10, 0);
    guest::wr<uint16_t>(sp + 0x40, guest::rd<uint16_t>(guest::rd<uint64_t>(base() + layout::kSceneActorPtr)));
    guest::call_n<8>(c, base() + layout::kSceneQueue, {0, scene, sp + 0x40, 0xffffffffull, 0, 0, 0, 0}, sp);
}

// The answer to this member's ask: the items paid (accepted) or kept (refused).
void apply_answer(::Ctx* c, const Answer& v) {
    State& s = st();
    const Ask& a = s.ask;
    s.have_state = false;   // a stage message from before this answer is older than it
    if (v.verdict == Verdict::Accepted) {
        uint32_t first = 0, count = 0;
        const uint64_t store = slice(c, 0, &first, &count);
        for (uint8_t k = 0; k < a.n; ++k) {
            const Item& it = a.items[k];
            const uint64_t slot = store ? store + static_cast<uint64_t>(it.slot) * kSlotBytes : 0;
            const uint32_t word = slot ? guest::rd<uint32_t>(slot + kSlotItem) : 0;
            const uint8_t held = slot ? guest::rd<uint8_t>(slot + kSlotCount) : 0;
            if (!slot || (word & kItemMask) != it.item || held < it.count) {
                NXLOG_W(kTag, "Co-op deposit: slot %u no longer holds item %u x%u (item %u x%u); left as it is", it.slot, it.item,
                        it.count, word & kItemMask, held);
                continue;
            }
            if (held == it.count) {   // as 0x5b2f34 empties a slot
                guest::wr<uint8_t>(slot + kSlotCount, 0);
                guest::wr<uint32_t>(slot + kSlotItem, word & kSlotKeep);
            } else {
                guest::wr<uint8_t>(slot + kSlotCount, static_cast<uint8_t>(held - it.count));
            }
        }
        takeout::owe_save();   // the paid items leave the saved store too
        set_stage(c, a.lo, a.hi, v.stage);
        queue_scenes(c, a.stage);
        NXLOG_I(kTag, "Co-op deposit: the authority took this member's deposit (ask %llx): the drop is at stage %u, %u slots paid",
                static_cast<unsigned long long>(a.id), v.stage, a.n);
    } else {
        if (v.stage) set_stage(c, a.lo, a.hi, v.stage);
        NXLOG_I(kTag, "Co-op deposit: the authority refused this member's deposit (ask %llx, reason %u); the items stay in the drop, "
                "its stage here is %u", static_cast<unsigned long long>(a.id), static_cast<unsigned>(v.why), v.stage);
        using strings::Str;
        notice::post(strings::get(base(), v.why == Why::Stage ? Str::CoopDepositStage
                                          : v.why == Why::Needs ? Str::CoopDepositNeeds : Str::CoopDepositNoDrop));
    }
    s.asking = false;
}

void member_tick(::Ctx* c, bool in_game) {
    State& s = st();
    const double now = qpc::seconds();
    const double dt = now - s.last_tick;
    s.last_tick = now;
    use_tick(c);
    if (s.asking && member()) {
        s.ask_in_session += dt < 1.0 ? dt : 0.0;
        if (s.ask_in_session >= kGiveUp) {
            s.asking = false;
            NXLOG_W(kTag, "Co-op deposit: no answer to ask %llx in %.0f s; dropped, the items stay in the drop",
                    static_cast<unsigned long long>(s.ask.id), kGiveUp);
            notice::post(strings::get(base(), strings::Str::CoopDepositNoAnswer));
        } else if (now >= s.ask.next) {
            s.ask.next = now + kResend;
            wire::Writer w(link::self());
            w.bytes(s.ask.key, kMemberKeyBytes).u64(s.ask.id).u32(s.ask.lo).u16(s.ask.hi).u8(s.ask.stage).u8(s.ask.n);
            for (uint8_t k = 0; k < s.ask.n; ++k) w.u16(s.ask.items[k].item).u8(s.ask.items[k].count);
            link::to_authority(wire::Type::DepositAsk, w.take());
        }
    }
    if (s.have_state && s.use == Use::None && !s.asking && member()) {
        set_stage(c, s.state_lo, s.state_hi, s.state_stage);
        s.have_state = false;
    }
    // The paid scenes, in order: each is queued again until its played bit is set (0x2e6ce0 drops one the event queue cannot take).
    while (!s.scenes.empty() && (scene_played(s.scenes.front().id) || now - s.scenes.front().at > kSceneGiveUp)) {
        if (!scene_played(s.scenes.front().id)) NXLOG_W(kTag, "Co-op deposit: scene %u did not play; given up", s.scenes.front().id);
        s.scenes.erase(s.scenes.begin());
    }
    if (!s.scenes.empty() && now >= s.scenes.front().next && session::can_walk(in_game) && !talk::talking()) {
        s.scenes.front().next = now + kSceneEvery;
        play_scene(c, s.scenes.front().id);
    }
}

}  // namespace

bool take_grant(::Ctx*, uint64_t g) {
    State& s = st();
    if (!member() || !guest::ok(g, layout::kDropTimer + 4) || guest::deref(g) != base() + layout::kDropClass) return false;
    if (s.asking || s.use != Use::None) {
        if (!s.busy_told) notice::post(strings::get(base(), strings::Str::CoopDepositBusy));
        s.busy_told = true;
        return false;
    }
    s.busy_told = false;
    s.use = Use::Granted;
    s.g = g;
    s.lo = guest::rd<uint32_t>(g + kCellLo);
    s.hi = guest::rd<uint16_t>(g + kCellHi);
    return true;
}

int on_store(::Ctx* c) {
    if ((c->x[kTypeReg] & 0xff) != layout::kDropType || !(c->x[kTempReg] & 1)) return 0;
    if (!member() && st().use == Use::None) return 0;
    c->x[kTempReg] = 0;   // the own common store, the one this module reads and clears
    return 0;
}

int on_confirm(::Ctx* c) {
    State& s = st();
    const uint64_t g = c->x[kGimmickReg];
    if (!(c->x[kAnswerReg] & 1) || link::is_authority() || !guest::ok(g, layout::kDropTimer + 4) ||
        guest::deref(g) != base() + layout::kDropClass)
        return 0;
    const bool ours = s.use != Use::None && g == s.g;
    if (!ours && !member()) return 0;   // single player, plain Link Play: the game's close
    c->x[kAnswerReg] = 0;               // the check, the scenes, the step and the clear do not run here
    if (!member() || s.asking) {
        NXLOG_I(kTag, "Co-op deposit: a confirmed close %s; nothing is taken, the items stay in the drop",
                s.asking ? "while an ask waits" : "outside the session");
        return 0;
    }
    Ask& a = s.ask;
    a = Ask{};
    std::memcpy(a.key, member_key_bytes(), kMemberKeyBytes);
    if (!s.nonce) s.nonce = static_cast<uint32_t>(qpc::seconds() * 1e6) | 1u;
    a.id = static_cast<uint64_t>(s.nonce) << 32 | ++s.counter;
    a.lo = guest::rd<uint32_t>(g + kCellLo);
    a.hi = guest::rd<uint16_t>(g + kCellHi);
    a.stage = guest::rd<uint8_t>(g + layout::kDropStage);
    uint32_t first = 0, count = 0;
    const uint64_t store = slice(c, static_cast<uint32_t>(c->x[kRecordReg] & 0xffff), &first, &count);
    for (uint32_t slot = first; store && slot < first + count && a.n < kMaxItems; ++slot) {
        const uint64_t at = store + static_cast<uint64_t>(slot) * kSlotBytes;
        const uint32_t item = guest::rd<uint32_t>(at + kSlotItem) & kItemMask;
        const uint8_t n = guest::rd<uint8_t>(at + kSlotCount);
        if (item < 1 || item > kItemMax || !n) continue;
        a.items[a.n++] = Item{static_cast<uint16_t>(slot), static_cast<uint16_t>(item), n};
    }
    a.next = 0;
    s.asking = true;
    s.ask_in_session = 0;
    NXLOG_I(kTag, "Co-op deposit: this member confirmed a deposit at (%u,%u,%u), stage %u, %u slots; the authority is asked (ask %llx)",
            a.lo & 0xfff, (a.lo >> 16) & 0xff, a.hi & 0xfff, a.stage, a.n, static_cast<unsigned long long>(a.id));
    return 0;
}

void on_ask_message(::Ctx*, int from, uint8_t, wire::Reader& r) {
    Ask a;
    r.bytes(a.key, kMemberKeyBytes);
    a.id = r.u64();
    a.lo = r.u32();
    a.hi = r.u16();
    a.stage = r.u8();
    a.n = r.u8();
    if (!r.ok() || a.n > kMaxItems || !link::is_authority()) return;
    for (uint8_t k = 0; k < a.n; ++k) {
        a.items[k].item = r.u16();
        a.items[k].count = r.u8();
    }
    if (!r.ok()) return;
    State& s = st();
    for (const Ask& q : s.incoming)
        if (q.id == a.id && !std::memcmp(q.key, a.key, kMemberKeyBytes)) return;   // a resend of one still waiting
    if (s.incoming.size() >= kIncoming) return;
    a.node = from;
    a.at = qpc::seconds();
    s.incoming.push_back(a);
}

void on_state_message(::Ctx* c, int, uint8_t, wire::Reader& r) {
    Answer v;
    v.lo = r.u32();
    v.hi = r.u16();
    v.stage = r.u8();
    v.verdict = static_cast<Verdict>(r.u8());
    v.why = static_cast<Why>(r.u8());
    r.bytes(v.key, kMemberKeyBytes);
    v.id = r.u64();
    if (!r.ok() || link::is_authority()) return;
    State& s = st();
    if (v.verdict == Verdict::None) {
        s.have_state = true;
        s.state_lo = v.lo;
        s.state_hi = v.hi;
        s.state_stage = v.stage;
        return;
    }
    if (v.verdict != Verdict::Accepted && v.verdict != Verdict::Refused) return;
    if (!s.asking || v.id != s.ask.id || std::memcmp(v.key, member_key_bytes(), kMemberKeyBytes)) return;   // not this member's ask
    apply_answer(c, v);
}

void tick(::Ctx* c, bool in_game) {
    if (!c) return;
    if (link::is_authority()) authority_tick(c, in_game);
    else member_tick(c, in_game);
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::deposit
