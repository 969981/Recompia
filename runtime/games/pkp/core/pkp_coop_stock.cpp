// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's shared shop stock (pkp_coop_internal.h). A shop's stock and its purchase counts are world state, so
// the group has one: the authority's. A purchase by anyone counts against it, the daily restock runs on the authority alone, and
// every member's shop reads the authority's numbers. The coins stay each buyer's own (the buy takes them on the buyer's process).
//
// The game, 2.0.1 (modding-docs coop-v2/SHOP-STOCK-RE.md, SHOP-UNLOCK-RE.md):
//   the own record   save root + 0x6ae708 + entry * 8 (entries 1..0x7ff): +0 bit 2 in today's lineup (0x5bdb4c reads it), +1 flags
//                    (bit 1 bought), +2 u8 stock left. On Cloud Island the game uses another record (+0xf9357c), which this part leaves alone.
//   0x646864(M, e)   the stock left. Not a visitor: the own record. A visitor off Cloud Island: the guest purchase history
//                    0x608654(R+0x6ae700, the host's id, 0, e), its own count per visited world (0 when it has none), so a member's
//                    shop counted the member's own buys only, against no one else's.
//   0x646ae8(M, e,n) sets it. Not a visitor: 0x638414 (the own record; on the host also kind 8 to the visitors). A visitor off Cloud
//                    Island: the guest history (0x608920). The shop's buy 0xe88868 sets left - count there, then the bought bit
//                    (0x5bdc5c) only when not a visitor (0xe889a8), then the give 0xe889f8.
//   0x632380(CM)     the daily lineup restock: clears the guest history, picks the lineup and refills the own record (bits 2 / 3,
//                    the stock), with visitor branches of its own (0x632410, 0x6324b0, 0x6324e4, 0x632538, 0x632738).
//   0x21f38c         the host's answer to a visitor's buy on Cloud Island (its own stock there): not this part's.
// Replaced (rule 7), on a member in a session, off Cloud Island:
//   - 0x646864 (entry hook) answers the own record's stock, which holds the authority's numbers (StockState);
//   - 0x646ae8 (entry hook) writes the own record and sends the count taken to the authority (StockAsk), which takes it from the
//     group's stock; the guest history is not used;
//   - 0x632380 (entry hook) does not run: the authority's restock reaches the member with its next StockState. (Besides the
//     records it writes R+0x6b77bc (0xb4 bytes) and the u8 R+0x6b7870 on a non-visitor; a visitor, every member, never did.)
//   All three only once the member holds the authority's records (StockState); before that the game's own paths run.
//   - the bought bit comes with the stock bytes: the authority sets the bought bit of every purchase in its log
//     (pkp_coop_purchases.cpp gives it there), so a buying member's bit is the group's. The lineup (+0) is not sent: a member's
//     shop reads the lineup from its copy of the host's shop, which pkp_coop_shop_rows.cpp fills.
// The authority reads its own records once a second and sends them (StockState) when they changed, and to a member that asks.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::stock {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kRecords = 0x6ae708;   // save root + : the own shop records, 8 bytes per entry
constexpr uint32_t kEntryMax = 0x7ff;     // entries 1..0x7ff (0x646864's own check)
constexpr uint32_t kCountMax = 999;
constexpr double kReadEvery = 1.0;        // the authority: seconds between two reads of its records
constexpr double kAskEvery = 5.0;         // a member: seconds between two asks until a state arrives
constexpr size_t kTakesPerMessage = 64;

struct Take {
    uint16_t entry, count;
};

struct State {
    // the authority
    double read_at = -1e9;
    uint64_t sent_hash = 0;
    bool sent = false;
    // a member
    bool have = false;         // a StockState arrived in this session
    bool restock_told = false; // the skipped restock's line was written in this session
    double asked_at = -1e9;
    std::vector<Take> takes;   // counts taken here, not yet with the authority
};
State& st() {
    static State s;
    return s;
}

uint64_t records() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return r && guest::ok(r + kRecords, (kEntryMax + 1) * 8) ? r + kRecords : 0;
}

bool on_cloud_island() {
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(game, layout::kStateStage + 1) && guest::rd<uint8_t>(game + layout::kStateStage) == layout::kStageCloudIsland;
}

// A member of a session, off Cloud Island, that has the authority's records: the group's stock is the authority's. Until the
// first StockState of the session (or with an authority whose build sends none) the game's own paths run.
bool member_here() {
    constexpr uint8_t kMember = layout::kSessionOnline | layout::kSessionVisitor;
    return st().have && link::in_session() && !link::is_authority() && (session_bits() & kMember) == kMember && !on_cloud_island();
}

bool valid(uint64_t e) { return e >= 1 && e <= kEntryMax; }

// The records' flags and stock bytes, entries 1..kEntryMax in order.
std::vector<uint8_t> snapshot(uint64_t recs) {
    std::vector<uint8_t> out(kEntryMax * 2);
    for (uint32_t e = 1; e <= kEntryMax; ++e) {
        out[(e - 1) * 2] = guest::rd<uint8_t>(recs + e * 8 + 1);
        out[(e - 1) * 2 + 1] = guest::rd<uint8_t>(recs + e * 8 + 2);
    }
    return out;
}

uint64_t fnv(const std::vector<uint8_t>& v) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : v) h = (h ^ b) * 1099511628211ull;
    return h;
}

std::vector<uint8_t> state_message(const std::vector<uint8_t>& snap) {
    return wire::Writer(link::self()).u16(1).u16(static_cast<uint16_t>(kEntryMax)).bytes(snap.data(), snap.size()).take();
}

void tick_authority() {
    State& s = st();
    const double now = qpc::seconds();
    if (now - s.read_at < kReadEvery) return;
    s.read_at = now;
    const uint64_t recs = records();
    if (!recs) return;
    const std::vector<uint8_t> snap = snapshot(recs);
    const uint64_t h = fnv(snap);
    if (s.sent && h == s.sent_hash) return;
    link::to_members(wire::Type::StockState, state_message(snap));
    s.sent = true;
    s.sent_hash = h;
}

void tick_member() {
    State& s = st();
    const double now = qpc::seconds();
    const bool want = !s.have && now - s.asked_at >= kAskEvery;
    if (!want && s.takes.empty()) return;
    const size_t n = std::min(s.takes.size(), kTakesPerMessage);
    wire::Writer w(link::self());
    w.u8(want ? 1 : 0).u16(static_cast<uint16_t>(n));
    for (size_t i = 0; i < n; ++i) w.u16(s.takes[i].entry).u16(s.takes[i].count);
    if (!link::to_authority(wire::Type::StockAsk, w.take())) return;
    s.takes.erase(s.takes.begin(), s.takes.begin() + static_cast<std::ptrdiff_t>(n));
    if (want) s.asked_at = now;
}

}  // namespace

int on_get(::Ctx* c) {
    if (!member_here() || !valid(c->x[1] & 0xffff)) return 0;
    const uint64_t recs = records();
    if (!recs) return 0;
    c->x[0] = guest::rd<uint8_t>(recs + (c->x[1] & 0xffff) * 8 + 2);
    return 1;   // the function returns x0
}

int on_set(::Ctx* c) {
    const uint64_t e = c->x[1] & 0xffff;
    if (!member_here() || !valid(e)) return 0;
    const uint64_t recs = records();
    if (!recs) return 0;
    const uint8_t left = guest::rd<uint8_t>(recs + e * 8 + 2);
    const uint8_t now = static_cast<uint8_t>(c->x[2]);
    guest::wr<uint8_t>(recs + e * 8 + 2, now);
    if (left > now) {
        st().takes.push_back({static_cast<uint16_t>(e), static_cast<uint16_t>(left - now)});
        NXLOG_I(kTag, "Co-op stock: shop entry %u, %u taken here (%u left); sent to the authority", static_cast<unsigned>(e),
                static_cast<unsigned>(left - now), now);
    }
    return 1;   // the game's set (the guest history) does not run
}

int on_restock(::Ctx*) {
    if (!member_here()) return 0;
    if (!st().restock_told) NXLOG_I(kTag, "Co-op stock: the daily restock is the authority's; skipped here");
    st().restock_told = true;
    return 1;
}

void on_ask_message(::Ctx*, int from, uint8_t origin, wire::Reader& r) {
    const bool want = r.u8() != 0;
    const uint16_t n = r.u16();
    if (!r.ok() || !link::is_authority()) return;
    const uint64_t recs = records();
    for (uint16_t i = 0; i < n; ++i) {
        const uint16_t e = r.u16(), count = r.u16();
        if (!r.ok()) return;
        if (!recs || !valid(e) || count < 1 || count > kCountMax) continue;
        const uint8_t left = guest::rd<uint8_t>(recs + uint64_t{e} * 8 + 2);
        const uint8_t now = left > count ? static_cast<uint8_t>(left - count) : 0;
        guest::wr<uint8_t>(recs + uint64_t{e} * 8 + 2, now);
        NXLOG_I(kTag, "Co-op stock: node %u took %u of shop entry %u (%u -> %u left)", origin, count, e, left, now);
    }
    if (n) st().read_at = -1e9;   // read and sent at the next tick
    if (want && recs) link::to_node(from, wire::Type::StockState, state_message(snapshot(recs)));
}

void on_state_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    const uint16_t first = r.u16(), n = r.u16();
    if (!r.ok() || link::is_authority() || first < 1 || n > kEntryMax || first + n - 1 > kEntryMax) return;
    std::vector<uint8_t> bytes(size_t{n} * 2);
    r.bytes(bytes.data(), bytes.size());
    if (!r.ok()) return;
    const uint64_t recs = records();
    if (!recs) return;
    State& s = st();
    for (uint16_t i = 0; i < n; ++i) {
        const uint64_t rec = recs + (uint64_t{first} + i) * 8;
        guest::wr<uint8_t>(rec + 1, bytes[i * 2]);
        guest::wr<uint8_t>(rec + 2, bytes[i * 2 + 1]);
    }
    if (!s.have) NXLOG_I(kTag, "Co-op stock: the authority's shop stock is in this player's records");
    s.have = true;
}

void tick(::Ctx* c) {
    if (!c || !link::in_session()) return;
    if (link::is_authority()) tick_authority();
    else tick_member();
}

void on_session_change() {
    State& s = st();
    s.have = false;
    s.restock_told = false;
    s.asked_at = -1e9;
    s.sent = false;
    s.read_at = -1e9;
}

void on_member_joined() { st().sent = false; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::stock
