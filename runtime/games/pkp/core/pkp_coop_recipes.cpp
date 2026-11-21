// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's shared crafting recipes (pkp_coop_internal.h). The game keeps the recipes a player knows in that
// player's own save, and some are given once per world (a story talk, an event's callback, a scene): only the player who was there
// learned it, and the others could never craft it. Each process shares the recipes its player knows (RecipeKnown), and every member
// learns every recipe any member knows (a union: recipes are knowledge, and a member that is behind never takes one away).
//
// The game, 2.0.1 (modding-docs coop-v2/RECIPES-RE.md):
//   save root + 0x6cfdec + id * 6    the record of recipe id 1..0x5db (m_recipeHistory, CMNDAT, saved): +2 u16 the learn order, +4
//                                    u8 flags. Bit 1 (kRecipeKnown) is "learned"; only the learn below sets it. The other bits are
//                                    the player's own marks (2 at the learn, 3 by the craft list, 4 by a setter, 5 a toggle of the
//                                    craft screen) and are never shared. Every reader reads the process's own record (no host
//                                    mirror), so a member's learn lands in its own save.
//   kLearn(player, id, quiet, item, w4, w5, w6, w7)
//                                    the one learn every source calls (quest effect 3, script 0xbcc, the event callbacks, item
//                                    history, the shop, ...): returns at once when bit 1 is set (and shows "already known" unless
//                                    quiet), else sets it, recounts the order, marks the item, and unless quiet queues the
//                                    game's "learned a recipe" event for the player's slot (player + 0x5a39). The player is the
//                                    player manager's +0x30 character (local_player()), as 0x3bcb38 passes it.
// Rules on a receiver: another member's known recipe is learned here through kLearn, at a safe moment (in the group's world, can
// walk, talks to no one), kSettle seconds after it arrived and only if bit 1 is still clear: the game's own path may teach it here
// meanwhile (quest effect 3 runs on every process, through the story's catch-up of the same step; an item give through the catch-up's
// items), and it must win, since its learn on a known id shows "already known" instead of its scene. A few at a time are taught with the game's own notice, one per safe moment (the notice is an event, so the next
// one waits for it to close); a larger batch (a join's whole list) is taught quietly, kApplyPerTick a frame, with one line in the
// message log when it is done.
// Each process reads bit 1 of every record each frame and sends the ids that became known (one that could not be sent goes at the
// next frame), and its whole known list every kAllEvery seconds and when it or another member joins, through the authority.
#include "core/pkp_coop_internal.h"
#include "core/pkp_strings.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <bitset>
#include <cstdio>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::recipes {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kLearn = 0x2e7240;     // (player, id, quiet, item, w4, w5, w6, w7): learn a recipe; 1 if it was not known
constexpr uint32_t kIdMax = layout::kRecipeMax;
constexpr double kAllEvery = 30.0;        // the whole list again (a member that missed a change)
constexpr unsigned kLoudMax = 2;          // at most this many waiting: each with the game's own notice
constexpr unsigned kApplyPerTick = 16;    // quiet learns in one frame, at most (each recounts the whole record table)
constexpr double kSettle = 8.0;           // seconds a received id waits for the game's own learn of the same step here

struct State {
    std::bitset<kIdMax + 1> seen;      // per id: known at the last read (and sent, once baseline_sent)
    std::bitset<kIdMax + 1> pending;   // per id: another member knows it, not taught here yet
    uint8_t origin[kIdMax + 1] = {};   // per pending id: the member that sent it
    double at[kIdMax + 1] = {};        // per pending id: when it arrived
    bool have_seen = false;            // the first read of a session only takes the baseline
    bool baseline_sent = false;        // the session's first list went out (until then a send is the baseline: no change lines)
    bool send_all = true;              // the whole list at the next read
    double all_at = 0.0;
    unsigned quiet_count = 0;          // recipes taught quietly in the current batch (its line goes when it is done)
    int quiet_origin = -1;             // the node of the batch's first recipe
};
State& st() {
    static State s;
    return s;
}

uint64_t records() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return r && guest::ok(r + layout::kSaveRecipes, (kIdMax + 1) * layout::kRecipeSize) ? r + layout::kSaveRecipes : 0;
}
bool known(uint64_t recs, uint32_t id) {
    return guest::rd<uint8_t>(recs + id * layout::kRecipeSize + layout::kRecipeFlags) & layout::kRecipeKnown;
}

bool shared_here() { return (session_bits() & layout::kSessionOnline) && link::in_session(); }

// This player is in the group's world (the host, or a member once the visitor bit is set), can walk and talks to no one.
bool safe_now(bool in_game) {
    const bool in_world = role() == CoopRole::Host || (session_bits() & layout::kSessionVisitor);
    return in_world && session::can_walk(in_game) && !talk::talking();
}

// Sends (u16 count, u16 id...) for every id that became known since the last read, or (all) every known id. An id is marked seen
// once it is sent; false: the send failed, and the same ids go at the next read.
bool share(uint64_t recs, bool all) {
    State& s = st();
    std::vector<uint16_t> ids;
    for (uint32_t id = 1; id <= kIdMax; ++id) {
        if (!known(recs, id)) {
            s.seen.reset(id);   // a reloaded save may have lost it: learnt again when it comes back
            continue;
        }
        if (all || !s.have_seen || !s.seen.test(id)) ids.push_back(static_cast<uint16_t>(id));
    }
    s.have_seen = true;
    if (ids.empty()) return true;
    wire::Writer w(link::self());
    w.u16(static_cast<uint16_t>(ids.size()));
    for (uint16_t id : ids) w.u16(id);
    if (!link::share(wire::Type::RecipeKnown, w.take())) return false;
    for (uint16_t id : ids) {
        if (s.baseline_sent && !s.seen.test(id)) NXLOG_I(kTag, "Co-op recipes: recipe %u learned here, shared", id);
        s.seen.set(id);
    }
    s.baseline_sent = true;
    return true;
}

// The batch's line, once nothing more waits.
void batch_line() {
    State& s = st();
    if (!s.quiet_count) return;
    notice::post(strings::fmt(base(), s.quiet_count == 1 ? strings::Str::CoopRecipesOne : strings::Str::CoopRecipesMany,
                              notice::name_of(static_cast<uint8_t>(s.quiet_origin)), s.quiet_count));
    s.quiet_count = 0;
    s.quiet_origin = -1;
}

// Teaches the waiting recipes that settled: one with the game's notice when only a few wait, else up to kApplyPerTick quietly.
void apply(::Ctx* c, uint64_t recs) {
    State& s = st();
    const uint64_t player = local_player();
    if (!player) return;
    if (s.pending.none()) {
        batch_line();
        return;
    }
    const double now = qpc::seconds();
    size_t waiting = 0;
    for (uint32_t id = 1; id <= kIdMax; ++id) waiting += s.pending.test(id) && now - s.at[id] >= kSettle;
    if (!waiting) return;
    const bool loud = waiting <= kLoudMax && !s.quiet_count;
    unsigned taught = 0;
    for (uint32_t id = 1; id <= kIdMax && taught < (loud ? 1u : kApplyPerTick); ++id) {
        if (!s.pending.test(id) || now - s.at[id] < kSettle) continue;
        s.pending.reset(id);
        if (known(recs, id)) continue;   // the game's own path taught it meanwhile: a loud learn would show "already known"
        ++taught;
        const uint64_t learned = guest::call_n<8>(c, base() + kLearn, {player, id, loud ? 0u : 1u, 0, 0, 0, 0, 0});
        if (s.have_seen) s.seen.set(id);   // ours now: not sent back as a change
        NXLOG_I(kTag, "Co-op recipes: recipe %u from node %u learned here%s%s", id, s.origin[id], loud ? " (the game's notice)" : "",
                learned & 1 ? "" : ", the game says it was known");
        if (!loud) {
            if (s.quiet_origin < 0) s.quiet_origin = s.origin[id];
            ++s.quiet_count;
        }
    }
    if (s.pending.none()) batch_line();
}

}  // namespace

void on_known_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint16_t n = r.u16();
    if (!r.ok() || !shared_here()) return;
    const uint64_t recs = records();
    if (!recs) return;
    for (uint16_t k = 0; k < n; ++k) {
        const uint16_t id = r.u16();
        if (!r.ok()) return;
        if (id < 1 || id > kIdMax || known(recs, id) || s.pending.test(id)) continue;
        s.pending.set(id);
        s.origin[id] = origin;
        s.at[id] = qpc::seconds();
    }
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    const uint64_t recs = records();
    if (!recs || !shared_here()) return;
    if (c && safe_now(in_game)) apply(c, recs);
    const double now = qpc::seconds();
    const bool all = s.send_all || now - s.all_at >= kAllEvery;
    if (!share(recs, all) || !all) return;
    s.send_all = false;
    s.all_at = now;
}

void on_session_change() {
    State& s = st();
    s.seen.reset();
    s.pending.reset();
    s.have_seen = false;
    s.baseline_sent = false;
    s.send_all = true;
    s.quiet_count = 0;
    s.quiet_origin = -1;
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::recipes

// The digests' reader (pkp_coop_digest.cpp): the recipes the save knows now.
namespace nxrt::games::pkp::coop::digest {

bool recipes(Entries& out) {
    const uint64_t recs = coop::recipes::records();
    if (!recs) return false;
    for (uint32_t id = 1; id <= coop::recipes::kIdMax; ++id)
        if (coop::recipes::known(recs, id)) out.push_back({id, 1});
    return true;
}

}  // namespace nxrt::games::pkp::coop::digest
