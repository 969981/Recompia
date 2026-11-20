// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, friendship with the group's creatures (pkp_coop_internal.h). A creature's friendship is the group's: it
// lives in the creature's 0x180-byte record in the authority's world (layout::kFriend*), and a member's play adds to it there.
//
// The game, 2.0.1:
//   0x3a89fc(comp, s16 amount, w2 flag)  every friendship gain (comp = the creature's component, u16 +8 its actor handle, u32 +0xc its
//                     chara): points = 0x31fb00(comp) + amount x a comfort factor, then 0x3bb660(comp, points, flag). Returns at once for
//                     amount < 1. Its callers (calls.tsv, all bl), each with the record byte it tests before the gain and sets after it
//                     (0x3b7e48 / 0x3b7708: the record this process selects; a member: its copy):
//                       0xcc258, 0xd5ec0, 0xdca78, 0xde1ac, 0x499018, script callback slot 174 0x4aae54   +0x177 (return addresses
//                       0xcc388, 0xd6050, 0xdcb40, 0xde294, 0x4992a4, 0x4ab17c)
//                       the gift 0x46e308 (after its +0x178 / +0x175 tests)                                  +0x175 (0x46e7c4)
//                       0x3a83a0 (0x3a8954 reads +0x17d, 0x3a8d44(comp, 1) sets it)                         +0x17d (0x3a8714)
//                       the talk event step 0x481e2c (event runner state 5; online non-host: +0x178 set)     none   (0x48224c)
//                       the request completion 0x619410 (two calls)                                          none
//   0x3bb660(comp, points, flag)  the store: clamps to the row's cap (+0x26), returns when unchanged, and on a visitor outside Cloud
//                     Island (0x376c4 true, 0x77ecb0 false: 0x3bb838..0x3bb844) returns before any store. Else +0x2c = points; on a
//                     level-up (0x3380bc before / after) the meter 0x3bc034(comp, tuning +0x218) and, with flag, 0x3e8dec marks per local
//                     player. The host marks the actor dirty only on Cloud Island (0x3bb93c), so no replication carries +0x2c elsewhere;
//                     GameReplication kind 0xb's reader 0x57e3d4 takes only +0x8..+0x1a of the record.
//   The game's own forward to the host is Cloud Island's only: there 0x3bb660 / 0x3bc034 also stash the value in the actor (+0x20
//   object +0x1ac / +0x1b0, the only writers 0x3bb874 / 0x3bc1b0), the talk-end walk 0x480b60 copies both into its entries and the
//   host's apply 0x2197a0 runs 0x3bb660 / 0x3bc034 with them (0x21ba5c, 0x21ba8c; 0 skips). The host's apply also re-runs a member's
//   gift 0x46e308 (0x21c484), but with w3 = 0x77ecb0's answer, which skips its friendship (0x46e610) outside Cloud Island.
// So outside Cloud Island a member's gains change nothing anywhere: the player report "friendship grows only for the host".
//
// Here, a member in the session, outside Cloud Island:
//   - a gain from one of the callers above (entry hook 0x3a89fc, by its return address) is sent to the authority (FriendAsk kind 1:
//     handle, chara, amount, the caller's record byte). The member's own call still runs (it stores nothing). Not sent: the request
//     completion 0x619410 (pkp_coop_outcomes.cpp applies a unique request's friendship once for the group; a general one is run on
//     the host by the game, 0x2197a0 -> 0x62e0a4 -> 0x6218e0, likely; a request bound to one player and 0x118b04's call stay as the
//     game has them), and the talk event step's gain when this player holds no talk lock (an event this player did not start by a talk:
//     kept out until it is known whether another process runs the same event);
//   - script callback slot 175 (0x4ab268) sets +0x17a in the member's copy only (0x4ab37c..0x4ab3c0): sent likewise (kind 2).
// The authority finds the creature (0x333f10 on the handle: its component, or the static stand-in main+0x3567db0 when the actor is not
// loaded here, which 0x3bb660 would dereference unchecked at 0x3bb7a4: dropped), and when the caller's byte is still clear in its own
// record makes the game's own gain 0x3a89fc(comp, amount, 0) (w2 = 0 as the game's apply 0x21ba5c passes) and sets the byte, as the
// caller does for its own player. A set byte means the group already had that once-gain: dropped, so a member's stale copy never counts
// it twice. Kind 2 sets the byte in the authority's own record, as the game's host write does.
// What the members see: the authority sends the records' friendship fields (FriendState: +0x2c, +0x2e, +0x175, +0x177, +0x17a,
// +0x17d) to every member when they change (polled every kScanEvery seconds) and all of them to a member that joins; a member writes
// them into its copy of the host's records (R + kFriendHostCopy), which its game reads for the creature (kind 0xb's reader 0x57e3d4
// does not carry these fields; what the join copies into the records was not read). The name tag's friendship test 0x3b7bd8 (caller 0xead130) answers false at once on a visitor outside Cloud
// Island (0x3b7cd0) and reads the points from the own records (0x3b7d44, no visitor select): a member in the session takes the host's
// path there with the copy's points instead (midhook 0x3b7cd0).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstring>
#include <deque>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::friendship {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kComponentOf = 0x333f10;   // (&u16 handle) -> the creature's component, or main + kNoComponent
constexpr uint64_t kNoComponent = 0x3567db0;
constexpr uint64_t kRecordOf = 0x3b7708;      // (comp) -> its 0x180 record as this process selects it
constexpr uint64_t kAddPoints = 0x3a89fc;     // (comp, s16 amount, w2 flag)
constexpr uint64_t kStage11 = 0x77ecb0;       // () -> bit 0: Cloud Island
constexpr uint64_t kCompHandle = 8, kCompChara = 0xc;

constexpr uint8_t kAskGain = 1, kAskFlag = 2;   // FriendAsk kinds
constexpr size_t kQueueMax = 32;
constexpr double kScanEvery = 1.0;

// The callers whose gains are sent (main + the return address of their bl 0x3a89fc) and the record byte each tests and sets.
struct Caller {
    uint64_t ret;
    uint16_t gate;   // 0: none
    bool talk;       // only while this player holds a talk
};
constexpr Caller kCallers[] = {
    {0xcc388, layout::kFriendDaily, false},  {0xd6050, layout::kFriendDaily, false}, {0xdcb40, layout::kFriendDaily, false},
    {0xde294, layout::kFriendDaily, false},  {0x4992a4, layout::kFriendDaily, false}, {0x4ab17c, layout::kFriendDaily, false},
    {0x46e7c4, layout::kFriendGift, false},  {0x3a8714, layout::kFriendOnce17d, false}, {0x48224c, 0, true},
};

bool gate_ok(uint16_t gate) {
    return gate == 0 || gate == layout::kFriendGift || gate == layout::kFriendDaily || gate == layout::kFriendOnce17d;
}

struct Ask {
    uint8_t kind;
    uint16_t handle;
    uint32_t chara;
    int16_t amount;
    uint16_t offset;   // kind 1: the caller's gate (0: none); kind 2: the flag's byte
};

// The friendship fields of one record, as FriendState carries them.
struct Fields {
    uint16_t points = 0, meter = 0;
    uint8_t flag[4] = {};
    bool operator==(const Fields& o) const { return points == o.points && meter == o.meter && std::memcmp(flag, o.flag, 4) == 0; }
};
constexpr uint64_t kFlagAt[4] = {layout::kFriendGift, layout::kFriendDaily, layout::kFriendFlag17a, layout::kFriendOnce17d};

struct State {
    std::deque<Ask> asks;                // a member: gains and flags not sent yet, in order
    std::vector<uint64_t> unknown;       // return addresses of callers not forwarded, logged once each
    bool have_snapshot = false;          // the authority: the fields last sent
    Fields snapshot[layout::kFriendCount];
    double next_scan = 0;
    alignas(8) uint16_t handle_arg = 0;  // 0x333f10's argument (guest addresses are host addresses)
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() { return guest::rd<uint64_t>(base() + layout::kSaveRoot); }

uint64_t record(uint64_t table, uint32_t chara) {
    const uint64_t r = save_root();
    const uint64_t at = r + table + static_cast<uint64_t>(chara) * layout::kFriendSize;
    return r && chara < layout::kFriendCount && guest::ok(at, layout::kFriendSize) ? at : 0;
}

bool member_here() {
    return role() == CoopRole::Visit && (session_bits() & layout::kSessionVisitor) && link::in_session() && !link::is_authority();
}

bool authority_here() { return role() == CoopRole::Host && link::is_authority(); }

void queue(const Ask& a) {
    State& s = st();
    if (s.asks.size() >= kQueueMax) {
        NXLOG_W(kTag, "Co-op friendship: %zu asks wait for the channel; the oldest (chara %u) is dropped", s.asks.size(), s.asks.front().chara);
        s.asks.pop_front();
    }
    s.asks.push_back(a);
}

Fields read_fields(uint64_t rec) {
    Fields f;
    f.points = guest::rd<uint16_t>(rec + layout::kFriendPoints);
    f.meter = guest::rd<uint16_t>(rec + layout::kFriendMeter);
    for (size_t k = 0; k < 4; ++k) f.flag[k] = guest::rd<uint8_t>(rec + kFlagAt[k]);
    return f;
}

void put(wire::Writer& w, uint32_t chara, const Fields& f) {
    w.u16(static_cast<uint16_t>(chara)).u16(f.points).u16(f.meter);
    for (uint8_t b : f.flag) w.u8(b);
}

// The authority: FriendState with every record whose fields are not all 0 (node >= 0: to that node), or with the records that
// changed since the last scan (node < 0: to every member).
void send_state(int node) {
    State& s = st();
    std::vector<uint32_t> charas;
    std::vector<Fields> fields;
    for (uint32_t chara = 0; chara < layout::kFriendCount; ++chara) {
        const uint64_t rec = record(layout::kFriendOwn, chara);
        if (!rec) return;
        const Fields f = read_fields(rec);
        const bool send = node >= 0 ? !(f == Fields{}) : !(f == s.snapshot[chara]);
        if (node < 0) s.snapshot[chara] = f;
        if (!send) continue;
        charas.push_back(chara);
        fields.push_back(f);
    }
    if (charas.empty()) return;
    wire::Writer w(link::self());
    w.u16(static_cast<uint16_t>(charas.size()));
    for (size_t i = 0; i < charas.size(); ++i) put(w, charas[i], fields[i]);
    if (node >= 0) link::to_node(node, wire::Type::FriendState, w.take());
    else link::to_members(wire::Type::FriendState, w.take());
}

void apply_gain(::Ctx* c, uint8_t origin, const Ask& a) {
    State& s = st();
    s.handle_arg = a.handle;
    const uint64_t comp = guest::call(c, base() + kComponentOf, reinterpret_cast<uint64_t>(&s.handle_arg));
    if (!comp || comp == base() + kNoComponent || !guest::ok(comp, kCompChara + 4) || guest::rd<uint32_t>(comp + kCompChara) != a.chara) {
        NXLOG_I(kTag, "Co-op friendship: node %u's gain of %d with chara %u (actor %04x) dropped: the creature is not loaded here", origin,
                a.amount, a.chara, a.handle);
        return;
    }
    const uint64_t rec = guest::call(c, base() + kRecordOf, comp);
    if (!guest::ok(rec, layout::kFriendSize)) return;
    if (a.offset && guest::rd<uint8_t>(rec + a.offset)) {
        NXLOG_I(kTag, "Co-op friendship: node %u's gain of %d with chara %u dropped: the group had that once-gain (+0x%x) already",
                origin, a.amount, a.chara, a.offset);
        return;
    }
    const uint16_t before = guest::rd<uint16_t>(rec + layout::kFriendPoints);
    guest::call(c, base() + kAddPoints, comp, static_cast<uint64_t>(static_cast<int64_t>(a.amount)), 0);
    if (a.offset) guest::wr<uint8_t>(rec + a.offset, 1);
    NXLOG_I(kTag, "Co-op friendship: node %u's player gained %d with chara %u (once-gate +0x%x): points %u -> %u", origin, a.amount,
            a.chara, a.offset, before, guest::rd<uint16_t>(rec + layout::kFriendPoints));
}

}  // namespace

int on_add_points(::Ctx* c) {
    if (!member_here()) return 0;
    const int16_t amount = static_cast<int16_t>(c->x[1]);
    const uint64_t comp = c->x[0];
    if (amount < 1 || comp == base() + kNoComponent || !guest::ok(comp, kCompChara + 4)) return 0;   // the game's own early return
    const uint32_t chara = guest::rd<uint32_t>(comp + kCompChara);
    if (chara >= layout::kFriendCount) return 0;
    if (guest::call(c, base() + kStage11, 0) & 1) return 0;   // Cloud Island: the game forwards it itself
    State& s = st();
    const uint64_t from = c->x[30] - base();
    const Caller* caller = nullptr;
    for (const Caller& k : kCallers)
        if (k.ret == from) caller = &k;
    if (!caller) {
        bool seen = false;
        for (uint64_t u : s.unknown) seen = seen || u == from;
        if (!seen && s.unknown.size() < 16) {
            s.unknown.push_back(from);
            NXLOG_I(kTag, "Co-op friendship: a gain of %d with chara %u from main+0x%llx is not sent (not a forwarded caller)",
                    amount, chara, static_cast<unsigned long long>(from));
        }
        return 0;
    }
    if (caller->talk && !talk::talking()) {
        NXLOG_I(kTag, "Co-op friendship: the talk event's gain of %d with chara %u is not sent: this player holds no talk", amount, chara);
        return 0;
    }
    queue({kAskGain, guest::rd<uint16_t>(comp + kCompHandle), chara, amount, caller->gate});
    NXLOG_I(kTag, "Co-op friendship: a gain of %d with chara %u (from main+0x%llx) is the group's: sent to the authority", amount, chara,
            static_cast<unsigned long long>(from));
    return 0;   // the member's own call runs too (it stores nothing here)
}

int on_flag_17a(::Ctx* c) {
    // Midhook 0x4ab358 in 0x4ab268: `tbz w0, #0` after 0x77ecb0 (w0 bit 0 = Cloud Island), w19 = the chara (0x4ab350).
    if (!member_here() || (c->x[0] & 1)) return 0;
    const uint32_t chara = static_cast<uint32_t>(c->x[19]);
    if (chara >= layout::kFriendCount) return 0;
    queue({kAskFlag, 0, chara, 0, static_cast<uint16_t>(layout::kFriendFlag17a)});
    NXLOG_I(kTag, "Co-op friendship: chara %u's flag +0x%x set here: sent to the authority", chara,
            static_cast<unsigned>(layout::kFriendFlag17a));
    return 0;
}

int on_name_tag(::Ctx* c) {
    // Midhook 0x3b7cd0 in 0x3b7bd8: `tbz w0, #0, 0x3b7d78` after 0x77ecb0 (reached on a visitor only; w0 bit 0 = Cloud Island, clear
    // returns false). x22 = the save root, read once at 0x3b7d44 as x22 + (chara + 4) * 0x180 + 0x14c (= the own record's +0x2c).
    if (!member_here() || (c->x[0] & 1)) return 0;
    c->x[0] = 1;
    c->x[22] += layout::kFriendHostCopy - layout::kFriendOwn;
    return 0;
}

void on_ask_message(::Ctx* c, int, uint8_t origin, wire::Reader& r) {
    Ask a{};
    a.kind = r.u8();
    a.handle = r.u16();
    a.chara = r.u32();
    a.amount = static_cast<int16_t>(r.u16());
    a.offset = r.u16();
    if (!r.ok() || !c || !authority_here() || a.chara >= layout::kFriendCount) return;
    if (a.kind == kAskGain) {
        if (a.amount < 1 || !gate_ok(a.offset)) return;
        apply_gain(c, origin, a);
    } else if (a.kind == kAskFlag && a.offset == layout::kFriendFlag17a) {
        const uint64_t rec = record(layout::kFriendOwn, a.chara);
        if (!rec) return;
        guest::wr<uint8_t>(rec + a.offset, 1);
        NXLOG_I(kTag, "Co-op friendship: node %u set chara %u's flag +0x%x in the group's record", origin, a.chara, a.offset);
    }
}

void on_state_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    const uint16_t n = r.u16();
    if (!r.ok() || link::is_authority()) return;
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t chara = r.u16();
        Fields f;
        f.points = r.u16();
        f.meter = r.u16();
        for (uint8_t& b : f.flag) b = r.u8();
        if (!r.ok()) return;
        const uint64_t rec = record(layout::kFriendHostCopy, chara);
        if (!rec) continue;
        guest::wr<uint16_t>(rec + layout::kFriendPoints, f.points);
        guest::wr<uint16_t>(rec + layout::kFriendMeter, f.meter);
        for (size_t k = 0; k < 4; ++k) guest::wr<uint8_t>(rec + kFlagAt[k], f.flag[k]);
    }
}

void on_member_joined(int node) {
    if (authority_here()) send_state(node);
}

void tick(::Ctx*) {
    State& s = st();
    if (member_here()) {
        while (!s.asks.empty()) {
            const Ask& a = s.asks.front();
            if (!link::to_authority(wire::Type::FriendAsk, wire::Writer(link::self())
                                                             .u8(a.kind)
                                                             .u16(a.handle)
                                                             .u32(a.chara)
                                                             .u16(static_cast<uint16_t>(a.amount))
                                                             .u16(a.offset)
                                                             .take()))
                break;   // kept for the next frame, in order
            s.asks.pop_front();
        }
        return;
    }
    if (!authority_here() || !save_root()) return;
    const double now = qpc::seconds();
    if (now < s.next_scan) return;
    s.next_scan = now + kScanEvery;
    if (!s.have_snapshot) {   // the first scan only takes the fields: a member gets all of them when it joins
        for (uint32_t chara = 0; chara < layout::kFriendCount; ++chara) {
            const uint64_t rec = record(layout::kFriendOwn, chara);
            if (!rec) return;
            s.snapshot[chara] = read_fields(rec);
        }
        s.have_snapshot = true;
        return;
    }
    send_state(-1);
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::friendship
