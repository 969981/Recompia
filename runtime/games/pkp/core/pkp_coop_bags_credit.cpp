// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's bag records, a gain credited at its source (pkp_coop_bags_internal.h; the rule is "write-backs and
// gains" in the header of pkp_coop_bags.cpp): the bracket a module that adds to this player's bag puts around its add, and how a
// credit goes into a record.
#include "core/pkp_coop_bags_internal.h"
#include "core/pkp_guest.h"

#include <algorithm>
#include <vector>

#include "nxrt/log.h"

namespace nxrt::games::pkp::coop::bags {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kWordMask = 0x7fffffff;   // a payload's packed word (bits 0..30); two stacks merge when theirs are equal

}  // namespace

uint64_t word_of(uint64_t p) { return p & kWordMask; }

// The game's max stack (one value for every item, a u8 in the slot).
uint32_t max_stack(::Ctx* c) {
    const uint32_t m = static_cast<uint32_t>(guest::call(c, base() + layout::kMaxStack, 0) & 0xffff);
    return m ? std::min<uint32_t>(m, 0xff) : 0xff;
}

// Puts a credit into a record by the header's order; false when it fits nowhere.
bool place(Record& r, const Credit& g, uint32_t max) {
    for (uint64_t& p : r.slot)
        if (items::id_of(p) && word_of(p) == g.word && items::count_of(p) + g.n <= max) {
            p = items::with_count(p, items::count_of(p) + g.n);
            return true;
        }
    const uint64_t fresh = items::with_count(g.word, g.n);
    if (g.slot < kSlots && !items::id_of(r.slot[g.slot])) return r.slot[g.slot] = fresh, true;
    size_t last = 0;   // one past the last slot in use in the bag's own 120 (kind 4's ten are a separate sub-area)
    for (size_t i = 0; i < layout::kBagSlots; ++i)
        if (items::id_of(r.slot[i])) last = i + 1;
    for (size_t i = 0; i < last; ++i)
        if (!items::id_of(r.slot[i])) return r.slot[i] = fresh, true;
    return false;
}


namespace {

// The gains from `pre` to `post` (both this bag, around a source's add), per slot; a loss or an unchanged slot gives none.
std::vector<Credit> gains_of(const Record& pre, const Record& post) {
    std::vector<Credit> out;
    for (size_t i = 0; i < kSlots; ++i) {
        const uint64_t a = pre.slot[i], b = post.slot[i];
        if (a == b || !items::id_of(b)) continue;
        const bool same = items::id_of(a) && word_of(a) == word_of(b);
        if (same && items::count_of(b) <= items::count_of(a)) continue;
        out.push_back({word_of(b), items::count_of(b) - (same ? items::count_of(a) : 0), i});
    }
    return out;
}

}  // namespace

void gain_begin(::Ctx* c) {
    State& s = st();
    s.gain_open = false;
    if (!c || !s.active) return;
    const Selection sel = select(c);
    size_t bad_slot = kSlots;
    if (!(link::is_authority() || sel.visitor) || !ready(sel, &bad_slot)) return;
    read_bag(sel.bag, &s.gain_pre);
    s.gain_bag = sel.bag;
    s.gain_open = true;
}

void gain_end(::Ctx* c) {
    State& s = st();
    if (!s.gain_open) return;
    s.gain_open = false;
    Kept& o = s.own;
    if (!c || !s.active || !o.rec.seq) return;   // no record yet: the first one is the bag as it is, this gain with it
    const Selection sel = select(c);
    size_t bad_slot = kSlots;
    if (sel.bag != s.gain_bag || !ready(sel, &bad_slot)) return;   // another bag now: its apply writes the record, the take follows
    Record post;
    read_bag(sel.bag, &post);
    const std::vector<Credit> gains = gains_of(s.gain_pre, post);
    if (gains.empty()) return;
    const uint32_t max = max_stack(c);
    uint32_t lost = 0, items_in = 0;
    for (const Credit& g : gains) {
        const uint64_t was = s.gain_pre.slot[g.slot];   // the same slot only when it held nothing or this word (a loss is never credited)
        if (o.rec.slot[g.slot] == was && (!items::id_of(was) || word_of(was) == g.word)) o.rec.slot[g.slot] = post.slot[g.slot];
        else if (!place(o.rec, g, max)) {
            lost += g.n;
            continue;
        }
        items_in += g.n;
        ++s.credited;
        if (!s.answered) s.early.push_back(g);
    }
    if (s.answered) {
        ++o.rec.seq;
        s.send = !link::is_authority();
    }
    o.dirty = true;
    const bool pending = s.apply || s.restore || !s.answered || screen_open() || (!link::is_authority() && linkqueue::unanswered());
    if (pending || lost)
        NXLOG_I(kTag, "Co-op bags: a gain of %u items is credited to record %llu at its source (a write-back is pending or the bag is "
                      "held); %u items fit nowhere in the record", items_in, static_cast<unsigned long long>(o.rec.seq), lost);
}

}  // namespace nxrt::games::pkp::coop::bags
