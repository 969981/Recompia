// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's shared event history (pkp_coop_internal.h). When a talk or a scene ends, the game writes what it
// leaves behind (which scene was played, which conversation was seen, how far each story line got) into the save of the player
// who was in it, on that player's process only: the other members kept offering the same conversations, and the story lines that
// wait for that scene never moved on in their saves (nor in the authority's, which is the group's save). Every process shares
// those records (EventHistory, StoryLines) and every member takes the furthest any member has (a union: a member that is behind
// never takes one away).
//
// The game, 2.0.1 (modding-docs coop-v2/EVENT-END-RE.md). The event runner's state 4 (0x47e140) writes, for the scene id s (1..2998)
// and the event id e of the slot's event:
//   save root + 0x4a2cec + s / 8     m_playedContentsSkitFlag (CMNDAT): bit s & 7, set by kPlayed(s, 1), the runner its only caller.
//   save root + 0x4a4100 + row       m_storyQuestUIState (CMNDAT): kQuestUi(s) moves the rows of table slot 0x1a9 tied to s to 5
//                                    (their first time also gives the row's character its friendship) or 3. Each row only once.
//   save root + 0x73b2f8 + row       m_crossTalkV2 (CMNDAT), rows 1..300 (event id -> row, map 0xa59bc0): bit 0 at state 4 (bit 1, from
//                                    the cleanup 0x47f360 and 0x2c6bc0, is not known well enough to share). The game has no setter
//                                    (the writes are inline). The conversation pickers (0x30fa0, 0x474b00, 0x4d3820, 0x4d3bc4,
//                                    0x3abbe0, 0x955f20) offer only rows whose bit 0 (or 0 and 1) is clear: a row seen on one process
//                                    was offered again on the others.
//   save root + 0x4a4410 + b * 0x10  the story lines b = 1..0xfe (m_StoryId / m_StoryCounter / m_StoryConditionFlags, CMNDAT): +8 u8
//                                    the line's id (1..0xfe valid), +0xa s16 its step (>= 0 valid), +0xc u16 one bit per condition of
//                                    that step (the unused bits are set). The event bus 0x10d260 (GM + 0xd0) sets the bits of the
//                                    conditions "scene s ended"; once all 16 are set, kStep(entry) moves the line to its next step:
//                                    it runs that step's actions (story flags, abilities, a stage flag: no scene and no talk), sets
//                                    the new step's bits from what already holds, and moves the lines that wait for this one.
//                                    The bus entries are 3 bytes at GM + 0xd0 + 1 + k * 3: +0 the line b, +2 busy (0x10b960 sets it
//                                    around its own kStep call). kStoryOn(b) is the bus's gate (the story on, or a line that runs
//                                    always).
// Left alone: the quest flag (the story's quest states carry it), the contents notices (the contents scheduler's part), the scene's
// own end actions 0x481b40 (they queue events on the talker's slot), the replay counters (m_skitHistoryV7, no reader) and m_flags
// bit 8 of event 0xe0f (one script condition; no setter).
// Rules on a receiver, at a safe moment (in the group's world, can walk, talks to no one): a played scene that is clear here is
// played here through kPlayed and kQuestUi; a seen conversation row gets the bits it lacks; on the authority only (a step runs world
// actions), a story line with the same id that is
// a step behind here gets all its bits and one kStep, repeated each frame until it is level, and one at the same step gets the bits
// it lacks (kStep once all are set). A line that kStep does not move gets its own bits back, and is let go after kStepTries tries. Each process reads the three
// areas each frame and shares what grew, and all of it every kAllEvery seconds and when it or another member joins.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
#include "core/pkp_layout.h"

#include <bitset>
#include <cstring>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::events {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kPlayedFlags = 0x4a2cec;   // save root +: the played-scene bits
constexpr uint32_t kSceneMax = 2998;          // scene ids 1..2998
constexpr size_t kPlayedBytes = kSceneMax / 8 + 1;
constexpr uint64_t kSeenRows = 0x73b2f8;      // save root +: the conversation rows
constexpr size_t kRowCount = 301;             // rows 0..300 (1..300 used)
constexpr uint8_t kRowBits = 0x01;   // bit 0: seen to the end (state 4)
constexpr uint64_t kLines = 0x4a4410;         // save root + b * kLineSize
constexpr uint64_t kLineSize = 0x10, kLineId = 8, kLineStep = 0xa, kLineBits = 0xc;
constexpr uint32_t kLineMax = 0xfe;
constexpr uint16_t kAllBits = 0xffff;
constexpr uint64_t kBus = 0xd0;               // GM +: the event bus
constexpr size_t kBusEntries = 255;           // GM + 0xd0 + 1 + k * 3
constexpr uint64_t kPlayed = 0x117650;        // (s, 1): the played bit
constexpr uint64_t kQuestUi = 0x1188c0;       // (s): the quest UI rows tied to s
constexpr uint64_t kStoryOn = 0x10bc94;       // (b) -> bit 0: the bus may move line b
constexpr uint64_t kStep = 0x10b580;          // (&entry): line *entry moves on while its bits are all set
constexpr double kAllEvery = 30.0;
constexpr unsigned kApplyPerTick = 16;        // played scenes per frame
constexpr unsigned kStepsPerTick = 4;         // kStep calls per frame
constexpr unsigned kStepTries = 8;

struct Line {
    uint8_t id = 0;
    int16_t step = -1;
    uint16_t bits = 0;
};
bool operator==(const Line& a, const Line& b) { return a.id == b.id && a.step == b.step && a.bits == b.bits; }

struct State {
    uint8_t played_seen[kPlayedBytes] = {};   // what was last sent (or taken from a member)
    uint8_t rows_seen[kRowCount] = {};
    Line lines_seen[kLineMax + 1];
    std::bitset<kSceneMax + 1> played_want;   // another member played it, not played here yet
    uint8_t played_origin[kSceneMax + 1] = {};
    uint8_t rows_want[kRowCount] = {};        // bits another member has, not set here yet
    Line lines_want[kLineMax + 1];            // the furthest another member has (id 0: none)
    uint8_t line_origin[kLineMax + 1] = {};
    uint8_t line_tries[kLineMax + 1] = {};
    std::bitset<kLineMax + 1> line_waits;     // logged once: a line another member has waits here (not started, not on the bus)
    bool have_seen = false;
    bool send_all = true;
    double all_at = 0.0;
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return r && guest::ok(r + kPlayedFlags, kPlayedBytes) && guest::ok(r + kSeenRows, kRowCount) &&
                   guest::ok(r + kLines, (kLineMax + 1) * kLineSize)
               ? r
               : 0;
}
uint64_t bus() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!guest::ok(gm + kBus, 8)) return 0;
    const uint64_t b = guest::rd<uint64_t>(gm + kBus);
    return b && guest::ok(b, 1 + kBusEntries * 3) ? b : 0;
}

bool played(uint64_t root, uint32_t s) { return guest::rd<uint8_t>(root + kPlayedFlags + (s >> 3)) & (1u << (s & 7)); }
Line line(uint64_t root, uint32_t b) {
    const uint64_t at = root + kLines + b * kLineSize;
    Line l;
    l.id = guest::rd<uint8_t>(at + kLineId);
    l.step = guest::rd<int16_t>(at + kLineStep);
    l.bits = guest::rd<uint16_t>(at + kLineBits);
    return l;
}
bool valid(const Line& l) { return l.id >= 1 && l.id <= kLineMax && l.step >= 0; }

bool shared_here() { return (session_bits() & layout::kSessionOnline) && link::in_session(); }
bool safe_now(bool in_game) {
    const bool in_world = role() == CoopRole::Host || (session_bits() & layout::kSessionVisitor);
    return in_world && session::can_walk(in_game) && !talk::talking();
}

// EventHistory: the played bits and the conversation rows, whole (they are small), when anything grew since the last send or (all)
// always. false: the send failed, and it goes again at the next read.
bool share_history(uint64_t root, bool all) {
    State& s = st();
    uint8_t played_now[kPlayedBytes], rows_now[kRowCount];
    for (size_t i = 0; i < kPlayedBytes; ++i) played_now[i] = guest::rd<uint8_t>(root + kPlayedFlags + i);
    played_now[0] &= 0xfe;   // scene 0 is no scene
    for (size_t i = 0; i < kRowCount; ++i) rows_now[i] = guest::rd<uint8_t>(root + kSeenRows + i) & kRowBits;
    rows_now[0] = 0;
    bool grew = all || !s.have_seen;
    for (size_t i = 0; i < kPlayedBytes && !grew; ++i) grew = played_now[i] & ~s.played_seen[i];
    for (size_t i = 0; i < kRowCount && !grew; ++i) grew = rows_now[i] & ~s.rows_seen[i];
    if (!grew) return true;
    wire::Writer w(link::self());
    w.u16(static_cast<uint16_t>(kPlayedBytes)).bytes(played_now, kPlayedBytes);
    w.u16(static_cast<uint16_t>(kRowCount)).bytes(rows_now, kRowCount);
    if (!link::share(wire::Type::EventHistory, w.take())) return false;
    if (s.have_seen)
        for (uint32_t id = 1; id <= kSceneMax; ++id)
            if ((played_now[id >> 3] & ~s.played_seen[id >> 3]) & (1u << (id & 7)))
                NXLOG_I(kTag, "Co-op events: scene %u played here, shared", id);
    std::memcpy(s.played_seen, played_now, kPlayedBytes);
    std::memcpy(s.rows_seen, rows_now, kRowCount);
    return true;
}

// StoryLines: every valid line that changed since the last send, or (all) every valid line.
bool share_lines(uint64_t root, bool all) {
    State& s = st();
    std::vector<uint8_t> changed;
    for (uint32_t b = 1; b <= kLineMax; ++b) {
        const Line l = line(root, b);
        if (!valid(l)) continue;
        if (all || !s.have_seen || !(l == s.lines_seen[b])) changed.push_back(static_cast<uint8_t>(b));
    }
    if (changed.empty()) return true;
    wire::Writer w(link::self());
    w.u8(static_cast<uint8_t>(changed.size()));
    for (uint8_t b : changed) {
        const Line l = line(root, b);
        w.u8(b).u8(l.id).u16(static_cast<uint16_t>(l.step)).u16(l.bits);
    }
    if (!link::share(wire::Type::StoryLines, w.take())) return false;
    for (uint8_t b : changed) {
        const Line l = line(root, b);
        if (s.have_seen && l.step != s.lines_seen[b].step)
            NXLOG_I(kTag, "Co-op events: story line %u (id %u) at step %d here, shared", b, l.id, l.step);
        s.lines_seen[b] = l;
    }
    return true;
}

// The bus entry of line b (0: none in this stage).
uint64_t entry_of(uint64_t gm_bus, uint32_t b) {
    for (size_t k = 0; k < kBusEntries; ++k) {
        const uint64_t e = gm_bus + 1 + k * 3;
        if (guest::rd<uint8_t>(e) == b) return e;
    }
    return 0;
}

// Plays the waiting scenes here (their bits, then the quest UI rows), up to kApplyPerTick.
void apply_played(::Ctx* c, uint64_t root) {
    State& s = st();
    unsigned done = 0;
    for (uint32_t id = 1; id <= kSceneMax && done < kApplyPerTick; ++id) {
        if (!s.played_want.test(id)) continue;
        s.played_want.reset(id);
        if (played(root, id)) continue;   // played here meanwhile
        ++done;
        guest::call(c, base() + kPlayed, id, 1);
        guest::call(c, base() + kQuestUi, id);
        if (played(root, id)) s.played_seen[id >> 3] |= static_cast<uint8_t>(1u << (id & 7));   // ours now: not sent back as new
        NXLOG_I(kTag, "Co-op events: scene %u from node %u marked played here", id, s.played_origin[id]);
        const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
        journal::Rec(journal::Area::Event, "scene marked")
            .in("scene=%u from n%u stage=%u", id, s.played_origin[id], gm ? guest::rd<uint8_t>(gm + layout::kStateStage) : 0xffu)
            .out("marked only: played bit %s, quest UI row set", played(root, id) ? "set" : "NOT set")
            .expect("played bit set")
            .ok(played(root, id))
            .note("the scene's own commands (world effects: doors, collision, objects) did not run here");
    }
}

// Gives the conversation rows the bits another member has (the game's own write: an OR into the row byte).
void apply_rows(uint64_t root) {
    State& s = st();
    for (size_t row = 1; row < kRowCount; ++row) {
        const uint8_t want = s.rows_want[row];
        if (!want) continue;
        s.rows_want[row] = 0;
        const uint8_t now = guest::rd<uint8_t>(root + kSeenRows + row);
        if ((now & want) == want) continue;
        guest::wr<uint8_t>(root + kSeenRows + row, static_cast<uint8_t>(now | want));
        s.rows_seen[row] |= want;
        NXLOG_I(kTag, "Co-op events: conversation row %zu seen by a member, marked here (bits %u)", row, want);
    }
}

// Moves the story lines that are behind another member's, through the bus's own step, up to kStepsPerTick calls.
void apply_lines(::Ctx* c, uint64_t root) {
    State& s = st();
    const uint64_t gm_bus = bus();
    if (!gm_bus) return;
    unsigned steps = 0;
    for (uint32_t b = 1; b <= kLineMax && steps < kStepsPerTick; ++b) {
        const Line want = s.lines_want[b];
        if (!want.id) continue;
        const Line here = line(root, b);
        const bool level = valid(here) && (here.step > want.step || (here.step == want.step && (here.bits & want.bits) == want.bits));
        if (level || (valid(here) && here.id != want.id)) {
            if (level) s.lines_seen[b] = here;   // as far as the member: not sent back as new
            s.lines_want[b] = Line{};
            s.line_tries[b] = 0;
            s.line_waits.reset(b);
            continue;
        }
        // A line starts at a stage start (0x10aaa8 from 0xb2f00, on every process from the same quest states): one not started here
        // yet, or not on this stage's bus, waits for that.
        const uint64_t entry = valid(here) ? entry_of(gm_bus, b) : 0;
        if (!entry) {
            if (!s.line_waits.test(b))
                NXLOG_I(kTag, "Co-op events: story line %u from node %u (step %d) waits: %s here", b, s.line_origin[b], want.step,
                        valid(here) ? "not on this stage's event bus" : "not started");
            s.line_waits.set(b);
            continue;
        }
        if (guest::rd<uint8_t>(entry + 2) || !(guest::call(c, base() + kStoryOn, b) & 1)) continue;   // not now
        s.line_waits.reset(b);
        const uint16_t bits = here.step < want.step ? kAllBits : static_cast<uint16_t>(here.bits | want.bits);
        guest::wr<uint16_t>(root + kLines + b * kLineSize + kLineBits, bits);
        if (bits == kAllBits) {
            ++steps;
            guest::wr<uint8_t>(entry + 2, 1);
            guest::call(c, base() + kStep, entry);
            guest::wr<uint8_t>(entry + 2, 0);
        }
        Line after = line(root, b);
        if (here.step < want.step && after.step == here.step) {   // it did not move: its own bits back, so nothing is faked
            guest::wr<uint16_t>(root + kLines + b * kLineSize + kLineBits, here.bits);
            after.bits = here.bits;
        }
        NXLOG_I(kTag, "Co-op events: story line %u (id %u) from node %u: step %d -> %d here (the member's %d)", b, want.id,
                s.line_origin[b], here.step, after.step, want.step);
        if (after.step == here.step && bits == kAllBits && ++s.line_tries[b] >= kStepTries) {
            NXLOG_W(kTag, "Co-op events: story line %u does not move on here past step %d; let go", b, here.step);
            s.lines_want[b] = Line{};
            s.line_tries[b] = 0;
        }
    }
}

}  // namespace

void on_history_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint16_t np = r.u16();
    std::vector<uint8_t> p(np);
    r.bytes(p.data(), np);
    const uint16_t nr = r.u16();
    std::vector<uint8_t> rows(nr);
    r.bytes(rows.data(), nr);
    if (!r.ok() || !shared_here()) return;
    const uint64_t root = save_root();
    if (!root) return;
    for (uint32_t id = 1; id <= kSceneMax && (id >> 3) < np; ++id) {
        if (!(p[id >> 3] & (1u << (id & 7))) || played(root, id) || s.played_want.test(id)) continue;
        s.played_want.set(id);
        s.played_origin[id] = origin;
    }
    for (size_t row = 1; row < kRowCount && row < nr; ++row) {
        const uint8_t lack = rows[row] & kRowBits & ~guest::rd<uint8_t>(root + kSeenRows + row);
        s.rows_want[row] |= lack;
    }
}

void on_lines_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint8_t n = r.u8();
    if (!r.ok() || !shared_here()) return;
    for (uint8_t k = 0; k < n; ++k) {
        const uint8_t b = r.u8();
        Line l;
        l.id = r.u8();
        l.step = static_cast<int16_t>(r.u16());
        l.bits = r.u16();
        if (!r.ok()) return;
        if (b < 1 || b > kLineMax || !valid(l)) continue;
        Line& want = s.lines_want[b];
        const bool further = !want.id || want.id != l.id || l.step > want.step;
        if (further) {
            want = l;
            s.line_origin[b] = origin;
            s.line_tries[b] = 0;
        } else if (l.step == want.step) {
            want.bits |= l.bits;
        }
    }
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    const uint64_t root = save_root();
    if (!root || !shared_here()) return;
    if (c && safe_now(in_game)) {
        apply_rows(root);
        apply_played(c, root);
        // A line's step runs that step's actions, some of them world ones (kind 6: the house arrival 0x9bdf50): the authority's save is
        // the group's, so only the authority moves its lines on for another member; a member's own lines move by its own scenes.
        if (link::is_authority()) apply_lines(c, root);
    }
    const double now = qpc::seconds();
    const bool all = s.send_all || now - s.all_at >= kAllEvery;
    const bool history_sent = share_history(root, all);
    const bool sent = share_lines(root, all) && history_sent;   // both every time: one failed send never holds the other back
    s.have_seen = true;
    if (!sent || !all) return;
    s.send_all = false;
    s.all_at = now;
}

void on_session_change() {
    State& s = st();
    s = State{};
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::events

// The digests' reader (pkp_coop_digest.cpp): the scenes the save has played.
namespace nxrt::games::pkp::coop::digest {

bool scenes(Entries& out) {
    const uint64_t root = coop::events::save_root();
    if (!root) return false;
    for (uint32_t id = 1; id <= coop::events::kSceneMax; ++id)
        if (coop::events::played(root, id)) out.push_back({id, 1});
    return true;
}

}  // namespace nxrt::games::pkp::coop::digest
