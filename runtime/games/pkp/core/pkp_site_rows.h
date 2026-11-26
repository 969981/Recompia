// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the one engine of a site-row table (co-op's role answers pkp_coop_role.cpp, co-op's stage answers pkp_coop_stage.cpp).
// A module lists its rows (one midhook per site, the main offset of the hooked instruction) in a .inc in any order, groups them as
// its readers need, and ends the array with one sentinel row past every address (~0), so a table with no row is still an array.
// Here: the rows sorted by site at compile time (insertion sort, so the table may group freely), the checks every such table makes
// (no site listed twice, every site an instruction address with one before it), the binary search a hook makes with its site, and
// the per-row marks a hook keeps at run time: its check, made the first time the row is reached (good / bad for the run), and its
// "first answer this session" log mark. The marks are atomics: a row can be reached on any thread that runs the game's code.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nxrt::games::pkp::site_rows {

// The rows of `in` (sentinel included, the last element) sorted by their uint64_t `site`.
template <class Row, size_t N>
constexpr std::array<Row, N> sorted(const Row (&in)[N]) {
    std::array<Row, N> a{};
    for (size_t i = 0; i < N; ++i) {
        size_t j = i;
        for (; j > 0 && a[j - 1].site > in[i].site; --j) a[j] = a[j - 1];
        a[j] = in[i];
    }
    return a;
}

// No site is listed twice (the sentinel included: no row may be at ~0).
template <class Row, size_t N>
constexpr bool unique(const std::array<Row, N>& a) {
    for (size_t i = 0; i + 1 < N; ++i)
        if (a[i].site == a[i + 1].site) return false;
    return true;
}

// Every row's site (the sentinel left out) is 4-aligned and has an instruction before it (a row's check reads site - 4).
template <class Row, size_t N>
constexpr bool aligned(const std::array<Row, N>& a) {
    for (size_t i = 0; i + 1 < N; ++i)
        if (a[i].site % 4 != 0 || a[i].site < 4) return false;
    return true;
}

// The row at site among the first n of a sorted array, or null.
template <class Row>
const Row* find(const Row* rows, size_t n, uint64_t site) {
    const Row* end = rows + n;
    const Row* r = std::lower_bound(rows, end, site, [](const Row& row, uint64_t a) { return row.site < a; });
    return r != end && r->site == site ? r : nullptr;
}

// The run-time marks of N rows.
template <size_t N>
class Marks {
public:
    // Row i's check passed: made once per run (check() the first time, its answer kept), so a row that fails stays off and its
    // check logs once.
    template <class Check>
    bool good(size_t i, Check&& check) {
        uint8_t v = checked_[i].load(std::memory_order_acquire);
        if (v == kUnchecked) {
            v = check() ? kGood : kBad;
            checked_[i].store(v, std::memory_order_release);
        }
        return v == kGood;
    }
    // True once per session for row i: the caller writes its first-answer line.
    bool first(size_t i) { return !logged_[i].exchange(true, std::memory_order_relaxed); }
    // The session closed: the next session logs each row's first answer again.
    void new_session() {
        for (auto& l : logged_) l.store(false, std::memory_order_relaxed);
    }
    void reset() {
        for (auto& v : checked_) v.store(kUnchecked, std::memory_order_relaxed);
        new_session();
    }

private:
    enum : uint8_t { kUnchecked = 0, kGood = 1, kBad = 2 };
    std::atomic<uint8_t> checked_[N] = {};
    std::atomic<bool> logged_[N] = {};
};

}  // namespace nxrt::games::pkp::site_rows
