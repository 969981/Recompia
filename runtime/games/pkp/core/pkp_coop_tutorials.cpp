// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's shared tips (pkp_coop_internal.h). A tip that waits for an action (a pop-up that stays until the player
// has done something) closes for every player once any player has done it, and a tip one player has seen is seen by all.
//
// The game, 2.0.1 (modding-docs coop-v2/TUTORIAL-RE.md). R = save root, `*(main+0x3561930)`; W = R + 0x972ba0, the stage's effect
// command record (STGDAT, the record pkp_coop_records.cpp copies from the authority, W + 8 on):
//   R + 0x6d7df5 + id * 2   m_tutorialHistory, tip / tutorial id 1..0x1f3 (CMNDAT): bit 0 the tip was shown (0xa8cc30 and 0xa8a8c0 set
//                           it through 0x5d1620(R + 0x108, id, 1) when their last argument is 1, and the callers that show a tip only
//                           once test it first). The game's setters only set it; quest effect 0x30 may clear it and runs on every
//                           process.
//   W + 0x18e               u16, the id of the tip that waits (0 none): the contents action 0x836 (0x12b94c) closes the stored tip, then
//                           stores the new id (0x5fa240) and shows it without a time (0xa8cc30(0, id, 0, 1)), or only closes and clears
//                           it (0x5fa260) in its second mode. The per-frame tip tick 0x10d16c shows the stored tip again while it is
//                           stored. A tip shown without a time stays until it is closed by id (0xa8cd70(0, id) -> 0xf6adb0), and
//                           nothing closes it when the id in W changes.
// Where the action that closes it runs depends on the contents record that waits for it (a notice posted on the process of the player
// who did the action), so the others' tip stayed open until each did it. Each process watches W + 0x18e: when the stored id of a
// tip leaves it (a script closed or replaced it here, or the authority's record copy cleared it), the tip is closed here too, and
// the group is told (TipClosed): every process closes that tip's widget and clears the id if it still holds it. The history's bit 0
// is shared as a union (TutorialKnown) the way the unlock lists are, set through the game's own setter (quiet: it shows nothing).
// A baseline is taken in the first frame of each stage, so a stage load's own reset of W is not taken for a close.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <bitset>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::tutorials {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kHistory = 0x6d7df5;       // R + this + id * 2: bit 0 shown
constexpr uint32_t kIdMax = 0x1f3;            // ids 1..0x1f3
constexpr uint64_t kSaveCommon = 0x108;       // R + this: the history setter's first argument
constexpr uint64_t kHistorySet = 0x5d1620;    // (R + 0x108, id, on): bit 0 and the entries linked to it
constexpr uint64_t kEffectRecord = 0x972ba0;  // W
constexpr uint64_t kTipId = kEffectRecord + 0x18e;   // u16: the tip that waits
constexpr uint64_t kTipClose = 0xa8cd70;      // (disabled flag 0, id): closes the tip with that id (shown or queued)
constexpr uint64_t kTipClear = 0x5fa260;      // (W): the stored id = 0

constexpr double kAllEvery = 30.0;
constexpr double kSettle = 2.0;
constexpr unsigned kApplyPerTick = 16;
constexpr size_t kQueue = 16;

struct State {
    std::bitset<kIdMax + 1> seen;      // per id: set at the last read (and sent, once the first read went)
    std::bitset<kIdMax + 1> pending;   // per id: another member has it, not set here yet
    uint8_t origin[kIdMax + 1] = {};
    double at[kIdMax + 1] = {};
    bool have_seen = false, send_all = true;
    double all_at = 0.0;
    // The waiting tip.
    bool tip_have = false;             // the baseline of this stage is taken
    uint8_t tip_stage = 0;
    uint16_t tip_last = 0;             // W + 0x18e at the last read
    std::vector<uint16_t> out;         // tips closed here, to send
    struct In {
        uint16_t id;
        uint8_t origin;
    };
    std::vector<In> in;                // tips closed elsewhere, to close here
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return r && guest::ok(r + kHistory, (kIdMax + 1) * 2) && guest::ok(r + kTipId, 2) ? r : 0;
}
bool has(uint64_t root, uint32_t id) { return guest::rd<uint8_t>(root + kHistory + id * 2) & 1; }
bool valid(uint32_t id) { return id >= 1 && id <= kIdMax; }
uint8_t area_stage() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(gm, layout::kStateStage + 1) ? guest::rd<uint8_t>(gm + layout::kStateStage) : 0;
}

bool shared_here() { return (session_bits() & layout::kSessionOnline) && link::in_session(); }
bool in_world() { return role() == CoopRole::Host || (session_bits() & layout::kSessionVisitor); }
bool safe_now(bool in_game) { return in_world() && session::can_walk(in_game) && !talk::talking(); }

// The ids of the history that became set since the last read (all: every set id), as one TutorialKnown.
bool share_history(uint64_t root, bool all) {
    State& s = st();
    const bool first = !s.have_seen;
    s.have_seen = true;
    wire::Writer w(link::self());
    std::vector<uint8_t> ids;
    uint16_t n = 0;
    for (uint32_t id = 1; id <= kIdMax; ++id) {
        if (!has(root, id)) {
            s.seen.reset(id);   // a reloaded save may have lost it: sent again when it comes back
            continue;
        }
        if (!all && !first && s.seen.test(id)) continue;
        ids.push_back(static_cast<uint8_t>(id));
        ids.push_back(static_cast<uint8_t>(id >> 8));
        ++n;
    }
    if (!n) return true;
    if (!link::share(wire::Type::TutorialKnown, w.u16(n).bytes(ids.data(), ids.size()).take())) return false;
    for (uint32_t id = 1; id <= kIdMax; ++id) {
        if (!has(root, id)) continue;
        if (!first && !s.seen.test(id)) NXLOG_I(kTag, "Co-op tutorials: tip %u shown here, shared", id);
        s.seen.set(id);
    }
    return true;
}

// Sets the ids that settled, through the game's own setter, a few a frame.
void apply_history(::Ctx* c, uint64_t root) {
    State& s = st();
    if (s.pending.none()) return;
    const double now = qpc::seconds();
    unsigned done = 0;
    for (uint32_t id = 1; id <= kIdMax && done < kApplyPerTick; ++id) {
        if (!s.pending.test(id) || now - s.at[id] < kSettle) continue;
        s.pending.reset(id);
        if (has(root, id)) continue;   // set here meanwhile
        guest::call(c, base() + kHistorySet, root + kSaveCommon, id, 1);
        if (s.have_seen && has(root, id)) s.seen.set(id);   // ours now: not sent back as a change
        ++done;
        NXLOG_I(kTag, "Co-op tutorials: tip %u from node %u set here%s", id, s.origin[id], has(root, id) ? "" : ", the game did not set it");
    }
}

// The waiting tip's id left W here: the tip is closed here too and told to the group.
void watch_tip(::Ctx* c, uint64_t root, bool in_game) {
    State& s = st();
    if (!safe_now(in_game)) return;
    const uint8_t stage = area_stage();
    const uint16_t cur = guest::rd<uint16_t>(root + kTipId);
    if (!s.tip_have || stage != s.tip_stage) {   // the first frame of a stage: its record has just been loaded
        s.tip_have = true;
        s.tip_stage = stage;
        s.tip_last = cur;
        return;
    }
    if (cur == s.tip_last) return;
    const uint16_t gone = s.tip_last;
    s.tip_last = cur;
    if (!valid(gone)) return;
    guest::call(c, base() + kTipClose, 0, gone);
    if (s.out.size() < kQueue) s.out.push_back(gone);
    NXLOG_I(kTag, "Co-op tutorials: the waiting tip %u closed here (stored id now %u), sent to the group", gone, cur);
}

void send_closed() {
    State& s = st();
    size_t sent = 0;
    for (uint16_t id : s.out) {
        if (!link::share(wire::Type::TipClosed, wire::Writer(link::self()).u16(id).take())) break;
        ++sent;
    }
    s.out.erase(s.out.begin(), s.out.begin() + static_cast<std::ptrdiff_t>(sent));
}

// A tip another process closed: its widget is closed here, and its stored id cleared if it still holds it.
void apply_closed(::Ctx* c, uint64_t root) {
    State& s = st();
    if (s.in.empty() || !in_world()) return;
    for (const State::In& t : s.in) {
        const bool stored = guest::rd<uint16_t>(root + kTipId) == t.id;
        if (stored) guest::call(c, base() + kTipClear, root + kEffectRecord);
        guest::call(c, base() + kTipClose, 0, t.id);
        if (s.tip_have && s.tip_stage == area_stage()) s.tip_last = guest::rd<uint16_t>(root + kTipId);   // not taken for a close here
        NXLOG_I(kTag, "Co-op tutorials: tip %u closed by node %u, closed here%s", t.id, t.origin, stored ? " (stored id cleared)" : "");
    }
    s.in.clear();
}

}  // namespace

void on_known_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint16_t n = r.u16();
    if (!r.ok() || !shared_here()) return;
    const uint64_t root = save_root();
    if (!root) return;
    for (uint16_t i = 0; i < n; ++i) {
        const uint16_t id = r.u16();
        if (!r.ok()) return;
        if (!valid(id) || has(root, id) || s.pending.test(id)) continue;
        s.pending.set(id);
        s.origin[id] = origin;
        s.at[id] = qpc::seconds();
    }
}

void on_closed_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint16_t id = r.u16();
    if (!r.ok() || !shared_here() || !valid(id)) return;
    if (s.in.size() < kQueue) s.in.push_back({id, origin});
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    const uint64_t root = save_root();
    if (!root || !shared_here()) return;
    if (c) {
        apply_closed(c, root);
        watch_tip(c, root, in_game);
        if (safe_now(in_game)) apply_history(c, root);
    }
    send_closed();
    const double now = qpc::seconds();
    const bool all = s.send_all || now - s.all_at >= kAllEvery;
    if (!share_history(root, all) || !all) return;
    s.send_all = false;
    s.all_at = now;
}

void on_session_change() {
    st() = State{};
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::tutorials
