// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's party answers (pkp_coop_internal.h): the hooks behind the rows of pkp_coop_party_table.inc. The rule
// for what gets a row is at the top of that table; the facts are pkp_coop_party.cpp.
//
// A fact row replaces a leaf handler of a condition evaluator. The contents evaluator 0x12483c calls a leaf with x0 = the
// condition's u32 words {kind, negate, arguments...} and x8 = the u32 result; it applies the negate word itself after the leaf
// returns. So the leaf answers "some player has it", and a script's negated use reads "no player has it" with no work here.
// A leaf runs on every process for its own records, so in a session each process gives the same answer.
// An off row is a midhook after one `bl`: x0 holds the callee's answer and nothing has read it yet; the hook writes the answer that
// does not refuse. The instruction before each off row is checked the first time the row is reached (`bl` to its callee); a row
// that fails is left off for the run and logged once. The first changed answer of each row in a session is logged (tag + site).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_site_check.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::party {

namespace {

constexpr const char* kTag = "pkp";

enum class Fact : uint8_t { Follows, FollowerAbility, None };

struct Row {
    uint64_t site;     // main offset of the hooked instruction
    Fact fact;         // a fact row's fact; None: an off row
    uint64_t callee;   // an off row's callee (main offset)
    uint64_t answer;   // an off row's answer
    const char* tag;
};

// The rows as the table lists them, then one sentinel past every address.
constexpr Row kListed[] = {
#define PKP_PARTY_FACT(entry, fact, word, tag, what) {entry, Fact::fact, 0, 0, tag},
#define PKP_PARTY_OFF(ret, callee, answer, word, tag, what) {ret, Fact::None, callee, answer, tag},
#include "core/pkp_coop_party_table.inc"
#undef PKP_PARTY_OFF
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
    }
    return true;
}
static_assert(well_formed(), "pkp_coop_party_table.inc: a site or a callee is not an instruction address");

enum : uint8_t { kUnchecked = 0, kGood = 1, kBad = 2 };

struct State {
    std::atomic<uint8_t> checked[kRowCount + 1] = {};   // off rows: kUnchecked / kGood / kBad, once per run
    std::atomic<bool> logged[kRowCount + 1] = {};       // this session's first changed answer written
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

}  // namespace

int on_fact(::Ctx* c, uint64_t site) {
    if (!link::in_session()) return 0;
    const Row* r = find(site);
    if (!r || r->fact == Fact::None) return 0;
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

int on_off(::Ctx* c, uint64_t site) {
    if (!link::in_session()) return 0;
    const Row* r = find(site);
    if (!r || r->fact != Fact::None) return 0;
    State& s = st();
    const size_t i = static_cast<size_t>(r - kRows.data());
    uint8_t v = s.checked[i].load(std::memory_order_acquire);
    if (v == kUnchecked) {
        v = check(*r) ? kGood : kBad;
        s.checked[i].store(v, std::memory_order_release);
    }
    if (v != kGood) return 0;
    const uint64_t was = c->x[0];
    c->x[0] = r->answer;
    if (was != r->answer && first_log(*r))
        NXLOG_I(kTag, "Co-op party: %s at main+0x%llx does not refuse in a session (0x%llx, the game's 0x%llx)", r->tag,
                static_cast<unsigned long long>(r->site), static_cast<unsigned long long>(r->answer),
                static_cast<unsigned long long>(was));
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
