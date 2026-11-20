// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's look at requests (pkp_journal_services_internal.h; RE: modding-docs coop-v2/REQUESTS-RE.md).
// General requests keep an entry of 0x30 bytes per chara (< 0x41a; state u8 at +8) in one of three copies, chosen as 0x628ec0 does: the
// stage-0x11 copy R+0xc81488, else on a visitor the host's mirror R+0x16f2af8, else the own CMNDAT R+0x22fef0. Unique requests keep
// four bytes per id 1..200 at R+0x6fabd9+id*4 (+0 state 1..5, +1 the UI's pre-state, +2 progress, +3 flags) and a 16-byte slot in the
// manager MU = *(main+0x3565160) at MU+id*16 (+0 u16 the bound creature's actor handle, +2 u8 the id).
//   midhooks 0x629004 / 0x6290ac  `strb w21, [x8, #8]` in the general state setter 0x628ec0: x8 the entry, w21 the new state, w22 the
//                                 old one, w23 the chara (0x629004: the own / mirror copies, 0x6290ac: the stage-0x11 copy)
//   midhook 0x61858c              `cbz x0, ..` before 0x618b00 in the unique completion 0x618304: x0 = CM (the challenge manager),
//                                 x19 = the id's slot; the "complete requests" town goal of THIS process's area counts +1
//   midhook 0x621be8              the same in the general completion 0x6218e0: x0 = CM, w21 = its w3 (1: a replay of someone's step)
//   entry 0x619410(&handle)       the friendship a completion adds
// The unique records are polled at 1 Hz and written when a byte changes.
#include "core/pkp_journal_services_internal.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::journal::services {

namespace {

constexpr uint64_t kEntryStage = 0xc81488, kEntryMirror = 0x16f2af8, kEntryOwn = 0x22fef0;   // save root +: the general entries' copies
constexpr uint64_t kEntrySize = 0x30;
constexpr uint32_t kEntryCharaMax = 0x41a;
constexpr uint64_t kManager = 0x3565160;       // *(main+...): MU
constexpr uint64_t kSlotSize = 16;
constexpr uint64_t kFriendFrom[] = {0x618710, 0x618758};   // the two calls of 0x619410 in the unique completion 0x618304 (return addresses)
constexpr uint32_t kIdMax = layout::kUniqueRequestCount;
constexpr size_t kChangeLines = 12;            // more changes than this in one poll (a save load) go out as one list
constexpr double kPollEvery = 1.0;

struct Record {
    uint8_t b[4];
    uint16_t handle;
};

struct Poll {
    bool have = false;
    double last = 0;
    Record rec[kIdMax + 1] = {};
};
Poll g_poll;

const char* copy_name(uint64_t off) {
    auto in = [&](uint64_t from) { return off >= from && off < from + kEntryCharaMax * kEntrySize; };
    if (in(kEntryStage)) return "stage-0x11 copy";
    if (in(kEntryMirror)) return "host mirror";
    if (in(kEntryOwn)) return "own CMNDAT";
    return "chara >= 0x41a: the base entry";
}

}  // namespace

void on_general_state(::Ctx* c) {
    const uint64_t r = save_root();
    const uint8_t sess = session();
    const uint8_t now = static_cast<uint8_t>(c->x[21]), was = static_cast<uint8_t>(c->x[22]);
    const uint32_t chara = static_cast<uint32_t>(c->x[23]);
    journal::Rec rc(journal::Area::Service, "general-state");
    rc.in("chara=%u old=%u new=%u copy=%s entry=R+0x%llx sess=%s stage=%u", chara, was, now, r ? copy_name(c->x[8] - r) : "?",
          static_cast<unsigned long long>(r ? c->x[8] - r : 0), session_text(sess).c_str(), stage());
    // 0x628ec0 raises the kind-6 event afterwards only on the host, for a new state other than 4 and a changed one.
    const bool raises = is_host(sess) && now != 4 && now != was;
    rc.out("stored; kind 6 raise %s", raises ? "possible (host, new != 4, new != old; needs the actor in the area list)" : "no");
    if (was == now) rc.note("same state");
}

void on_goal_unique(::Ctx* c) {
    const uint64_t slot = c->x[19];
    uint16_t handle = 0;
    uint8_t id = 0;
    if (guest::ok(slot, 4)) {
        handle = guest::rd<uint16_t>(slot);
        id = guest::rd<uint8_t>(slot + 2);
    }
    journal::Rec r(journal::Area::Service, "goal-count");
    r.in("unique id=%u handle=0x%04x CM=0x%llx stage=GM+0x18=%u sess=%s", id, handle, static_cast<unsigned long long>(c->x[0]), stage(),
         session_text(session()).c_str());
    if (c->x[0]) r.out("+1 on every active 'complete requests' goal of this process's area %u", stage());
    else r.out("skipped (CM is 0)");
}

void on_goal_general(::Ctx* c) {
    const uint8_t replay = static_cast<uint8_t>(c->x[21] & 0xff);
    journal::Rec r(journal::Area::Service, "goal-count");
    r.in("general replay=%u (w21=0x%llx) CM=0x%llx stage=GM+0x18=%u sess=%s", replay & 1, static_cast<unsigned long long>(c->x[21] & 0xffffffff),
         static_cast<unsigned long long>(c->x[0]), stage(), session_text(session()).c_str());
    if (c->x[0]) r.out("+1 on every active 'complete requests' goal of this process's area %u", stage());
    else r.out("skipped (CM is 0)");
    if (c->x[0] && (replay & 1)) r.note("a replay of someone's step: counted again unless a later hook zeroes x0");
}

void on_friendship(::Ctx* c) {
    const uint64_t off = c->x[30] - base();
    const char* from = off == kFriendFrom[0] || off == kFriendFrom[1] ? " (unique completion 0x618304)" : "";
    journal::Rec r(journal::Area::Service, "friendship");
    r.in("handle=0x%04x from=%s%s sess=%s", guest::ok(c->x[0], 2) ? guest::rd<uint16_t>(c->x[0]) : 0, lr_text(c).c_str(), from,
         session_text(session()).c_str());
}

void requests_frame(::Ctx*, bool in_game) {
    if (!in_game) return;
    const double now = qpc::seconds();
    if (g_poll.have && now - g_poll.last < kPollEvery) return;
    g_poll.last = now;
    const uint64_t r = save_root(), mu = guest::deref(base() + kManager);
    if (!r || !mu || !guest::ok(r + layout::kSaveUniqueRequests, (kIdMax + 1) * 4) || !guest::ok(mu, (kIdMax + 1) * kSlotSize)) return;
    std::vector<std::string> changes;
    std::vector<Record> next(kIdMax + 1);
    for (uint32_t id = 1; id <= kIdMax; ++id) {
        Record& n = next[id];
        guest::read(r + layout::kSaveUniqueRequests + id * 4, n.b, 4);
        n.handle = guest::rd<uint16_t>(mu + id * kSlotSize);
        const Record& o = g_poll.rec[id];
        if (!g_poll.have || (!std::memcmp(n.b, o.b, 4) && n.handle == o.handle)) continue;
        changes.push_back(journal::fmt("id=%u state %u->%u pre %u->%u progress %u->%u flags 0x%02x->0x%02x handle 0x%04x->0x%04x", id, o.b[0], n.b[0], o.b[1], n.b[1],
                                       o.b[2], n.b[2], o.b[3], n.b[3], o.handle, n.handle));
    }
    if (!g_poll.have) {
        uint32_t per[8] = {};
        for (uint32_t id = 1; id <= kIdMax; ++id) ++per[next[id].b[0] < 7 ? next[id].b[0] : 7];
        journal::Rec s(journal::Area::Service, "request-start");
        s.in("unique requests by state: 0=%u 1=%u 2=%u 3=%u 4=%u 5=%u other=%u", per[0], per[1], per[2], per[3], per[4], per[5], per[6] + per[7]);
    } else if (changes.size() > kChangeLines) {
        journal::list(journal::Area::Service, "request-changes", changes);
    } else {
        for (const std::string& ch : changes) journal::Rec(journal::Area::Service, "request-change").in("%s stage=%u sess=%s", ch.c_str(), stage(), session_text(session()).c_str());
    }
    for (uint32_t id = 1; id <= kIdMax; ++id) g_poll.rec[id] = next[id];
    g_poll.have = true;
}

void requests_reset() { g_poll = Poll{}; }

}  // namespace nxrt::games::pkp::journal::services
