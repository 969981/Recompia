// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's events watcher (pkp_journal_watch.h): the event queue, the two event runners and the contents scheduler's
// records and notices. Offsets are main-relative (RE: modding-docs coop-v2/QUEUE-ENQUEUE-RE.md, KYOGRE-BEACH-RE.md, EVENT-END-RE.md,
// quests/QUEST-START-RE.md; 2.0.1a). Q = the event queue (x0 of 0x45d5a0, = *(main+0x3565100)), ER = *(main+0x35650f8) the runners
// (runner = ER + slot*0x2010), GM = *(main+0x3561710), SC = *(GM+0xd8) the contents scheduler (0x31406c `ldr x0, [x8, #0xd8]`).
//   0x45d5a0 (entry) enqueue(Q, w1 slot, x2 rec): every talk, scene and event offer ends here. It returns 0 from one exit for every
//            refusal, so the reason is computed at entry from the same fields in the game's order (Offer below); a prediction is
//            checked on the next frame against the record slot the game takes (or the runner) and a wrong one is a MISMATCH.
//   0x12b544 (entry, co-op's PlayScene site) a content's "play scene" command (cmd; scene u16 at +4): the scene is put on the offer
//            its 0x2e6ce0 makes (the offer's builder's caller is 0x12b544), or logged alone when no offer follows (no valid event).
//   0x132f00 (entry) a content record leaves its list (x0 list, x1 record, w2): its result is computed at entry; a removal right after
//            a refused content offer is the Kyogre loss (#830: the one-shot record is done, its scene never played).
//   0x1353a0 (entry) the contents notifier (SC, list, p3, p4): the notice kind at SC+0x11aae0 and its payload fields.
//   0x47e140 (entry) runner state 4, the end writes (played bit, rows, story bus, notices); one call per event end (its one exit
//            sets state 5). 0x47bd00 (entry, co-op's TalkEnd site) the end of a slot's chain.
//   poll     each runner's asked-for state (+0xc) and event (+0x10): one step line per change, so a crash names the event step.
// Lines from per-frame callers (mode-1 content records re-offer every frame, notices of unread rate) are kept once per key and
// counted for kRepeat seconds.
#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
#include "core/pkp_journal_actor.h"
#include "core/pkp_journal_watch.h"
#include "core/pkp_layout.h"

#include <cinttypes>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nxrt/qpc.h"

namespace nxrt::games::pkp::journal {

namespace {

constexpr Area kArea = Area::Event;
constexpr double kRepeat = 5.0;          // a key's repeats are counted this long, then written as one line
constexpr double kLostWindow = 2.0;      // a record removal this soon after a refused content offer is flagged
constexpr uint64_t kTextEnd = 0x2749480; // main's code ends here (text.bin): a return address below it is main's

// ---- the event queue (0x45d5a0; per slot kSlotSize from Q) ----
// The queue and runner layout as pkp_coop_trace.cpp / pkp_coop_talk.cpp have it (anonymous there), plus this pass's 0x45d5a0 read.
constexpr uint64_t kSlotSize = 0x20fa0, kRecSize = 0x708, kNodes = 0x1c200, kNodeSize = 0x130;
constexpr int kRecords = 0x40, kNodeCount = 0x40, kSlots = 2;
constexpr uint64_t kChainNode = 0x20e00;   // u64: the node new offers join as children (path A below), its u32 +0x10 child count
constexpr uint64_t kPendingCount = 0x20e08, kPendingList = 0x20e10;   // u32 count, u64 nodes[kPendingMax]
constexpr uint32_t kChainMax = 0x20, kPendingMax = 0x30;
// node: +8 record, +0x10 u32 child count, +0x18 children (u64), +0x128 u8 started
constexpr uint64_t kNodeRec = 8, kNodeChildCount = 0x10, kNodeChildren = 0x18, kNodeStarted = 0x128;
// record (0x708): u16 +0 id, u16 / u32 +0x30, u32 +0x68 / +0x110, u16 +0x1b4 / +0x1b8 sub ids, u32 +0x3f0 (merged for 0x107c),
// u16 +0x660 subject actor, u32 +0x664 kind, u32 +0x6ec mode, u16 +0x704 | u8 +0x706 << 16 flags
constexpr uint64_t kRecId = 0, kRec30 = 0x30, kRec68 = 0x68, kRec110 = 0x110, kRecSub1 = 0x1b4, kRecSub2 = 0x1b8, kRecSubject = 0x660,
                   kRecKind = 0x664, kRecMode = 0x6ec, kRecFlags = 0x704;
constexpr uint32_t kIdCount = 0x18ff, kKindFree = 0xd1, kMergedId = 0x107c, kNoValue = 0x80000000u;
constexpr uint32_t kBitNoDup = 1u << 1, kBitOne = 1u << 2, kBitAnyStage = 1u << 4, kBitChain = 1u << 10, kBitPlayer = 1u << 13;

// ---- the runners (ER + slot * kRunnerSize) ----
constexpr uint64_t kRunners = 0x35650f8, kRunnerSize = 0x2010;
constexpr uint64_t kRunNow = 8, kRunNext = 0xc;   // u32: the state 0x475fb8 runs this frame (copied from +0xc first), the state asked for
constexpr uint64_t kRunEvent = 0x10, kRunCmd = 0x14, kRunSub1 = 0x15ec, kRunSub2 = 0x15f0, kRunScene = 0x1468, kRunSubject = 0x1a86;
constexpr uint64_t kRunSlot = 0x2000, kRunChainA = 0x2008, kRunChainB = 0x2009;   // u8 each
constexpr uint32_t kRunIdle = 9;   // +8 and +0xc both >= 9 (0xffffffff after the reset 0x47b9c0): idle
// 0x475fb8's jump table (0x2809f3d, bytes 0 5 11 21 26 36 41 31 46): 0 0x47bf80 start (an actor not there: abort to 6), 1 waits
// 0xa9ff4c, 2 0x47ca04 / 0x47ce00, 3 0x47c320 the script, 4 0x47e140 end writes, 5 0x47e3e0 (while 0x117624 is clear), 6 0x47f140,
// 7 0x47c6c0 actors + cleanup, 8 0x47c820 the next event or 0x47bd00
constexpr const char* kStateNames[] = {"start", "wait", "setup", "script", "end-writes", "callback", "finish", "cleanup", "next"};

// ---- game state, players, save ----
// kTripRequest and kPlayedFlags as pkp_coop_contents.cpp / pkp_coop_trace.cpp have them (anonymous there).
constexpr uint64_t kTripRequest = 0x3bb600;   // GM: u32 phase, u32 kind (0x92210(this, slot): phase <= 4 and (slot != 0 or kind != 0xf))
constexpr uint64_t kBusBusy = 0x688;          // *(GM + layout::kStateEvent): u8, 0x117624's answer
constexpr uint64_t kStoryOn = 0x8c;           // GM: u8 bit 0, the contents gate of 0x1353a0
constexpr uint64_t kContents = 0xd8;          // GM: SC
constexpr uint64_t kPlayedFlags = 0x4a2cec;   // save root: the played-scene bits (0x117650)
constexpr uint32_t kSceneCount = 0xbb7;       // scenes 1..0xbb7 (0x12b544, 0x47e140)
constexpr uint64_t kSecondPlayer = 0x38, kActorIdOf = 0x27fe;   // player manager +0x38 the second player; player: u16 actor id
constexpr uint64_t kBusyDefault = 0x3567e50, kBusyPart = 0x3778, kBusyByte = 0x15;   // bit 13: the busy part's byte (default record)
constexpr uint64_t kVtHasPart = 0x1e0, kVtPart = 0x3f0;   // actor vtable slots 0x45d5a0 calls (leaves: pkp_journal_actor.h)
constexpr uint64_t kUiBusy = 0xa89bd8;        // (slot) -> bool: a leaf (loads only, no call, no store)

// ---- contents scheduler (SC) ----
constexpr uint64_t kEntries = 0x180, kEntrySize = 0x25c8, kEntryContent = 4;   // content entry i < 0x2d: +4 u16 content id
constexpr uint32_t kEntryCount = 0x2d;
constexpr uint64_t kLists = 0x786b8, kListSize = 0x4208;   // 39 trigger lists; 0..2 the polled ones
constexpr uint32_t kListCount = 39;
constexpr uint64_t kListHead = 0x10, kListCount32 = 0x24, kLnNext = 0x10;   // list: first node, u32 count; node: +0 record, +0x10 next
// script record (0x50): +0 u32 entry, +4 u32 record id, +8 s32 script id, +0xc u8 bits 0..1 disabled, +0x10 u32 mode (1 stays),
// +0x22 u16 times run, +0x2c u16 bits, +0x48 its progress in the saved record (+0xc u8 state 1 running 2 done)
constexpr uint64_t kCrEntry = 0, kCrId = 4, kCrScript = 8, kCrBits = 0xc, kCrMode = 0x10, kCrRuns = 0x22, kCrFlags = 0x2c,
                   kCrProgress = 0x48, kProgState = 0xc;
constexpr uint64_t kNoticeKind = 0x11aae0;

// ---- functions (for the caller's name; ranges from the lift's function list) ----
struct Fn {
    uint64_t lo, hi;
    const char* name;
};
constexpr Fn kBuilders[] = {
    {0x2e6ce0, 0x2e7058, "scene"},    {0x4bf7c0, 0x4c024c, "talk"},    {0x243820, 0x243b60, "callback"},
    {0x2400e0, 0x2402a0, "generic"},  {0x4628e0, 0x462ac0, "item-get"}, {0x3b9060, 0x3b91c0, "dex"},
    {0x45f2a0, 0x45f580, "habitat"},
};
constexpr Fn kPlaySceneFn = {0x12b544, 0x12b634, "content 2000"};

// ---- helpers ----
uint64_t main_at(uint64_t off) { return base() + off; }

// "0x<main offset>" for a main code address, else the raw value.
std::string code(uint64_t a) {
    const uint64_t b = base();
    return a >= b && a < b + kTextEnd ? fmt("0x%" PRIx64, a - b) : fmt("raw 0x%" PRIx64, a);
}
const char* fn_name(uint64_t a, const Fn* fns, size_t n) {
    const uint64_t b = base();
    for (size_t i = 0; i < n; ++i)
        if (a >= b + fns[i].lo && a < b + fns[i].hi) return fns[i].name;
    return nullptr;
}
bool in_fn(uint64_t a, const Fn& f) { return a >= base() + f.lo && a < base() + f.hi; }

template <class T> T at(uint64_t a, T dflt = T{}) {
    T v = dflt;
    guest::read(a, v);
    return v;
}
uint32_t flags_of(uint64_t rec) { return at<uint16_t>(rec + kRecFlags) | (static_cast<uint32_t>(at<uint8_t>(rec + kRecFlags + 2)) << 16); }

uint64_t game_state() { return guest::deref(main_at(layout::kGameState)); }
uint64_t runners() { return guest::deref(main_at(kRunners)); }
int played(uint32_t scene) {   // 1 / 0, -1 unknown
    const uint64_t root = guest::deref(main_at(layout::kSaveRoot));
    uint8_t b = 0;
    if (scene - 1 >= kSceneCount || !root || !guest::read(root + kPlayedFlags + (scene >> 3), b)) return -1;
    return (b >> (scene & 7)) & 1;
}
const char* state_name(uint32_t s) { return s < 9 ? kStateNames[s] : "idle"; }

using actor::leaf;

// ---- dedupe: a key once, its repeats counted for kRepeat seconds ----
struct Seen {
    double at = 0;
    uint32_t more = 0;
    const char* what = "";
    std::string label;
};

// ---- the offer (0x45d5a0) ----
enum class Verdict : uint8_t { Taken, Refused, Merged, Open };   // Open: a test that cannot be read decides it

struct Offer {
    uint64_t q = 0, rec = 0, lr = 0, from = 0;
    uint32_t slot_w = 0;   // w1 as given; the queue indexes it as sxtb, the runner tests as & 0xff
    int slot = 0;
    uint16_t id = 0, u30 = 0, subject = 0;
    uint32_t kind = 0, mode = 0, flags = 0;
};

struct Outcome {
    Verdict v = Verdict::Taken;
    std::string reason;   // Refused / Merged: the reason; Taken: where it went
    std::string open;     // the tests that could not be read, in order
    int free_rec = -1;    // the record slot the game takes (the first free one), -1 none
};

struct Confirm {
    uint64_t q;
    int slot, free_rec;
    uint16_t id;
    uint32_t kind;
    Verdict v;
    std::string label, reason, open;
};

struct Stash {   // the last 0x12b544 look, for the offer it makes
    bool on = false;
    uint32_t scene = 0;
    int played = -1;
    uint64_t lr = 0;
    double at = 0;
};

struct Runner {
    bool seen = false;
    uint32_t next = 0;
    uint16_t event = 0;
};

struct LastRefused {
    double at = -1e9;
    uint16_t id = 0;
    uint32_t scene = 0;
    std::string reason;
};

struct State {
    std::mutex m;
    std::unordered_map<std::string, Seen> seen;
    std::vector<Confirm> confirms;
    Stash stash;
    Runner run[kSlots];
    LastRefused content_refused;   // the last refused offer made by a content command (always slot 0: 0x12b544 passes w0 = 0)
};
State& st() {
    static State s;
    return s;
}

bool first(State& s, const std::string& key, const char* what, const std::string& label, double now) {
    auto it = s.seen.find(key);
    if (it == s.seen.end()) {
        s.seen.emplace(key, Seen{now, 0, what, label});
        return true;
    }
    ++it->second.more;
    return false;
}

void flush_seen(State& s, double now) {
    for (auto it = s.seen.begin(); it != s.seen.end();) {
        if (now - it->second.at < kRepeat) {
            ++it;
            continue;
        }
        if (it->second.more)
            Rec(kArea, it->second.what).in("%s", it->second.label.c_str()).out("%u more in %.0f s", it->second.more, now - it->second.at);
        it = s.seen.erase(it);
    }
}

// The pending tree (0x479b80, walked by bit 2): 0x45d5a0 refuses when the FIRST node the ready test 0x479c60 passes (not started, a
// valid record) has bit 2. 0x479c60 is not a leaf (it calls the role getters, 0x313418), so it is not run: the walk lists the
// candidates. None with bit 2: the test passes for sure.
void bit2_candidates(uint64_t node, int depth, int& budget, std::vector<uint16_t>& ids) {
    if (!node || depth > 8 || --budget < 0 || !guest::ok(node, kNodeSize)) return;
    if (!guest::rd<uint8_t>(node + kNodeStarted)) {
        const uint64_t rec = guest::rd<uint64_t>(node + kNodeRec);
        if (rec && guest::ok(rec, kRecSize)) {
            const uint16_t id = guest::rd<uint16_t>(rec + kRecId);
            const bool valid = static_cast<uint32_t>(id - 1) < kIdCount || guest::rd<uint32_t>(rec + kRecKind) <= 0xd0;
            if (valid && (flags_of(rec) & kBitOne)) ids.push_back(id);
        }
    }
    const int32_t n = guest::rd<int32_t>(node + kNodeChildCount);
    for (int32_t i = 0; i < n && i < 34; ++i) bit2_candidates(guest::rd<uint64_t>(node + kNodeChildren + i * 8), depth + 1, budget, ids);
}

// 0x45d5a0's tests at entry, in its order (main.img 0x45d5a0..0x45db1c).
Outcome predict(::Ctx* c, const Offer& o) {
    Outcome r;
    auto refuse = [&](std::string why) {
        r.v = Verdict::Refused;
        r.reason = std::move(why);
        return r;
    };
    const uint64_t sb = o.q + static_cast<uint64_t>(o.slot) * kSlotSize;
    const uint64_t er = runners();
    const bool in_range = static_cast<uint32_t>(o.id - 1) < kIdCount;
    // free record slot (computed now for the next frame's check)
    if (o.slot >= 0 && o.slot < kSlots && guest::ok(sb, kSlotSize))
        for (int i = 0; i < kRecords; ++i) {
            const uint64_t ri = sb + i * kRecSize;
            if (static_cast<uint32_t>(guest::rd<uint16_t>(ri + kRecId) - 1) >= kIdCount && guest::rd<uint32_t>(ri + kRecKind) >= kKindFree) {
                r.free_rec = i;
                break;
            }
        }
    // 1. 0x45d5c0: an id outside 1..0x18ff needs kind <= 0xd0
    if (!in_range && o.kind > 0xd0) return refuse(fmt("bad-record (id %u, kind 0x%x > 0xd0)", o.id, o.kind));
    // 2. 0x45d5ec: no runners
    if (!er) return refuse("no-runner (ER null)");
    if (o.slot < 0 || o.slot >= kSlots || !guest::ok(sb, kSlotSize) || !guest::ok(er, kRunnerSize * kSlots)) {
        r.v = Verdict::Open;
        r.open = fmt("slot %d outside 0..1 or queue / runners unreadable: not computed", o.slot);
        return r;
    }
    const uint64_t run = er + static_cast<uint64_t>(o.slot) * kRunnerSize;
    // 3. 0x45d618: bit 1 clear: duplicates
    if (!(o.flags & kBitNoDup)) {
        if (static_cast<uint32_t>(o.id - 1) <= kIdCount - 1) {
            for (int i = 0; i < kRecords; ++i)
                if (guest::rd<uint16_t>(sb + i * kRecSize + kRecId) == o.id) {
                    if (o.id != kMergedId) return refuse(fmt("dup-queued (record %d)", i));
                    r.v = Verdict::Merged;
                    r.reason = fmt("merged-0x107c (+0x3f0 copied into record %d, returns 0)", i);
                    return r;
                }
            if (guest::rd<uint16_t>(run + kRunEvent) == o.id)
                return refuse(fmt("dup-running (runner state %u)", guest::rd<uint32_t>(run + kRunNext)));
        } else {
            const uint16_t s1 = guest::rd<uint16_t>(o.rec + kRecSub1), s2 = guest::rd<uint16_t>(o.rec + kRecSub2);
            const bool by1 = static_cast<uint32_t>(s1 - 1) <= 0x3e6, by2 = !by1 && static_cast<uint32_t>(s2 - 1) <= 0x1f3;
            if (by1 || by2) {
                const uint64_t field = by1 ? kRecSub1 : kRecSub2;
                const uint16_t sub = by1 ? s1 : s2;
                for (int i = 0; i < kRecords; ++i)
                    if (guest::rd<uint16_t>(sb + i * kRecSize + field) == sub) return refuse(fmt("dup-sub (+0x%" PRIx64 " %u in record %d)", field, sub, i));
                if (guest::rd<uint16_t>(run + (by1 ? kRunSub1 : kRunSub2)) == sub)
                    return refuse(fmt("dup-sub (+0x%" PRIx64 " %u running)", field, sub));
            }
        }
    }
    // 4. 0x45d738: bit 2: the runner busy, or the first ready pending node has bit 2
    if (o.flags & kBitOne) {
        if ((o.slot_w & 0xff) <= 1) {
            const uint32_t now = guest::rd<uint32_t>(run + kRunNow), next = guest::rd<uint32_t>(run + kRunNext);
            if (now < kRunIdle || next < kRunIdle)
                return refuse(fmt("bit2-runner-busy (+8 %u +0xc %u, event %u)", now, next, guest::rd<uint16_t>(run + kRunEvent)));
        }
        const int32_t n = guest::rd<int32_t>(sb + kPendingCount);
        std::vector<uint16_t> ids;
        int budget = 256;
        for (int32_t k = 0; k < n && k < static_cast<int32_t>(kPendingMax); ++k)
            bit2_candidates(guest::rd<uint64_t>(sb + kPendingList + k * 8), 0, budget, ids);
        if (!ids.empty()) {
            std::string list;
            for (uint16_t id : ids) list += (list.empty() ? "" : ",") + std::to_string(id);
            r.open += fmt("bit2-pending? (pending with bit 2: %s; refused if one is the first the ready test passes)", list.c_str());
        }
    }
    // 5. 0x45d824: bit 4 clear: the stage-change request
    if (!(o.flags & kBitAnyStage)) {
        const uint64_t gm = game_state();
        if (!gm || !guest::ok(gm + kTripRequest, 8)) {
            r.open += std::string(r.open.empty() ? "" : "; ") + "stage-change request unreadable";
        } else {
            const uint32_t phase = guest::rd<uint32_t>(gm + kTripRequest), kind = guest::rd<uint32_t>(gm + kTripRequest + 4);
            if (phase <= 4 && ((o.slot_w & 0xff) != 0 || kind != 0xf)) return refuse(fmt("stage-change phase=%u kind=%u", phase, kind));
        }
    }
    // 6. 0x45d854: bit 13: the slot's player's busy byte (PM +0x30 slot 0, +0x38 else; actor 0x281360(PM, &player+0x27fe))
    if (o.flags & kBitPlayer) {
        const uint64_t pm = guest::deref(main_at(layout::kPlayerManager));
        const uint64_t player = pm ? guest::deref(pm + ((o.slot_w & 0xff) ? kSecondPlayer : layout::kManagerPlayer)) : 0;
        if (pm && player) {
            const uint16_t aid = at<uint16_t>(player + kActorIdOf);
            const auto actor = actor::find(pm, aid);
            std::optional<uint64_t> part;
            if (actor && !*actor) part = main_at(kBusyDefault);
            else if (actor) {
                const auto has = leaf(*actor, kVtHasPart);
                if (has && !(*has & 0xffffffffu)) part = main_at(kBusyDefault);
                else if (has) {
                    const auto comp = leaf(*actor, kVtPart);
                    if (comp) part = *comp ? *comp + kBusyPart : main_at(kBusyDefault);
                }
            }
            uint8_t busy = 0;
            if (!part || !guest::read(*part + kBusyByte, busy))
                r.open += std::string(r.open.empty() ? "" : "; ") + "bit13 set: player-busy test not read";
            else if (busy)
                return refuse(fmt("bit13-player-busy (actor 0x%x, %s +0x15 = %u)", aid,
                                  *part == main_at(kBusyDefault) ? "default record" : "its part +0x3778", busy));
        }
    }
    // 7. 0x45d8e0: u16 +0x30 outside 1..0xbb7: 0xa89bd8(slot)
    if (static_cast<uint32_t>(o.u30 - 1) >= kSceneCount && (guest::call(c, main_at(kUiBusy), o.slot_w) & 1))
        return refuse(fmt("ui-busy (0xa89bd8(%u) = 1, +0x30 = %u)", o.slot_w & 0xff, o.u30));
    // 8. 0x45d91c: a free record, then a free node
    if (r.free_rec < 0) return refuse("queue-full (no free record of 0x40)");
    bool node = false;
    for (int j = 0; j < kNodeCount && !node; ++j) {
        const uint64_t nd = sb + kNodes + j * kNodeSize;
        node = !guest::rd<uint64_t>(nd + kNodeRec) && !guest::rd<uint8_t>(nd + kNodeStarted);
    }
    if (!node) return refuse("no-node (no free node of 0x40)");
    // 9. 0x45da1c: path A (a child of the chain node) or B (a pending root), each with its own limit
    const uint8_t a = guest::rd<uint8_t>(run + kRunChainA);
    const uint32_t r30 = at<uint32_t>(o.rec + kRec30), r68 = at<uint32_t>(o.rec + kRec68), r110 = at<uint32_t>(o.rec + kRec110);
    auto other = [&] {   // 0x45da58: 0x117624() (the event bus's u8 +0x688), runner +0x2009, then bit 10
        const uint64_t gm = game_state();
        const uint64_t bus = gm ? guest::deref(gm + layout::kStateEvent) : 0;
        return (bus && at<uint8_t>(bus + kBusBusy)) || guest::rd<uint8_t>(run + kRunChainB) || (o.flags & kBitChain);
    };
    bool chain;
    if (r30 == kNoValue && r68 == kNoValue) chain = a || (r110 != kNoValue ? other() : (o.flags & kBitChain) != 0);
    else chain = a || other();
    const uint64_t cn = chain ? guest::rd<uint64_t>(sb + kChainNode) : 0;
    if (cn) {
        if (at<uint32_t>(cn + kNodeChildCount) == kChainMax) return refuse("chain-full (the chain node has 0x20 children)");
        r.reason = fmt("record %d, a child of the chain node", r.free_rec);
    } else {
        if (guest::rd<uint32_t>(sb + kPendingCount) == kPendingMax) return refuse("pending-full (0x30 pending)");
        r.reason = fmt("record %d, pending", r.free_rec);
    }
    if (!r.open.empty()) r.v = Verdict::Open;
    return r;
}

void on_offer(::Ctx* c) {
    State& s = st();
    Offer o;
    o.q = c->x[0];
    o.slot_w = static_cast<uint32_t>(c->x[1]);
    o.slot = static_cast<int8_t>(o.slot_w & 0xff);
    o.rec = c->x[2];
    o.lr = c->x[30];
    if (!guest::ok(o.rec, kRecSize)) {
        Rec(kArea, "offer").in("slot=%d rec=0x%" PRIx64 " by=%s", o.slot, o.rec, code(o.lr).c_str()).note("record unreadable");
        return;
    }
    o.from = guest::ok(c->x[29] + 8, 8) ? guest::rd<uint64_t>(c->x[29] + 8) : 0;   // the builder's frame record: its caller
    o.id = guest::rd<uint16_t>(o.rec + kRecId);
    o.u30 = guest::rd<uint16_t>(o.rec + kRec30);
    o.subject = guest::rd<uint16_t>(o.rec + kRecSubject);
    o.kind = guest::rd<uint32_t>(o.rec + kRecKind);
    o.mode = guest::rd<uint32_t>(o.rec + kRecMode);
    o.flags = flags_of(o.rec);
    const Outcome out = predict(c, o);

    std::lock_guard<std::mutex> lk(s.m);
    const double now = qpc::seconds();
    std::string scene;
    uint32_t scene_id = 0;
    if (s.stash.on && o.from && in_fn(o.from, kPlaySceneFn)) {
        scene_id = s.stash.scene;
        scene = fmt(" (content 2000: scene=%u played=%d)", s.stash.scene, s.stash.played);
        s.stash.on = false;
    }
    const char* reason = out.v == Verdict::Taken ? "taken" : out.v == Verdict::Open ? "open" : out.reason.c_str();
    // dedupe key: the reason's name only (its numbers change frame to frame)
    const std::string why = out.v == Verdict::Refused || out.v == Verdict::Merged ? out.reason.substr(0, out.reason.find(' ')) : reason;
    const std::string label = fmt("slot=%d id=%u reason=%s", o.slot, o.id, why.c_str());
    if (out.v == Verdict::Refused && o.slot == 0 && in_fn(o.from, kPlaySceneFn))
        s.content_refused = LastRefused{now, o.id, scene_id, out.reason};
    if (!first(s, fmt("o%d.%u.%s", o.slot, o.id, why.c_str()), "offer repeated", label, now)) return;
    const char* bname = fn_name(o.lr, kBuilders, sizeof kBuilders / sizeof kBuilders[0]);
    Rec r(kArea, "offer");
    r.in("slot=%d id=%u kind=0x%x mode=%u flags=0x%06x u30=%u subject=0x%x by=%s %s <- %s%s", o.slot, o.id, o.kind, o.mode, o.flags, o.u30,
         o.subject, bname ? bname : "?", code(o.lr).c_str(), o.from ? code(o.from).c_str() : "?", scene.c_str());
    r.expect("taken");
    switch (out.v) {
        case Verdict::Taken: r.out("taken (predicted: %s)", out.reason.c_str()); break;
        case Verdict::Refused:
            r.out("dropped").refused("%s%s%s", out.reason.c_str(), out.open.empty() ? "" : "; or earlier: ", out.open.c_str());
            break;
        case Verdict::Merged: r.out("%s", out.reason.c_str()); break;
        case Verdict::Open: r.out("open: %s; else taken (%s)", out.open.c_str(), out.reason.c_str()); break;
    }
    if (out.v != Verdict::Merged && s.confirms.size() < 32)
        s.confirms.push_back(Confirm{o.q, o.slot, out.free_rec, o.id, o.kind, out.v, label, out.reason, out.open});
}

// The next frame: did the game take it? The record slot it would take holds the id and kind, or the runner runs the id.
void check_confirms(State& s) {
    const uint64_t er = runners();
    for (const Confirm& k : s.confirms) {
        if (k.slot < 0 || k.slot >= kSlots) continue;
        const uint64_t sb = k.q + static_cast<uint64_t>(k.slot) * kSlotSize;
        bool taken = false;
        if (k.free_rec >= 0 && guest::ok(sb + k.free_rec * kRecSize, kRecSize)) {
            const uint64_t ri = sb + k.free_rec * kRecSize;
            taken = guest::rd<uint16_t>(ri + kRecId) == k.id && guest::rd<uint32_t>(ri + kRecKind) == k.kind;
        }
        if (!taken && k.id && er && guest::ok(er + k.slot * kRunnerSize, kRunnerSize))
            taken = guest::rd<uint16_t>(er + k.slot * kRunnerSize + kRunEvent) == k.id && k.v != Verdict::Refused;
        if (k.v == Verdict::Open) {
            Rec(kArea, "offer resolved").in("%s", k.label.c_str()).out("%s", taken ? "taken" : "dropped").note("open was: %s", k.open.c_str());
        } else if ((k.v == Verdict::Taken) != taken) {
            Rec(kArea, "offer check")
                .in("%s", k.label.c_str())
                .out("%s", taken ? "taken" : "not in record slot or runner")
                .expect("%s", k.v == Verdict::Taken ? "taken" : "dropped")
                .ok(false)
                .note("predicted %s", k.reason.c_str());
        }
    }
    s.confirms.clear();
}

// ---- the other sites ----
void on_play_scene(::Ctx* c) {
    State& s = st();
    const uint64_t cmd = c->x[0];
    uint32_t w = 0;
    if (!guest::read(cmd + 4, w)) return;
    std::lock_guard<std::mutex> lk(s.m);
    s.stash = Stash{true, w & 0xffff, played(w & 0xffff), c->x[30], qpc::seconds()};
}

void check_stash(State& s, double now) {   // a play-scene look no offer took: the scene's event is not valid (or the scene id)
    if (!s.stash.on) return;
    const Stash k = s.stash;
    s.stash.on = false;
    if (!first(s, fmt("p%u", k.scene), "content play-scene repeated", fmt("scene=%u", k.scene), now)) return;
    Rec(kArea, "content play-scene")
        .in("scene=%u played=%d by=%s", k.scene, k.played, code(k.lr).c_str())
        .out("no offer followed (scene outside 1..0xbb7, its #487 event outside 1..0x18ff, or an exit of 0x2e6ce0)");
}

void on_record_removal(::Ctx* c) {
    State& s = st();
    const uint64_t list = c->x[0], rec = c->x[1];
    const uint32_t w2 = static_cast<uint32_t>(c->x[2]);
    if (!guest::ok(rec, 0x50) || !guest::ok(list, kListCount32 + 4)) return;
    // 0x132f00: +0xc bits 0..1 keep it; mode 1 stays unless w2 bit 0; else it leaves the list if it is in it
    const uint8_t bits = guest::rd<uint8_t>(rec + kCrBits);
    const uint32_t mode = guest::rd<uint32_t>(rec + kCrMode);
    const char* result = "removed";
    if (bits & 3) result = "kept (+0xc bits 0..1)";
    else if (!(w2 & 1) && mode == 1) result = "kept (mode 1)";
    else {
        bool found = false;
        uint64_t n = guest::rd<uint32_t>(list + kListCount32) ? guest::rd<uint64_t>(list + kListHead) : 0;
        for (int i = 0; n && i < 4096 && guest::ok(n, kLnNext + 8); ++i) {
            if (guest::rd<uint64_t>(n) == rec) {
                found = true;
                break;
            }
            n = guest::rd<uint64_t>(n + kLnNext);
            if (n == list) break;
        }
        if (!found) result = "not in its list";
    }
    const uint64_t gm = game_state();
    const uint64_t sc = gm ? guest::deref(gm + kContents) : 0;
    const uint32_t entry = guest::rd<uint32_t>(rec + kCrEntry);
    const uint16_t content = sc && entry < kEntryCount ? at<uint16_t>(sc + kEntries + entry * kEntrySize + kEntryContent) : 0;
    std::string where = fmt("0x%" PRIx64, list);
    if (sc && list >= sc + kLists && list < sc + kLists + kListCount * kListSize && (list - sc - kLists) % kListSize == 0)
        where = fmt("SC+0x%" PRIx64 " (list %" PRIu64 "%s)", list - sc, (list - sc - kLists) / kListSize, list - sc - kLists < 3 * kListSize ? ", polled" : "");
    const uint64_t prog = guest::rd<uint64_t>(rec + kCrProgress);
    const int pstate = guest::ok(prog, kProgState + 1) ? guest::rd<uint8_t>(prog + kProgState) : -1;
    const uint32_t rid = guest::rd<uint32_t>(rec + kCrId);

    std::lock_guard<std::mutex> lk(s.m);
    const double now = qpc::seconds();
    const bool lost = result[0] == 'r' && now - s.content_refused.at <= kLostWindow;
    if (!lost && !first(s, fmt("r%" PRIx64 ".%s", rec, result), "content record repeated", fmt("content=%u record=%u %s", content, rid, result), now))
        return;
    Rec r(kArea, "content record");
    r.in("content=%u entry=%u record=%u script=%d mode=%u +0xc=0x%x runs=%u +0x2c=0x%x progress=%d list=%s w2=%u by=%s", content, entry, rid,
         guest::rd<int32_t>(rec + kCrScript), mode, bits, guest::rd<uint16_t>(rec + kCrRuns), guest::rd<uint16_t>(rec + kCrFlags), pstate,
         where.c_str(), w2, code(c->x[30]).c_str());
    r.out("%s", result);
    if (lost) {   // linked by time only (the record that ran the command is not visible at 0x12b544)
        r.note("follows REFUSED content offer id=%u scene=%u %.2f s before (%s): likely its record, the scene is lost", s.content_refused.id,
               s.content_refused.scene, now - s.content_refused.at, s.content_refused.reason.c_str())
            .warn();
        s.content_refused = LastRefused{};
    }
}

// 0x1353a0: the notice kind and its payload (QUEST-START-RE section 4: offsets from SC).
struct Field {
    uint32_t off;
    uint8_t size;   // 1, 2 or 12 (a position: hex)
    const char* name;
};
struct Notice {
    uint32_t kind;
    const char* name;
    Field f[5];
};
constexpr Field kBlock{0x11aae4, 2, "block"}, kObj{0x11aae6, 2, "obj"}, kPos{0x11aafc, 12, "pos"}, kItem{0x11aad0, 2, "item"},
    kData{0x11aaea, 2, "data"}, kMove{0x11ab0c, 1, "move"};
constexpr Notice kNotices[] = {
    {0, "stage-entered", {{0x11ab38, 1, "type"}}},
    {1, "talked-to", {{0x11aaa0, 2, "data"}}},
    {2, "ended", {{0x11aaf2, 2, "scene"}, {0x11aaf4, 2, "id"}, {0x11aaf6, 1, "n"}}},
    {3, nullptr, {{0x11aaf2, 2, "scene"}, {0x11aaf8, 2, "c"}}},
    {4, nullptr, {{0x11aafa, 2, "n"}}},
    {5, "placed", {kBlock, kObj, kPos, {0x11ab15, 1, "result"}}},
    {6, nullptr, {kObj}},
    {7, "broken", {kBlock, kObj, kPos, kMove}},
    {8, nullptr, {}},
    {9, nullptr, {kObj, kPos}},
    {10, "item-got", {kItem}},
    {11, "item-used", {kItem}},
    {12, nullptr, {{0x11aad6, 2, "a"}, {0x11aad4, 2, "b"}, kPos}},
    {14, nullptr, {{0x11aad6, 2, "a"}, {0x11aad4, 2, "b"}, {0x11aad8, 1, "mode"}}},
    {15, "memory-piece", {{0x11ab12, 2, "n"}}},
    {17, "new-day", {{0x11ab16, 1, "v"}}},
    {18, "move-used", {kMove}},
    {19, "move-hit", {kMove, kData, {0x11ab17, 1, "flag"}, kObj, kBlock}},
    {20, nullptr, {kBlock}},
    {21, "habitat", {{0x11ab0e, 2, "habitat"}}},
    {22, nullptr, {kObj}},
    {23, nullptr, {{0x11ab10, 1, "n"}}},
    {25, nullptr, {{0x11ab11, 1, "n"}}},
    {26, nullptr, {kData}},
    {27, nullptr, {}},
    {28, nullptr, {{0x11aade, 1, "n"}}},
    {29, nullptr, {{0x11aad2, 2, "data"}, kItem}},
    {30, nullptr, {kData}},
    {31, nullptr, {kData, {0x11aadd, 1, "n"}}},
    {35, "time-period", {{0x11aaec, 1, "period"}}},
};

void on_notice(::Ctx* c) {
    State& s = st();
    const uint64_t sc = c->x[0], list = c->x[1];
    uint32_t kind = 0;
    if (!guest::read(sc + kNoticeKind, kind) || !guest::ok(sc + 0x11aaa0, 0xa0)) return;
    const Notice* n = nullptr;
    for (const Notice& k : kNotices)
        if (k.kind == kind) n = &k;
    std::string pay, ids;   // ids: the payload without positions, for the dedupe key
    if (n) {
        for (const Field& f : n->f) {
            if (!f.size) break;
            if (f.size == 12) pay += fmt(" %s=%s", f.name, hex_bytes(reinterpret_cast<const void*>(sc + f.off), 12).c_str());
            else {
                const std::string v = fmt(" %s=%u", f.name, f.size == 1 ? guest::rd<uint8_t>(sc + f.off) : guest::rd<uint16_t>(sc + f.off));
                pay += v;
                ids += v;
            }
        }
    } else {
        pay = ids = fmt(" +aaf2=%u +aaf4=%u", guest::rd<uint16_t>(sc + 0x11aaf2), guest::rd<uint16_t>(sc + 0x11aaf4));
    }
    const uint64_t gm = game_state();
    const bool story = gm && (at<uint8_t>(gm + kStoryOn) & 1);
    const std::string label = fmt("kind=%d list=SC+0x%" PRIx64 "%s", static_cast<int32_t>(kind), list - sc, ids.c_str());
    std::lock_guard<std::mutex> lk(s.m);
    if (!first(s, "n" + label, "contents notice repeated", label, qpc::seconds())) return;
    Rec r(kArea, "contents notice");
    r.in("kind=%d (%s) list=SC+0x%" PRIx64 " p3=%u p4=%u%s by=%s", static_cast<int32_t>(kind), n && n->name ? n->name : "-", list - sc,
         static_cast<uint32_t>(c->x[2]), static_cast<uint32_t>(c->x[3]), pay.c_str(), code(c->x[30]).c_str());
    if (!story) r.out("story gate off (GM+0x8c bit 0 clear): no record runs");
}

void on_end_writes(::Ctx* c) {
    const uint64_t run = c->x[0];
    if (!guest::ok(run, kRunnerSize)) return;
    const uint32_t scene = guest::rd<uint32_t>(run + kRunScene);
    Rec(kArea, "runner end-writes")
        .in("slot=%u event=%u scene=%u played-before=%d subject=0x%x", guest::rd<uint8_t>(run + kRunSlot), guest::rd<uint16_t>(run + kRunEvent),
            scene, played(scene), guest::rd<uint16_t>(run + kRunSubject))
        .out("%s", scene - 1 < kSceneCount ? "played bit, scene end actions, notices, quest flags, story bus here" : "no scene: event rows only")
        .step();
}

void on_chain_end(::Ctx* c) {
    const uint64_t run = c->x[0];
    if (!guest::ok(run, kRunnerSize)) return;
    Rec(kArea, "runner chain-end")
        .in("slot=%u last-event=%u subject=0x%x", guest::rd<uint8_t>(run + kRunSlot), guest::rd<uint16_t>(run + kRunEvent),
            guest::rd<uint16_t>(run + kRunSubject))
        .step();
}

void poll_runners(State& s) {
    const uint64_t er = runners();
    if (!er || !guest::ok(er, kRunnerSize * kSlots)) return;
    for (int i = 0; i < kSlots; ++i) {
        const uint64_t run = er + i * kRunnerSize;
        const uint32_t next = guest::rd<uint32_t>(run + kRunNext);
        const uint16_t event = guest::rd<uint16_t>(run + kRunEvent);
        Runner& was = s.run[i];
        const bool idle = next >= kRunIdle, was_idle = !was.seen || was.next >= kRunIdle;
        if ((was.seen && ((idle && was_idle) || (next == was.next && event == was.event))) || (!was.seen && idle)) {
            was = Runner{true, next, event};
            continue;
        }
        const uint32_t scene = guest::rd<uint32_t>(run + kRunScene);
        Rec r(kArea, "runner");
        r.in("slot=%d %s(%d) -> %s(%d) event=%u scene=%u played=%d cmd=%d +8=%d subject=0x%x", i, was.seen ? state_name(was.next) : "?",
             was.seen ? static_cast<int>(was.next) : -1, state_name(next), static_cast<int>(next), event, scene, played(scene),
             guest::rd<int32_t>(run + kRunCmd), static_cast<int>(guest::rd<uint32_t>(run + kRunNow)), guest::rd<uint16_t>(run + kRunSubject));
        if (was.seen && was.next == 0 && next == 6) r.note("abort: an actor of the event is not there (0x47bf80)");
        else if (was.seen && next == 6 && was.next != 5) r.note("6 without 5 (an abort from 0x47bf80 within one frame, or a state not seen)");
        r.step();
        was = Runner{true, next, event};
    }
}

void game_frame(::Ctx*, bool) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    const double now = qpc::seconds();
    check_confirms(s);
    check_stash(s, now);
    poll_runners(s);
    flush_seen(s, now);
}

void reset() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    s.seen.clear();
    s.confirms.clear();
    s.stash = Stash{};
    for (Runner& r : s.run) r = Runner{};
    s.content_refused = LastRefused{};
}

constexpr WatchSite kSites[] = {
    {{0x45d5a0, kPacibsp, true}, &on_offer, "event queue enqueue 0x45d5a0"},
    {{0x12b544, 0xb9400408u, true}, &on_play_scene, "content play scene 0x12b544"},      // ldr w8, [x0, #4]
    {{0x132f00, 0x39403028u, true}, &on_record_removal, "content record removal 0x132f00"},   // ldrb w8, [x1, #0xc]
    {{0x1353a0, kPacibsp, true}, &on_notice, "contents notifier 0x1353a0"},
    {{0x47e140, kPacibsp, true}, &on_end_writes, "runner state 4 0x47e140"},
    {{0x47bd00, kPacibsp, true}, &on_chain_end, "runner chain end 0x47bd00"},
};

}  // namespace

const Watcher kEventsWatcher = {"events", kSites, sizeof kSites / sizeof kSites[0], &game_frame, &reset};

}  // namespace nxrt::games::pkp::journal
