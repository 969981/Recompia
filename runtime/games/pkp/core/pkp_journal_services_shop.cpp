// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's look at the shop (pkp_journal_services_internal.h; RE: modding-docs coop-v2/SHOP-STOCK-RE.md, #780 / #782).
// Stock per shop entry (1..0x7ff) lives in an 8-byte purchase record (+1 bit 1 bought, +2 u8 stock left) at R+0x6ae708+entry*8, on Cloud
// Island at R+0xf9357c+entry*8. The shop manager M (x0 of the getter and setter) holds a mirror u16 at M+entry*0x20+0x548e. Which of them the
// stock getter / setter use depends on the visitor bit and the stage:
//   entry 0x646864(M, entry)         get: not a visitor -> the own record; visitor off Cloud Island -> the guest purchase history
//                                    R+0x6ae700 keyed by the island owner's user id; visitor on Cloud Island -> the mirror
//   entry 0x646ae8(M, entry, n)      set: not a visitor, or on Cloud Island -> the own record; visitor off it -> the guest history
//   entry 0xe889f8(slot, entry, count, flag)  the grant after a buy (the shop's 0xe88868 tail-calls it; so does the host's answer to a
//                                    visitor's buy through 0x226280), after the stock was set and, off a visitor, the bought bit
//   entry 0xe87260(entry)            "this row is for offline play only" (row +0x23 bit 2), asked per row by the list builders
// The getter and the row test run once per row of every list build: they are written once per key and counted (Seen), the counts go
// out at most every 30 s.
#include "core/pkp_journal_services_internal.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::journal::services {

namespace {

constexpr uint64_t kPurchaseOwn = 0x6ae708, kPurchaseCloud = 0xf9357c, kPurchaseSize = 8;   // save root +
constexpr uint64_t kBought = 1, kStock = 2;                                                 // purchase record: u8 flags (bit 1 bought), u8 stock
constexpr uint64_t kMirror = 0x548e, kMirrorSize = 0x20;                                    // M +: u16 per entry
constexpr uint64_t kOwnerIndex = 0xb3380, kSelfIndex = 0xb3381;                             // **(main + kOwnMemberIndexRef) +: u8 the island owner's member index / this process's
constexpr uint32_t kEntryMax = 0x7ff;
constexpr double kFlushEvery = 30.0;

enum class Path : uint8_t { Own, OwnCloud, Guest, Mirror };
const char* path_name(Path p) {
    switch (p) {
        case Path::Own: return "own record R+0x6ae708";
        case Path::OwnCloud: return "own record, Cloud Island R+0xf9357c";
        case Path::Guest: return "guest history keyed by the island owner";
        case Path::Mirror: return "mirror M+entry*0x20+0x548e";
    }
    return "?";
}

Path get_path(uint8_t sess, bool cloud) { return is_visitor(sess) ? (cloud ? Path::Mirror : Path::Guest) : (cloud ? Path::OwnCloud : Path::Own); }
Path set_path(uint8_t sess, bool cloud) { return !is_visitor(sess) || cloud ? (cloud ? Path::OwnCloud : Path::Own) : Path::Guest; }

Seen g_stock, g_rows;
double g_flushed = 0;

// The island owner's member index as text: "own", "m<i>" or "none" (an index above 3 reads the null id).
std::string owner_text() {
    const uint64_t nm = guest::deref(guest::deref(base() + layout::kOwnMemberIndexRef));
    if (!nm || !guest::ok(nm + kOwnerIndex, 2)) return "?";
    const uint8_t owner = guest::rd<uint8_t>(nm + kOwnerIndex), self = guest::rd<uint8_t>(nm + kSelfIndex);
    if (static_cast<uint8_t>(owner + 1) > 4) return journal::fmt("none(index %u)", owner);
    return owner == self ? journal::fmt("own(index %u)", owner) : journal::fmt("m%u(index %u, this process is %u)", owner, owner, self);
}

// The stock the path reads, or -1 when it is a guest history (only readable through the game's own lookup).
int stock_of(Path p, uint32_t entry, uint64_t shop) {
    if (p == Path::Guest) return -1;
    if (p == Path::Mirror) return shop && guest::ok(shop + entry * kMirrorSize + kMirror, 2) ? guest::rd<uint16_t>(shop + entry * kMirrorSize + kMirror) : -1;
    const uint64_t r = save_root();
    const uint64_t rec = r + (p == Path::OwnCloud ? kPurchaseCloud : kPurchaseOwn) + entry * kPurchaseSize;
    return r && guest::ok(rec, kPurchaseSize) ? guest::rd<uint8_t>(rec + kStock) : -1;
}

uint64_t key_of(bool set, Path p, uint32_t entry, uint32_t value) {
    return static_cast<uint64_t>(set) << 63 | static_cast<uint64_t>(p) << 48 | static_cast<uint64_t>(entry & 0xffff) << 16 | (value & 0xffff);
}
std::string key_text(uint64_t k) {
    return journal::fmt("%s entry=%u path=%s value=%u", k >> 63 ? "set" : "get", static_cast<unsigned>(k >> 16 & 0xffff),
                        path_name(static_cast<Path>(k >> 48 & 0xff)), static_cast<unsigned>(k & 0xffff));
}

}  // namespace

void on_stock_get(::Ctx* c) {
    const uint32_t entry = static_cast<uint32_t>(c->x[1]) & 0xffff;
    const uint8_t sess = session();
    const Path p = get_path(sess, stage() == kCloudStage);
    const int stock = stock_of(p, entry, c->x[0]);
    if (!g_stock.hit(key_of(false, p, entry, static_cast<uint32_t>(stock)))) return;
    journal::Rec r(journal::Area::Service, "shop-stock-get");
    r.in("entry=%u sess=%s stage=%u owner=%s from=%s", entry, session_text(sess).c_str(), stage(), owner_text().c_str(), lr_text(c).c_str());
    if (stock >= 0) r.out("path=%s stock=%d", path_name(p), stock);
    else r.out("path=%s stock=? (read by the game's own lookup)", path_name(p));
    if (entry - 1u > kEntryMax - 1u) r.note("entry outside 1..0x7ff: the game returns 0");
}

void on_stock_set(::Ctx* c) {
    const uint32_t entry = static_cast<uint32_t>(c->x[1]) & 0xffff, n = static_cast<uint32_t>(c->x[2]);
    const uint8_t sess = session();
    const Path p = set_path(sess, stage() == kCloudStage);
    const int before = stock_of(p, entry, c->x[0]);
    if (!g_stock.hit(key_of(true, p, entry, n))) return;
    journal::Rec r(journal::Area::Service, "shop-stock-set");
    r.in("entry=%u stock=%u sess=%s stage=%u owner=%s from=%s", entry, n, session_text(sess).c_str(), stage(), owner_text().c_str(), lr_text(c).c_str());
    if (before >= 0) r.out("path=%s, was %d", path_name(p), before);
    else r.out("path=%s, was unreadable here (guest history)", path_name(p));
}

void on_shop_grant(::Ctx* c) {
    const uint32_t entry = static_cast<uint32_t>(c->x[1]) & 0xffff;
    const uint8_t sess = session();
    const bool cloud = stage() == kCloudStage;
    journal::Rec r(journal::Area::Service, "shop-grant");
    r.in("slot=%u entry=%u count=%u flag=%u sess=%s stage=%u from=%s", static_cast<unsigned>(c->x[0] & 0xffffffff), entry, static_cast<unsigned>(c->x[2] & 0xffff),
         static_cast<unsigned>(c->x[3] & 1), session_text(sess).c_str(), stage(), lr_text(c).c_str());
    const uint64_t root = save_root();
    const uint64_t rec = root + (cloud ? kPurchaseCloud : kPurchaseOwn) + entry * kPurchaseSize;
    if (root && entry - 1u < kEntryMax && guest::ok(rec, kPurchaseSize))
        r.out("own record: %s (bought bit %d, stock %u)", journal::hex_bytes(reinterpret_cast<const void*>(rec), kPurchaseSize).c_str(),
              guest::rd<uint8_t>(rec + kBought) >> 1 & 1, guest::rd<uint8_t>(rec + kStock));
    if (is_visitor(sess)) r.note("a visitor's buy never sets the bought bit (0xe889a8); the stock went to the guest history or the mirror");
}

void on_offline_row(::Ctx* c) {
    const uint32_t entry = static_cast<uint32_t>(c->x[0]) & 0xffff;
    const uint8_t sess = session();
    const uint64_t off = c->x[30] - base();
    if (!g_rows.hit(static_cast<uint64_t>(sess) << 56 | (off & 0xffffff) << 16 | entry)) return;
    journal::Rec r(journal::Area::Service, "shop-row-offline-only");
    r.in("entry=%u sess=%s from=%s", entry, session_text(sess).c_str(), lr_text(c).c_str());
    r.note("the list builders hide the row when its +0x23 bit 2 is set and sess bit 0 (online) is set: %s",
           sess & layout::kSessionOnline ? "online now" : "offline now");
}

void shop_frame(::Ctx*, bool) {
    const double now = qpc::seconds();
    if (now - g_flushed < kFlushEvery) return;
    g_flushed = now;
    const std::vector<std::string> stock = g_stock.grown(&key_text);
    if (!stock.empty()) journal::list(journal::Area::Service, "shop-stock-repeats", stock);
    const std::vector<std::string> rows = g_rows.grown([](uint64_t k) {
        return journal::fmt("entry=%u from=main+0x%llx sess=0x%02x", static_cast<unsigned>(k & 0xffff), static_cast<unsigned long long>(k >> 16 & 0xffffff), static_cast<unsigned>(k >> 56));
    });
    if (!rows.empty()) journal::list(journal::Area::Service, "shop-row-repeats", rows);
}

void shop_reset() {
    g_stock.clear();
    g_rows.clear();
    g_flushed = 0;
}

}  // namespace nxrt::games::pkp::journal::services
