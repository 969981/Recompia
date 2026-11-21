// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's shared shop rows (pkp_coop_internal.h). Which of the shop's rows are open (their unlock conditions),
// today's lineup and the rows' hints are world state, so the group has one answer per row: the authority's. A member's shop reads it
// where the game has a visitor read its copy of the host's shop.
//
// The game, 2.0.1 (modding-docs coop-v2/SHOP-CATALOG-RE.md):
//   the visitor's copy   shop manager (CM, *(main + 0x3565170)) + entry * 0x20 (entries 1..0x7ff): +0x5488 u32 lineup slot word,
//                        +0x548c u16 lineup price, +0x5490 / +0x5491 u8 unlock condition 0 / 1 met, +0x5492 bit 0 listed; CM +
//                        0x15478 the hint bits (hint id < 300), CM + 0x18618 the lineup's day. Written on a visitor by 0x57dda0, the
//                        apply of the host's kind-8 record (0x637988 builds it: 0x630a60, 0x6382e0(e, 0 / 1, 0), the lineup slot of
//                        0x5c1204, the hint conditions with first argument 1).
//   0x630c24 (visitor)   a row whose #70 +0x23 bit 1 is clear (every row of the regular catalog) is open only when +0x5490 / +0x5491
//                        is set; 0x648a9c and the shop's row test 0xe86494 go through it. The host evaluates the conditions live.
//   The game sends a row's record only after the host raises it (0x4b1d54 kind 8, from its sweep 0x630460 when the row's answer
//   changes); a member had the daily rows' records but not the regular rows', so every regular row's conditions read 0 there.
// Replaced (rule 7), on a member in a session, off Cloud Island: the authority computes each row's record as 0x637988 does, with the
// game's own functions, and sends the whole set (ShopRows) when it changes, every kResendEvery seconds and when a member joins; the
// member writes it into its copy as 0x57dda0 does, again every kApplyEvery seconds, so the game's own records cannot leave it stale.
// The two unlock conditions are sent as the host answers them (its record's latch, else live), not live alone as 0x637988 does.
// The stock (+0x548e, the own record +2) is pkp_coop_stock.cpp's.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_tables.h"

#include <cstddef>
#include <utility>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::shoprows {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint32_t kEntryMax = 0x7ff;        // entries 1..0x7ff
constexpr uint32_t kPerFrame = 32;           // the authority: rows computed per frame (a pass takes about a second)
constexpr double kPassEvery = 5.0;           // the authority: seconds between the starts of two passes (a join starts one)
constexpr double kResendEvery = 30.0;        // the authority: the set is sent again after this long without a change
constexpr double kApplyEvery = 1.0;          // a member: seconds between two writes of the set into its copy
// The game (2.0.1).
constexpr uint64_t kShopTable = 0x230;       // the data-table manager's shop table (#70), read at +0 while the count is <= 0x46
constexpr uint64_t kHintTable = 0x218;       // its hint table, read at +0 while the count is <= 0x43
constexpr uint64_t kShopDefault = 0x3485460; // main + : the shop table's default record (a row without one)
constexpr uint64_t kHintDefault = 0x34867a8; // main + : the hint table's default record
constexpr uint64_t kKindTable = 0x3484e48;   // main + : the condition kinds' functions (u8 kind 1..0x17 -> fn(area, a, b, flag))
constexpr uint64_t kListed = 0x630a60;       // (CM, entry): the host's "row listed"
constexpr uint64_t kCondition = 0x6382e0;    // (entry, which, 0): unlock condition `which` met
constexpr uint64_t kInLineup = 0x5c158c;     // (entry): the row is in today's lineup
constexpr uint64_t kLineupSlot = 0x5c1204;   // (entry): its lineup slot (0..14), 15 or more for none
constexpr uint64_t kLineup = 0x6b77bc;       // save root + : the 15 lineup slots, 12 bytes: u16 entry, u16 +2 price, u32 +4 word
constexpr uint64_t kLineupDay = 0x6b7870;    // save root + : the lineup's day
constexpr uint64_t kRecords = 0x6ae708;      // save root + entry * 8: the own shop record, +0 bit 2 the row's opened latch
constexpr uint64_t kRow = 0x5478;            // CM + entry * 0x20 + : the visitor's copy of row `entry`
constexpr uint64_t kRowSize = 0x20;
constexpr uint64_t kRowWord = 0x5488 - kRow, kRowPrice = 0x548c - kRow, kRowMet = 0x5490 - kRow, kRowListed = 0x5492 - kRow;
constexpr uint64_t kHints = 0x15478;         // CM + : the hint bits
constexpr uint64_t kDay = 0x18618;           // CM + : u32 the lineup's day
constexpr uint32_t kHintMax = 300;

// One row's record as the wire carries it.
enum Bits : uint8_t { kListedBit = 1, kMet0 = 2, kMet1 = 4, kHint0 = 8, kHint1 = 16, kInArea = 128 };
struct Row {
    uint8_t bits = 0;
    uint16_t price = 0, hint0 = 0, hint1 = 0;
    uint32_t word = 0;
};

struct State {
    // the authority
    std::vector<Row> pass;     // the pass being computed
    uint32_t next = 0;         // its next entry; 0 while no pass runs
    double pass_at = -1e9, sent_at = -1e9;
    uint64_t sent_hash = 0;
    bool sent = false;
    // a member
    std::vector<Row> rows;     // the authority's set (index entry - 1), empty until it arrives
    uint8_t day = 0;
    double applied_at = -1e9;
};
State& st() {
    static State s;
    return s;
}

uint8_t stage() {
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    return game && guest::ok(game + layout::kStateStage, 1) ? guest::rd<uint8_t>(game + layout::kStateStage) : 0;
}

uint64_t manager() {
    const uint64_t cm = guest::rd<uint64_t>(base() + layout::kChallengeManager);
    return cm && guest::ok(cm + kDay, 4) ? cm : 0;
}

// A table row's record: [row + 8], or the table's default record.
uint64_t record(::Ctx* c, uint64_t table, uint64_t fallback, uint32_t id) {
    const uint64_t row = tables::row_or_first(c, base(), table, id);
    if (!row || !guest::ok(row + 8, 8)) return 0;
    const uint64_t rec = guest::rd<uint64_t>(row + 8);
    return rec ? rec : guest::rd<uint64_t>(base() + fallback);
}

// A hint's condition, as 0x637988 asks it for a visitor (first argument 1, flag 0); false for no hint or no function.
bool hint_met(::Ctx* c, uint16_t hint) {
    if (hint < 1 || hint >= kHintMax) return false;
    const uint64_t rec = record(c, kHintTable, kHintDefault, hint);
    if (!rec || !guest::ok(rec, 6)) return false;
    const uint8_t kind = guest::rd<uint8_t>(rec);
    if (kind < 1 || kind > 0x17) return false;
    const uint64_t fn = guest::rd<uint64_t>(guest::rd<uint64_t>(base() + kKindTable) + uint64_t{kind} * 8);
    if (!fn || fn == base()) return false;
    const int64_t a = static_cast<int16_t>(guest::rd<uint16_t>(rec + 2)), b = static_cast<int16_t>(guest::rd<uint16_t>(rec + 4));
    return guest::call(c, fn, 1, static_cast<uint64_t>(a), static_cast<uint64_t>(b), 0) & 1;
}

// Row `e`'s record on the authority, as 0x637988 builds it for a visitor (the conditions as the host itself answers them).
Row compute(::Ctx* c, uint64_t cm, uint64_t root, uint32_t e) {
    Row r;
    const uint64_t rec = record(c, kShopTable, kShopDefault, e);
    if (!rec || !guest::ok(rec, 0x24)) return r;
    if (guest::call(c, base() + kListed, cm, e) & 1) r.bits |= kListedBit;
    const uint8_t area = guest::rd<uint8_t>(rec + 0x22);
    if (area < 1 || area > 31) return r;   // 0x57dda0 writes the listed bit alone for such a row
    r.bits |= kInArea;
    // The host's own answer (0x630c24, 0x648a9c): the record's latch, else the condition live. 0x637988 sends the live one alone.
    const bool latched = guest::rd<uint8_t>(root + kRecords + uint64_t{e} * 8) & 4;
    if (latched || (guest::call(c, base() + kCondition, e, 0, 0) & 1)) r.bits |= kMet0;
    if (latched || (guest::call(c, base() + kCondition, e, 1, 0) & 1)) r.bits |= kMet1;
    const uint32_t slot = static_cast<uint32_t>(guest::call(c, base() + kLineupSlot, e));
    if (slot < 15) {
        const uint64_t at = root + kLineup + uint64_t{slot} * 12;
        r.word = guest::rd<uint32_t>(at + 4);
        if (guest::call(c, base() + kInLineup, e) & 1) r.price = guest::rd<uint16_t>(at + 2);
    }
    r.hint0 = guest::rd<uint16_t>(rec + 6);
    r.hint1 = guest::rd<uint16_t>(rec + 0x1a);
    if (hint_met(c, r.hint0)) r.bits |= kHint0;
    if (hint_met(c, r.hint1)) r.bits |= kHint1;
    return r;
}

std::vector<uint8_t> message(const std::vector<Row>& rows, uint8_t day) {
    wire::Writer w(link::self());
    w.u8(day).u16(1).u16(static_cast<uint16_t>(rows.size()));
    for (const Row& r : rows) w.u8(r.bits).u16(r.price).u32(r.word).u16(r.hint0).u16(r.hint1);
    return w.take();
}

uint64_t fnv(const std::vector<uint8_t>& v) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = wire::kHeader; i < v.size(); ++i) h = (h ^ v[i]) * 1099511628211ull;
    return h;
}

// The game's own builder runs only in the world, and nobody needs the rows without a member: no pass then.
void tick_authority(::Ctx* c, bool in_game) {
    State& s = st();
    const double now = qpc::seconds();
    const uint64_t cm = manager();
    const uint64_t root = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    if (!in_game || !link::member_count() || !cm || !root || !guest::ok(root + kRecords, (kEntryMax + 1) * 8) ||
        !guest::ok(root + kLineup, kLineupDay - kLineup + 1) || stage() == layout::kStageCloudIsland) {
        s.next = 0;
        return;
    }
    if (!s.next) {
        if (s.sent && now - s.pass_at < kPassEvery) return;
        s.pass_at = now;
        s.pass.assign(kEntryMax, Row{});
        s.next = 1;
    }
    for (uint32_t i = 0; i < kPerFrame && s.next <= kEntryMax; ++i, ++s.next) s.pass[s.next - 1] = compute(c, cm, root, s.next);
    if (s.next <= kEntryMax) return;
    s.next = 0;
    std::vector<uint8_t> msg = message(s.pass, guest::rd<uint8_t>(root + kLineupDay));
    const uint64_t h = fnv(msg);
    if (s.sent && h == s.sent_hash && now - s.sent_at < kResendEvery) return;
    link::to_members(wire::Type::ShopRows, msg);
    s.sent = true;
    s.sent_hash = h;
    s.sent_at = now;
}

bool member_here() {
    constexpr uint8_t kMember = layout::kSessionOnline | layout::kSessionVisitor;
    return link::in_session() && !link::is_authority() && (session_bits() & kMember) == kMember && stage() != layout::kStageCloudIsland;
}

void set_hint(uint64_t cm, uint16_t hint, bool met) {
    if (hint < 1 || hint >= kHintMax) return;
    const uint64_t byte = cm + kHints + (hint >> 3);
    const uint8_t mask = static_cast<uint8_t>(1u << (hint & 7)), old = guest::rd<uint8_t>(byte);
    guest::wr<uint8_t>(byte, static_cast<uint8_t>(met ? (old | mask) : (old & ~mask)));
}

// Writes the authority's set into this member's copy, as 0x57dda0 writes one record.
void apply() {
    State& s = st();
    const uint64_t cm = manager();
    if (!cm || s.rows.empty()) return;
    for (uint32_t e = 1; e <= s.rows.size(); ++e) {
        const Row& r = s.rows[e - 1];
        const uint64_t at = cm + kRow + uint64_t{e} * kRowSize;
        guest::wr<uint8_t>(at + kRowListed, static_cast<uint8_t>((guest::rd<uint8_t>(at + kRowListed) & 0xfe) | (r.bits & kListedBit)));
        if (!(r.bits & kInArea)) continue;
        guest::wr<uint32_t>(at + kRowWord, r.word);
        guest::wr<uint16_t>(at + kRowPrice, r.price);
        guest::wr<uint8_t>(at + kRowMet, (r.bits & kMet0) ? 1 : 0);
        guest::wr<uint8_t>(at + kRowMet + 1, (r.bits & kMet1) ? 1 : 0);
        set_hint(cm, r.hint0, r.bits & kHint0);
        set_hint(cm, r.hint1, r.bits & kHint1);
    }
    guest::wr<uint32_t>(cm + kDay, s.day);
}

void tick_member() {
    State& s = st();
    if (s.rows.empty() || !member_here()) return;
    const double now = qpc::seconds();
    if (now - s.applied_at < kApplyEvery) return;
    s.applied_at = now;
    apply();
}

}  // namespace

void on_rows_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    const uint8_t day = r.u8();
    const uint16_t first = r.u16(), n = r.u16();
    if (!r.ok() || link::is_authority() || first != 1 || n > kEntryMax) return;
    std::vector<Row> rows(n);
    for (Row& row : rows) {
        row.bits = r.u8();
        row.price = r.u16();
        row.word = r.u32();
        row.hint0 = r.u16();
        row.hint1 = r.u16();
    }
    if (!r.ok()) return;
    State& s = st();
    if (s.rows.empty()) NXLOG_I(kTag, "Co-op shop: the authority's shop rows arrived (%u rows)", static_cast<unsigned>(n));
    s.rows = std::move(rows);
    s.day = day;
    s.applied_at = -1e9;   // written at the next tick
}

void tick(::Ctx* c, bool in_game) {
    if (!c || !link::in_session()) return;
    if (link::is_authority()) tick_authority(c, in_game);
    else tick_member();
}

void on_session_change() {
    State& s = st();
    s.rows.clear();
    s.applied_at = -1e9;
    s.sent = false;
    s.next = 0;
    s.pass_at = -1e9;
}

void on_member_joined() { st().sent = false; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::shoprows
