// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's group travel (pkp_coop_internal.h). The group moves to another area together or not at all, and every
// move is decided by the game's own poll (kind 1, "Headed to <area>. Please respond to the poll."): every player votes in the game's
// own UI, and the result is unanimous (0x582ae0). The facts are in modding-docs/coop-v2/TRAVEL-RE.md.
//
// Only the host's move takes the members along (0xbd5a0 on the host files kind 1, and 0xbd84c sends StageMove), so:
//   - a member's area move by index (0xbda20, the hook) never runs: the member asks the host for a poll (0x539408 sends
//     SurveyCommand cmd 1; the host opens the poll in the member's name);
//   - the host's own area move by index runs only after a poll for its stage passed: refused, the poll opens in the host's name, and
//     on a yes the move is made again by call (replay);
//   - a poll a member asked for (the map's StartPoll(5), or the ask above) that passed is carried out by the host, which files the
//     move itself (0xbd5a0 with the arguments of the map's departure, cb 0x7b): vanilla does nothing on the host then (cb 0x7c runs
//     on the requester alone and returns on a visitor).
// A No (or a player who does not answer) leaves everyone where they are: the move is cancelled.
// The host's own map travel is vanilla already (StartPoll(5), then the departure on a yes), and the Dream Island trips (0xbd5a0 from
// 0x99ae98 / 0x99bea8) run as they are: neither reaches 0xbda20.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::travel {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kNetManager = 0x3561a20;      // the network manager; +kOwnSlot: u8, this player's index in the session
constexpr uint64_t kOwnSlot = 0xb3381;
constexpr uint64_t kFlagRoot = 0x3483b60;        // **(main+kFlagRoot) + kPaletteFlag, bit 1: the departure's w2 for Palette Town
constexpr uint64_t kPaletteFlag = 0x13d40d0;
constexpr uint8_t kPaletteTown = 0xc;
constexpr uint64_t kTripRecord = 0x3bb600;       // game state: the trip record; +0 u32 its phase (a move is filed only at >= 5)
constexpr uint64_t kPoll = 0x4d25b4;             // game state: the poll; +0 kind, +1 state, +2 requester, +3 target, +8 result
constexpr uint8_t kKindArea = 1;                 // the poll kind of an area move
constexpr uint8_t kStateResult = 3;              // +1: 1 ask, 2 vote, 3 result, 4 end
constexpr uint8_t kMaxStage = 31;

constexpr uint64_t kMoveByStage = 0xbd5a0;       // (rec, stage, w2, w3, w4, w5): the area move by stage
constexpr uint64_t kMoveByIndex = 0xbda20;       // (rec, index, w2): the area move by destination index (the hook)
constexpr uint64_t kIndexStage = 0xa3ed34;       // (index) -> the stage
constexpr uint64_t kAskPoll = 0x539408;          // (poll, kind, &target): a member asks the host; the host opens it
constexpr uint64_t kOpenPoll = 0x580a98;         // (poll, kind, requester, &target): host only, the poll on every process

constexpr uint8_t kStateEnd = 4;
constexpr uint32_t kRecordReady = 5;             // the trip record's phase from which a filer files (below it refuses silently)

constexpr double kPassedFor = 15.0;   // a passed poll lets its stage's move through this long (a script's StartPoll, then its move)
constexpr double kOpenWait = 2.0;     // the host's own poll did not open by then: the move it held is dropped
constexpr double kAskAgain = 10.0;    // the same stage is not asked about again this soon (a step that tries every frame)
constexpr double kGoWait = 5.0;       // a decided move waits this long for the trip record to be ready

// A poll as last seen open (the struct is cleared once it ends, so its facts are kept while it runs). Its result is taken once, at
// the result state (the host writes +8 before it sets the state, 0x582480); the move is made once the poll ended, as vanilla's
// departure runs only after the end (0x5820bc).
struct Seen {
    bool open = false;
    bool decided = false;
    bool yes = false;
    uint8_t state = 0;
    uint8_t requester = 0;
    uint8_t target = 0;
};

// The host's own index move, held for its poll.
struct Held {
    bool on = false;
    uint32_t index = 0;
    uint32_t w2 = 0;
    uint8_t stage = 0;
    double since = 0.0;
    bool opened = false;
};

// A move the group agreed to, made by the host as soon as the trip record is ready.
struct Go {
    bool on = false;
    bool by_index = false;   // the held move (0xbda20 with its own arguments), else a stage move (0xbd5a0)
    uint32_t index = 0;
    uint32_t w2 = 0;
    uint8_t stage = 0;
    uint8_t requester = 0;
    double since = 0.0;
};

struct State {
    Seen seen;
    Held held;
    Go go;
    uint8_t passed_stage = 0;   // the stage of the last poll that passed, and when
    double passed_at = -1e9;
    uint8_t asked_stage = 0;    // the stage this player last asked about, and when
    double asked_at = -1e9;
    bool replaying = false;     // the hook's own call of the index move: let it run
};
State& st() {
    static State s;
    return s;
}

uint64_t game_state() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(gm, kTripRecord + 0x10) && guest::ok(gm, kPoll + 0x10) ? gm : 0;
}

uint8_t own_slot() {
    const uint64_t nm = guest::rd<uint64_t>(base() + kNetManager);
    return guest::ok(nm, kOwnSlot + 1) ? guest::rd<uint8_t>(nm + kOwnSlot) : 0;
}

bool is_host() { return session_bits() & layout::kSessionHost; }
bool in_session() { return (session_bits() & layout::kSessionOnline) && link::in_session(); }

uint8_t palette_flag() {
    const uint64_t p = guest::rd<uint64_t>(base() + kFlagRoot);
    if (!guest::ok(p, 8)) return 0;
    const uint64_t r = guest::rd<uint64_t>(p);
    return guest::ok(r + kPaletteFlag, 1) ? (guest::rd<uint8_t>(r + kPaletteFlag) >> 1) & 1 : 0;
}

// Scratch below the game thread's stack (free at the frame tick and inside a hook, where the game's frame is above it).
uint64_t scratch(::Ctx* c) { return (c->x[31] - 0x100) & ~uint64_t{0xf}; }

// The target goes to the poll by pointer: a u32 on the scratch stack, the stack pointer below it.
void ask_poll(::Ctx* c, uint64_t gm, uint8_t stage) {
    const uint64_t sp = scratch(c);
    guest::wr<uint32_t>(sp + 0x10, stage);
    guest::call(c, base() + kAskPoll, gm + kPoll, kKindArea, sp + 0x10, 0, 0, sp);
}
void open_poll(::Ctx* c, uint64_t gm, uint8_t requester, uint8_t stage) {
    const uint64_t sp = scratch(c);
    guest::wr<uint32_t>(sp + 0x10, stage);
    guest::call(c, base() + kOpenPoll, gm + kPoll, kKindArea, requester, sp + 0x10, 0, sp);
}

bool passed_recently(const State& s, uint8_t stage) { return s.passed_stage == stage && qpc::seconds() - s.passed_at < kPassedFor; }
bool asked_recently(const State& s, uint8_t stage) { return s.asked_stage == stage && qpc::seconds() - s.asked_at < kAskAgain; }
bool poll_open(uint64_t gm) {
    const uint8_t kind = guest::rd<uint8_t>(gm + kPoll);
    return kind >= 1 && kind <= 5;
}

// A poll that passed, once it ended: the host makes the move when vanilla does not make it by itself.
void on_passed(State& s, const Seen& p) {
    if (!is_host()) return;
    if (p.requester == own_slot()) {
        if (!s.held.on || s.held.stage != p.target) return;   // the host's own map trip or script poll: vanilla moves
        s.go = Go{true, true, s.held.index, s.held.w2, s.held.stage, p.requester, qpc::seconds()};
        s.held = Held{};
        return;
    }
    s.go = Go{true, false, 0, p.target == kPaletteTown ? palette_flag() : 0u, p.target, p.requester, qpc::seconds()};
}

// The agreed move, on the host: made once the trip record is ready, or dropped after kGoWait.
void make_move(::Ctx* c, State& s, uint64_t gm) {
    if (!s.go.on) return;
    const uint8_t current = guest::rd<uint8_t>(gm + layout::kStateStage);
    if (current == s.go.stage) {
        s.go = Go{};
        return;
    }
    if (guest::rd<uint32_t>(gm + kTripRecord) < kRecordReady) {
        if (qpc::seconds() - s.go.since > kGoWait) {
            NXLOG_W(kTag, "Co-op travel: the group said yes to stage %u, but the game was not ready to travel; nobody moves", s.go.stage);
            s.go = Go{};
        }
        return;
    }
    const Go g = s.go;
    s.go = Go{};
    if (g.by_index) {
        s.replaying = true;
        guest::call(c, base() + kMoveByIndex, gm + kTripRecord, g.index, g.w2, 0, 0, scratch(c));
        s.replaying = false;
    } else {
        guest::call_n<6>(c, base() + kMoveByStage, {gm + kTripRecord, g.stage, g.w2, 1, 0, 0}, scratch(c));
    }
    NXLOG_I(kTag, "Co-op travel: the group said yes to player %u's move; the host takes everyone to stage %u (%s, record phase now %u)",
            g.requester, g.stage, g.by_index ? "by index" : "by stage", guest::rd<uint32_t>(gm + kTripRecord));
}

}  // namespace

int on_area_move(::Ctx* c) {
    State& s = st();
    if (s.replaying || !in_session() || link::member_count() == 0) return 0;   // alone, the game's own move (and poll) is enough
    const uint64_t gm = game_state();
    if (!gm) return 0;
    const uint32_t index = static_cast<uint32_t>(c->x[1]);
    const uint32_t w2 = static_cast<uint32_t>(c->x[2]);
    const uint8_t stage = static_cast<uint8_t>(guest::call(c, base() + kIndexStage, index, 0, 0, 0, 0, scratch(c)));
    const uint8_t current = guest::rd<uint8_t>(gm + layout::kStateStage);
    if (stage == 0 || stage > kMaxStage || stage == current) return 0;   // the game refuses or moves nowhere; let it decide
    if (is_host() && passed_recently(s, stage)) return 0;                // a script's own poll for it passed: the group goes
    c->x[0] = 0;
    // Refused. A new poll only when none is open and this stage was not just asked about (a step may try every frame).
    if (passed_recently(s, stage) || s.held.on || s.go.on || poll_open(gm) || asked_recently(s, stage)) return 1;
    s.asked_stage = stage;
    s.asked_at = qpc::seconds();
    if (!is_host()) {
        // A member never moves alone: the host's move brings it, after a poll the host opens in its name.
        ask_poll(c, gm, stage);
        NXLOG_I(kTag, "Co-op travel: this member's move to stage %u (index %u) is held; the host is asked for a poll", stage, index);
        return 1;
    }
    s.held = Held{true, index, w2, stage, qpc::seconds(), false};
    open_poll(c, gm, own_slot(), stage);
    NXLOG_I(kTag, "Co-op travel: the host's move to stage %u (index %u) waits for the group's poll", stage, index);
    return 1;
}

void tick(::Ctx* c) {
    State& s = st();
    const uint64_t gm = in_session() ? game_state() : 0;
    if (!gm) {
        const bool replaying = s.replaying;
        s = State{};
        s.replaying = replaying;
        return;
    }
    const uint8_t kind = guest::rd<uint8_t>(gm + kPoll);
    const uint8_t state = guest::rd<uint8_t>(gm + kPoll + 1);
    const bool area = kind == kKindArea && state >= 1;
    if (area && s.seen.open && state < s.seen.state) s.seen = Seen{};   // a new poll in place of the last one
    if (area) {
        s.seen.open = true;
        s.seen.state = state;
        s.seen.requester = guest::rd<uint8_t>(gm + kPoll + 2);
        s.seen.target = guest::rd<uint8_t>(gm + kPoll + 3);
        if (s.held.on && s.seen.requester == own_slot() && s.seen.target == s.held.stage) s.held.opened = true;
        if (state >= kStateResult && !s.seen.decided) {
            s.seen.decided = true;
            s.seen.yes = guest::rd<uint8_t>(gm + kPoll + 8) == 1 && s.seen.target && s.seen.target <= kMaxStage;
            if (s.seen.yes) {
                s.passed_stage = s.seen.target;   // from now: the move is the group's, nobody asks about it again
                s.passed_at = qpc::seconds();
            } else {
                NXLOG_I(kTag, "Co-op travel: the group did not agree on player %u's move to stage %u; everyone stays", s.seen.requester,
                        s.seen.target);
                if (s.held.on && s.seen.requester == own_slot()) s.held = Held{};
            }
        }
    }
    if (s.seen.open && (!area || state >= kStateEnd)) {
        // Ended (or replaced): a yes is carried out now; a poll whose result was not seen leaves a held move off.
        if (s.seen.decided && s.seen.yes) on_passed(s, s.seen);
        else if (!s.seen.decided && s.held.on && s.seen.requester == own_slot()) s.held = Held{};
        s.seen = area ? Seen{true, true, false, state, s.seen.requester, s.seen.target} : Seen{};   // a lingering end is not re-run
    }
    if (s.held.on && !s.held.opened && qpc::seconds() - s.held.since > kOpenWait) {
        NXLOG_W(kTag, "Co-op travel: the poll for the host's move to stage %u did not open; the move is off", s.held.stage);
        s.held = Held{};
    }
    make_move(c, s, gm);
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::travel
