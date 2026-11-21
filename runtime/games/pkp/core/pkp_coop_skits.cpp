// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's story talks (pkp_coop_internal.h). The story talk each character has to give, its talk bubble and
// the objective that follows it are the authority's on every member. RE: modding-docs coop-v2/STORY-SKITS-RE.md.
//
// A character's story talk ("skit": the scene its next talk plays) lives in its talk record, one 0x38-byte record per data id
// (index data id + 4, data ids 1..0x415): +0x14 f32 a timer, +0x18 u8 the timer is on, +0x1a u16 the scene, +0x1c u16 the content
// that set it. A process reads its own records (R+0x93ba0) or, on a Link Play visitor (every co-op member), the visitor copy
// (R+0x15567a8); Cloud Island has its own (R+0xae5138). The professor (data id 0x2c) keeps his in the stage data instead, at
// R+0x95bbe0 with the same field offsets. The getter 0x3b2278(data id) picks the record that way; the talk selector (0x4c1a18..
// 0x4c7058, through 0x3ad37c), the "has something to say" test 0x3b1f9c and the objective list's state 0x10f180 (a row's goal or
// start scene against the getter) read it. The only writers of a story are the contents actions (0x7da "set story talk",
// 0x3b2570, by the talking component; 0x7df), quest effect 0x2c (0x3b2360, a world kind the authority alone runs) and the timer
// tick, all funnelling into the setter 0x3ad540(data id, scene, content): it writes the record, turns its timer off, and for the
// professor refreshes his talk mark (0x3b1800), then tells the story-talk marker set (0x11dbc0 on [0x3484660]) the new scene.
// The contents run per process, advanced by notices that arrive only where they were posted (a talked-to or an event's end on the
// acting player's process; pkp_coop_feed.cpp posts a member's on the authority too), and nothing carries the records: so a member's
// story talks stayed where its own scheduler left them (alpha 13: after the first habitat's confirm only the host saw the creature's
// and the professor's next talks, and the members' objective stayed "build the habitat").
//   The authority: every kSendEvery seconds it sends the records that changed since it last sent them (StorySkits), and every one
//   of them every kAllEvery seconds and when a member joins. The professor's (stage data) is sent only once play has run in this
//   area, with the area, and a member takes it only there.
//   A member: at a safe moment (in the group's world, can walk, no talk) it compares its own record with the authority's latest copy
//   every kScanEvery seconds and, where the scene or content differ, calls the game's own setter 0x3ad540 with the authority's
//   values (so the talk mark and the marker set follow), then the authority's timer when it is on. A record this member's own
//   contents changed since the last look (its player just played the talk, and the authority has not answered yet) is held for
//   kHold seconds first, so its own step is not undone before the authority's follows.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::skits {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kOwnRecords = 0x93ba0;        // save root + : this save's talk records
constexpr uint64_t kVisitorRecords = 0x15567a8;  // save root + : a visitor's copy of them
constexpr uint64_t kProfessorBlock = 0x95bbe0;   // save root + : the professor's story talk (stage data)
constexpr uint64_t kRecordBytes = 0x38;
constexpr uint64_t kTimer = 0x14;                // f32
constexpr uint64_t kTimerOn = 0x18;              // u8
constexpr uint64_t kScene = 0x1a;                // u16
constexpr uint64_t kContent = 0x1c;              // u16
constexpr uint64_t kSetter = 0x3ad540;           // (data id, scene, content): the game's own story-talk setter
constexpr uint16_t kProfessor = 0x2c;
constexpr uint16_t kSceneMax = 0xbb7;            // a valid scene: 1..0xbb7 (0x3ad540's own bound)
constexpr uint16_t kIds = 0x416;                 // data ids 1..0x415 have a record of their own (0x3b2278's bound)
constexpr size_t kPerMessage = 256;
constexpr double kSendEvery = 1.0;
constexpr double kAllEvery = 30.0;
constexpr double kScanEvery = 0.5;
constexpr double kHold = 15.0;
constexpr size_t kApplyMax = 16;                 // setter calls per look
constexpr double kLogGap = 5.0;                  // a look's "set" lines after its first: at most once per this many seconds

struct Talk {
    uint16_t scene = 0, content = 0;
    uint8_t on = 0;
    float timer = -1.0f;
    // The setter keeps the content only with a valid scene (1..0xbb7), so the content counts only then.
    bool valid() const { return static_cast<uint16_t>(scene - 1) < kSceneMax; }
    bool same(const Talk& o) const { return scene == o.scene && (!valid() || content == o.content); }
};

struct State {
    // The authority.
    std::array<Talk, kIds> sent{};
    std::array<bool, kIds> sent_have{};
    double sent_at = -1e9, all_at = -1e9;
    bool send_all = true;
    int settled_stage = -1;
    // A member.
    std::array<Talk, kIds> theirs{};
    std::array<bool, kIds> known{};
    uint8_t professor_stage = 0;
    std::array<Talk, kIds> seen{};       // this save's value at the last look (or right after the last write)
    std::array<double, kIds> hold{};     // own change: not written before this time
    std::array<Talk, kIds> stuck{};      // an authority value the setter did not leave in this save: not tried again
    std::array<bool, kIds> is_stuck{};
    bool seen_valid = false;
    double scan_at = -1e9;
    double logged_at = -1e9;
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() { return guest::rd<uint64_t>(base() + layout::kSaveRoot); }
uint8_t area_stage() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(gm, layout::kStateStage + 1) ? guest::rd<uint8_t>(gm + layout::kStateStage) : 0;
}

// The record 0x3b2278 reads for this data id on this process (Cloud Island excluded by the callers), or 0.
uint64_t record_of(uint64_t root, uint16_t id) {
    if (!root || id == 0 || id >= kIds) return 0;
    const uint64_t at = id == kProfessor ? root + kProfessorBlock
                                         : root + ((session_bits() & layout::kSessionVisitor) ? kVisitorRecords : kOwnRecords) +
                                               (static_cast<uint64_t>(id) + 4) * kRecordBytes;
    return guest::ok(at, kRecordBytes) ? at : 0;
}

Talk read_talk(uint64_t at) {
    Talk t;
    t.scene = guest::rd<uint16_t>(at + kScene);
    t.content = guest::rd<uint16_t>(at + kContent);
    t.on = guest::rd<uint8_t>(at + kTimerOn);
    t.timer = guest::rd<float>(at + kTimer);
    return t;
}

// The authority: the records that changed since last sent (all of them when due), kPerMessage per message.
void send(bool in_game) {
    State& s = st();
    if (!link::is_authority() || !(session_bits() & layout::kSessionOnline) || !link::member_count() || !local_player()) return;
    const uint8_t stage = area_stage();
    if (in_game) s.settled_stage = stage;
    const double now = qpc::seconds();
    if (now - s.sent_at < kSendEvery || stage == layout::kStageCloudIsland) return;
    s.sent_at = now;
    const uint64_t root = save_root();
    if (!root) return;
    const bool all = s.send_all || now - s.all_at >= kAllEvery;
    std::vector<uint16_t> ids;
    for (uint16_t id = 1; id < kIds; ++id) {
        if (id == kProfessor && s.settled_stage != stage) continue;   // stage data: only once play ran in this area
        const uint64_t at = record_of(root, id);
        if (!at) continue;
        const Talk t = read_talk(at);
        if (!all && s.sent_have[id] && s.sent[id].same(t) && s.sent[id].on == t.on) continue;
        s.sent[id] = t;
        s.sent_have[id] = true;
        ids.push_back(id);
    }
    for (size_t first = 0; first < ids.size(); first += kPerMessage) {
        const size_t n = std::min(kPerMessage, ids.size() - first);
        wire::Writer w(link::self());
        w.u8(stage).u16(static_cast<uint16_t>(n));
        for (size_t i = first; i < first + n; ++i) {
            const Talk& t = s.sent[ids[i]];
            uint32_t timer_bits = 0;
            std::memcpy(&timer_bits, &t.timer, sizeof timer_bits);
            w.u16(ids[i]).u16(t.scene).u16(t.content).u8(t.on).u32(timer_bits);
        }
        link::to_members(wire::Type::StorySkits, w.take());
    }
    const size_t total = ids.size();
    if (all) {
        s.send_all = false;
        s.all_at = now;
    } else if (total) {
        NXLOG_I(kTag, "Co-op skits: %zu story talks changed here, sent to the members", total);
    }
}

bool member_safe(bool in_game) {
    return role() == CoopRole::Visit && !link::is_authority() && link::in_session() && (session_bits() & layout::kSessionVisitor) &&
           session::can_walk(in_game) && !talk::talking();
}

// A member: the authority's story talks written where this save differs.
void take(::Ctx* c, bool in_game) {
    State& s = st();
    const double now = qpc::seconds();
    if (now - s.scan_at < kScanEvery || !member_safe(in_game)) return;
    s.scan_at = now;
    const uint8_t stage = area_stage();
    const uint64_t root = save_root();
    if (!root || stage == layout::kStageCloudIsland) return;
    size_t applied = 0;
    for (uint16_t id = 1; id < kIds; ++id) {
        const uint64_t at = record_of(root, id);
        if (!at) continue;
        const Talk here = read_talk(at);
        const Talk& t = s.theirs[id];
        const bool usable = s.known[id] && (id != kProfessor || s.professor_stage == stage);
        if (s.seen_valid && !here.same(s.seen[id]) && !(usable && here.same(t))) {
            s.hold[id] = now + kHold;   // this member's own contents changed it since the last look
            NXLOG_I(kTag, "Co-op skits: character %u's story talk changed here to scene %u (content %u); the authority's is kept "
                    "back for %.0f s", id, here.scene, here.content, kHold);
        }
        s.seen[id] = here;
        if (!usable || here.same(t)) continue;
        if (now < s.hold[id] || applied == kApplyMax || (s.is_stuck[id] && s.stuck[id].same(t))) continue;
        guest::call(c, base() + kSetter, id, t.scene, t.content);
        if (t.on) {
            guest::wr<float>(at + kTimer, t.timer);
            guest::wr<uint8_t>(at + kTimerOn, 1);
        }
        s.seen[id] = read_talk(at);
        ++applied;
        s.is_stuck[id] = !s.seen[id].same(t);
        if (s.is_stuck[id]) {
            s.stuck[id] = t;
            NXLOG_W(kTag, "Co-op skits: character %u: the setter left scene %u (content %u), not the authority's %u (content %u); "
                    "not tried again until the authority's changes", id, s.seen[id].scene, s.seen[id].content, t.scene, t.content);
        }
        if (applied == 1 || now - s.logged_at >= kLogGap)
            NXLOG_I(kTag, "Co-op skits: character %u's story talk set to the authority's: scene %u -> %u (content %u)", id, here.scene,
                    t.scene, t.content);
    }
    if (applied) s.logged_at = now;
    s.seen_valid = true;
}

}  // namespace

void on_skits_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    State& s = st();
    const uint8_t stage = r.u8();
    const uint16_t n = r.u16();
    if (!r.ok() || link::is_authority()) return;
    for (uint16_t i = 0; i < n; ++i) {
        const uint16_t id = r.u16();
        Talk t;
        t.scene = r.u16();
        t.content = r.u16();
        t.on = r.u8();
        const uint32_t timer_bits = r.u32();
        if (!r.ok()) return;
        if (id == 0 || id >= kIds) continue;
        std::memcpy(&t.timer, &timer_bits, sizeof t.timer);
        s.theirs[id] = t;
        s.known[id] = true;
        if (id == kProfessor) s.professor_stage = stage;
    }
}

void tick(::Ctx* c, bool in_game) {
    if (!c) return;
    if (!link::in_session()) {
        if (st().seen_valid || !st().send_all) st() = State{};   // left the session: nothing kept
        return;
    }
    send(in_game);
    take(c, in_game);
}

void on_session_change() { st() = State{}; }

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::skits
