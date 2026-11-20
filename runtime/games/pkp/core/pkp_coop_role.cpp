// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's role answers (pkp_coop_internal.h). A member is treated as the host for every permission: at the sites
// listed in pkp_coop_role_table.inc, a member in a session gets the host's answer to "am I the visitor / the host"; every other
// place, and every process outside a session or the authority, gets the game's own answer.
//
// The game asks in two ways (main 2.0.1a; GM = [main+0x3561710], its u8 +0x90 the session bits, bit 1 host, bit 2 visitor):
// - through its role getters, leaf functions of six instructions (0 when GM is null):
//     0x376c4 IsVisitor  w0 = GM+0x90 bit 2     0xa40bc IsHost  w0 = GM+0x90 bit 1
//   Each has a thousand or more call sites, many run every frame, so the getters are not hooked: a hook at a getter's entry would put
//   the hook engine's lookup on every call, in single player too. A getter row is a midhook of its own at the instruction after one
//   `bl getter`. There w0 holds the answer (the getter's `ubfx` zero-extends it into x0) and nothing has read it yet; the hook sets x0
//   to the host's answer (IsVisitor 0, IsHost 1). A tail call (`b getter`) returns to its caller's caller and can never be a row.
// - inline: `ldrb wN, [x8, #0x90]` then `tbz / tbnz wN, #bit` right after it. A session-bit row is a midhook at that test: it clears
//   the visitor bit (sets the host bit) in wN before the test reads it. Every row's wN is read by the test alone (the path the host
//   answer takes writes it, or calls out, before any other read), so nothing else in the function sees the changed byte.
// Each hook returns 0: the game's own code runs on with the answer. Only these sites change, so the cost is nil anywhere else.
//
// Each row is checked the first time it is reached: a getter row's instruction before it must be `bl` to its getter, a session-bit
// row's must be the `ldrb wN, [.., #0x90]` and the row itself `tbz / tbnz wN, #bit` (the word the site check reads is only the row's
// own). A row that fails is left off for the run and logged once. The first override of each row in a session is logged (tag + site),
// so testers' logs show which gates fired; the marks clear when the session closes.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::answers {

namespace {

constexpr const char* kTag = "pkp";

enum class Kind : uint8_t { IsVisitor, IsHost, SessionBit };

struct Row {
    uint64_t site;   // main offset of the hooked instruction
    Kind kind;
    uint8_t reg;     // SessionBit: the register holding GM+0x90
    uint8_t bit;     // SessionBit: the bit its test reads (1 host: set, 2 visitor: cleared)
    const char* tag;
};

// The rows as the table lists them, then one sentinel past every address (a table with no row is still an array).
constexpr Row kListed[] = {
#define PKP_ROLE_ROW(ret, getter, word, tag, what) {ret, Kind::getter, 0, 0, tag},
#define PKP_ROLE_BIT(site, reg, bit, word, tag, what) {site, Kind::SessionBit, reg, bit, tag},
#include "core/pkp_coop_role_table.inc"
#undef PKP_ROLE_BIT
#undef PKP_ROLE_ROW
    {~0ull, Kind::IsVisitor, 0, 0, nullptr},
};
constexpr size_t kRowCount = sizeof kListed / sizeof kListed[0] - 1;

// The same rows sorted by site, for the binary search (the table groups them by kind and system).
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
static_assert(unique(), "pkp_coop_role_table.inc: a site is listed twice");
constexpr bool well_formed() {
    for (size_t i = 0; i < kRowCount; ++i) {
        const Row& r = kRows[i];
        if (r.site % 4 != 0 || r.site < 4) return false;
        if (r.kind == Kind::SessionBit && (r.reg > 30 || (r.bit != 1 && r.bit != 2))) return false;
    }
    return true;
}
static_assert(well_formed(), "pkp_coop_role_table.inc: a site is not an instruction address, or a session-bit row's register or bit "
                             "is not one it can take (bit 1 host or bit 2 visitor)");

constexpr uint64_t getter_at(Kind k) { return k == Kind::IsHost ? layout::kIsHost : layout::kIsVisitor; }

// `bl` at main+at calls main+to.
bool is_bl_to(uint32_t word, uint64_t at, uint64_t to) {
    if ((word & 0xfc000000u) != 0x94000000u) return false;
    const int64_t imm = static_cast<int64_t>(static_cast<int32_t>(word << 6) >> 6) * 4;   // imm26, sign-extended, in bytes
    return at + static_cast<uint64_t>(imm) == to;
}
// `ldrb wReg, [xN, #0x90]` (unsigned offset, any base register).
bool is_session_load(uint32_t word, uint8_t reg) {
    constexpr uint32_t kLdrbImm = 0x39400000u, kOffset = static_cast<uint32_t>(layout::kStateSession) << 10;
    return (word & 0xffc0001fu) == (kLdrbImm | kOffset | reg);
}
// `tbz / tbnz wReg, #bit, ...`.
bool is_bit_test(uint32_t word, uint8_t reg, uint8_t bit) {
    return (word & 0xfe000000u) == 0x36000000u && ((word >> 19) & 0x1f) == bit && (word & 0x1f) == reg;
}

enum : uint8_t { kUnchecked = 0, kGood = 1, kBad = 2 };

struct State {
    std::atomic<uint8_t> checked[kRowCount + 1] = {};   // kUnchecked / kGood / kBad, once per run
    std::atomic<bool> logged[kRowCount + 1] = {};       // this session's first override written
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

bool check(const Row& r) {
    uint32_t before = 0, at = 0;
    const bool read = guest::read(base() + r.site - 4, before) && guest::read(base() + r.site, at);
    if (r.kind == Kind::SessionBit) {
        if (read && is_session_load(before, r.reg) && is_bit_test(at, r.reg, r.bit)) return true;
        NXLOG_W(kTag, "Co-op role: %s at main+0x%llx left off: %08x %08x is not ldrb w%u, [.., #0x90] then a test of its bit %u", r.tag,
                static_cast<unsigned long long>(r.site), before, at, r.reg, r.bit);
        return false;
    }
    if (read && is_bl_to(before, r.site - 4, getter_at(r.kind))) return true;
    NXLOG_W(kTag, "Co-op role: %s at main+0x%llx left off: main+0x%llx is %08x, not bl main+0x%llx", r.tag,
            static_cast<unsigned long long>(r.site), static_cast<unsigned long long>(r.site - 4), before,
            static_cast<unsigned long long>(getter_at(r.kind)));
    return false;
}

}  // namespace

int on_answer(::Ctx* c, uint64_t site) {
    if (!link::in_session() || link::is_authority()) return 0;
    const Row* r = find(site);
    if (!r) return 0;
    State& s = st();
    const size_t i = static_cast<size_t>(r - kRows.data());
    uint8_t v = s.checked[i].load(std::memory_order_acquire);
    if (v == kUnchecked) {
        v = check(*r) ? kGood : kBad;
        s.checked[i].store(v, std::memory_order_release);
    }
    if (v != kGood) return 0;
    uint64_t& reg = r->kind == Kind::SessionBit ? c->x[r->reg] : c->x[0];
    const uint64_t was = reg;
    if (r->kind == Kind::SessionBit) {
        const uint64_t mask = uint64_t{1} << r->bit;
        reg = r->bit == 1 ? reg | mask : reg & ~mask;   // bit 1 the host's: set; bit 2 the visitor's: cleared
    } else {
        reg = r->kind == Kind::IsHost ? 1 : 0;
    }
    if (!s.logged[i].exchange(true, std::memory_order_relaxed)) {
        const char* what = r->kind == Kind::IsHost ? "IsHost" : r->kind == Kind::IsVisitor ? "IsVisitor" : "the session bits";
        NXLOG_I(kTag, "Co-op role: %s at main+0x%llx answers as the host (%s 0x%llx, the game's 0x%llx)", r->tag,
                static_cast<unsigned long long>(r->site), what, static_cast<unsigned long long>(reg),
                static_cast<unsigned long long>(was));
    }
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

}  // namespace nxrt::games::pkp::coop::answers
