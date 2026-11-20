// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's look at creature work (item making) and the talk-end forward that carries a member's service to the
// host (pkp_journal_services_internal.h; RE: modding-docs coop-v2/VISITOR-SERVICES-RE.md). A creature's job is the record
// m_CraftRefine (kCraftWorkSize bytes per chara) in one of three bases: Cloud Island's, or, off it, the host's copy on a visitor, or
// the own one (0x77ecb0 first, then 0x376c4, in every reader). Hooks, all entry hooks:
//   0x480b60(runner)           talk end, per runner: on a visitor each talked creature's delta goes to the host as one record
//                              (0x2159c0), unless runner +0x4022 is set or 0x92210 says a stage change is pending (recomputed here)
//   0x48fdac(&handle, rec)     script callback 25, a talk with a held item: line 0x1c5 "I need a little more time" when the creature's
//                              record in the selected base is refining or finished, else the offer
//   0x4984a0(&handle, rec)     script callback 76, the start: a visitor only stores item / count in the creature's delta and sends
//                              it; the host and offline start the job (0x34dc64)
//   0x34dc64(comp, count, item, &user, flag)  the job: recipe / timer / count unless on a visitor, always the user id
// The event record's item list (u32 count at +0x1c60, 8-byte entries from +0x1c64, the last one is the item in hand) and its slot byte
// (+0x2000) are the script callbacks' own reads (0x48ff68, 0x4984b8).
#include "core/pkp_journal_services_internal.h"

namespace nxrt::games::pkp::journal::services {

namespace {

constexpr uint64_t kRunnerSlot = 0x2000, kRunnerEvent = 0x10, kRunnerActors = 0x1ab0;   // runner: u8 slot, u16 event, u32 count then u16 handles at +0x1ab4
constexpr uint32_t kRunnerActorMax = 6;
constexpr uint64_t kEventRunners = 0x35650f8, kRunnerBusy = 0x4022;                      // *(main+...) then u8: set -> no forward
constexpr uint64_t kTripRecord = 0x3bb600;                                               // GM +: the stage-change request record {u32 phase, u32 kind}
constexpr uint64_t kStateVisitorFlag = 0x11;                                             // GM +: u8, bit 0 set -> the talk end does not forward
constexpr uint64_t kRecItems = 0x1c60, kRecItemList = 0x1c64, kRecSlot = 0x2000;         // event record
constexpr uint64_t kRecStartCount = 0x1658, kRecStartRow = 0x1650;                       // 0x4984a0's own reads: u32, u16
constexpr uint64_t kCloudCopy = 0xab3db8;                                                // save root +: Cloud Island's records
constexpr uint32_t kJobMax = layout::kCharaCount;                                        // 0x41a records
constexpr uint64_t kCompChara = 0xc, kCompHandle = 8;                                    // job component: u32 chara, u16 actor handle

// The records of the base the readers select now, and its name.
struct Base {
    uint64_t at = 0;
    const char* name = "?";
};
Base selected_base() {
    const uint64_t r = save_root();
    if (!r) return {};
    if (stage() == kCloudStage) return {r + kCloudCopy, "cloud island R+0xab3db8"};
    if (is_visitor(session())) return {r + layout::kCraftWorkHostCopy, "host copy R+0x1525428"};
    return {r + layout::kCraftWorkOwn, "own R+0x62820"};
}

struct Counts {
    uint32_t jobs = 0, refining = 0, finished = 0;
};
// Jobs in a base: a recipe in 1..0x5db; refining = timer above 0, finished = end byte set (0x2b6220, 0x666780).
Counts count_jobs(uint64_t at) {
    Counts n;
    if (!at || !guest::ok(at, kJobMax * layout::kCraftWorkSize)) return n;
    for (uint32_t i = 0; i < kJobMax; ++i) {
        const uint64_t rec = at + i * layout::kCraftWorkSize;
        const uint16_t recipe = guest::rd<uint16_t>(rec + layout::kCraftWorkRecipe);
        if (recipe - 1u >= layout::kRecipeMax) continue;
        ++n.jobs;
        if (guest::rd<float>(rec + layout::kCraftWorkTimer) > 0.0f) ++n.refining;
        if (guest::rd<uint8_t>(rec + layout::kCraftWorkEnd)) ++n.finished;
    }
    return n;
}

// The item in hand and how many the talk listed (the callbacks' own reads); item 0 when the list is empty.
struct Held {
    uint32_t listed = 0;
    uint16_t item = 0;
};
Held held_item(uint64_t rec) {
    Held h;
    if (!guest::ok(rec + kRecItems, 4)) return h;
    h.listed = guest::rd<uint32_t>(rec + kRecItems);
    const uint64_t last = rec + kRecItemList + (h.listed - 1) * 8ull;
    if (h.listed && h.listed < 0x100 && guest::ok(last, 8)) h.item = guest::rd<uint16_t>(last);
    return h;
}

}  // namespace

void on_talk_end(::Ctx* c) {
    const uint64_t runner = c->x[0];
    if (!guest::ok(runner + kRunnerSlot, 1) || !guest::ok(runner + kRunnerActors, 4)) return;
    const uint8_t sess = session();
    const uint64_t gm = game_state();
    const uint8_t slot = guest::rd<uint8_t>(runner + kRunnerSlot);
    const uint32_t n = guest::rd<uint32_t>(runner + kRunnerActors);
    std::string actors;
    for (uint32_t i = 0; i < n && i < kRunnerActorMax && guest::ok(runner + kRunnerActors + 4 + i * 2, 2); ++i)
        actors += journal::fmt(i ? " 0x%04x" : "0x%04x", guest::rd<uint16_t>(runner + kRunnerActors + 4 + i * 2));
    const uint8_t gm11 = gm ? guest::rd<uint8_t>(gm + kStateVisitorFlag) : 0;
    const uint64_t er = guest::deref(base() + kEventRunners);
    const uint8_t busy = er && guest::ok(er + kRunnerBusy, 1) ? guest::rd<uint8_t>(er + kRunnerBusy) : 0;
    const uint32_t phase = gm && guest::ok(gm + kTripRecord, 8) ? guest::rd<uint32_t>(gm + kTripRecord) : 0xffffffff;
    const uint32_t kind = gm && guest::ok(gm + kTripRecord, 8) ? guest::rd<uint32_t>(gm + kTripRecord + 4) : 0;
    // 0x92210: a stage change is pending (phase <= 4) and this is not slot 0, or slot 0 with a kind other than 0xf.
    const bool cut = phase <= 4 && (slot != 0 || kind != 0xf);
    journal::Rec r(journal::Area::Service, "talk-end");
    r.in("runner-slot=%u event=%u actors=%u[%s] sess=%s gm+0x11=0x%02x runner+0x4022=%u trip-phase=%d trip-kind=%u", slot,
         guest::rd<uint16_t>(runner + kRunnerEvent), n, actors.c_str(), session_text(sess).c_str(), gm11, busy,
         static_cast<int>(phase), kind);
    // A visitor in the host's world (online, not host, GM+0x11 bit 0 clear) forwards; everyone else commits locally.
    if (!is_visitor(sess) || is_host(sess) || (gm11 & 1)) {
        r.out("committed here (no forward)");
    } else {
        r.expect("forwarded to the host (0x2159c0)");
        if (busy) r.refused("send skipped: runner+0x4022 is set");
        else if (cut) r.refused("send skipped: 0x92210 true (stage-change request phase %d, kind %u, slot %u)", static_cast<int>(phase), kind, slot);
        else r.out("forward goes");
    }
    r.note("state at the walk's entry; the skip is tested again at its end");
}

void on_service_offer(::Ctx* c) {
    const uint64_t handle = c->x[0], rec = c->x[1];
    if (!guest::ok(handle, 2) || !guest::ok(rec + kRecSlot, 1)) return;
    const Held held = held_item(rec);
    const Base chosen = selected_base();
    const uint64_t r = save_root();
    const Counts mine = count_jobs(chosen.at);
    const Counts own = r ? count_jobs(r + layout::kCraftWorkOwn) : Counts{};
    const Counts host = r ? count_jobs(r + layout::kCraftWorkHostCopy) : Counts{};
    journal::Rec rc(journal::Area::Service, "service-offer");
    rc.in("actor=0x%04x slot=%u item=%u listed=%u sess=%s stage=%u", guest::rd<uint16_t>(handle), guest::rd<uint8_t>(rec + kRecSlot), held.item, held.listed,
          session_text(session()).c_str(), stage());
    rc.out("reads %s: jobs=%u refining=%u finished=%u (own: %u/%u/%u, host copy: %u/%u/%u)", chosen.name, mine.jobs, mine.refining, mine.finished,
           own.jobs, own.refining, own.finished, host.jobs, host.refining, host.finished);
    rc.note("line 0x1c5 when this creature's record in the chosen base is refining or finished");
}

void on_service_start(::Ctx* c) {
    const uint64_t handle = c->x[0], rec = c->x[1];
    if (!guest::ok(handle, 2) || !guest::ok(rec + kRecSlot, 1) || !guest::ok(rec + kRecStartCount, 4)) return;
    const Held held = held_item(rec);
    // The guard 0x498504 reads: the 4 bytes after the item in the last list entry; non-zero returns without starting.
    uint32_t guard = 0;
    if (held.listed && held.listed < 0x100) guard = guest::rd<uint32_t>(rec + kRecItemList + (held.listed - 1) * 8ull + 4);
    const uint8_t sess = session();
    journal::Rec r(journal::Area::Service, "service-start");
    r.in("actor=0x%04x slot=%u listed=%u last-item=%u last+4=%u rec+0x1658=%u rec+0x1650=%u sess=%s", guest::rd<uint16_t>(handle),
         guest::rd<uint8_t>(rec + kRecSlot), held.listed, held.item, guard, guest::rd<uint32_t>(rec + kRecStartCount),
         guest::rd<uint16_t>(rec + kRecStartRow), session_text(sess).c_str());
    if (!held.listed || guard) r.refused("the callback returns at once (no listed item, or last+4 != 0)");
    else if (is_visitor(sess)) r.out("visitor: item / count go into the creature's delta (+0x1a4 / +0x1a8), post-talk action 5; no job here");
    else r.out("host or offline: the job starts through 0x34dc64");
}

void on_job_start(::Ctx* c) {
    const uint64_t comp = c->x[0], user = c->x[3];
    if (!guest::ok(comp, 0x10)) return;
    const uint32_t chara = guest::rd<uint32_t>(comp + kCompChara);
    const Base b = selected_base();
    journal::Rec r(journal::Area::Service, "job-start");
    r.in("chara=%u actor=0x%04x item=%u count=%u flag=%u user=%s sess=%s from=%s", chara, guest::rd<uint16_t>(comp + kCompHandle),
         static_cast<unsigned>(c->x[2] & 0xffff), static_cast<unsigned>(c->x[1] & 0xff), static_cast<unsigned>(c->x[4] & 1), user_text(user).c_str(),
         session_text(session()).c_str(), lr_text(c).c_str());
    if (chara < kJobMax && b.at && guest::ok(b.at + chara * layout::kCraftWorkSize, layout::kCraftWorkSize)) {
        const uint64_t rec = b.at + chara * layout::kCraftWorkSize;
        r.out("before, in %s: recipe=%u count=%u made=%u taken=%u end=%u timer=%.1f user=%s", b.name, guest::rd<uint16_t>(rec + layout::kCraftWorkRecipe),
              guest::rd<uint8_t>(rec + layout::kCraftWorkCount), guest::rd<uint8_t>(rec + layout::kCraftWorkMade),
              guest::rd<uint8_t>(rec + layout::kCraftWorkTaken), guest::rd<uint8_t>(rec + layout::kCraftWorkEnd),
              guest::rd<float>(rec + layout::kCraftWorkTimer), user_text(rec + layout::kCraftWorkUser).c_str());
    }
    if (is_visitor(session())) r.note("visitor: the recipe and timer stay unwritten, only the user id and counters");
}

}  // namespace nxrt::games::pkp::journal::services
