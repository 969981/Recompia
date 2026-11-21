// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's shared shop purchases (pkp_coop_internal.h). What one member buys in a shop (an item, an upgrade, a
// recipe, anything) is given to every member too, and to every member that joins the group later; only the buyer pays.
//
// The game, 2.0.1 (modding-docs coop-v2/SHOP-STOCK-RE.md):
//   kGrant(slot, entry, count, flag)  the purchase's give, after the price and the stock were taken: by the item's category it learns
//                                     the recipe (0x2e7240), adds the item to the bag, or queues the upgrade's scripted event
//                                     (0x4676c0: events 0xea8 / 0x107e / 0xea7 / 0x108f, the Backpack Upgrade family); 1 when given.
//                                     Its two callers are the two ways one purchase ends on the buyer's process: the shop's buy
//                                     0xe88868 (tail call), and on a visitor the host's answer to the visitor's buy (0x550b08 ->
//                                     0x226280, which takes the price there and calls kGrant(0, entry, count, 0)). So a purchase passes
//                                     the entry of kGrant once, on the buyer's process only.
//   kBought(entry)                    the purchase record's bought bit (save root + 0x6ae709 + entry * 8, bit 1; CMNDAT). The buy sets it
//                                     only when kVisitorCheck is 0, and so does a received purchase here.
//   The stock: the game already keeps it with the host (a visitor's buy goes through the host), so a received purchase leaves it.
//   kOfflineOnly(entry)               the shop row's byte +0x23 bit 2 (shop rows: LINKDATA #70). Its only two callers, the shop's
//                                     listing tests 0xe86494 (@0xe86674) and 0xe87424 (@0xe87628), hide such a row whenever IsOnline
//                                     (0x207b0) is set: Packing Tips (the bag upgrades, items 0xf91..0xf94), PP Up, the handy bag and
//                                     28 more rows (issue #780: the host's shop lacked them in co-op). In a co-op session it answers 0,
//                                     so the shop lists them as offline. A member's shop is the game's visitor shop (it lists what the
//                                     host's shop publishes, and its buys go through the host); what the host buys reaches it below.
//   kShopTile()                       0xe8d428, the PC menu's shop tile test for a visitor (only caller 0xeff7a0, tile 1, with entry
//                                     0): 1 when mask bit 23 (the per-row visitor lock) is set, online, not Cloud Island, a visitor.
//                                     The tile's other way in, 0xe88524, wants the host. A member's mask has bit 23 cleared
//                                     (pkp_coop_story.cpp), so the tile was grey on members; in a session it answers 1.
// Rules:
//   - every purchase on a process in the session (the entry hook, not our own replays) goes to the log: the authority appends its
//     own at once, a member sends it (Purchase) and the authority appends it. The log is the authority's, numbered from 1, kept in a
//     file next to its save (<save dir>/account/<key>.coop_purchases), so a later session still has it; each entry holds the buyer's
//     member key (the launch's 32 hex digits, the same on every launch of that player);
//   - every new entry goes to every member (PurchaseLog); a member asks for what it lacks (PurchaseAsk: the log it knows and the
//     last entry it applied) when it joins a session and when it sees a gap, and gets the rest from the authority;
//   - every process (the authority too) applies the log in order, one entry per safe moment (it can walk, talks to no one): its own
//     purchases are skipped, another's are given here through kGrant (no price), then the bought bit as the buy would. A bag give
//     that fails (a full bag) is tried again later, kTries times, then skipped with a line;
//   - the last entry applied (and the log it belongs to) is kept in this player's own save, in unused elements of the unique request
//     record array (layout::kSaveUniqueRequests; pkp_coop_outcomes.cpp uses 201..208, readers check id <= 200): element 209
//     kMarkMagic, 210 the last entry applied, 211..212 the log id. It is saved with the bag, so a give and its mark are kept or
//     lost together. Another log (a new group's, or the authority's file lost) starts over at its first entry.
#include "core/pkp_coop_internal.h"
#include "core/pkp_strings.h"
#include "core/pkp_guest.h"
#include "core/pkp_items.h"
#include "core/pkp_layout.h"
#include "core/pkp_save_files.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <vector>

#include "nxrt/file_write.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::purchases {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kGrant = 0xe889f8;         // (slot, entry, count, flag) -> 1: given
constexpr uint64_t kBought = 0x5bdc5c;        // (entry): the purchase record's bought bit
constexpr uint64_t kVisitorCheck = layout::kOnline;   // () -> bit 0; the buy sets the bought bit only when it is 0 (0xe889a8)
constexpr uint64_t kEntryToItem = 0x63e3b8;   // (shop manager, entry) -> the entry's item (bits 0..12)
constexpr uint64_t kShopManager = layout::kChallengeManager;   // main + : the manager that also holds the shop's stock
constexpr uint32_t kEntryMax = 0x7ff;         // entries 1..0x7ff (the grant's own check)
constexpr uint32_t kCountMax = 999;
constexpr uint64_t kItemCategory = 0x3a;      // item record + : u8 category (the grant's switch, 0x0e..0x15)
constexpr uint8_t kCategoryEvent = 0x12;      // the grant queues the item's scripted event (PP Up, the handy bag, Packing Tips)
constexpr double kEventQuiet = 30.0;          // seconds after such a give in which the event's own item gives are not shared again
constexpr uint8_t kCategoryRecipe = 0x0f;     // the grant learns a recipe (0 when known: not a failure to retry)

// The mark in this player's save: elements of the unique request record array (4 bytes each).
constexpr uint32_t kRecordCount = 256;
constexpr uint32_t kMarkMagicAt = 209, kMarkSeqAt = 210, kMarkLogAt = 211;   // 211..212: the log id
constexpr uint32_t kMarkMagic = 0x706f6873;   // "shop"; the default element is 01 01 00 00

constexpr size_t kKeyBytes = kMemberKeyBytes;
constexpr size_t kEntryBytes = 4 + kKeyBytes + 2 + 2;   // seq, buyer, entry, count
constexpr size_t kPerMessage = 32;                     // entries in one PurchaseLog
constexpr char kFileMagic[8] = {'P', 'K', 'P', 'C', 'P', 'U', 'R', '1'};
constexpr double kAskEvery = 10.0;   // seconds between two asks while a gap stays
constexpr double kRetryAfter = 30.0;
constexpr int kTries = 5;

struct Entry {
    uint32_t seq = 0;
    uint8_t buyer[kKeyBytes] = {};
    uint16_t entry = 0, count = 0;
};

struct State {
    int remote = 0;                    // > 0 while our own give runs: not logged again
    // the authority
    bool log_loaded = false;
    uint64_t log_id = 0;
    std::vector<Entry> log;            // seq i + 1 at index i
    std::string log_path;
    // this player
    std::vector<Entry> unsent;         // a member's purchases not yet with the authority
    uint64_t known_log = 0;            // the log the authority last named (0: none yet)
    uint32_t known_end = 0;            // its last entry, as the authority last said
    std::map<uint32_t, Entry> pending; // received, not applied yet
    bool ask = true;
    double asked_at = -1e9;
    int tries = 0;
    double retry_at = 0.0;
    double quiet_until = 0.0;          // a received upgrade's scripted event may still run until then
};
State& st() {
    static State s;
    return s;
}

// ---- this player's mark ----

uint64_t records() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return r && guest::ok(r + layout::kSaveUniqueRequests, kRecordCount * 4) ? r + layout::kSaveUniqueRequests : 0;
}

// The last entry applied of log `id` here (0: none of that log).
uint32_t applied(uint64_t id) {
    const uint64_t recs = records();
    if (!recs || guest::rd<uint32_t>(recs + kMarkMagicAt * 4) != kMarkMagic) return 0;
    const uint64_t have = guest::rd<uint32_t>(recs + kMarkLogAt * 4) | uint64_t{guest::rd<uint32_t>(recs + (kMarkLogAt + 1) * 4)} << 32;
    return have == id ? guest::rd<uint32_t>(recs + kMarkSeqAt * 4) : 0;
}

// The log this player's mark belongs to (0: no mark).
uint64_t marked_log() {
    const uint64_t recs = records();
    if (!recs || guest::rd<uint32_t>(recs + kMarkMagicAt * 4) != kMarkMagic) return 0;
    return guest::rd<uint32_t>(recs + kMarkLogAt * 4) | uint64_t{guest::rd<uint32_t>(recs + (kMarkLogAt + 1) * 4)} << 32;
}

void set_applied(uint64_t id, uint32_t seq) {
    const uint64_t recs = records();
    if (!recs) return;
    guest::wr<uint32_t>(recs + kMarkMagicAt * 4, kMarkMagic);
    guest::wr<uint32_t>(recs + kMarkSeqAt * 4, seq);
    guest::wr<uint32_t>(recs + kMarkLogAt * 4, static_cast<uint32_t>(id));
    guest::wr<uint32_t>(recs + (kMarkLogAt + 1) * 4, static_cast<uint32_t>(id >> 32));
}

bool mine(const Entry& e) {
    return std::memcmp(e.buyer, member_key_bytes(), kKeyBytes) == 0;
}

// ---- the authority's log ----

void put_entry(std::string& out, const Entry& e) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>(e.seq >> (8 * i));
    out.append(reinterpret_cast<const char*>(e.buyer), kKeyBytes);
    out += static_cast<char>(e.entry);
    out += static_cast<char>(e.entry >> 8);
    out += static_cast<char>(e.count);
    out += static_cast<char>(e.count >> 8);
}

bool get_entry(const uint8_t* p, Entry* e) {
    e->seq = p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
    std::memcpy(e->buyer, p + 4, kKeyBytes);
    e->entry = static_cast<uint16_t>(p[20] | p[21] << 8);
    e->count = static_cast<uint16_t>(p[22] | p[23] << 8);
    return e->seq && e->entry >= 1 && e->entry <= kEntryMax && e->count >= 1 && e->count <= kCountMax;
}

void save_log() {
    State& s = st();
    std::string out(kFileMagic, sizeof kFileMagic);
    for (int i = 0; i < 8; ++i) out += static_cast<char>(s.log_id >> (8 * i));
    for (const Entry& e : s.log) put_entry(out, e);
    std::string err;
    if (!write_file_atomic(s.log_path, out, &err)) NXLOG_W(kTag, "Co-op purchases: the log is not written: %s", err.c_str());
}

// A new, empty log with a new id (every member then starts at its first entry).
void new_log() {
    State& s = st();
    std::random_device rd;
    s.log_id = (uint64_t{rd()} << 32 | rd()) | 1;
    s.log.clear();
    save_log();
    NXLOG_I(kTag, "Co-op purchases: a new purchase log for this world");
}

// The authority's log, read from its file once (a new log, with a new id, when there is none).
void load_log() {
    State& s = st();
    if (s.log_loaded) return;
    s.log_loaded = true;
    s.log_path = save_files::u8(save_files::save_dir() / "account" / (save_files::account_key() + ".coop_purchases"));
    std::string text;
    if (std::ifstream in{save_files::from_u8(s.log_path), std::ios::binary}) text.assign(std::istreambuf_iterator<char>(in), {});
    const auto* p = reinterpret_cast<const uint8_t*>(text.data());
    if (text.size() >= 16 && std::memcmp(p, kFileMagic, sizeof kFileMagic) == 0) {
        for (int i = 0; i < 8; ++i) s.log_id |= uint64_t{p[8 + i]} << (8 * i);
        for (size_t at = 16; at + kEntryBytes <= text.size(); at += kEntryBytes) {
            Entry e;
            if (!get_entry(p + at, &e) || e.seq != s.log.size() + 1) {
                NXLOG_W(kTag, "Co-op purchases: the log file is cut at entry %zu (not in order)", s.log.size() + 1);
                break;
            }
            s.log.push_back(e);
        }
    }
    if (!s.log_id) {
        new_log();
    } else {
        NXLOG_I(kTag, "Co-op purchases: the purchase log holds %zu purchases", s.log.size());
    }
}

std::vector<uint8_t> log_message(size_t from, size_t to) {
    const State& s = st();
    wire::Writer w(link::self());
    for (int i = 0; i < 8; ++i) w.u8(static_cast<uint8_t>(s.log_id >> (8 * i)));
    w.u32(static_cast<uint32_t>(s.log.size())).u16(static_cast<uint16_t>(to - from));
    for (size_t i = from; i < to; ++i) {
        const Entry& e = s.log[i];
        w.u32(e.seq).bytes(e.buyer, kKeyBytes).u16(e.entry).u16(e.count);
    }
    return w.take();
}

// The authority: a purchase into the log, then to every member (and to this process's own apply).
void append(const uint8_t* buyer, uint16_t entry, uint16_t count) {
    State& s = st();
    load_log();
    Entry e;
    e.seq = static_cast<uint32_t>(s.log.size() + 1);
    std::memcpy(e.buyer, buyer, kKeyBytes);
    e.entry = entry;
    e.count = count;
    s.log.push_back(e);
    save_log();
    link::to_members(wire::Type::PurchaseLog, log_message(s.log.size() - 1, s.log.size()));
    s.known_log = s.log_id;
    s.known_end = e.seq;
    s.pending[e.seq] = e;
    NXLOG_I(kTag, "Co-op purchases: purchase %u: shop entry %u x%u", e.seq, entry, count);
}

// ---- applying ----

uint8_t category(::Ctx* c, uint16_t entry) {
    const uint64_t mgr = guest::rd<uint64_t>(base() + kShopManager);
    if (!mgr) return 0;
    const uint32_t item = static_cast<uint32_t>(guest::call(c, base() + kEntryToItem, mgr, entry)) & items::kIdMask;
    const uint64_t rec = item ? items::record(c, base(), item) : 0;
    return rec && guest::ok(rec + kItemCategory, 1) ? guest::rd<uint8_t>(rec + kItemCategory) : 0;
}

std::string item_name(::Ctx* c, uint16_t entry) {
    const uint64_t mgr = guest::rd<uint64_t>(base() + kShopManager);
    if (!mgr) return {};
    const uint32_t item = static_cast<uint32_t>(guest::call(c, base() + kEntryToItem, mgr, entry)) & items::kIdMask;
    std::string name = item ? items::name(c, base(), item) : std::string();
    if (name.find('<') != std::string::npos) name.clear();   // markup: not shown raw
    return name;
}

// Gives entry e here (not this player's own). True: done with it (given, or given up).
bool give(::Ctx* c, const Entry& e) {
    State& s = st();
    const uint8_t cat = category(c, e.entry);
    ++s.remote;
    bags::gain_begin(c);   // a bag give is credited to the bag record (no write-back takes it away)
    const bool given = guest::call(c, base() + kGrant, 0, e.entry, e.count, 0) & 1;
    bags::gain_end(c);
    --s.remote;
    if (given && cat == kCategoryEvent) s.quiet_until = qpc::seconds() + kEventQuiet;
    if (!given && cat != kCategoryRecipe) {
        if (++s.tries < kTries) {
            s.retry_at = qpc::seconds() + kRetryAfter;
            if (s.tries == 1) notice::post(strings::get(base(), strings::Str::CoopBuyRetry));
            NXLOG_I(kTag, "Co-op purchases: purchase %u (shop entry %u x%u) not given here (try %d of %d)", e.seq, e.entry, e.count,
                    s.tries, kTries);
            return false;
        }
        NXLOG_W(kTag, "Co-op purchases: purchase %u (shop entry %u x%u) not given here after %d tries, skipped", e.seq, e.entry,
                e.count, kTries);
        notice::post(strings::get(base(), strings::Str::CoopBuyFailed));
        s.tries = 0;
        return true;
    }
    s.tries = 0;
    if (!(guest::call(c, base() + kVisitorCheck, 0) & 1)) guest::call(c, base() + kBought, e.entry);
    const std::string name = item_name(c, e.entry);
    using strings::Str;
    if (name.empty())
        notice::post(e.count > 1 ? strings::fmt(base(), Str::CoopBoughtSomethingCount, e.count) : strings::get(base(), Str::CoopBoughtSomething));
    else notice::post(e.count > 1 ? strings::fmt(base(), Str::CoopBoughtCount, name, e.count) : strings::fmt(base(), Str::CoopBought, name));
    NXLOG_I(kTag, "Co-op purchases: purchase %u (shop entry %u x%u, category 0x%x) given here", e.seq, e.entry, e.count, cat);
    return true;
}

void tick_apply(::Ctx* c, bool in_game) {
    State& s = st();
    if (!s.known_log || !records()) return;
    uint32_t done = applied(s.known_log);
    // Entries this player has applied already, or of another log, are dropped.
    while (!s.pending.empty() && s.pending.begin()->first <= done) s.pending.erase(s.pending.begin());
    if (s.pending.empty() || s.pending.begin()->first != done + 1) {
        if (s.known_end > done) s.ask = true;   // a gap: the rest is asked for
        return;
    }
    const Entry e = s.pending.begin()->second;
    if (mine(e)) {   // this player's own purchase: it has it
        set_applied(s.known_log, e.seq);
        s.pending.erase(s.pending.begin());
        return;
    }
    if (!session::can_walk(in_game) || talk::talking() || qpc::seconds() < s.retry_at) return;
    if (!give(c, e)) return;
    set_applied(s.known_log, e.seq);
    s.pending.erase(s.pending.begin());
}

void tick_send() {
    State& s = st();
    if (link::is_authority()) {
        for (const Entry& e : s.unsent) append(e.buyer, e.entry, e.count);
        s.unsent.clear();
        return;
    }
    while (!s.unsent.empty()) {
        const Entry& e = s.unsent.front();
        if (!link::to_authority(wire::Type::Purchase, wire::Writer(link::self()).u16(e.entry).u16(e.count).bytes(e.buyer, kKeyBytes).take()))
            return;
        s.unsent.erase(s.unsent.begin());
    }
}

void tick_ask() {
    State& s = st();
    if (!s.ask || link::is_authority()) return;
    const double now = qpc::seconds();
    if (now - s.asked_at < kAskEvery) return;
    const uint64_t id = s.known_log ? s.known_log : marked_log();   // the log this player knows: only what it lacks is sent
    const uint32_t done = id ? applied(id) : 0;
    wire::Writer w(link::self());
    for (int i = 0; i < 8; ++i) w.u8(static_cast<uint8_t>(id >> (8 * i)));
    if (!link::to_authority(wire::Type::PurchaseAsk, w.u32(done).take())) return;
    s.ask = false;
    s.asked_at = now;
}

}  // namespace

bool replaying() {
    const State& s = st();
    return s.remote > 0 || qpc::seconds() < s.quiet_until;
}

int on_offline_only(::Ctx* c) {
    if (!(session_bits() & layout::kSessionOnline)) return 0;
    c->x[0] = 0;
    return 1;   // the function returns x0
}

int on_shop_tile(::Ctx* c) {
    constexpr uint8_t kMember = layout::kSessionOnline | layout::kSessionVisitor;
    if ((session_bits() & kMember) != kMember) return 0;
    c->x[0] = 1;
    return 1;   // the function returns x0
}

int on_grant(::Ctx* c) {
    State& s = st();
    if (s.remote || !link::in_session()) return 0;
    const uint32_t entry = static_cast<uint32_t>(c->x[1]) & 0xffff;
    const uint32_t count = static_cast<uint32_t>(c->x[2]) & 0xffff;
    if (entry < 1 || entry > kEntryMax || count < 1 || count > kCountMax) return 0;
    Entry e;
    std::memcpy(e.buyer, member_key_bytes(), kKeyBytes);
    e.entry = static_cast<uint16_t>(entry);
    e.count = static_cast<uint16_t>(count);
    s.unsent.push_back(e);
    NXLOG_I(kTag, "Co-op purchases: bought shop entry %u x%u here; shared with the group", entry, count);
    return 0;
}

void on_purchase_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    const uint16_t entry = r.u16(), count = r.u16();
    uint8_t buyer[kKeyBytes];
    r.bytes(buyer, kKeyBytes);
    if (!r.ok() || !link::is_authority() || entry < 1 || entry > kEntryMax || count < 1 || count > kCountMax) return;
    NXLOG_I(kTag, "Co-op purchases: node %u bought shop entry %u x%u", origin, entry, count);
    append(buyer, entry, count);
}

void on_ask_message(::Ctx*, int from, uint8_t origin, wire::Reader& r) {
    State& s = st();
    uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id |= uint64_t{r.u8()} << (8 * i);
    const uint32_t done = r.u32();
    if (!r.ok() || !link::is_authority()) return;
    load_log();
    size_t from_at = id == s.log_id ? std::min<size_t>(done, s.log.size()) : 0;
    if (from_at == s.log.size()) {   // nothing to send: the end is named, so the member knows the log
        link::to_node(from, wire::Type::PurchaseLog, log_message(from_at, from_at));
        return;
    }
    NXLOG_I(kTag, "Co-op purchases: node %u has %u of %zu purchases; the rest is sent", origin, static_cast<unsigned>(from_at),
            s.log.size());
    for (; from_at < s.log.size(); from_at = std::min(from_at + kPerMessage, s.log.size()))
        link::to_node(from, wire::Type::PurchaseLog, log_message(from_at, std::min(from_at + kPerMessage, s.log.size())));
}

void on_log_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    State& s = st();
    uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id |= uint64_t{r.u8()} << (8 * i);
    const uint32_t end = r.u32();
    const uint16_t n = r.u16();
    if (!r.ok() || !id || link::is_authority()) return;
    if (id != s.known_log) {
        if (s.known_log) NXLOG_I(kTag, "Co-op purchases: the authority's purchase log is another one; this player starts at its first entry");
        s.known_log = id;
        s.pending.clear();
    }
    s.known_end = std::max(s.known_end, end);
    for (uint16_t i = 0; i < n; ++i) {
        uint8_t raw[kEntryBytes];
        r.bytes(raw, kEntryBytes);
        Entry e;
        if (!r.ok()) return;
        if (get_entry(raw, &e)) s.pending[e.seq] = e;
    }
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!c || !link::in_session()) return;
    if (link::is_authority() && !s.known_log && records()) {   // the authority applies its own log as every member does
        load_log();
        // A log the authority's own save has no mark of is not this world's (the co-op save was made again in this folder): the
        // authority applies every entry, its own too, so a world that has seen the log carries the mark.
        if (!s.log.empty() && marked_log() != s.log_id) {
            NXLOG_W(kTag, "Co-op purchases: the purchase log file belongs to another world; a new log is started");
            new_log();
        }
        s.known_log = s.log_id;
        s.known_end = static_cast<uint32_t>(s.log.size());
        for (uint32_t seq = applied(s.log_id) + 1; seq <= s.log.size(); ++seq) s.pending[seq] = s.log[seq - 1];
    }
    tick_send();
    tick_ask();
    tick_apply(c, in_game);
}

void on_session_change() {
    State& s = st();
    s.ask = true;
    s.asked_at = -1e9;
    s.known_end = 0;
    s.pending.clear();
    s.known_log = 0;   // learnt again: from the authority's answer, or (the authority) from its own log at the next tick
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::purchases
