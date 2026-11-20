// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's contents scenes (pkp_coop_internal.h; RE: modding-docs coop-v2/KYOGRE-BEACH-RE.md).
// A content record's "play scene" command (2000, 0x12b544(cmd): scene u16 at cmd +4) queues the scene's event with 0x2e6ce0, and the
// event queue 0x45d5a0 drops it when it cannot take it now (the same event queued or running, a stage-change request below phase 5
// (0x92210 on GM+0x3bb600), the bit-13 player test, no free queue slot). The command does not look: the record counts as run, and a
// record of mode 0 runs once (0x134b80 takes it out of its list, its saved progress says done). So a refused scene is lost for good,
// saved with it. Kyogre's arc: content 151 (quest 122) queues the beach scene 150 once, when the player stands at
// the beach (its NPCs give their talks right there); without scene 150 quest 122 (close: scene 150 ended) stays 3 and quest 60
// (Kyogre's talk) never comes.
//   0x12b544 (entry hook): a scene not played yet in this save is remembered. Every kEvery seconds at a safe moment (in the group's
//   world, can walk, no talk, slot 0's runner idle, no stage-change request) the game's own 0x12b544 is called again for it, until
//   the scene is played (its
//   played bit, 0x117650) or its event is seen running; the queue drops a second copy while one is queued or running. Dropped on a
//   stage change or after kGiveUp seconds.
//   Saves where it was lost already: a quest at 3 whose scene was lost is given it again when the player is where the record asked
//   (kLost, the record's own condition from its content's data). Run on the authority only, like a quest step.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::contents {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kPlayScene = 0x12b544;     // content command 2000: (cmd) -> queues scene u16 cmd +4
constexpr uint64_t kCmdScene = 4;
constexpr uint64_t kCurrentEvent = 0x2946d0;  // (slot) -> u16 the event slot's runner plays now (1..0x18ff), else none
constexpr uint64_t kSceneTable = 0x300;       // the data-table manager: the scene table (row u16 +0x1a its event)
constexpr uint64_t kSceneEvent = 0x1a;
constexpr uint64_t kPlayedFlags = 0x4a2cec;   // save root +: the played-scene bits (m_playedContentsSkitFlag)
constexpr uint64_t kStageRequest = 0x3bb600;  // game state: the stage-change request (u32 +0 phase, +4 kind); 0x92210 refuses events
                                              // without flag bit 4 while the phase is below 5 (slot 0 kind 0xf excepted)
constexpr uint32_t kSceneMax = 2999;
constexpr uint32_t kEventMax = 0x18ff;
constexpr double kFirst = 3.0;                // the first retry after the refusal
constexpr double kEvery = 3.0;
constexpr double kGiveUp = 600.0;
constexpr size_t kMax = 8;

// A scene a mode-0 record queued once and that was lost in a save already: the quest that waits on it, and the record's condition
// (cells, x / z half-open, y any). Content 151 (#7249) record 9: `3000 (1000, -1, 1162)..(1035, -1, 1222) -> 2000 scene 150`.
struct Lost {
    uint32_t quest, scene;
    int16_t x0, z0, x1, z1;
};
constexpr Lost kLost[] = {
    {122, 150, 1000, 1162, 1035, 1222},
};

struct Pending {
    uint32_t scene;
    uint16_t event;
    uint8_t stage;
    double at, next;
    uint32_t tries;
};

struct State {
    std::vector<Pending> pending;
    bool replaying = false;   // our own call of the command: not remembered again
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() { return guest::rd<uint64_t>(base() + layout::kSaveRoot); }

bool played(uint32_t scene) {
    const uint64_t root = save_root();
    const uint64_t at = root + kPlayedFlags + (scene >> 3);
    return !root || !guest::ok(at, 1) || (guest::rd<uint8_t>(at) & (1u << (scene & 7)));
}

uint8_t stage() {
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    return game && guest::ok(game + layout::kStateStage, 1) ? guest::rd<uint8_t>(game + layout::kStateStage) : 0;
}

uint8_t quest_state(uint32_t id) {
    const uint64_t root = save_root();
    const uint64_t at = root + layout::quest_word(id);
    return root && guest::ok(at, 2) ? static_cast<uint8_t>(guest::rd<uint16_t>(at)) : 0;
}

// The scene's event (scene table row +0x1a), as 0x12b544 reads it; 0 when there is none.
uint16_t scene_event(::Ctx* c, uint32_t scene) {
    const uint64_t mgr = guest::rd<uint64_t>(base() + layout::kTables);
    if (!mgr || !guest::ok(mgr + layout::kTableCount, 4)) return 0;
    const uint64_t off = guest::rd<uint32_t>(mgr + layout::kTableCount) > kSceneTable / 8 ? kSceneTable : 0;
    const uint64_t holder = guest::table_lookup<5>(c, guest::rd<uint64_t>(mgr + 0x10 + off), scene);
    const uint64_t row = holder && guest::ok(holder + 8, 8) ? guest::rd<uint64_t>(holder + 8) : 0;
    if (!row || !guest::ok(row + kSceneEvent, 2)) return 0;
    const uint16_t ev = guest::rd<uint16_t>(row + kSceneEvent);
    return static_cast<uint16_t>(ev - 1) < kEventMax ? ev : 0;
}

// The stage-change request refuses the scene now (0x92210(GM+0x3bb600, 0), the test 0x45d5a0 makes for these events).
bool stage_request(uint32_t* phase, uint32_t* kind) {
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!game || !guest::ok(game + kStageRequest, 8)) return false;
    *phase = guest::rd<uint32_t>(game + kStageRequest);
    *kind = guest::rd<uint32_t>(game + kStageRequest + 4);
    return *phase < 5 && *kind != 0xf;
}

uint16_t current_event(::Ctx* c) {
    const uint16_t ev = static_cast<uint16_t>(guest::call(c, base() + kCurrentEvent, 0));
    return static_cast<uint16_t>(ev - 1) < kEventMax ? ev : 0;
}

void remember(::Ctx* c, uint32_t scene, const char* why) {
    State& s = st();
    for (const Pending& p : s.pending)
        if (p.scene == scene) return;
    if (s.pending.size() >= kMax) return;
    const double now = qpc::seconds();
    s.pending.push_back(Pending{scene, scene_event(c, scene), stage(), now, now + kFirst, 0});
    uint32_t phase = 0, kind = 0;
    const bool refused = stage_request(&phase, &kind);
    NXLOG_I(kTag,
            "Co-op contents: scene %u (event %u) %s; it is asked for again until it plays (now: talking %d, slot 0 plays event %u, "
            "stage-change request phase %d kind %u%s)",
            scene, s.pending.back().event, why, talk::talking() ? 1 : 0, current_event(c), static_cast<int>(phase), kind,
            refused ? ", refuses it" : "");
}

// The game's own command, with a command record on the scratch stack (free at the frame tick).
void play(::Ctx* c, uint32_t scene) {
    State& s = st();
    const uint64_t sp = (c->x[31] - 0x100) & ~uint64_t{0xf};
    guest::wr<uint32_t>(sp + 0x10, 2000);
    guest::wr<uint32_t>(sp + 0x10 + kCmdScene, scene);
    s.replaying = true;
    guest::call(c, base() + kPlayScene, sp + 0x10, 0, 0, 0, 0, sp);
    s.replaying = false;
}

bool in_box(const Lost& l) {
    const uint64_t player = local_player();
    if (!player || !guest::ok(player + layout::kPosition, 12)) return false;
    const float x = guest::rd<float>(player + layout::kPosition), z = guest::rd<float>(player + layout::kPosition + 8);
    const int cx = static_cast<int>((x + 102400.0f) / 100.0f), cz = static_cast<int>((z + 102400.0f) / 100.0f);
    return cx >= l.x0 && cx < l.x1 && cz >= l.z0 && cz < l.z1;
}

}  // namespace

int on_play_scene(::Ctx* c) {
    State& s = st();
    if (s.replaying || !link::in_session() || !c->x[0] || !guest::ok(c->x[0] + kCmdScene, 4)) return 0;
    const uint32_t scene = guest::rd<uint32_t>(c->x[0] + kCmdScene) & 0xffff;
    if (scene - 1 >= kSceneMax || played(scene)) return 0;   // the game's own test refuses these; a played scene is a replay
    remember(c, scene, "was queued by a content");
    return 0;   // the game's code runs: most of the time the queue takes it
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!c || !link::in_session() || !story::world_started()) {
        s.pending.clear();
        return;
    }
    const double now = qpc::seconds();
    const uint8_t here = stage();
    const uint16_t running = current_event(c);
    for (size_t i = 0; i < s.pending.size();) {
        const Pending& p = s.pending[i];
        const bool done = played(p.scene) || (p.event && running == p.event);
        if (done || p.stage != here || now - p.at >= kGiveUp) {
            NXLOG_I(kTag, "Co-op contents: scene %u %s after %u retries", p.scene,
                    done ? "is playing or played" : (p.stage != here ? "dropped (the stage changed)" : "given up"), p.tries);
            s.pending.erase(s.pending.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    uint32_t phase = 0, kind = 0;
    if (!session::can_walk(in_game) || talk::talking() || running || stage_request(&phase, &kind)) return;   // not now
    for (Pending& p : s.pending) {
        if (now < p.next) continue;
        p.next = now + kEvery;
        ++p.tries;
        play(c, p.scene);
        return;   // one a frame
    }
    // Saves where the scene was lost already (the authority: the group's quest).
    if (!link::is_authority()) return;
    for (const Lost& l : kLost) {
        if (quest_state(l.quest) != 3 || played(l.scene) || !in_box(l)) continue;
        bool known = false;
        for (const Pending& p : s.pending) known = known || p.scene == l.scene;
        if (known) continue;
        remember(c, l.scene, "was lost in this save (quest is still waiting on it)");
        return;
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::contents
