// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's talk watcher (pkp_journal_watch.h): one j.talk record per talk press, joining what the press read, the
// path the talk handler took and the event chain it started, so two players' different answers from one character show their cause.
//
// The game, main 2.0.1a (read from main.img; co-op's own reading is in pkp_coop_talk.cpp):
//   0x3180e0(obj, w1 slot, x2 &u16 actor id, w3 flags)  the one real talk start (A press 0x31347c w3 = 1; event callback 11 0x48fd50
//                         w3 = 0). Its own busy test first: T = [[main+0x34842a0]], byte T+0x10 set, u16 T+0x18 == the actor id and the
//                         slot's player (PM +0x30 slot 0, +0x38 else; u16 +0x27fe actor id) != u16 T+0x1a -> line 0xf1 via 0xa8fba0, no
//                         talk. Else it builds the talk request at its sp+8 (= entry sp - 0x398: frame 0x40 + 0x360), u16 req+6 = [main+
//                         0x3482ef8], calls the selector 0x4c3ec8(obj, slot, &id, req, 0, 0) and the talk event call 0x4bf7c0(.., slot,
//                         req, flags & 1). Co-op's TalkStart (after this hook) may end the chain with 0: no 0x3181cc then.
//   0x4c3ec8              the selector: x19 = x3 = req (0x4c3ef4); stages in order, the first returning 1 claims the talk: 0x4c40b8,
//                         0x4c4320, 0x4c4b4c, the talk handler 0x4c024c (0x4c3f78, x3 = x19), 0x4c4dc4, 0x4c5048, 0x4c5554, 0x4be0a4,
//                         0x4c612c, 0x4c62a0, 0x4c6f1c, 0x4c7094, 0x4c7260, 0x4c8120, then the tail call 0x4c8ba0.
//   0x4c06d8 (mid)        in the talk handler 0x4c024c (x19 = x3 = req at 0x4c0274, its only x19 write before here; x20 &actor id), online
//                         and outside Cloud Island: `orr w8, w25, w0`, w0 = IsHost 0xa40bc. w8 bit 0 clear -> the visitor stub 0x4c07e4
//                         (kinds 10 / 11); set -> the host's path (0x4c06ec kind 3, or the character's talk table 0xa59b00 / 0x3af8bc).
//                         Co-op's TalkKind (after this hook) sets w0 = 1 on a member, so w0 here is the game's own answer and the kind at
//                         0x3181cc says which path ran. Joined to the press by x19 == 0x3180e0's sp+8 (the same req: 0x318170
//                         `add x3, sp, #8` -> 0x4c3ec8 -> 0x4c024c).
//   0x3181cc (mid)        `mov w0, wzr` after 0x4bf7c0: sp+8 the req (u16 +2 kind, u16 +6 target, u32 +8), w22 bit 0 = the selector
//                         claimed a talk (clear: the function returns 0, no talk), w19 the slot
//   0x47bd00(x0 runner)   the end of a slot's event chain: u8 +0x2000 slot, u32 +0x10 the last event, u16 +0x1a86 subject
//   0x4cc1a8 / 0x4cd518 (entry)  a candidate talk row's condition set (w1 slot, w2 set id = the candidate's u8 +8, x3 &u16 actor id):
//                         0x4c8ba0 calls 0x4cc1a8 at 0x4c8e24 / 0x4c8fa0 / 0x4c92d4 (candidates from 0xa598c0 / 0xa59920) and adds a
//                         passing one with 0x4ccbc0; 0x4be0a4 calls 0x4cd518. Both: no actor or not a resident (vt+0x1d8) -> 1; the
//                         set's record = row(set) of the table at +0x580 / +0x278 (or the default [main+0x34853c0] / [main+0x34853a0]),
//                         19 entries of 3 bytes (key, cond A, cond B) at +0xc / +0x13; the first entry whose key == the resident's
//                         u8 +0x398e decides: A && B, each `blr [main+0x34c2958 + id*8]`(w0 slot, x1 &actor id copy); an id outside
//                         1..0xdf passes, a null handler fails; no key match -> 1. Frame 0x40, no other sp change.
//   0x4cc280 / 0x4cd5f0 (mid, the first `ldrb w11, [x9, #0xc / #0x13]`)  x9 the set's record (default applied), w10 the key byte
//   0x4ccb7c / 0x4cdeec (mid, `cmp w21, #0` after cond B)          w21 cond A's result, w0 cond B's
//   0x4ccb88 / 0x4cdef8 (mid, `cmp w21, #0` before `cset w0, ne`)  w21 the set's result
// The sets read under a press (same slot, sp below 0x3180e0's entry sp, before 0x3181cc) go into its record as one list; the
// selector's probes outside a press run the same sets every frame and are not logged here.
// The character (actor, data id, idx, co-op's key, species, talk key) is read at the press by memory (pkp_journal_actor.h); a set's
// triples and key byte come from the game's own registers at the record site.
// Identical presses (same character, path, sets and results) within kRepeatGap are counted; the count goes on the next line.
// Talk conditions (A-unhooked sites of the retired story co-op, read by the talk selector's probes every frame, so deduped on
// (condition, result, role byte); the character is not in a register at these sites):
//   0x4cfe20 (mid, `mov w8, #1` after IsOnline)  condition 0x19 "not online": w0 = IsOnline, result !w0
//   0x4d4860 (mid, `tbnz w0`)                     condition 0x60 offline house check: w0 = IsOnline, online -> 0
//   0x4d04fc (entry, `b 0x207b0`)                 condition 0x22 = IsOnline: result = GM+0x90 bit 0
#include <cinttypes>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
#include "core/pkp_journal_actor.h"
#include "core/pkp_journal_watch.h"
#include "core/pkp_layout.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::journal {

namespace {

constexpr uint64_t kTalkBusy = 0x34842a0;       // main + : -> -> the talk-busy record (u8 +0x10 on, u16 +0x18 actor, u16 +0x1a talker)
constexpr uint64_t kPlayerActor = 0x27fe;       // player character: u16 its actor id
constexpr uint64_t kReqFromEntrySp = 0x398;     // 0x3180e0: req = entry sp - this (sp+8 after the 0x3a0 frame)
constexpr uint64_t kReqAtRecord = 8;            // 0x3181cc: req = sp + this
constexpr uint64_t kRunnerSlot = 0x2000, kRunnerEvent = 0x10, kRunnerSubject = 0x1a86;
constexpr uint16_t kStubKindA = 10, kStubKindB = 11;   // the visitor stub's kinds (0x4c07e4)
constexpr uint8_t kSlots = 2;
constexpr double kChainTimeout = 120.0;         // a started talk whose chain end was not seen by then is written as such
constexpr double kRepeatGap = 30.0;             // a deduped condition or press writes its repeat count at most this often

// The two condition-set functions: their entry and the offset of the first (key, A, B) triple in a set's record.
struct SetFn {
    uint64_t entry, first;
};
constexpr SetFn kSetFns[] = {{0x4cc1a8, 0xc}, {0x4cd518, 0x13}};
constexpr int kSetEntries = 19;          // (key, cond A, cond B) triples
constexpr uint64_t kSetFrame = 0x40;     // both: `sub sp, sp, #0x40`, no other sp change
constexpr size_t kMaxEvals = 48;         // sets kept per press (more are counted)
constexpr uint8_t kCondHost = 0x16;      // 0x4cfb08 `b IsHost`: false on every member (P0-RE 12)

// Talk conditions that read the session (P0-RE 12, conds2.py, depth 0): the getter each reads, for the line's tag.
const char* role_read(uint8_t id) {
    switch (id) {
    case 0x16: return "IsHost";
    case 0x19: case 0x22: case 0x5f: case 0x60: return "IsOnline";
    case 0x1a: case 0x1b: case 0x25: case 0x2a: case 0x2b: case 0x2e: case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
    case 0x62: case 0x63: case 0x7f: return "IsVisitor";
    default: return nullptr;
    }
}
bool cond_valid(uint8_t id) { return id - 1u <= 0xdeu; }

// One condition set read under a press.
struct Eval {
    uint8_t fn = 0;            // index in kSetFns
    uint8_t set = 0;           // w2 & 0xff
    uint64_t from = 0, sp = 0; // caller (main-relative), entry sp
    int key = -1;              // the key byte the triples are matched on (w10 at the record site), -1 not seen
    int entry = -2;            // the entry that decides: -1 no key matched, -2 the record site not seen
    uint8_t ca = 0, cb = 0;    // its conditions
    int ra = -1, rb = -1;      // their results (-1 not seen)
    int res = -1;              // the set's result (-1: returned before the end site)
    bool resident = true;
    bool open = true;          // the call still running: calls are sequential, so only the press's last eval can be open
};

enum class Phase : uint8_t { None, Pressed, Talking };

struct Press {
    Phase phase = Phase::None;
    uint16_t actor = 0;
    uint32_t flags = 0;
    uint64_t from = 0;
    uint64_t req = 0;
    uint8_t bits = 0;
    bool game_busy = false;
    bool handler = false;       // 0x4c06d8 seen for this press's req
    uint32_t ishost = 0, w25 = 0;
    uint16_t kind = 0, target = 0;
    uint32_t req8 = 0, sel = 0;
    double at = 0.0;
    uint64_t sp = 0;            // 0x3180e0's entry sp: sets read below it belong to this press
    actor::Character ch;
    std::vector<Eval> evals;
    uint32_t evals_more = 0;    // past kMaxEvals
    struct Stage2 {
        uint8_t row, cond, res;
    };
    std::vector<Stage2> stage2;   // the selector's stage 2 (0x4c4320): (candidate row, condition id, result), every press
    uint32_t stage2_more = 0;                          // past kMaxEvals
    uint32_t end_event = 0;
    uint16_t end_subject = 0;
};

struct Last {   // the slot's last written press, for the identical-press count
    std::string sig;
    double at = -1e9;
    uint32_t repeats = 0;
};

struct State {
    std::mutex m;
    Press slot[kSlots];
    Last last[kSlots];
    struct Rep {
        uint32_t count = 0;
        double since = 0.0;
    };
    std::unordered_map<uint32_t, Rep> conds;
};
State& st() {
    static State s;
    return s;
}

uint8_t session_byte() {
    const uint64_t gm = guest::deref(base() + layout::kGameState);
    uint8_t b = 0;
    return gm && guest::read(gm + layout::kStateSession, b) ? b : 0;
}

const char* role_of(uint8_t b) {
    if (!b) return "offline";
    if (b & layout::kSessionVisitor) return "visitor";
    if (b & layout::kSessionHost) return "host";
    return "online";
}

bool stub_kind(uint16_t k) { return k == kStubKindA || k == kStubKindB; }

// The game's own busy test of 0x3180e0, recomputed at entry from the same fields.
bool game_busy(uint8_t slot, uint16_t actor) {
    const uint64_t t = guest::deref(guest::deref(base() + kTalkBusy));
    uint8_t on = 0;
    uint16_t who = 0, talker = 0;
    if (!t || !guest::read(t + 0x10, on) || !on || !guest::read(t + 0x18, who) || who != actor) return false;
    guest::read(t + 0x1a, talker);
    const uint64_t pm = guest::deref(base() + layout::kPlayerManager);
    const uint64_t player = pm ? guest::deref(pm + layout::kManagerPlayer + (slot ? 8 : 0)) : 0;
    uint16_t mine = 0;
    return player && guest::read(player + kPlayerActor, mine) && mine != talker;
}

// "set 0x12 via 0x4cc1a8@0x4c8e28: e2 0x19/IsOnline=1 0x22/IsOnline=0 -> 0" (no commas: journal::list joins items with them).
std::string eval_text(const Eval& e) {
    std::string t = fmt("set 0x%x via 0x%" PRIx64 "@0x%" PRIx64 ":", e.set, kSetFns[e.fn].entry, e.from);
    if (!e.resident) t += " not a resident";
    else if (e.entry == -2) t += " entry ?";
    else if (e.entry == -1) t += fmt(" key %d no match", e.key);
    else {
        const auto one = [](uint8_t id, int r) {
            if (!cond_valid(id)) return fmt(" 0x%x(none)", id);
            const char* role = role_read(id);
            return fmt(" 0x%x%s%s=%s", id, role ? "/" : "", role ? role : "", r < 0 ? "?" : r ? "1" : "0");
        };
        t += fmt(" key %d e%d", e.key, e.entry) + one(e.ca, e.ra) + one(e.cb, e.rb);
    }
    t += e.res < 0 ? " -> early" : e.res ? " -> 1" : " -> 0";
    return t;
}

// The one record of a press (st().m held). why: how it ended.
void write(Press& p, Last& last, uint8_t slot, const char* why, const std::string& chain) {
    const bool member = p.bits & layout::kSessionVisitor;
    const bool reached = p.phase == Phase::Talking || p.kind || p.target;
    std::vector<std::string> sets;
    sets.reserve(p.evals.size() + 1);
    int passed = 0, host_fail = 0;
    std::string sig = fmt("%x %x %x %u %u %u %u %u %u %x %x %u %u %s %u %x|", p.actor, p.ch.key, p.flags, p.bits, p.game_busy,
                          p.handler, p.ishost & 1, p.w25 & 1, p.kind, p.target, p.req8, p.sel & 1, slot, why, p.end_event,
                          p.end_subject);
    for (const Eval& e : p.evals) {
        sets.push_back(eval_text(e));
        sig += sets.back() + ";";
        passed += e.res > 0;
        if (e.entry >= 0 && ((e.ca == kCondHost && e.ra == 0) || (e.cb == kCondHost && e.rb == 0))) ++host_fail;
    }
    if (p.evals_more) sets.push_back(fmt("+%u more sets", p.evals_more));
    if (!p.stage2.empty()) {
        std::string t = "stage 2 conditions:";
        for (const Press::Stage2& e : p.stage2) {
            const char* role = role_read(e.cond);
            t += fmt(" row %u 0x%x%s%s=%u", e.row, e.cond, role ? "/" : "", role ? role : "", e.res);
            if (e.cond == kCondHost && !e.res) ++host_fail;
        }
        if (p.stage2_more) t += fmt(" +%u more", p.stage2_more);
        sets.push_back(t);
        sig += t + ";";
    }
    const double now = qpc::seconds();
    if (sig == last.sig && now - last.at < kRepeatGap) {
        ++last.repeats;
        p = Press{};
        return;
    }
    const uint32_t repeats = last.repeats;
    last = Last{sig, now, 0};
    const std::string list_name = fmt("press sets slot %u", slot);
    if (!sets.empty()) list(Area::Talk, list_name.c_str(), sets);
    Rec rec(Area::Talk, "press");
    rec.in("%s slot=%u flags=0x%x(%s) from 0x%" PRIx64 " role=0x%02x (%s)%s", actor::describe(p.ch).c_str(), slot, p.flags,
           p.flags & 1 ? "press" : "chained", p.from, p.bits, role_of(p.bits), p.game_busy ? " game-busy=1" : "");
    if (p.handler)
        rec.in("handler: IsHost w0=%u w25=%u -> game path %s", p.ishost & 1, p.w25 & 1,
               (p.ishost | p.w25) & 1 ? "host" : "visitor stub 0x4c07e4");
    if (!p.evals.empty()) rec.in("sets: %zu read, %d passed (\"%s\" above)", p.evals.size(), passed, list_name.c_str());
    if (!p.stage2.empty()) rec.in("stage 2: %zu conditions read (\"%s\" above)", p.stage2.size(), list_name.c_str());
    if (reached)
        rec.out("kind=%u target=0x%x req+8=0x%x selector=%u -> %s", p.kind, p.target, p.req8, p.sel & 1,
                stub_kind(p.kind) ? "visitor stub" : "host path");
    rec.out("%s%s", why, chain.c_str());
    if (member && (reached || host_fail)) {
        bool good = true;
        if (reached) {
            rec.expect("host path (kind not 10/11)");
            good = !stub_kind(p.kind);
        }
        if (host_fail) {
            rec.expect("cond 0x16 IsHost = 1 (the host's answer) in %d set(s) that read 0", host_fail);
            good = false;
        }
        rec.ok(good);
    } else if (p.game_busy) {
        rec.refused("the character talks to another player (line 0xf1)");
    }
    if (repeats) rec.note("the previous line's press came x%u more (identical)", repeats);
    p = Press{};
}

// A press that never reached 0x3181cc returned before the selector (0x3181cc follows 0x4c3ec8 and 0x4bf7c0 unconditionally), so sets
// read since then (later probes on the slot) are not its own.
void write_early(Press& p, Last& last, uint8_t slot) {
    p.evals.clear();
    p.stage2.clear();
    p.evals_more = 0;
    write(p, last, slot, "returned before the talk event call (co-op lock or the game's busy test)", "");
}

void on_talk_start(::Ctx* c) {   // entry 0x3180e0
    const uint8_t slot = static_cast<uint8_t>(c->x[1]);
    if (slot >= kSlots) return;
    uint16_t actor = 0;
    guest::read(c->x[2], actor);
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    Press& p = s.slot[slot];
    Last& last = s.last[slot];
    if (p.phase == Phase::Pressed) write_early(p, last, slot);
    else if (p.phase == Phase::Talking) write(p, last, slot, "talk started", "; a new talk start came before its chain end");
    p.phase = Phase::Pressed;
    p.actor = actor;
    p.flags = static_cast<uint32_t>(c->x[3]);
    p.from = c->x[30] >= base() ? c->x[30] - base() : c->x[30];
    p.req = c->x[31] - kReqFromEntrySp;
    p.sp = c->x[31];
    p.bits = session_byte();
    p.game_busy = game_busy(slot, actor);
    p.ch = actor::character(actor);
    p.at = qpc::seconds();
}

void on_talk_kind(::Ctx* c) {   // mid 0x4c06d8: before co-op's TalkKind
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    for (Press& p : s.slot)
        if (p.phase == Phase::Pressed && p.req == c->x[19]) {
            p.handler = true;
            p.ishost = static_cast<uint32_t>(c->x[0]);
            p.w25 = static_cast<uint32_t>(c->x[25]);
            return;
        }
}

void on_talk_record(::Ctx* c) {   // mid 0x3181cc
    const uint8_t slot = static_cast<uint8_t>(c->x[19]);
    if (slot >= kSlots) return;
    const uint64_t req = c->x[31] + kReqAtRecord;
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    Press& p = s.slot[slot];
    if (p.phase != Phase::Pressed) return;
    guest::read(req + 2, p.kind);
    guest::read(req + 6, p.target);
    guest::read(req + 8, p.req8);
    p.sel = static_cast<uint32_t>(c->x[22]);
    if (!(p.sel & 1)) {
        write(p, s.last[slot], slot, "no talk started (the selector claimed none)",
              fmt("; 0x4bf7c0 w0=%u", static_cast<uint32_t>(c->x[0])));
        return;
    }
    p.phase = Phase::Talking;
}

void on_talk_end(::Ctx* c) {   // entry 0x47bd00
    const uint64_t runner = c->x[0];
    uint8_t slot = 0;
    if (!guest::read(runner + kRunnerSlot, slot) || slot >= kSlots) return;
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    Press& p = s.slot[slot];
    if (p.phase != Phase::Talking) return;
    guest::read(runner + kRunnerEvent, p.end_event);
    guest::read(runner + kRunnerSubject, p.end_subject);
    const std::string chain =
        fmt("; chain end: last event=%u subject=0x%x after %.1f s", p.end_event, p.end_subject, qpc::seconds() - p.at);
    write(p, s.last[slot], slot, "talk ran", chain);
}

// Entry of a condition-set function: the call, kept on the press it runs under.
void on_set(::Ctx* c, uint8_t fn) {
    const uint32_t slot = static_cast<uint32_t>(c->x[1]);
    if (slot >= kSlots) return;
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    Press& p = s.slot[slot];
    if (p.phase != Phase::Pressed || c->x[31] >= p.sp) return;
    if (!p.evals.empty()) p.evals.back().open = false;   // the previous call returned (early, if its end site was not seen)
    if (p.evals.size() >= kMaxEvals) {
        ++p.evals_more;
        return;
    }
    Eval e;
    e.fn = fn;
    e.set = static_cast<uint8_t>(c->x[2]);
    e.from = c->x[30] >= base() ? c->x[30] - base() : c->x[30];
    e.sp = c->x[31];
    uint16_t id = 0;
    if (guest::read(c->x[3], id)) {
        const actor::Character ch = id == p.actor ? p.ch : actor::character(id);
        e.resident = !ch.read || ch.resident;
    }
    p.evals.push_back(e);
}

// The open eval of the set function call whose entry sp is `sp` (st().m held), or null.
Eval* eval_at(uint8_t fn, uint64_t sp) {
    for (Press& p : st().slot)
        if (p.phase == Phase::Pressed && !p.evals.empty()) {
            Eval& e = p.evals.back();
            if (e.open && e.fn == fn && e.sp == sp) return &e;
        }
    return nullptr;
}
void on_set_rec(::Ctx* c, uint8_t fn) {   // x9 the record, w10 the key byte, before the first triple is read
    std::lock_guard<std::mutex> lk(st().m);
    Eval* e = eval_at(fn, c->x[31] + kSetFrame);
    if (!e) return;
    e->resident = true;
    e->key = static_cast<int>(c->x[10] & 0xff);
    uint8_t tri[kSetEntries * 3] = {};
    if (!guest::read(c->x[9] + kSetFns[fn].first, tri, sizeof tri)) return;
    e->entry = -1;
    for (int i = 0; i < kSetEntries; ++i)
        if (tri[i * 3] == e->key) {
            e->entry = i;
            e->ca = tri[i * 3 + 1];
            e->cb = tri[i * 3 + 2];
            break;
        }
}
void on_set_b(::Ctx* c, uint8_t fn) {   // after cond B: w21 cond A, w0 cond B
    std::lock_guard<std::mutex> lk(st().m);
    if (Eval* e = eval_at(fn, c->x[31] + kSetFrame)) {
        e->ra = static_cast<int>(c->x[21] & 1);
        e->rb = static_cast<int>(c->x[0] & 1);
    }
}
void on_set_end(::Ctx* c, uint8_t fn) {   // w21: the set's result
    std::lock_guard<std::mutex> lk(st().m);
    if (Eval* e = eval_at(fn, c->x[31] + kSetFrame)) {
        e->res = static_cast<uint32_t>(c->x[21]) != 0;
        e->open = false;
        if (e->entry >= 0 && e->ra < 0 && !cond_valid(e->cb)) e->ra = e->res;   // no cond B: the result is cond A's
    }
}
// 0x4c4820 (mid, `tbz w0, #0` after the condition call in the selector's stage 2 0x4c4320): w21 the slot, w19 the condition id - 1
// (the row record's byte +6, 0x4c47f4), w0 its result, u8 sp+0x2c the candidate row (strb w26 at 0x4c477c; table slot 0x122). Runs on every press, also the ones a later stage never reaches (the talk handler 0x4c024c claims them at stage 4).
void on_stage2(::Ctx* c) {
    const uint32_t slot = static_cast<uint32_t>(c->x[21]);
    if (slot >= kSlots) return;
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    Press& p = s.slot[slot];
    if (p.phase != Phase::Pressed || c->x[31] >= p.sp) return;
    if (p.stage2.size() >= kMaxEvals) {
        ++p.stage2_more;
        return;
    }
    uint8_t row = 0xff;
    guest::read(c->x[31] + 0x2c, row);
    p.stage2.push_back({row, static_cast<uint8_t>(c->x[19] + 1), static_cast<uint8_t>(c->x[0] & 1)});
}

void on_set_rows(::Ctx* c) { on_set(c, 0); }
void on_set_rows_rec(::Ctx* c) { on_set_rec(c, 0); }
void on_set_rows_b(::Ctx* c) { on_set_b(c, 0); }
void on_set_rows_end(::Ctx* c) { on_set_end(c, 0); }
void on_set_early(::Ctx* c) { on_set(c, 1); }
void on_set_early_rec(::Ctx* c) { on_set_rec(c, 1); }
void on_set_early_b(::Ctx* c) { on_set_b(c, 1); }
void on_set_early_end(::Ctx* c) { on_set_end(c, 1); }

// One talk condition's answer, deduped on (condition, result, role byte).
void condition(uint32_t cond, bool result, const char* what) {
    const uint8_t bits = session_byte();
    const uint32_t key = (cond << 16) | (result ? 0x100u : 0u) | bits;
    State& s = st();
    uint32_t repeats = 0;
    {
        std::lock_guard<std::mutex> lk(s.m);
        const double now = qpc::seconds();
        auto [it, fresh] = s.conds.try_emplace(key, State::Rep{0, now});
        if (!fresh) {
            ++it->second.count;
            if (now - it->second.since < kRepeatGap) return;
            repeats = it->second.count;
            it->second = State::Rep{0, now};
        }
    }
    Rec rec(Area::Talk, "condition");
    rec.in("cond=0x%x (%s) role=0x%02x (%s)", cond, what, bits, role_of(bits)).out("%u", result ? 1u : 0u);
    rec.note("differs by role: a host and a member read it differently");
    if (repeats) rec.note("x%u more since the last line", repeats);
}
void on_cond_not_online(::Ctx* c) { condition(0x19, !(c->x[0] & 1), "not online"); }
void on_cond_offline_house(::Ctx* c) { condition(0x60, !(c->x[0] & 1), "offline house check"); }
void on_cond_online(::Ctx*) { condition(0x22, session_byte() & layout::kSessionOnline, "online"); }

constexpr WatchSite kSites[] = {
    {{0x3180e0, kPacibsp, true}, &on_talk_start, "talk start 0x3180e0"},
    {{0x4c06d8, 0x2a000328u, false}, &on_talk_kind, "talk handler IsHost 0x4c06d8"},   // orr w8, w25, w0
    {{0x3181cc, 0x2a1f03e0u, false}, &on_talk_record, "talk record 0x3181cc"},         // mov w0, wzr
    {{0x47bd00, kPacibsp, true}, &on_talk_end, "talk chain end 0x47bd00"},
    {{0x4cfe20, 0x52800028u, false}, &on_cond_not_online, "talk condition 0x19 0x4cfe20"},       // mov w8, #1
    {{0x4d4860, 0x37001da0u, false}, &on_cond_offline_house, "talk condition 0x60 0x4d4860"},    // tbnz w0, #0, 0x4d4c14
    {{0x4d04fc, 0x17ed40adu, true}, &on_cond_online, "talk condition 0x22 0x4d04fc"},           // b 0x207b0
    {{0x4c4820, 0x3607f920u, false}, &on_stage2, "talk selector stage 2 condition 0x4c4820"},   // tbz w0, #0, 0x4c4744
    {{0x4cc1a8, kPacibsp, true}, &on_set_rows, "talk condition set 0x4cc1a8"},
    {{0x4cc280, 0x3940312bu, false}, &on_set_rows_rec, "talk condition set 0x4cc1a8 record 0x4cc280"},   // ldrb w11, [x9, #0xc]
    {{0x4ccb7c, 0x710002bfu, false}, &on_set_rows_b, "talk condition set 0x4cc1a8 cond B 0x4ccb7c"},     // cmp w21, #0
    {{0x4ccb88, 0x710002bfu, false}, &on_set_rows_end, "talk condition set 0x4cc1a8 end 0x4ccb88"},      // cmp w21, #0
    {{0x4cd518, kPacibsp, true}, &on_set_early, "talk condition set 0x4cd518"},
    {{0x4cd5f0, 0x39404d2bu, false}, &on_set_early_rec, "talk condition set 0x4cd518 record 0x4cd5f0"},  // ldrb w11, [x9, #0x13]
    {{0x4cdeec, 0x710002bfu, false}, &on_set_early_b, "talk condition set 0x4cd518 cond B 0x4cdeec"},    // cmp w21, #0
    {{0x4cdef8, 0x710002bfu, false}, &on_set_early_end, "talk condition set 0x4cd518 end 0x4cdef8"},     // cmp w21, #0
};

// Once a frame: a press still at its start returned early (no 0x3181cc came on the same call); a talk whose chain end is overdue.
void poll(::Ctx*, bool) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    const double now = qpc::seconds();
    for (uint8_t i = 0; i < kSlots; ++i) {
        Press& p = s.slot[i];
        if (p.phase == Phase::Pressed)
            write_early(p, s.last[i], i);
        else if (p.phase == Phase::Talking && now - p.at > kChainTimeout)
            write(p, s.last[i], i, "talk started", fmt("; no chain end after %.0f s", kChainTimeout));
    }
}

void reset() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    for (Press& p : s.slot) p = Press{};
    for (Last& l : s.last) l = Last{};
    s.conds.clear();
}

}  // namespace

const Watcher kTalkWatcher = {"talk", kSites, sizeof kSites / sizeof kSites[0], &poll, &reset};

}  // namespace nxrt::games::pkp::journal
