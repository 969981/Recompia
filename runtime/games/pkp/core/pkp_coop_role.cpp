// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's role answers (pkp_coop_internal.h). A member is treated as the host for every permission: at the call
// sites listed in pkp_coop_role_table.inc, a member in a session gets the host's answer from the game's role getters; every other
// caller, and every process outside a session or the authority, gets the game's own answer.
//
// The getters (main 2.0.1a, leaf functions of six instructions: GM = [main+0x3561710], 0 when GM is null):
//   0x376c4 IsVisitor  w0 = GM+0x90 bit 2     0xa40bc IsHost  w0 = GM+0x90 bit 1
// Each has a thousand or more call sites, many run every frame, so the getters are not hooked: a hook at a getter's entry would put
// the hook engine's lookup on every call, in single player too. Each table row is a midhook of its own at the instruction after one
// `bl getter`, which costs nothing anywhere else. There w0 holds the answer (the getter's `ubfx` zero-extends it into x0) and nothing
// has read it yet; the hook sets x0 to the host's answer (IsVisitor 0, IsHost 1) and returns 0, so the game's own code runs on with it.
// A tail call (`b getter`) returns to its caller's caller and can never be a row.
//
// Each row is checked the first time it is reached: the instruction before its site must be `bl` to its getter (else the row is
// left off for the run and logged once). The first override of each row in a session is logged (tag + site), so testers' logs show
// which gates fired; the marks clear when the session closes.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::answers {

namespace {

constexpr const char* kTag = "pkp";

enum class Getter : uint8_t { IsVisitor, IsHost };

struct Row {
    uint64_t ret;   // main offset of the instruction after the `bl`
    Getter getter;
    const char* tag;
};

// The rows, then one sentinel past every address (a table with no row is still an array).
constexpr Row kRows[] = {
#define PKP_ROLE_ROW(ret, getter, word, tag, what) {ret, Getter::getter, tag},
#include "core/pkp_coop_role_table.inc"
#undef PKP_ROLE_ROW
    {~0ull, Getter::IsVisitor, nullptr},
};
constexpr size_t kRowCount = sizeof kRows / sizeof kRows[0] - 1;

constexpr bool sorted_unique() {
    for (size_t i = 0; i + 1 < sizeof kRows / sizeof kRows[0]; ++i)
        if (kRows[i].ret >= kRows[i + 1].ret) return false;
    return true;
}
static_assert(sorted_unique(), "pkp_coop_role_table.inc: rows must be sorted by return address, with no address twice");
constexpr bool aligned() {
    for (size_t i = 0; i < kRowCount; ++i)
        if (kRows[i].ret % 4 != 0 || kRows[i].ret < 4) return false;
    return true;
}
static_assert(aligned(), "pkp_coop_role_table.inc: a return address is not an instruction address");

constexpr uint64_t getter_at(Getter g) { return g == Getter::IsHost ? layout::kIsHost : layout::kIsVisitor; }
constexpr uint64_t host_answer(Getter g) { return g == Getter::IsHost ? 1 : 0; }

// `bl` at main+at calls main+to.
bool is_bl_to(uint32_t word, uint64_t at, uint64_t to) {
    if ((word & 0xfc000000u) != 0x94000000u) return false;
    const int64_t imm = static_cast<int64_t>(static_cast<int32_t>(word << 6) >> 6) * 4;   // imm26, sign-extended, in bytes
    return at + static_cast<uint64_t>(imm) == to;
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
    const Row* end = kRows + kRowCount;
    const Row* r = std::lower_bound(kRows, end, site, [](const Row& row, uint64_t a) { return row.ret < a; });
    return r != end && r->ret == site ? r : nullptr;
}

bool check(const Row& r) {
    const uint64_t at = base() + r.ret - 4;
    uint32_t word = 0;
    if (guest::read(at, word) && is_bl_to(word, r.ret - 4, getter_at(r.getter))) return true;
    NXLOG_W(kTag, "Co-op role: %s at main+0x%llx left off: main+0x%llx is %08x, not bl main+0x%llx", r.tag,
            static_cast<unsigned long long>(r.ret), static_cast<unsigned long long>(r.ret - 4), word,
            static_cast<unsigned long long>(getter_at(r.getter)));
    return false;
}

}  // namespace

int on_answer(::Ctx* c, uint64_t site) {
    if (!link::in_session() || link::is_authority()) return 0;
    const Row* r = find(site);
    if (!r) return 0;
    State& s = st();
    const size_t i = static_cast<size_t>(r - kRows);
    uint8_t v = s.checked[i].load(std::memory_order_acquire);
    if (v == kUnchecked) {
        v = check(*r) ? kGood : kBad;
        s.checked[i].store(v, std::memory_order_release);
    }
    if (v != kGood) return 0;
    const uint64_t want = host_answer(r->getter);
    if (!s.logged[i].exchange(true, std::memory_order_relaxed))
        NXLOG_I(kTag, "Co-op role: %s at main+0x%llx answers as the host (%s %llu, the game's %llu)", r->tag,
                static_cast<unsigned long long>(r->ret), r->getter == Getter::IsHost ? "IsHost" : "IsVisitor",
                static_cast<unsigned long long>(want), static_cast<unsigned long long>(c->x[0] & 1));
    c->x[0] = want;
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
