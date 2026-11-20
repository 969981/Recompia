// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's travel watcher (pkp_journal_watch.h): area trips, stage loads, fades and the session's own ends, phase by
// phase, so a crash or a stuck trip shows WHICH step it was in. Every line is a step (flushed at once, kept for the crash report).
// Facts: modding-docs/coop-v2/TRAVEL-RE.md, P0-RE.md (798-804), coop/SEAMLESS-COOP-RE.md (the record), coop-session/LEAVE-DOORS-RE.md.
// Parts:
//   the polls (game_frame, compare, write on change): the stage-change record GM+0x3bb600, the stage GM+0x18, the frame state GM+0x84828
//   the entry sites: the trip filers, the trip manager's submits, the stage loader, the fades, the callbacks of the map move and the
//     self-disconnect funnel. A site whose arguments are not mapped logs x0..x3 raw and says so.
// The per-slot load phases (ER+slot*0x2010 +8 / +0xc) are the events watcher's, not logged here. Everything here only reads.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
#include "core/pkp_journal_watch.h"
#include "core/pkp_layout.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp::journal {

namespace {

constexpr Area kArea = Area::Travel;

// GM = *(main+layout::kGameState). The stage-change record (TRAVEL-RE 1, SEAMLESS-COOP-RE 5): +0 u32 phase, +4 u32 kind, +8 stage,
// +9 arg, +0xc u32, +0x10..+0x15 flags.
constexpr uint64_t kRecord = 0x3bb600;
constexpr size_t kRecordBytes = 0x16;
constexpr uint32_t kIdle = 0xffffffffu;
constexpr uint32_t kRecordReady = 5;               // a filer files only from phase 5 up (below it the filers refuse silently)
constexpr uint64_t kFrameState = 0x84828;          // GM: u32, the game frame's state: 1..0x10 boot steps, 0x11 the simulation (P0-RE 889)
constexpr uint64_t kSessionBackup = 0x10;          // GM: u8 low byte of the u16 the stage entry sets (0xbe5f0 tests & 0x81)
constexpr uint64_t kTripRequest = 0x48;            // GM: the trip request 0xbcde0 fills and 0x113b460 submits (0x48 bytes up to GM+0x90)
constexpr size_t kTripRequestBytes = 0x48;
constexpr uint64_t kTravelObject = 0x3565120;      // *(main+..): the map's travel object: +0x45 target stage, +0x97 player (TRAVEL-RE 2)
constexpr uint8_t kMaxStage = 31;

struct Poll {
    bool seen = false;
    uint8_t rec[kRecordBytes] = {};
    double t0 = -1.0;               // when the record left idle (log_elapsed seconds); kept until the stage change takes it, -1 none
    uint8_t stage = 0;
    uint32_t frame_state = 0;
    bool frame_seen = false;
    double since_stage = 0.0;       // when the stage last changed
};
Poll g_poll;
std::atomic<double> g_t0{-1.0};     // g_poll.t0 while the record is active, for the hooks (a hook may run on another thread than the poll)

// A site that can run every frame in a session: one line per key change, the repeat count on a change and every 30 s at most.
struct Dedupe {
    uint64_t key = 0;
    bool have = false;
    uint32_t n = 0;
    double last = 0.0;
};
constexpr double kRepeatEvery = 30.0;

double now() { return log_elapsed(); }

uint64_t gm() {
    const uint64_t g = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(g, kRecord + 0x40) ? g : 0;
}

uint64_t lr(::Ctx* c) { return c->x[30] - base(); }

const char* phase_name(uint32_t p) {
    switch (p) {   // SEAMLESS-COOP-RE 5: the record's phases
        case kIdle: return "idle";
        case 0: return "delay";
        case 1: return "wait";
        case 2: return "wait-status";
        case 3: return "act";
        case 4: return "done";
        default: return "?";
    }
}

const char* kind_name(uint32_t k) {   // only the kinds the docs name (TRAVEL-RE 1, 4; SEAMLESS-COOP-RE 5; P0-RE 793)
    switch (k) {
        case 0: return "member-move";
        case 1: return "host-move";
        case 2: return "stage-move-received";
        case 4: return "own-stage-cleanup";
        case 6: return "leave-online";
        default: return "";
    }
}

std::string kind_text(uint32_t k) {
    const char* n = kind_name(k);
    return *n ? fmt("%u(%s)", k, n) : fmt("%u", k);
}

std::string phase_text(uint32_t p) { return p == kIdle ? std::string("idle(-1)") : fmt("%u(%s)", p, phase_name(p)); }

std::string rec_text(const uint8_t* r) {
    uint32_t phase, kind, f0c;
    std::memcpy(&phase, r, 4);
    std::memcpy(&kind, r + 4, 4);
    std::memcpy(&f0c, r + 0xc, 4);
    return fmt("phase=%s kind=%s stage=%u arg=0x%02x f0c=0x%x b10..15=%02x %02x %02x %02x %02x %02x", phase_text(phase).c_str(),
               kind_text(kind).c_str(), r[8], r[9], f0c, r[0x10], r[0x11], r[0x12], r[0x13], r[0x14], r[0x15]);
}

bool read_rec(uint8_t* out) {
    const uint64_t g = gm();
    return g && guest::read(g + kRecord, out, kRecordBytes);
}

// "stage=3 session=0x03 rec=phase=idle(-1) ..." : where the game is now, on every site's line.
std::string env_text() {
    const uint64_t g = gm();
    if (!g) return "game state not ready";
    uint8_t r[kRecordBytes] = {};
    read_rec(r);
    return fmt("stage=%u session=0x%02x rec: %s", guest::rd<uint8_t>(g + layout::kStateStage), guest::rd<uint8_t>(g + layout::kStateSession),
               rec_text(r).c_str());
}

// The record's phase right now, kIdle-like 0xfffffffe when GM is not readable.
uint32_t rec_phase() {
    const uint64_t g = gm();
    return g ? guest::rd<uint32_t>(g + kRecord) : 0xfffffffeu;
}

std::string since_text() {
    const double t0 = g_t0.load(std::memory_order_relaxed);
    return t0 < 0.0 ? std::string("no trip record active") : fmt("trip record active for %.2f s", now() - t0);
}

// "0xAA BB .." of the trip request GM+0x48.. (the 72 bytes up to GM+0x90), empty when GM is not readable.
std::string request_hex() {
    const uint64_t g = gm();
    return g && guest::ok(g + kTripRequest, kTripRequestBytes) ? hex_bytes(reinterpret_cast<const void*>(g + kTripRequest), kTripRequestBytes, kTripRequestBytes)
                                                                : std::string("unreadable");
}

// ---- the polls ----

void poll_record(uint64_t g) {
    uint8_t r[kRecordBytes];
    if (!guest::read(g + kRecord, r, sizeof r)) return;
    Poll& p = g_poll;
    uint32_t phase;
    std::memcpy(&phase, r, 4);
    uint32_t was = kIdle;
    if (p.seen) std::memcpy(&was, p.rec, 4);
    if (p.seen && std::memcmp(r, p.rec, sizeof r) == 0) return;
    if (!p.seen && phase >= kRecordReady) {   // first sight and nothing filed: the baseline, no line
        p.seen = true;
        std::memcpy(p.rec, r, sizeof r);
        return;
    }
    const bool was_active = p.seen && was < kRecordReady, active = phase < kRecordReady;
    if (active && !was_active) {
        p.t0 = now();
        g_t0.store(p.t0, std::memory_order_relaxed);
    }
    const double dt = p.t0 < 0.0 ? -1.0 : now() - p.t0;
    Rec rec(kArea, "record");
    rec.in("%s", rec_text(r).c_str());
    if (p.seen) rec.in("was: %s", rec_text(p.rec).c_str());
    rec.out("stage=%u session=0x%02x frame_state=%u", guest::rd<uint8_t>(g + layout::kStateStage), guest::rd<uint8_t>(g + layout::kStateSession),
            guest::rd<uint32_t>(g + kFrameState));
    if (dt >= 0.0) rec.note("t+%.2f s since the request left idle", dt);
    if (was_active && !active) rec.note("record back to idle");
    rec.step();
    rec.write();
    if (was_active && !active) g_t0.store(-1.0, std::memory_order_relaxed);   // not active any more; p.t0 stays for the stage change
    p.seen = true;
    std::memcpy(p.rec, r, sizeof r);
}

void poll_stage(uint64_t g) {
    Poll& p = g_poll;
    const uint8_t stage = guest::rd<uint8_t>(g + layout::kStateStage);
    if (p.stage == stage && p.since_stage != 0.0) return;
    const bool first = p.since_stage == 0.0;
    Rec rec(kArea, first ? "stage first seen" : "stage changed");
    if (first) rec.in("stage=%u", stage);
    else rec.in("from=%u to=%u", p.stage, stage);
    rec.out("session=0x%02x frame_state=%u", guest::rd<uint8_t>(g + layout::kStateSession), guest::rd<uint32_t>(g + kFrameState));
    if (!first) {
        rec.note("in the previous stage %.2f s", now() - p.since_stage);
        if (p.t0 >= 0.0) rec.note("%.2f s since the record left idle (the trip took that long to change the stage)", now() - p.t0);
        else rec.note("no record was active: not a filed trip (a room, a load, or the record already finished)");
    }
    rec.step();
    rec.write();
    p.stage = stage;
    p.since_stage = now();
    p.t0 = -1.0;   // this trip is accounted for
}

void poll_frame_state(uint64_t g) {
    Poll& p = g_poll;
    uint32_t s;
    if (!guest::read(g + kFrameState, s)) return;
    if (p.frame_seen && s == p.frame_state) return;
    Rec rec(kArea, "frame state");
    if (p.frame_seen) rec.in("from=%u to=%u", p.frame_state, s);
    else rec.in("state=%u", s);
    rec.out("%s", s == 0x11 ? "simulation (0x231890 every frame)" : s >= 1 && s < 0x11 ? "boot step" : "not mapped");
    rec.note("GM+0x84828; states from P0-RE 889");
    rec.step();
    rec.write();
    p.frame_seen = true;
    p.frame_state = s;
}

// The areas' progress in this save (layout: kSaveArea*, kSaveGate*), once a second, a line per change: each stage's own level, the
// visitor mirror (the host's levels, what the game reads on a visitor), the max, Cloud Island's level, and the gate fields. The first
// look of a save lists every stage. The co-op digest compares the level the game reads with the authority's (pkp_coop_digest.cpp).
struct Area {
    uint8_t own[layout::kAreaStages], mirror[layout::kAreaStages], max[layout::kAreaStages];
    uint8_t cloud, gates[layout::kGateHistoryBytes], gate_mirror[layout::kGateHistoryBytes], last_gate, skit, trainer;
};
struct AreaPoll {
    bool seen = false;
    uint64_t root = 0;
    double at = 0.0;
    Area a = {};
};
AreaPoll g_area;
constexpr double kAreaEvery = 1.0;

bool read_area(uint64_t r, Area& a) {
    return guest::read(r + layout::kSaveAreaLevel, a.own, sizeof a.own) && guest::read(r + layout::kSaveAreaLevelMirror, a.mirror, sizeof a.mirror) &&
           guest::read(r + layout::kSaveAreaLevelMax, a.max, sizeof a.max) && guest::read(r + layout::kSaveCloudLevel, a.cloud) &&
           guest::read(r + layout::kSaveGateHistory, a.gates, sizeof a.gates) &&
           guest::read(r + layout::kSaveGateMirror, a.gate_mirror, sizeof a.gate_mirror) && guest::read(r + layout::kSaveLastGate, a.last_gate) &&
           guest::read(r + layout::kSaveSkitInGate, a.skit) && guest::read(r + layout::kSaveTrainerLevel, a.trainer);
}

std::string gates_text(const Area& a) {
    return fmt("gate history %s visitor mirror %s last_gate=%u skit_in_gate=%u trainer_level=%u", hex_bytes(a.gates, sizeof a.gates, sizeof a.gates).c_str(),
               hex_bytes(a.gate_mirror, sizeof a.gate_mirror, sizeof a.gate_mirror).c_str(), a.last_gate, a.skit, a.trainer);
}

void poll_area(uint64_t g) {
    AreaPoll& p = g_area;
    const double t = now();
    if (p.seen && t - p.at < kAreaEvery) return;
    p.at = t;
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    Area a;
    if (!r || !read_area(r, a)) return;
    const bool visitor = guest::rd<uint8_t>(g + layout::kStateSession) & layout::kSessionVisitor;
    const char* reads = visitor ? "this process is a visitor: the game reads the mirror" : "the game reads this save's own levels";
    if (!p.seen || r != p.root) {   // a save looked at first (or loaded again): the whole state
        std::vector<std::string> rows;
        for (size_t s = 0; s < layout::kAreaStages; ++s)
            if (a.own[s] || a.mirror[s] || a.max[s]) rows.push_back(fmt("stage %zu: own=%u mirror=%u max=%u", s, a.own[s], a.mirror[s], a.max[s]));
        rows.push_back(fmt("Cloud Island (stage %u) level=%u", layout::kCloudStage, a.cloud));
        rows.push_back(gates_text(a));
        list(kArea, fmt("area state (%s)", reads).c_str(), rows);
    } else {
        for (size_t s = 0; s < layout::kAreaStages; ++s) {
            if (a.own[s] == p.a.own[s] && a.mirror[s] == p.a.mirror[s] && a.max[s] == p.a.max[s]) continue;
            Rec(kArea, "area level")
                .in("stage=%zu was own=%u mirror=%u max=%u", s, p.a.own[s], p.a.mirror[s], p.a.max[s])
                .out("own=%u mirror=%u max=%u", a.own[s], a.mirror[s], a.max[s])
                .note("%s; current stage %u", reads, guest::rd<uint8_t>(g + layout::kStateStage))
                .step();
        }
        if (a.cloud != p.a.cloud) Rec(kArea, "area level").in("Cloud Island was %u", p.a.cloud).out("%u", a.cloud).step();
        if (std::memcmp(a.gates, p.a.gates, sizeof a.gates) || std::memcmp(a.gate_mirror, p.a.gate_mirror, sizeof a.gate_mirror) ||
            a.last_gate != p.a.last_gate || a.skit != p.a.skit || a.trainer != p.a.trainer)
            Rec(kArea, "gates")
                .in("was %s", gates_text(p.a).c_str())
                .out("%s", gates_text(a).c_str())
                .note("stage %u; %s; last_gate / skit offsets likely (save_fields)", guest::rd<uint8_t>(g + layout::kStateStage), reads)
                .step();
    }
    p.seen = true;
    p.root = r;
    p.a = a;
}

void frame(::Ctx*, bool) {
    const uint64_t g = gm();
    if (!g) return;
    poll_record(g);
    poll_stage(g);
    poll_frame_state(g);
    poll_area(g);
}

void reset() {
    g_area = AreaPoll{};
    g_poll = Poll{};
    g_t0.store(-1.0, std::memory_order_relaxed);
}

// ---- the entry sites ----

// 0xbd5a0(rec, w1 stage, w2, w3, w4, w5), 0xbda20(rec, w1 index, w2): the area-move filers. A filer refuses silently before any write
// when the stage is not 1..0x1f or the record's phase is below 5 (TRAVEL-RE 1; the index of 0xbda20 must be <= 31).
void trip_by_stage(::Ctx* c) {
    const uint32_t stage = static_cast<uint32_t>(c->x[1]);
    const uint32_t phase = rec_phase();
    Rec rec(kArea, "area-move filer by stage 0xbd5a0");
    rec.in("stage=%u w2=%u w3=%u w4=%u w5=%u (w4: leaving Dream Island, w5: going to it); %s; from=0x%llx", stage, static_cast<uint32_t>(c->x[2]),
           static_cast<uint32_t>(c->x[3]), static_cast<uint32_t>(c->x[4]), static_cast<uint32_t>(c->x[5]), env_text().c_str(),
           static_cast<unsigned long long>(lr(c)));
    if (stage < 1 || stage > kMaxStage) rec.refused("stage %u is not 1..0x1f: the filer writes nothing", stage);
    else if (phase < kRecordReady) rec.refused("record busy, phase %s < 5: the filer writes nothing", phase_text(phase).c_str());
    else rec.out("files the record (phase 0), input lock 0xf, then 0xbd84c");
    rec.step();
}

void trip_by_index(::Ctx* c) {
    const uint32_t index = static_cast<uint32_t>(c->x[1]);
    const uint32_t phase = rec_phase();
    Rec rec(kArea, "area-move filer by index 0xbda20");
    rec.in("index=%u w2=%u; %s; from=0x%llx", index, static_cast<uint32_t>(c->x[2]), env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    if (index > kMaxStage) rec.refused("index %u > 31: the filer writes nothing", index);
    else if (phase < kRecordReady) rec.refused("record busy, phase %s < 5: the filer writes nothing", phase_text(phase).c_str());
    else rec.out("looks the stage up (slots 401, 126), files the record (phase 0), input lock 0xf, then 0xbd84c");
    rec.note("co-op hooks this site and may refuse it after this line (a member asks the host instead)");
    rec.step();
}

// 0xbd45c(rec, w1, w2, w3): files a trip home (LEAVE-DOORS-RE 46: w1 reason 4, w2 member 0xff, w3 flag); 0xbdc68(rec): close the
// connection; 0xbdd28(rec): end of GameShare (LEAVE-DOORS-RE 24, 27).
void trip_home(::Ctx* c) {
    Rec rec(kArea, "trip-home filer 0xbd45c");
    rec.in("w1=%u w2=0x%x w3=%u; %s; from=0x%llx", static_cast<uint32_t>(c->x[1]), static_cast<uint32_t>(c->x[2]), static_cast<uint32_t>(c->x[3]),
           env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.note("w1..w3 as LEAVE-DOORS-RE 46 names them: reason, member, flag");
    rec.step();
}

void trip_close(::Ctx* c) {
    Rec rec(kArea, "close-the-connection filer 0xbdc68");
    rec.in("%s; from=0x%llx", env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.step();
}

void trip_gameshare(::Ctx* c) {
    Rec rec(kArea, "gameshare-end filer 0xbdd28");
    rec.in("%s; from=0x%llx", env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.step();
}

// 0x5483a0: RequestRejectMember's handler (HANDLERS-RE 24): its arguments are not mapped.
void reject_member(::Ctx* c) {
    Rec rec(kArea, "request-reject-member handler 0x5483a0");
    rec.in("x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx; %s; from=0x%llx", static_cast<unsigned long long>(c->x[0]),
           static_cast<unsigned long long>(c->x[1]), static_cast<unsigned long long>(c->x[2]), static_cast<unsigned long long>(c->x[3]),
           env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.note("args not mapped");
    rec.step();
}

// 0xbe5f0(rec, w1 area, w2 arg, w3 kind, w4, w5): the receiver of StageMove (SESSION-RE 179). It starts a self-disconnect STAGE_MOVE
// (0x5373a4(3)) when the record's phase is below 5, GM+0x10 & 0x81, or GM+0x90 bit 2 is clear (TRAVEL-RE 4): predicted here.
void stage_move_receive(::Ctx* c) {
    const uint64_t g = gm();
    const uint32_t phase = rec_phase();
    std::string why;
    if (g) {
        if (phase < kRecordReady) why += fmt("record phase %s < 5; ", phase_text(phase).c_str());
        if (guest::rd<uint8_t>(g + kSessionBackup) & 0x81) why += "GM+0x10 & 0x81; ";
        if (!(guest::rd<uint8_t>(g + layout::kStateSession) & layout::kSessionVisitor)) why += "GM+0x90 bit 2 clear; ";
    }
    Rec rec(kArea, "stage-move received 0xbe5f0");
    rec.in("area=%u arg=%u kind=%s w4=%u w5=%u; %s; from=0x%llx", static_cast<uint32_t>(c->x[1]), static_cast<uint32_t>(c->x[2]),
           kind_text(static_cast<uint32_t>(c->x[3])).c_str(), static_cast<uint32_t>(c->x[4]), static_cast<uint32_t>(c->x[5]), env_text().c_str(),
           static_cast<unsigned long long>(lr(c)));
    if (!why.empty()) rec.out("predicted self-disconnect STAGE_MOVE: %s", why.c_str()).warn();
    else rec.out("files the received move into the record (phase 0)");
    rec.step();
}

// 0xbca8c(rec): the drop check, called by the record's step (0xbaf54) once a frame while the phase is above 4 (P0-RE 793): a line per
// key change (session byte, GM+0x10 low byte, record phase), the repeat count on a change and every 30 s.
void drop_check(::Ctx* c) {
    static Dedupe d;
    const uint64_t g = gm();
    const uint32_t phase = rec_phase();
    const uint64_t key = g ? (static_cast<uint64_t>(guest::rd<uint8_t>(g + layout::kStateSession)) << 40) |
                                 (static_cast<uint64_t>(guest::rd<uint8_t>(g + kSessionBackup)) << 32) | phase
                           : 0;
    const double t = now();
    if (d.have && key == d.key) {
        ++d.n;
        if (t - d.last < kRepeatEvery) return;
    }
    Rec rec(kArea, "drop check 0xbca8c");
    rec.in("%s; from=0x%llx", env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    if (d.have) rec.note("%u calls since the last line (%s key)", d.n, key == d.key ? "same" : "previous");
    rec.step();
    rec.write();
    d.key = key;
    d.have = true;
    d.n = 0;
    d.last = t;
}

// 0xb0d74: the stage-entry initialiser (P0-RE 798-804): resets the record and the event queue. The record shown is the one it resets.
void state_reset(::Ctx* c) {
    Rec rec(kArea, "stage load: entry initialiser 0xb0d74");
    rec.in("%s; from=0x%llx", env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.out("resets the record, the event queue and the session masks; %s", since_text().c_str());
    rec.step();
}

// 0x241968: the stage loader sequence's entry (trip manager kind 3, P0-RE 798-804), with the trip request it loads from.
void stage_loader(::Ctx* c) {
    Rec rec(kArea, "stage load: loader sequence entry 0x241968");
    rec.in("seq=0x%llx; %s; from=0x%llx", static_cast<unsigned long long>(c->x[0]), env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.out("trip request GM+0x48..: %s", request_hex().c_str());
    rec.note("%s", since_text().c_str());
    rec.step();
}

// 0xbcde0(rec): the record's act step (phase 3): fills the trip request from the record by kind and submits trip kind 4 (P0-RE 798-804).
void trip_act(::Ctx* c) {
    Rec rec(kArea, "act: trip request from the record 0xbcde0");
    rec.in("%s; from=0x%llx", env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.out("trip request before the fill: %s", request_hex().c_str());
    rec.note("%s", since_text().c_str());
    rec.step();
}

// 0xbd84c(rec): the StageMove sender; it sends only online and with the host bit (TRAVEL-RE 1).
void stage_move_send(::Ctx* c) {
    const uint64_t g = gm();
    const uint8_t s = g ? guest::rd<uint8_t>(g + layout::kStateSession) : 0;
    Rec rec(kArea, "stage-move send 0xbd84c");
    rec.in("%s; from=0x%llx", env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.out("%s", (s & layout::kSessionOnline) && (s & layout::kSessionHost) ? "online and host: sends the record's area and arg" : "no message (needs online and the host bit)");
    rec.step();
}

// 0x113b460(tm, kind) / 0x113b490(tm, kind, param*): file the next sequence of the trip manager (P0-RE 798-804). Kinds the docs name:
// 0 boot / splash, 1 save load + title, 3 stage loader, 4 a trip (the request GM+0x48..).
const char* sequence_name(uint32_t k) {
    switch (k) {
        case 0: return "boot / splash";
        case 1: return "save load + title";
        case 3: return "stage loader";
        case 4: return "trip";
        default: return "";
    }
}

void submit_sequence(::Ctx* c, const char* what, bool with_param) {
    const uint32_t k = static_cast<uint32_t>(c->x[1]);
    Rec rec(kArea, what);
    rec.in("tm=0x%llx kind=%u%s%s%s; %s; from=0x%llx", static_cast<unsigned long long>(c->x[0]), k, *sequence_name(k) ? "(" : "",
           sequence_name(k), *sequence_name(k) ? ")" : "", env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    if (with_param) {
        uint8_t p[0x1c];
        if (guest::read(c->x[2], p, sizeof p)) rec.in("param: %s", hex_bytes(p, sizeof p).c_str());
        else rec.in("param=0x%llx unreadable", static_cast<unsigned long long>(c->x[2]));
    }
    if (k == 4) rec.out("trip request GM+0x48..: %s", request_hex().c_str());
    rec.note("%s", since_text().c_str());
    rec.step();
}
void submit_a(::Ctx* c) { submit_sequence(c, "sequence submit 0x113b460", false); }
void submit_b(::Ctx* c) { submit_sequence(c, "sequence submit 0x113b490", true); }

// 0x5373b8(NM, reason): every self-disconnect passes here (P0-RE 798-804). Reasons the docs name: 0 quality, 1 resend data, 2 resend
// count, 3 STAGE_MOVE, 5 UGC, 6 RETURN_FROM_BG. Never in a healthy session: Warn.
void self_disconnect(::Ctx* c) {
    static const char* const kNames[] = {"quality", "resend data", "resend count", "STAGE_MOVE", "?", "UGC", "RETURN_FROM_BG"};
    const uint32_t reason = static_cast<uint32_t>(c->x[1]);
    const char* name = reason < sizeof kNames / sizeof kNames[0] && kNames[reason][0] != '?' ? kNames[reason] : "";
    Rec rec(kArea, "self-disconnect 0x5373b8");
    rec.in("nm=0x%llx reason=%u%s%s%s; %s; from=0x%llx", static_cast<unsigned long long>(c->x[0]), reason, *name ? "(" : "", name, *name ? ")" : "",
           env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.out("arms the leave (NM+0xb3360 = 0); the session ends on the next tick");
    rec.note("%s", since_text().c_str());
    rec.warn().step();
}

// 0x547278: error 3103, the session was lost (co-op's own site comment); 0xa88214: the title's continue (co-op's comment). Arguments
// of both are not mapped.
void error_3103(::Ctx* c) {
    Rec rec(kArea, "error 3103 (session lost) 0x547278");
    rec.in("x0=0x%llx x1=0x%llx x2=0x%llx; %s; from=0x%llx", static_cast<unsigned long long>(c->x[0]), static_cast<unsigned long long>(c->x[1]),
           static_cast<unsigned long long>(c->x[2]), env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.note("args not mapped");
    rec.warn().step();
}

void past_title(::Ctx* c) {
    Rec rec(kArea, "past the title 0xa88214");
    rec.in("x0=0x%llx x1=0x%llx; %s; from=0x%llx", static_cast<unsigned long long>(c->x[0]), static_cast<unsigned long long>(c->x[1]), env_text().c_str(),
           static_cast<unsigned long long>(lr(c)));
    rec.note("args not mapped");
    rec.step();
}

// The save writers: a write in the middle of a trip matters. 0x241324(MGR, req*, mode, flag): req = {u32 type, u8 stage, u8 home,
// u8 online, pad, u32 kind} (SAVE-BAG-RE 27). 0x245200 (the S00 writer) and 0x5a8a34 (the G00 / B00 job) have no mapped arguments.
void save_request(::Ctx* c) {
    uint8_t q[12];
    Rec rec(kArea, "save write request 0x241324");
    rec.in("mode=%u flag=%u", static_cast<uint32_t>(c->x[2]), static_cast<uint32_t>(c->x[3]));
    if (guest::read(c->x[1], q, sizeof q)) {
        uint32_t type, kind;
        std::memcpy(&type, q, 4);
        std::memcpy(&kind, q + 8, 4);
        rec.in("req: type=%u stage=%u home=%u online=%u kind=%u", type, q[4], q[5], q[6], kind);
    } else {
        rec.in("req=0x%llx unreadable", static_cast<unsigned long long>(c->x[1]));
    }
    rec.in("%s; from=0x%llx", env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.note("%s", since_text().c_str());
    rec.step();
}

void save_raw(::Ctx* c, const char* what) {
    Rec rec(kArea, what);
    rec.in("x0=0x%llx x1=0x%llx x2=0x%llx; %s; from=0x%llx", static_cast<unsigned long long>(c->x[0]), static_cast<unsigned long long>(c->x[1]),
           static_cast<unsigned long long>(c->x[2]), env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.note("args not mapped; %s", since_text().c_str());
    rec.step();
}
void save_system(::Ctx* c) { save_raw(c, "save write: system data 0x245200"); }
void save_job(::Ctx* c) { save_raw(c, "save write: G00 / B00 job 0x5a8a34"); }

// The fades (QUEST-EFFECTS-RE 196-199): 0xa9fcc4 fades out, 0xa9fa34 fades in. Both start with the word 12001c08. Arguments: s0 and
// x0..x4 raw (the meaning of each is not mapped).
void fade_line(::Ctx* c, const char* what) {
    Rec rec(kArea, what);
    rec.in("s0=%.3f x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x4=0x%llx; %s; from=0x%llx", static_cast<double>(c->v[0].f[0]),
           static_cast<unsigned long long>(c->x[0]), static_cast<unsigned long long>(c->x[1]), static_cast<unsigned long long>(c->x[2]),
           static_cast<unsigned long long>(c->x[3]), static_cast<unsigned long long>(c->x[4]), env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.note("args not mapped; %s", since_text().c_str());
    rec.step();
}
void fade_out(::Ctx* c) { fade_line(c, "fade out 0xa9fcc4"); }
void fade_in(::Ctx* c) { fade_line(c, "fade in 0xa9fa34"); }

// 0x4b1b34(obj, w1 player): callback 0x7b, the end of the map's departure event: the move itself (TRAVEL-RE 2). obj = *(main+0x3565120):
// +0x45 target stage, +0x97 the player. The target equal to the current stage is a relocation inside the stage (0x4e7c40).
void map_move(::Ctx* c) {
    const uint64_t g = gm();
    const uint64_t obj = c->x[0];
    uint8_t target = 0, who = 0;
    const bool ok = guest::ok(obj, 0x9a);
    if (ok) {
        target = guest::rd<uint8_t>(obj + 0x45);
        who = guest::rd<uint8_t>(obj + 0x97);
    }
    Rec rec(kArea, "map move end (cb 0x7b) 0x4b1b34");
    rec.in("obj=0x%llx w1_player=%u", static_cast<unsigned long long>(obj), static_cast<uint32_t>(c->x[1]));
    if (ok) rec.in("target=%u obj+0x97=%u obj==*(0x3565120): %s", target, who, obj == guest::deref(base() + kTravelObject) ? "yes" : "no");
    rec.in("%s; from=0x%llx", env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    if (ok && g) {
        if (target == guest::rd<uint8_t>(g + layout::kStateStage)) rec.out("same stage: a relocation inside the stage (0x4e7c40), no filer");
        else rec.out("other stage: files 0xbd5a0 unless this is an online visitor (then 0x4e7c40 only)");
    }
    rec.step();
}

// 0x4a3f60(req, ctx): callback 0x7c, the post-poll step of online map travel (TRAVEL-RE 2): ctx+0x1734 u32 stage, ctx+0x2000 the player,
// the kind-1 result block ctx+0x1f6d..0x1f71 (+0x1f6e the yes bits, +0x1f6f the participant bits, +0x1f71 non-zero once a poll ran).
void post_poll(::Ctx* c) {
    const uint64_t ctx = c->x[1];
    Rec rec(kArea, "map poll result (cb 0x7c) 0x4a3f60");
    rec.in("req=0x%llx ctx=0x%llx", static_cast<unsigned long long>(c->x[0]), static_cast<unsigned long long>(ctx));
    if (guest::ok(ctx + 0x1734, 4) && guest::ok(ctx + 0x1f6d, 5) && guest::ok(ctx + 0x2000, 1)) {
        uint8_t b[5];
        guest::read(ctx + 0x1f6d, b, sizeof b);
        const uint32_t stage = guest::rd<uint32_t>(ctx + 0x1734);
        rec.in("stage=%u player=%u block 0x1f6d..71=%02x %02x %02x %02x %02x", stage, guest::rd<uint8_t>(ctx + 0x2000), b[0], b[1], b[2], b[3], b[4]);
        const uint8_t yes = b[1], part = b[2];
        if (b[4] == 0) rec.out("no poll ran (result block empty): the game queues 0x106f / 0x1094");
        else if ((part & ~yes & 0xf) == 0) rec.out("every participant said yes (participants=0x%02x yes=0x%02x): the departure runs", part, yes);
        else rec.out("a participant did not say yes (participants=0x%02x yes=0x%02x): nothing is queued, the trip is cancelled", part, yes);
    } else {
        rec.in("ctx unreadable");
    }
    rec.in("%s; from=0x%llx", env_text().c_str(), static_cast<unsigned long long>(lr(c)));
    rec.note("acts only online, outside the leave window and on the host (the host bit is GM+0x90 bit 1); the all-yes test is the game's tbnz at 0x4a3ffc");
    rec.step();
}

// ---- area gates (coop-v2 AREA-GATE-RE): the building-entrance door (vtable 0x3266e68), its gate record and opening ----
// 0x7b0420 (door slot 5, interact): x0 door, w1 action, x2 actor. Not yet recorded: the open test 0x7b0f20 (-> 0x114970), then the
// record 0x5c059c and the opening event 0x463ae0. Recorded: the entry 0x7b0d80, the passage, then 0xbda20.
uint8_t session_byte() {
    const uint64_t g = gm();
    return g ? guest::rd<uint8_t>(g + layout::kStateSession) : 0;
}

uint32_t save_byte(uint64_t off) {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return r && guest::ok(r + off, 1) ? guest::rd<uint8_t>(r + off) : 0xffu;
}

void gate_press(::Ctx* c) {
    Rec rec(kArea, "gate press 0x7b0420");
    rec.in("door=0x%llx w1=%u actor=0x%llx session=0x%02x; from=0x%llx", static_cast<unsigned long long>(c->x[0]),
           static_cast<uint32_t>(c->x[1]), static_cast<unsigned long long>(c->x[2]), session_byte(), static_cast<unsigned long long>(lr(c)));
    rec.out("next: the open test 0x7b0f20 (gate not recorded) or the entry 0x7b0d80 (recorded)");
    rec.step();
}

// 0x7b0f20(door): the gate id is the door's s8 +0x118 (ldrsb at 0x7b0f7c).
void gate_open_test(::Ctx* c) {
    const uint64_t door = c->x[0];
    const int gate = guest::ok(door + 0x118, 1) ? guest::rd<int8_t>(door + 0x118) : -1;
    const bool known = gate >= 0 && gate < static_cast<int>(layout::kGateHistoryBytes);
    Rec rec(kArea, "gate open test 0x7b0f20");
    rec.in("door=0x%llx gate=%d history=%u mirror=%u trainer_level=%u session=0x%02x", static_cast<unsigned long long>(door), gate,
           known ? save_byte(layout::kSaveGateHistory + gate) : 0xffu, known ? save_byte(layout::kSaveGateMirror + gate) : 0xffu,
           save_byte(layout::kSaveTrainerLevel), session_byte());
    rec.out("a pass records the gate (0x5c059c) and plays its opening event (0x463ae0); no record line after this: the test failed");
    rec.step();
}

// 0x114970(w0): returns at once for w0 outside 1..4; reads m_trainerLevel (ldrb R+0x4a4400 at 0x1149a4). Other callers too: deduped.
void gate_rank_test(::Ctx* c) {
    static Dedupe d;
    const uint64_t g = gm();
    const uint32_t w0 = static_cast<uint32_t>(c->x[0]) & 0xff;
    const uint32_t story = g ? guest::rd<uint8_t>(g + 0x8c) & 1u : 0xffu;
    const uint32_t trainer = save_byte(layout::kSaveTrainerLevel);
    const uint64_t key = static_cast<uint64_t>(w0) << 32 | static_cast<uint64_t>(story) << 16 | trainer;
    const double t = now();
    if (d.have && key == d.key && t - d.last < kRepeatEvery) {
        ++d.n;
        return;
    }
    Rec rec(kArea, "gate rank test 0x114970");
    rec.in("w0=%u story(GM+0x8c bit 0)=%u trainer_level=%u; from=0x%llx", w0, story, trainer, static_cast<unsigned long long>(lr(c)));
    if (w0 - 1 > 3) rec.out("w0 outside 1..4: the game's early exit");
    rec.note("the required rank is the gate row's (table slot 401 row +8), not read here");
    if (d.have && d.n) rec.note("%u calls since the last line", d.n);
    rec.step();
    d.key = key;
    d.have = true;
    d.n = 0;
    d.last = t;
}

// 0x5c059c(w0 id): bit 0 of history+id, but nothing for an id above 0x1f or while 0x207b0 (GM+0x90 bit 0, a session) is true.
void gate_record(::Ctx* c) {
    const uint32_t id = static_cast<uint32_t>(c->x[0]) & 0xff;
    const uint8_t sess = session_byte();
    Rec rec(kArea, "gate record 0x5c059c");
    rec.in("gate=%u history=%u session=0x%02x; from=0x%llx", id, id < layout::kGateHistoryBytes ? save_byte(layout::kSaveGateHistory + id) : 0xffu,
           sess, static_cast<unsigned long long>(lr(c)));
    rec.expect("history bit 0 set for gate %u", id);
    if (id > 0x1f) rec.refused("gate %u > 31: nothing written", id);
    else if (sess & layout::kSessionOnline) rec.refused("session open (0x207b0, GM+0x90 bit 0): the game writes nothing, on host and visitor alike");
    else rec.out("bit 0 set");
    rec.step();
}

// 0x463ae0(w0 event): plays an event by id; other callers too: deduped by (event, caller).
void gate_event(::Ctx* c) {
    static Dedupe d;
    const uint32_t id = static_cast<uint32_t>(c->x[0]);
    const uint64_t key = static_cast<uint64_t>(id) << 32 | (lr(c) & 0xffffffffu);
    const double t = now();
    if (d.have && key == d.key && t - d.last < kRepeatEvery) {
        ++d.n;
        return;
    }
    Rec rec(kArea, "opening event 0x463ae0");
    rec.in("event=0x%x session=0x%02x; from=0x%llx", id, session_byte(), static_cast<unsigned long long>(lr(c)));
    rec.note("gates use 0xe11 / 0xe05 / 0xe10 (0xe67 for gate 13)");
    if (d.have && d.n) rec.note("%u calls since the last line", d.n);
    rec.step();
    d.key = key;
    d.have = true;
    d.n = 0;
    d.last = t;
}

void gate_entry(::Ctx* c) {
    Rec rec(kArea, "gate entry 0x7b0d80");
    rec.in("x0=0x%llx x1=0x%llx session=0x%02x; from=0x%llx", static_cast<unsigned long long>(c->x[0]), static_cast<unsigned long long>(c->x[1]),
           session_byte(), static_cast<unsigned long long>(lr(c)));
    rec.out("the gate is recorded: the passage, then the area-move filer 0xbda20");
    rec.note("args not mapped");
    rec.step();
}

constexpr WatchSite kSites[] = {
    {{0xbd45c, 0xb9400008u, true}, &trip_home, "trip-home filer 0xbd45c"},
    {{0xbdc68, kPacibsp, true}, &trip_close, "close-connection filer 0xbdc68"},
    {{0xbdd28, kPacibsp, true}, &trip_gameshare, "gameshare-end filer 0xbdd28"},
    {{0x5483a0, kPacibsp, true}, &reject_member, "reject-member handler 0x5483a0"},
    {{0xbe5f0, kPacibsp, true}, &stage_move_receive, "stage-move receiver 0xbe5f0"},
    {{0xbca8c, kPacibsp, true}, &drop_check, "drop check 0xbca8c"},
    {{0xb0d74, kPacibsp, true}, &state_reset, "stage-entry initialiser 0xb0d74"},
    {{0x547278, kPacibsp, true}, &error_3103, "error 3103 0x547278"},
    {{0xa88214, 0xb00156c8u, true}, &past_title, "past title 0xa88214"},
    {{0xbda20, kPacibsp, true}, &trip_by_index, "area-move filer by index 0xbda20"},
    {{0xbd5a0, 0x51008028u, true}, &trip_by_stage, "area-move filer by stage 0xbd5a0"},
    {{0xbd84c, kPacibsp, true}, &stage_move_send, "stage-move sender 0xbd84c"},
    {{0xbcde0, kPacibsp, true}, &trip_act, "trip act step 0xbcde0"},
    {{0x113b460, kPacibsp, true}, &submit_a, "sequence submit 0x113b460"},
    {{0x113b490, kPacibsp, true}, &submit_b, "sequence submit 0x113b490"},
    {{0x241968, kPacibsp, true}, &stage_loader, "stage loader entry 0x241968"},
    {{0x5373b8, kPacibsp, true}, &self_disconnect, "self-disconnect funnel 0x5373b8"},
    {{0x241324, kPacibsp, true}, &save_request, "save request 0x241324"},
    {{0x245200, kPacibsp, true}, &save_system, "system save 0x245200"},
    {{0x5a8a34, kPacibsp, true}, &save_job, "save job 0x5a8a34"},
    {{0xa9fcc4, 0x12001c08u, true}, &fade_out, "fade out 0xa9fcc4"},
    {{0xa9fa34, 0x12001c08u, true}, &fade_in, "fade in 0xa9fa34"},
    {{0x4b1b34, kPacibsp, true}, &map_move, "map move cb 0x7b 0x4b1b34"},
    {{0x4a3f60, kPacibsp, true}, &post_poll, "map poll result cb 0x7c 0x4a3f60"},
    {{0x7b0420, kPacibsp, true}, &gate_press, "gate press 0x7b0420"},
    {{0x7b0f20, kPacibsp, true}, &gate_open_test, "gate open test 0x7b0f20"},
    {{0x114970, kPacibsp, true}, &gate_rank_test, "gate rank test 0x114970"},
    {{0x5c059c, kPacibsp, true}, &gate_record, "gate record 0x5c059c"},
    {{0x463ae0, kPacibsp, true}, &gate_event, "opening event 0x463ae0"},
    {{0x7b0d80, kPacibsp, true}, &gate_entry, "gate entry 0x7b0d80"},
};

}  // namespace

const Watcher kTravelWatcher = {"travel", kSites, sizeof kSites / sizeof kSites[0], &frame, &reset};

}  // namespace nxrt::games::pkp::journal
