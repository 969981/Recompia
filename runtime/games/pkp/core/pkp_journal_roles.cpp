// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's roles watcher (pkp_journal_watch.h): the session bits and the host / visitor branches the game takes.
//
// The session bits are polled, never hooked at their getters (0x376c4 visitor, 0xa40bc host, 0x207b0 online: hundreds of call edges,
// many per frame): GM = [main+0x3561710], u8 GM+0x90 (bit 0 online, bit 1 host, bit 2 visitor, bit 3 unknown, 0xb62e4's) and the
// u32 mask GM+0x8c..0x8f beside it. A co-op host reads 0x03, every member 0x05 (each member is a Link Play visitor). Every j.role
// line prints that raw byte (#785: a named bool hid which bit a test read).
// The branch sites (main 2.0.1a, entry hooks unless noted). An entry hook cannot see the branch, so "out" is the path the function's
// own visitor test takes for the byte read at entry; the site's argument registers are printed raw:
//   0xe889ac (mid, `tbnz w0`)   shop buy: w0 = IsVisitor (0xe889a8 `bl 0x376c4`); set skips the bought bit 0x5bdc5c (SHOP-STOCK-RE)
//   0x62f7e0                   area-visit writer: returns at once on a visitor (UNLOCK-LISTS-RE)
//   0x9bdf50 (w0 chara)        house arrival: refused on a visitor (PR #796)
//   0x2c2480 / 0x2c2e80 / 0x659588   create NPC / spawn character / party NPC: a visitor sends a request message and returns
//   0x62b3e0 / 0x62b72c / 0x62be5c   general progress types 3 / 9 / 0x2d: return on a visitor before counting (ROLE-GATES-RE)
//   0x1355ec (x0 GM+0xd8, x1 &actor id)   contents feed: skipped on a visitor
//   0xe87260 (w0 shop entry)   "offline-only shop row": hidden whenever online; co-op's OfflineGoods hook (after this one) shows it
//   0x11435c (x0 &u16 handle)  unique-request bind gate: 0x286de4 && !visitor && !Cloud Island; co-op's GiverCheck makes it true
// Rates: all are events (a buy, an area load, an actor init), but an actor init or a shop list build can come in bursts: each line is
// deduped on (site, args, byte); a repeat only counts, and the count is written when the key changes or after kRepeatGap.
#include <cinttypes>
#include <mutex>
#include <unordered_map>

#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
#include "core/pkp_journal_watch.h"
#include "core/pkp_layout.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::journal {

namespace {

constexpr uint64_t kMask = 0x8c;          // game state: u32 mask, beside the session byte (layout::kStateSession = 0x90)
constexpr uint8_t kSessionBit3 = 0x08;    // read by 0xb62e4, meaning not known
constexpr double kRepeatGap = 30.0;       // a deduped key writes its repeat count at most this often

struct Roles {
    std::mutex m;
    bool seen = false;
    uint8_t bits = 0;
    uint32_t mask = 0;
    struct Rep {
        uint32_t count = 0;
        double since = 0.0;
    };
    std::unordered_map<uint64_t, Rep> reps;   // dedupe key -> repeats not written yet
};
Roles& rs() {
    static Roles r;
    return r;
}

uint64_t game_state() { return guest::deref(base() + layout::kGameState); }

uint8_t session_byte() {
    const uint64_t gm = game_state();
    uint8_t b = 0;
    return gm && guest::read(gm + layout::kStateSession, b) ? b : 0;
}

std::string decode(uint8_t b) {
    if (!b) return "offline";
    std::string s;
    auto add = [&](uint8_t bit, const char* n) {
        if (!(b & bit)) return;
        if (!s.empty()) s += "+";
        s += n;
    };
    add(layout::kSessionOnline, "online");
    add(layout::kSessionHost, "host");
    add(layout::kSessionVisitor, "visitor");
    add(kSessionBit3, "bit3");
    if (b & 0xf0) s += fmt("+0x%02x", b & 0xf0);
    return s;
}

uint64_t caller(::Ctx* c) { return c->x[30] >= base() ? c->x[30] - base() : c->x[30]; }

// True when the line for key is due: a new key, or one whose repeats are kRepeatGap old (*repeats: the count to print).
bool due(uint64_t key, uint32_t* repeats) {
    Roles& r = rs();
    std::lock_guard<std::mutex> lk(r.m);
    const double now = qpc::seconds();
    auto [it, fresh] = r.reps.try_emplace(key, Roles::Rep{0, now});
    *repeats = 0;
    if (fresh) return true;
    ++it->second.count;
    if (now - it->second.since < kRepeatGap) return false;
    *repeats = it->second.count;
    it->second = Roles::Rep{0, now};
    return true;
}

uint64_t key_of(uint64_t site, uint64_t a, uint64_t b, uint8_t bits) {
    return (site << 40) ^ (a << 20) ^ b ^ (static_cast<uint64_t>(bits) << 56);
}

// One visitor-gated branch at the entry of its function: "out" from the byte the function's own test reads.
void branch(::Ctx* c, uint64_t site, const char* what, const char* args, uint64_t a, uint64_t b) {
    const uint8_t bits = session_byte();
    uint32_t repeats = 0;
    if (!due(key_of(site, a, b, bits), &repeats)) return;
    const bool visitor = bits & layout::kSessionVisitor;
    Rec rec(Area::Role, what);
    rec.in("%s role=0x%02x (%s) from 0x%" PRIx64, args, bits, decode(bits).c_str(), caller(c))
        .out("%s", visitor ? "visitor path (skipped)" : "host path");
    if (visitor) rec.expect("host path: the authority does it for the member (co-op)");
    if (repeats) rec.note("x%u more since the last line", repeats);
}

void on_shop_buy(::Ctx* c) {   // mid 0xe889ac: w0 = IsVisitor's answer
    const bool visitor = c->x[0] & 1;
    const uint8_t bits = session_byte();
    uint32_t repeats = 0;
    if (!due(key_of(0xe889ac, visitor, 0, bits), &repeats)) return;
    Rec rec(Area::Role, "shop buy bought-bit");
    rec.in("IsVisitor w0=%u role=0x%02x (%s)", visitor ? 1u : 0u, bits, decode(bits).c_str())
        .out("%s", visitor ? "visitor path (bought bit 0x5bdc5c skipped)" : "host path (bought bit set)");
    if (repeats) rec.note("x%u more since the last line", repeats);
}

void on_area_visit(::Ctx* c) {
    const uint64_t gm = game_state();
    uint8_t stage = 0;
    if (gm) guest::read(gm + layout::kStateStage, stage);
    branch(c, 0x62f7e0, "area-visit write", fmt("stage=%u", stage).c_str(), stage, 0);
}
void on_house(::Ctx* c) {
    const uint32_t chara = static_cast<uint32_t>(c->x[0]);
    branch(c, 0x9bdf50, "house arrival", fmt("chara=0x%x", chara).c_str(), chara, 0);
}
void npc(::Ctx* c, uint64_t site, const char* what) {
    branch(c, site, what, fmt("x0=0x%" PRIx64 " w1=0x%x w2=0x%x", c->x[0], static_cast<uint32_t>(c->x[1]),
                                    static_cast<uint32_t>(c->x[2])).c_str(),
           static_cast<uint32_t>(c->x[1]), static_cast<uint32_t>(c->x[2]));
}
void on_create_npc(::Ctx* c) { npc(c, 0x2c2480, "create NPC"); }
void on_spawn_chara(::Ctx* c) { npc(c, 0x2c2e80, "spawn character"); }
void on_party_npc(::Ctx* c) { npc(c, 0x659588, "party NPC"); }
void progress(::Ctx* c, uint64_t site, const char* what) {
    branch(c, site, what, fmt("x0=0x%" PRIx64 " w1=0x%x", c->x[0], static_cast<uint32_t>(c->x[1])).c_str(), caller(c), 0);
}
void on_progress3(::Ctx* c) { progress(c, 0x62b3e0, "general progress type 3"); }
void on_progress9(::Ctx* c) { progress(c, 0x62b72c, "general progress type 9"); }
void on_progress2d(::Ctx* c) { progress(c, 0x62be5c, "general progress type 0x2d"); }
void on_contents_feed(::Ctx* c) {
    uint16_t actor = 0;
    guest::read(c->x[1], actor);
    branch(c, 0x1355ec, "contents feed", fmt("actor=0x%x", actor).c_str(), actor, 0);
}

void on_offline_row(::Ctx* c) {   // entry 0xe87260: co-op's OfflineGoods runs after this
    const uint32_t entry = static_cast<uint32_t>(c->x[0]);
    const uint8_t bits = session_byte();
    uint32_t repeats = 0;
    if (!due(key_of(0xe87260, entry, 0, bits), &repeats)) return;
    const bool online = bits & layout::kSessionOnline;
    Rec rec(Area::Role, "shop offline-only row test");
    rec.in("entry=%u role=0x%02x (%s) from 0x%" PRIx64, entry, bits, decode(bits).c_str(), caller(c))
        .out("%s", online ? "online: the game hides a row with #70 +0x23 bit 2" : "offline: shown");
    if (online) rec.expect("shown (co-op OfflineGoods, #780)");
    if (repeats) rec.note("x%u more since the last line", repeats);
}

void on_bind_gate(::Ctx* c) {   // entry 0x11435c: co-op's GiverCheck runs after this
    uint16_t handle = 0;
    guest::read(c->x[0], handle);
    const uint8_t bits = session_byte();
    uint32_t repeats = 0;
    if (!due(key_of(0x11435c, handle, 0, bits), &repeats)) return;
    const bool visitor = bits & layout::kSessionVisitor;
    Rec rec(Area::Role, "unique bind gate");
    rec.in("handle=0x%x role=0x%02x (%s) from 0x%" PRIx64, handle, bits, decode(bits).c_str(), caller(c))
        .out("%s", visitor ? "game: false (visitor)" : "game: 0x286de4 && not Cloud Island");
    if (visitor) rec.expect("true (co-op GiverCheck)");
    if (repeats) rec.note("x%u more since the last line", repeats);
}

constexpr WatchSite kSites[] = {
    {{0xe889ac, 0x37000060u, false}, &on_shop_buy, "shop buy bought-bit 0xe889ac"},   // tbnz w0, #0, 0xe889b8
    {{0x62f7e0, kPacibsp, true}, &on_area_visit, "area-visit writer 0x62f7e0"},
    {{0x9bdf50, 0x7110641fu, true}, &on_house, "house arrival 0x9bdf50"},              // cmp w0, #0x419
    {{0x2c2480, kPacibsp, true}, &on_create_npc, "create NPC 0x2c2480"},
    {{0x2c2e80, kPacibsp, true}, &on_spawn_chara, "spawn character 0x2c2e80"},
    {{0x659588, kPacibsp, true}, &on_party_npc, "party NPC 0x659588"},
    {{0x62b3e0, kPacibsp, true}, &on_progress3, "general progress 3 0x62b3e0"},
    {{0x62b72c, kPacibsp, true}, &on_progress9, "general progress 9 0x62b72c"},
    {{0x62be5c, kPacibsp, true}, &on_progress2d, "general progress 0x2d 0x62be5c"},
    {{0x1355ec, kPacibsp, true}, &on_contents_feed, "contents feed 0x1355ec"},
    {{0xe87260, kPacibsp, true}, &on_offline_row, "offline-only shop row 0xe87260"},
    {{0x11435c, kPacibsp, true}, &on_bind_gate, "unique bind gate 0x11435c"},
};

// Once a frame: one byte and one word compared; a line only on change.
void poll(::Ctx*, bool) {
    const uint64_t gm = game_state();
    uint8_t bits = 0;
    uint32_t mask = 0;
    if (!gm || !guest::read(gm + layout::kStateSession, bits) || !guest::read(gm + kMask, mask)) return;
    Roles& r = rs();
    uint8_t was_bits;
    uint32_t was_mask;
    bool was_seen;
    {
        std::lock_guard<std::mutex> lk(r.m);
        if (r.seen && r.bits == bits && r.mask == mask) return;
        was_seen = r.seen;
        was_bits = r.bits;
        was_mask = r.mask;
        r.seen = true;
        r.bits = bits;
        r.mask = mask;
    }
    Rec rec(Area::Role, "session bits");
    if (was_seen) rec.in("was 0x%02x (%s) mask %08x", was_bits, decode(was_bits).c_str(), was_mask);
    rec.out("GM+0x90=0x%02x (%s) mask GM+0x8c=%08x", bits, decode(bits).c_str(), mask);
}

void reset() {
    Roles& r = rs();
    std::lock_guard<std::mutex> lk(r.m);
    r.seen = false;
    r.bits = 0;
    r.mask = 0;
    r.reps.clear();
}

}  // namespace

const Watcher kRolesWatcher = {"roles", kSites, sizeof kSites / sizeof kSites[0], &poll, &reset};

}  // namespace nxrt::games::pkp::journal
