// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, a member's take-out removes from the store it listed (pkp_coop_internal.h; RE: modding-docs
// coop-v2/role-census/build/menus.md). Ordered Items (container kind 0x14, slots 80..89) and Gifts (kind 0x1a, slots
// 1298..1307) live in each player's own save, in its common container store (save root + 0x13d5970), as bags and coins do.
//
// The game, main 2.0.1a:
//   The take-out screen (UI class 0x80) lists and counts both from the own store (0x5b1c84 with w3 = 0) and takes from it through
//   0xe8d6a0 (take all) and 0xe8d920 (take one). Each calls the shared move 0x2ee774(player, &own slot, slot, 0, 1, kind, 0, ..), which
//   adds the item to the bag (0x2e70e0) and then removes the amount added from the source: at 0x2eea70 `bl IsVisitor`, 0x2eea74
//   `and w3, w0, #1`, then 0x5b1c84(.., kind w21, id, w3) at 0x2eea90 and the decrement of [x0 + slot * 16]. For a common-store kind
//   a set w3 makes 0x5b1c84 return the visitor's temp list (save root + 0x170f658) instead of the own store. On a member (a Link Play
//   visitor) the own slot was never emptied: the same items could be taken again, and the decrement landed in the temp list.
//   The visitor rule is right for the generic container screen, which lists a visitor's containers from that temp list (0xe6dc2c ->
//   0x5bc618 with IsVisitor) and moves through the same function (0xe6ad0c, 0xe6fc80). Nothing lists or writes kinds 0x14 / 0x1a
//   in the temp list: the screen's own reads, the third take 0xe8db80 (its move descriptor's temp byte is 0), the shop's order write
//   (0xe88e24) and the gift box write (0x492240 / 0x492664) all use the own store. Vanilla never reaches this case: the screen is
//   offered to the host or offline only.
// The midhook at 0x2eea74: w0 = IsVisitor's answer, read by the `and` alone. It is set to 0 (the own store) when the kind (w21, the
// move's w5) is 0x14 or 0x1a and 0x2ee774 was called by one of the two take-out calls (its frame record, x29 + 8, holds the return
// address: 0xe8d870 or 0xe8db18). Every other kind and caller keeps the game's answer. A non-visitor already gets the own store, so
// single player and the host see no change; on a member the own store is the one listed in every state (in or out of a session), so
// the hook asks nothing of the link.
// The save: the box is emptied in the member's game save (CMNDAT), the item goes into its bag record (pkp_coop_bags.cpp, on disk within
// kWriteEvery). A visitor's game save is written only by the autosave (600 s timer, and only while the world-dirty byte is set; leaving
// a session does not save), so a quit or crash in between would bring the box back full with the item also in the record. After each
// such take (on a member only) the game's autosave is asked to save now, the way the game's Save and Quit asks it (0x4577e4(AS, 1, 0),
// as ui/pkp_save_now.cpp does), with the world-dirty byte set first so its step 1 does not stop at "nothing changed" (0x5adc94). A
// visitor's autosave writes CMNDAT and the screenshot, never the island (0x5ae78c kind 3). The take counts as saved only when a save
// that started after it wrote: its step went past 1 (the snapshot, 0x5ae428, and the job queued) and, back at idle, the save manager's
// last job has result 0 (or none), the test the autosave's own step 2 makes before it reports an error. A save the game stops (its gate
// 0x5ae060 refuses, or nothing to save: step 1 straight to 5) or one whose job failed is asked for again kRetry later. A take while a
// save runs gets a save of its own after it (the save holds the takes made before its last idle frame, a lower bound).
// The call is made here, not through ui/pkp_save_now.cpp: that feature can be off ([pkp] save_now), and it posts notices; the game
// call and the two autosave words are the same (pkp_layout.h). The bag record is not held back until the save is written (as bags
// hold for an unanswered request): an apply at a selection change does not wait for a hold, and the session can end before the save,
// so a record without the item would later be written over the bag that has it.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <atomic>
#include <cstdint>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::takeout {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kFromTakeAll = 0xe8d870;   // 0xe8d6a0's `bl 0x2ee774` returns here
constexpr uint64_t kFromTakeOne = 0xe8db18;   // 0xe8d920's
constexpr uint8_t kOrdered = 0x14, kGifts = 0x1a;
constexpr int kKindReg = 21;                  // 0x2ee774 keeps its w5 (the source's container kind) in w21 to the end
constexpr int kFpReg = 29;
constexpr uint64_t kCallerReturn = 8;         // 0x2ee774's frame record (x29 = sp + 0x40): +8 the caller's x30
constexpr double kRetry = 1.0;                // seconds between two asks while a save is still owed
constexpr double kOwedLog = 30.0;             // seconds a save may stay owed before it is logged (again)

struct State {
    std::atomic<uint32_t> taken{0};   // takes made on the own box (the hook)
    uint32_t saved = 0;               // the takes a written save holds
    uint32_t idle_taken = 0;          // taken, at the last frame the autosave was idle (a save that starts after it holds at least these)
    uint32_t snap = 0;                // the running save's idle_taken
    bool watching = false;            // an autosave runs: is it written?
    bool wrote = false;               // it went past step 1 (the snapshot taken, the job queued)
    bool judged = false;              // its job's result was read at its step 5 (the job is done there)
    bool ok = false;                  // that result: written
    double next_ask = 0;
    double owed_since = -1.0;
    double owed_logged = 0;
};
State& st() {
    static State s;
    return s;
}

// The save manager's last job wrote (the autosave's own test in its step 2: no job, or result 0).
bool job_written() {
    const uint64_t mgr = guest::rd<uint64_t>(base() + layout::kSaveManager);
    if (!guest::ok(mgr, layout::kSaveMgrJob + 8)) return false;
    const uint64_t job = guest::rd<uint64_t>(mgr + layout::kSaveMgrJob);
    if (!job) return true;
    return guest::ok(job, layout::kSaveJobResult + 4) && guest::rd<uint32_t>(job + layout::kSaveJobResult) == 0;
}

}  // namespace

int on_remove_store(::Ctx* c) {
    if (!(c->x[0] & 1)) return 0;   // not a visitor: 0x5b1c84 returns the own store already
    const uint8_t kind = static_cast<uint8_t>(c->x[kKindReg]);
    if (kind != kOrdered && kind != kGifts) return 0;
    const uint64_t rec = c->x[kFpReg] + kCallerReturn;
    if (!guest::ok(rec, 8)) return 0;
    const uint64_t from = guest::rd<uint64_t>(rec) - base();
    if (from != kFromTakeAll && from != kFromTakeOne) return 0;
    c->x[0] = 0;
    const uint32_t n = st().taken.fetch_add(1, std::memory_order_relaxed) + 1;
    NXLOG_I(kTag, "Co-op take-out: %s taken out of this player's own box (%s, take %u); removed there, as listed; the game save is "
                  "asked for", kind == kOrdered ? "an ordered item" : "a gift", from == kFromTakeAll ? "take all" : "take one", n);
    return 0;
}

void tick(::Ctx* c) {
    State& s = st();
    const uint32_t taken = s.taken.load(std::memory_order_relaxed);
    if (s.saved == taken && !s.watching) return;
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!guest::ok(game, layout::kStateAutosaveFlags + 4)) return;
    const uint32_t step = guest::rd<uint32_t>(game + layout::kStateAutosave);
    const uint32_t flags = guest::rd<uint32_t>(game + layout::kStateAutosaveFlags);
    const double now = qpc::seconds();
    if (s.watching) {
        s.wrote |= step >= 2 && step <= 4;
        if (step == 5 && s.wrote && !s.judged) {   // the job is done at step 5: its result is read before anything else replaces it
            s.judged = true;
            s.ok = job_written();
        }
        if (step != 0) return;
        s.watching = false;
        if (s.wrote && (s.judged ? s.ok : job_written())) {
            if (s.snap > s.saved) {
                s.saved = s.snap;
                NXLOG_I(kTag, "Co-op take-out: the game save holds the box after take %u%s", s.saved,
                        s.saved == taken ? "" : "; a later take is saved next");
            }
        } else {
            s.next_ask = now + kRetry;   // the game stopped it (its gate, nothing to save, or the write failed): asked again
            NXLOG_W(kTag, "Co-op take-out: the game's autosave ended without writing; it is asked again");
        }
    } else if (step != 0) {   // a save started (ours or the game's): it holds the takes made before its last idle frame
        s.watching = true;
        s.wrote = step >= 2 && step <= 4;
        s.judged = s.ok = false;
        s.snap = s.idle_taken;
        return;
    }
    s.idle_taken = taken;
    if (s.saved == taken) {
        s.owed_since = -1.0;
        return;
    }
    if (s.owed_since < 0) s.owed_since = s.owed_logged = now;
    if (now - s.owed_logged >= kOwedLog) {
        s.owed_logged = now;
        NXLOG_W(kTag, "Co-op take-out: the game has allowed no save for %.0f s; the box's take %u is not on disk yet", now - s.owed_since,
                taken);
    }
    if (now < s.next_ask || !(flags & 1)) return;   // the autosave is off (title, loading): it is asked once it is on
    s.next_ask = now + kRetry;
    guest::wr<uint8_t>(base() + layout::kWorldDirty, 1);
    guest::call(c, base() + layout::kAutosaveNow, game + layout::kStateAutosaveObj, 1, 0);
}

void owe_save() { st().taken.fetch_add(1, std::memory_order_relaxed); }

void reset() {
    State& s = st();
    s.taken.store(0, std::memory_order_relaxed);
    s.saved = s.idle_taken = s.snap = 0;
    s.watching = s.wrote = s.judged = s.ok = false;
    s.next_ask = 0;
    s.owed_since = -1.0;
    s.owed_logged = 0;
}

}  // namespace nxrt::games::pkp::coop::takeout
