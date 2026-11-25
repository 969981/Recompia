// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's party answers (pkp_coop_internal.h): the hooks behind the rows of pkp_coop_party_table.inc. The rule
// for what gets a row is at the top of that table; the facts are pkp_coop_party.cpp.
//
// A fact row replaces a leaf handler of a condition evaluator. The contents evaluator 0x12483c calls a leaf with x0 = the
// condition's u32 words {kind, negate, arguments...} and x8 = the u32 result; it applies the negate word itself after the leaf
// returns. So the leaf answers "some player has it", and a script's negated use reads "no player has it" with no work here.
// A leaf runs on every process for its own records, so in a session each process gives the same answer.
// An off row is a midhook after one `bl`: x0 holds the callee's answer and nothing has read it yet; the hook writes the answer that
// does not refuse. A count row is a midhook after the `bl` that counted an item in this player's bag: x0 holds that count, the
// item is on the handler's stack and the wanted number and the compare are in registers. The hook leaves x0, or writes the count
// of another player (pkp_coop_party_bags.cpp) that the game's own compare then reads:
//   at least / more than n, at most / fewer than n   the largest count any one player holds. Counts are never added up across
//                                                    bags: nothing could take a pooled amount from one bag. "At most" and "fewer"
//                                                    are the absence tests: they pass only when they pass for every player
//   exactly n                                        this player's own when it is n, else a teammate's that is n
// A predicate row is an entry hook on a function many places call: on a member the fact answers it (x0 = 1, the function does
// not run), except for the callers listed with the fact below, which keep the game's own answer.
// The instruction before each off or count row is checked the first time the row is reached (`bl` to its callee); a row
// that fails is left off for the run and logged once. The first changed answer of each row in a session is logged (tag + site).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_site_check.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::party {

namespace {

constexpr const char* kTag = "pkp";

enum class Fact : uint8_t { Follows, FollowerAbility, Owns, None };

// 0x284ef4's callers (main + the return address of their call) that keep the game's answer on a member:
//   0x3bb744  the friendship store 0x3bb660: "own" goes on to 0x3bb7a4 `bl 0x281360` and loads through its answer with no null
//             check (an actor that is not loaded), and stores nothing on a visitor anyway (0x3bb838)
//   0x3bc118  the meter store 0x3bc034: no effect either way (it returns at 0x3bc170 on a visitor)
//   0x3b8ee8  0x3b88b4's second site and
//   0x3b9500  0x3b91c0: steps of the talk-end walk 0x481e2c that the authority applies itself from the talk-end record
//   0x23f2c8  the per-frame actor scan 0x23f000, which queues events on the local runner
// The tail-call wrapper 0x23fb4c (callers 0x1bfec0, 0x29d910) is never reached on a member in a session.
constexpr uint64_t kOwnsKept[] = {0x23f2c8, 0x3b8ee8, 0x3b9500, 0x3bb744, 0x3bc118};

constexpr uint8_t kAnyKind = 0xff;   // a count row's kind register: none, the row answers whatever reached the site
constexpr uint8_t kAtLeast = 0xff;   // a count row's compare register: none, the site compares "at least n"

struct Row {
    uint64_t site;     // main offset of the hooked instruction
    Fact fact;         // a fact row's fact; None: an off or a count row
    uint64_t callee;   // an off or count row's callee (main offset)
    uint64_t answer;   // an off row's answer
    const char* tag;
    bool count = false;    // a count row
    uint8_t item_sp = 0;   // sp + : the u32 whose low 13 bits are the item id
    uint8_t n_reg = 0;     // the register holding the wanted number
    uint8_t op_reg = 0;    // the register holding the compare (0 more than, 1 at least, 2 exactly, 3 at most, 4 fewer), or kAtLeast
    uint8_t kind_reg = kAnyKind;   // a count row for some kinds only: the register holding the kind (0..63), or kAnyKind
    uint64_t kinds = 0;            // and a bit per kind the row answers
};

// The rows as the table lists them, then one sentinel past every address.
constexpr Row kListed[] = {
#define PKP_PARTY_FACT(entry, fact, word, tag, what) {entry, Fact::fact, 0, 0, tag},
#define PKP_PARTY_PRED(entry, fact, word, tag, what) {entry, Fact::fact, 0, 0, tag},
#define PKP_PARTY_OFF(ret, callee, answer, word, tag, what) {ret, Fact::None, callee, answer, tag},
#define PKP_PARTY_COUNT(ret, callee, item_sp, n_reg, op_reg, word, tag, what) {ret, Fact::None, callee, 0, tag, true, item_sp, n_reg, op_reg},
#define PKP_PARTY_COUNT_OF(ret, callee, item_sp, kind_reg, kinds, word, tag, what) \
    {ret, Fact::None, callee, 0, tag, true, item_sp, 0, kAtLeast, kind_reg, kinds},
#include "core/pkp_coop_party_table.inc"
#undef PKP_PARTY_COUNT_OF
#undef PKP_PARTY_COUNT
#undef PKP_PARTY_OFF
#undef PKP_PARTY_PRED
#undef PKP_PARTY_FACT
    {~0ull, Fact::None, 0, 0, nullptr},
};
constexpr size_t kRowCount = sizeof kListed / sizeof kListed[0] - 1;

constexpr std::array<Row, kRowCount + 1> sorted_rows() {
    std::array<Row, kRowCount + 1> a{};
    for (size_t i = 0; i <= kRowCount; ++i) {
        size_t j = i;
        for (; j > 0 && a[j - 1].site > kListed[i].site; --j) a[j] = a[j - 1];
        a[j] = kListed[i];
    }
    return a;
}
constexpr std::array<Row, kRowCount + 1> kRows = sorted_rows();

constexpr bool unique() {
    for (size_t i = 0; i < kRowCount; ++i)
        if (kRows[i].site == kRows[i + 1].site) return false;
    return true;
}
static_assert(unique(), "pkp_coop_party_table.inc: a site is listed twice");
constexpr bool well_formed() {
    for (size_t i = 0; i < kRowCount; ++i) {
        const Row& r = kRows[i];
        if (r.site % 4 != 0 || r.site < 4) return false;
        if (r.fact == Fact::None && (r.callee % 4 != 0 || r.callee == 0)) return false;
        if (r.count && (r.n_reg > 30 || (r.op_reg > 30 && r.op_reg != kAtLeast))) return false;
    }
    return true;
}
static_assert(well_formed(), "pkp_coop_party_table.inc: a site or a callee is not an instruction address");

enum : uint8_t { kUnchecked = 0, kGood = 1, kBad = 2 };

struct State {
    std::atomic<uint8_t> checked[kRowCount + 1] = {};   // off rows: kUnchecked / kGood / kBad, once per run
    std::atomic<bool> logged[kRowCount + 1] = {};       // this session's first changed answer written
    std::atomic<uint8_t> satisfier[kRowCount + 1] = {}; // a count row: the node whose bag last gave the answer (wire::kNobody: none)
    bool was_open = false;                              // game thread (tick)
};
State& st() {
    static State s;
    return s;
}

const Row* find(uint64_t site) {
    const Row* end = kRows.data() + kRowCount;
    const Row* r = std::lower_bound(kRows.data(), end, site, [](const Row& row, uint64_t a) { return row.site < a; });
    return r != end && r->site == site ? r : nullptr;
}

bool first_log(const Row& r) { return !st().logged[static_cast<size_t>(&r - kRows.data())].exchange(true, std::memory_order_relaxed); }

bool check(const Row& r) {
    uint32_t before = 0;
    if (guest::read(base() + r.site - 4, before) && is_bl_to(before, r.site - 4, r.callee)) return true;
    NXLOG_W(kTag, "Co-op party: %s at main+0x%llx left off: main+0x%llx is %08x, not bl main+0x%llx", r.tag,
            static_cast<unsigned long long>(r.site), static_cast<unsigned long long>(r.site - 4), before,
            static_cast<unsigned long long>(r.callee));
    return false;
}

constexpr uint64_t kCondArg = 8;   // a condition's words: + this, its first argument (u32)
constexpr uint32_t kItemId = 0x1fff;   // an item word's id
enum : uint32_t { kMore = 0, kLeast = 1, kExactly = 2, kMost = 3, kFewer = 4 };   // the compare of contents kinds 0x7e4 / 0x7da

// The row is good to answer: its callee checked once per run.
bool good(const Row& r) {
    State& s = st();
    const size_t i = static_cast<size_t>(&r - kRows.data());
    uint8_t v = s.checked[i].load(std::memory_order_acquire);
    if (v == kUnchecked) {
        v = check(r) ? kGood : kBad;
        s.checked[i].store(v, std::memory_order_release);
    }
    return v == kGood;
}

}  // namespace

int on_fact(::Ctx* c, uint64_t site) {
    if (!link::in_session()) return 0;
    const Row* r = find(site);
    if (!r || r->fact == Fact::None || r->fact == Fact::Owns) return 0;
    const uint64_t cond = c->x[0], out = c->x[8];
    if (!guest::ok(cond + kCondArg, 4) || !guest::ok(out, 4)) return 0;   // not the evaluator's call: the game's handler
    const uint32_t arg = guest::rd<uint32_t>(cond + kCondArg);
    bool own = false;
    const bool yes = r->fact == Fact::Follows ? follows(c, arg, &own) : follower_ability(c, arg, &own);
    guest::wr<uint32_t>(out, yes ? 1u : 0u);
    if (yes && !own && first_log(*r))
        NXLOG_I(kTag, "Co-op party: %s at main+0x%llx answers for the party (argument %u: true of a teammate, not of this player)",
                r->tag, static_cast<unsigned long long>(r->site), arg);
    return 1;   // the handler's whole job is done
}

int on_pred(::Ctx* c, uint64_t site) {
    // Many calls a frame: the cheap tests first.
    if (!quests::member_rules()) return 0;
    const Row* r = find(site);
    if (!r || r->fact != Fact::Owns) return 0;
    const uint64_t from = c->x[30] - base();
    if (std::find(std::begin(kOwnsKept), std::end(kOwnsKept), from) != std::end(kOwnsKept)) return 0;
    if (!owns(c, c->x[0])) return 0;
    c->x[0] = 1;
    if (first_log(*r))
        NXLOG_I(kTag, "Co-op party: %s at main+0x%llx answers for the party (the creature has parents; asked from main+0x%llx)", r->tag,
                static_cast<unsigned long long>(r->site), static_cast<unsigned long long>(from));
    return 1;   // the function's whole job is done
}

int on_off(::Ctx* c, uint64_t site) {
    if (!link::in_session()) return 0;
    const Row* r = find(site);
    if (!r || r->fact != Fact::None || r->count || !good(*r)) return 0;
    const uint64_t was = c->x[0];
    c->x[0] = r->answer;
    if (was != r->answer && first_log(*r))
        NXLOG_I(kTag, "Co-op party: %s at main+0x%llx does not refuse in a session (0x%llx, the game's 0x%llx)", r->tag,
                static_cast<unsigned long long>(r->site), static_cast<unsigned long long>(r->answer),
                static_cast<unsigned long long>(was));
    return 0;
}

int on_count(::Ctx* c, uint64_t site) {
    if (!link::in_session()) return 0;
    const Row* r = find(site);
    if (!r || !r->count || !good(*r)) return 0;
    if (r->kind_reg != kAnyKind && !(r->kinds >> (c->x[r->kind_reg] & 0x3f) & 1)) return 0;
    uint32_t word = 0;
    if (!guest::read(c->x[31] + r->item_sp, word)) return 0;
    const uint32_t id = word & kItemId, own = static_cast<uint32_t>(c->x[0]) & 0xffff;
    const uint32_t n = static_cast<uint32_t>(c->x[r->n_reg]);
    const uint32_t op = r->op_reg == kAtLeast ? static_cast<uint32_t>(kLeast) : static_cast<uint32_t>(c->x[r->op_reg]);
    State& s = st();
    const size_t i = static_cast<size_t>(r - kRows.data());
    uint8_t node = wire::kNobody;
    uint32_t pick = own;
    if (op == kExactly) {
        if (own != n && teammate_holds(id, n, &node)) pick = n;
    } else if (op <= kFewer) {
        const uint32_t most = teammates_most(id, &node);
        if (most > own) pick = most;
        else node = wire::kNobody;
    }
    s.satisfier[i].store(pick != own ? node : static_cast<uint8_t>(wire::kNobody), std::memory_order_relaxed);
    if (pick == own) return 0;
    c->x[0] = pick;
    if (first_log(*r))
        NXLOG_I(kTag, "Co-op party: %s at main+0x%llx counts item %u in the party's bags (node %u holds %u, this player %u; wanted %u, "
                "compare %u)", r->tag, static_cast<unsigned long long>(r->site), id, node, pick, own, n, op);
    return 0;
}

void tick() {
    State& s = st();
    const bool open = link::in_session();
    if (s.was_open && !open)
        for (auto& l : s.logged) l.store(false, std::memory_order_relaxed);
    s.was_open = open;
}

void reset() {
    State& s = st();
    for (auto& v : s.checked) v.store(kUnchecked, std::memory_order_relaxed);
    for (auto& l : s.logged) l.store(false, std::memory_order_relaxed);
    s.was_open = false;
}

}  // namespace nxrt::games::pkp::coop::party
