// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's group travel (pkp_coop_internal.h). The group moves to another area together or not at all, and every
// move is decided by the game's own poll (kind 1, "Headed to <area>. Please respond to the poll."): every player votes in the game's
// own UI, and the result is unanimous (0x582ae0). The facts are in modding-docs/coop-v2/TRAVEL-RE.md.
//
// Only the host's move takes the members along (0xbd5a0 on the host files kind 1, and 0xbd84c sends StageMove), and only the host
// opens the poll (0x580a98), so:
//   - a member never moves by itself: its area move by index (0xbda20, the hook) is refused and its gate entry held, and it asks the
//     host for the poll (TravelAsk); the host opens it in the member's name and, on a yes, files the move itself (0xbd5a0 with the
//     arguments of the map's departure, cb 0x7b): vanilla does nothing on the host then (cb 0x7c runs on the requester alone and
//     returns on a visitor);
//   - the host's own area move by index runs only after a poll for its stage passed: refused, the poll opens in the host's name, and
//     on a yes the move is made again by call (replay);
//   - an area gate is polled at its door, before anything fades: the gate entry (0x7b0d80, which opens the gate's passage) is held
//     and the poll opens; a yes enters the gate (replay) and the passage's exit then moves the group (0x7cbc88 -> 0xbda20 at
//     0x7cbefc); a No leaves the player at the closed gate. The exit itself is never refused on the host: by the time it calls
//     0xbda20 it has closed the screen (0xa9199c), locked the player (0x243ce0 id 0x12 at 0x7ccd00) and faded out (0xa9fcc4), and it
//     has no way back (state 4 only waits, then resets itself, nothing reopens the screen), so a refusal there was a black screen
//     for good.
// The host's poll is opened for a stage the host has not been to yet, too: 0x580a98 refuses kind 1 when the target's bit in the
// stages it may name (R + 0x98) is clear, and a gate's first trip leads to such an area. The bit is set for the call alone.
// A No (or a player who does not answer) leaves everyone where they are: the move is cancelled.
// The host's own map travel is vanilla already (StartPoll(5), then the departure on a yes), and the Dream Island trips (0xbd5a0 from
// 0x99ae98 / 0x99bea8) run as they are: neither reaches 0xbda20.
#include "core/pkp_coop_internal.h"
#include "core/pkp_coop_wire.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::travel {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kNetManager = 0x3561a20;      // the network manager; +kOwnSlot: u8, this player's index in the session
constexpr uint64_t kOwnSlot = 0xb3381;
constexpr uint8_t kMaxSlot = 3;                  // poll requesters are 0..3 (participants poll[4..7])
constexpr uint64_t kFlagRoot = 0x3483b60;        // **(main+kFlagRoot) + kPaletteFlag, bit 1: the departure's w2 for Palette Town
constexpr uint64_t kPaletteFlag = 0x13d40d0;
constexpr uint8_t kPaletteTown = 0xc;
constexpr uint64_t kPollStages = 0x98;           // R (layout::kSaveRoot) + this: u8[4], a bit per stage the area poll may name
constexpr uint64_t kTripRecord = 0x3bb600;       // game state: the trip record; +0 u32 its phase (a move is filed only at >= 5)
constexpr uint64_t kPoll = 0x4d25b4;             // game state: the poll; +0 kind, +1 state, +2 requester, +3 target, +8 result
constexpr uint8_t kKindArea = 1;                 // the poll kind of an area move
constexpr uint8_t kStateResult = 3;              // +1: 1 ask, 2 vote, 3 result, 4 end
constexpr uint8_t kMaxStage = 31;

constexpr uint64_t kMoveByStage = 0xbd5a0;       // (rec, stage, w2, w3, w4, w5): the area move by stage
constexpr uint64_t kMoveByIndex = 0xbda20;       // (rec, index, w2): the area move by destination index (the hook)
constexpr uint64_t kIndexStage = 0xa3ed34;       // (index) -> the stage
constexpr uint64_t kOpenPoll = 0x580a98;         // (poll, kind, requester, &target): host only, the poll on every process
constexpr uint64_t kGateEntry = 0x7b0d80;        // (trigger): an area gate's entry, opens its passage (the hook)
constexpr uint64_t kGateExitCall = 0x7cbf00;     // the return address of the passage exit's 0xbda20 (0x7cbc88 state 3)
constexpr uint64_t kRoomManager = 0x3577da0;     // *(main+this): the lookup 0x724680(rm, &{u32 trigger+0x18, u16 trigger+0x1c})
constexpr uint64_t kRoomFind = 0x724680;         // -> the gate's door (as 0x7b0690 finds it)
constexpr uint64_t kTriggerKey = 0x18;
constexpr uint64_t kDoorGate = 0x118;            // the door: s8, its gate id (0..31)
constexpr uint64_t kGateIndex = 0x7cc01c;        // (gate id) -> the destination index (table slot 401 row +3; 0xff for none)

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

// What the host holds for its own poll, and makes on a yes.
enum class Kind : uint8_t {
    Index,   // an area move by index (0xbda20 with its own arguments)
    Gate,    // an area gate's entry (0x7b0d80 on its trigger): the passage, whose exit then moves the group
    Stage,   // a member's poll: the host files the move by stage (0xbd5a0)
};

// The host's own move or gate entry, held for its poll.
struct Held {
    bool on = false;
    Kind kind = Kind::Index;
    uint64_t trigger = 0;   // Gate: the gate's trigger
    uint32_t index = 0;
    uint32_t w2 = 0;
    uint8_t stage = 0;
    uint8_t from = 0;       // the stage the player was in
    double since = 0.0;
    bool opened = false;
};

// A move the group agreed to, made by the host as soon as the game is ready for it.
struct Go {
    bool on = false;
    Held what;
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
    uint8_t walk_stage = 0;     // the stage whose gate the host entered after a poll (or alone): its exit is the group's move
    bool replaying = false;     // the hook's own call of the index move or the gate entry: let it run
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
uint8_t current_stage(uint64_t gm) { return guest::rd<uint8_t>(gm + layout::kStateStage); }

uint8_t palette_flag() {
    const uint64_t p = guest::rd<uint64_t>(base() + kFlagRoot);
    if (!guest::ok(p, 8)) return 0;
    const uint64_t r = guest::rd<uint64_t>(p);
    return guest::ok(r + kPaletteFlag, 1) ? (guest::rd<uint8_t>(r + kPaletteFlag) >> 1) & 1 : 0;
}

// Scratch below the game thread's stack (free at the frame tick and inside a hook, where the game's frame is above it).
uint64_t scratch(::Ctx* c) { return (c->x[31] - 0x100) & ~uint64_t{0xf}; }

// The host opens the area poll in requester's name; the target goes by pointer (a u32 on the scratch stack, the stack pointer below
// it). 0x580a98 names only a stage whose bit is set in R + kPollStages (0x5810b4: a clear bit refuses kind 1), which is the case for
// every map destination but not for an area a gate leads to for the first time: the bit is set for the call and put back after it.
// Palette Town also needs R + 0x6cfdd9 bit 5 there; no gate leads to it, and that bit is left alone.
void open_poll(::Ctx* c, uint64_t gm, uint8_t requester, uint8_t stage) {
    const uint64_t sp = scratch(c);
    guest::wr<uint32_t>(sp + 0x10, stage);
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t at = r + kPollStages + (stage >> 3);
    const uint8_t bit = static_cast<uint8_t>(1u << (stage & 7));
    const bool lend = guest::ok(at, 1) && !(guest::rd<uint8_t>(at) & bit);
    if (lend) guest::wr<uint8_t>(at, guest::rd<uint8_t>(at) | bit);
    guest::call(c, base() + kOpenPoll, gm + kPoll, kKindArea, requester, sp + 0x10, 0, sp);
    if (lend) guest::wr<uint8_t>(at, guest::rd<uint8_t>(at) & static_cast<uint8_t>(~bit));
}

// A member asks the host for the poll (it opens it in the member's name, on_ask_message).
bool ask_host(uint8_t stage) {
    return link::to_authority(wire::Type::TravelAsk, wire::Writer(link::self()).u8(own_slot()).u8(stage).take());
}

bool passed_recently(const State& s, uint8_t stage) { return s.passed_stage == stage && qpc::seconds() - s.passed_at < kPassedFor; }
bool asked_recently(const State& s, uint8_t stage) { return s.asked_stage == stage && qpc::seconds() - s.asked_at < kAskAgain; }
bool poll_open(uint64_t gm) {
    const uint8_t kind = guest::rd<uint8_t>(gm + kPoll);
    return kind >= 1 && kind <= 5;
}

// The stage an area gate's trigger leads to (0: none): its door as 0x7b0690 finds it, the door's gate id, that gate's destination
// index and the index's stage, each through the game's own lookup.
uint8_t gate_stage(::Ctx* c, uint64_t trigger, uint32_t* index) {
    const uint64_t rm = guest::rd<uint64_t>(base() + kRoomManager);
    if (!guest::ok(rm, 8) || !guest::ok(trigger + kTriggerKey, 6)) return 0;
    const uint64_t sp = scratch(c);
    guest::wr<uint32_t>(sp + 0x10, guest::rd<uint32_t>(trigger + kTriggerKey));
    guest::wr<uint16_t>(sp + 0x14, guest::rd<uint16_t>(trigger + kTriggerKey + 4));
    const uint64_t door = guest::call(c, base() + kRoomFind, rm, sp + 0x10, 0, 0, 0, sp);
    if (!guest::ok(door + kDoorGate, 1)) return 0;
    const int8_t gate = guest::rd<int8_t>(door + kDoorGate);
    if (gate < 0 || gate > static_cast<int8_t>(kMaxStage)) return 0;
    *index = static_cast<uint8_t>(guest::call(c, base() + kGateIndex, static_cast<uint8_t>(gate), 0, 0, 0, 0, sp));
    if (*index > kMaxStage) return 0;
    const uint8_t stage = static_cast<uint8_t>(guest::call(c, base() + kIndexStage, *index, 0, 0, 0, 0, sp));
    return stage <= kMaxStage ? stage : 0;
}

// A poll that passed, once it ended: the host makes the move when vanilla does not make it by itself.
void on_passed(State& s, const Seen& p) {
    if (!is_host()) return;
    if (p.requester == own_slot()) {
        if (!s.held.on || s.held.stage != p.target) return;   // the host's own map trip or script poll: vanilla moves
        s.go = Go{true, s.held, p.requester, qpc::seconds()};
        s.held = Held{};
        return;
    }
    Held stage_move{};
    stage_move.kind = Kind::Stage;
    stage_move.stage = p.target;
    stage_move.w2 = p.target == kPaletteTown ? palette_flag() : 0u;
    s.go = Go{true, stage_move, p.requester, qpc::seconds()};
}

// The agreed move, on the host: a gate entered at once (its exit files the move), a move filed once the trip record is ready, or
// dropped after kGoWait.
void make_move(::Ctx* c, State& s, uint64_t gm) {
    if (!s.go.on) return;
    const uint8_t current = current_stage(gm);
    const Held& w = s.go.what;
    if (current == w.stage) {
        s.go = Go{};
        return;
    }
    if (w.kind == Kind::Gate) {
        const Go g = s.go;
        s.go = Go{};
        if (current != g.what.from || !guest::ok(g.what.trigger + kTriggerKey, 6)) {
            NXLOG_W(kTag, "Co-op travel: the group said yes to the gate to stage %u, but the host is no longer at it; nobody moves", g.what.stage);
            return;
        }
        s.walk_stage = g.what.stage;
        s.replaying = true;
        guest::call(c, base() + kGateEntry, g.what.trigger, 0, 0, 0, 0, scratch(c));
        s.replaying = false;
        NXLOG_I(kTag, "Co-op travel: the group said yes; the host goes through the gate to stage %u, and its exit takes everyone", g.what.stage);
        return;
    }
    if (guest::rd<uint32_t>(gm + kTripRecord) < kRecordReady) {
        if (qpc::seconds() - s.go.since > kGoWait) {
            NXLOG_W(kTag, "Co-op travel: the group said yes to stage %u, but the game was not ready to travel; nobody moves", w.stage);
            s.go = Go{};
        }
        return;
    }
    const Go g = s.go;
    s.go = Go{};
    if (g.what.kind == Kind::Index) {
        s.replaying = true;
        guest::call(c, base() + kMoveByIndex, gm + kTripRecord, g.what.index, g.what.w2, 0, 0, scratch(c));
        s.replaying = false;
    } else {
        guest::call_n<6>(c, base() + kMoveByStage, {gm + kTripRecord, g.what.stage, g.what.w2, 1, 0, 0}, scratch(c));
    }
    NXLOG_I(kTag, "Co-op travel: the group said yes to player %u's move; the host takes everyone to stage %u (%s, record phase now %u)",
            g.requester, g.what.stage, g.what.kind == Kind::Index ? "by index" : "by stage", guest::rd<uint32_t>(gm + kTripRecord));
}

// A move this player wants to make that needs the group's poll first: false when one runs already (held, decided or open).
bool busy(const State& s, uint64_t gm) { return s.held.on || s.go.on || poll_open(gm); }

}  // namespace

int on_area_move(::Ctx* c) {
    State& s = st();
    if (s.replaying || !in_session() || link::member_count() == 0) return 0;   // alone, the game's own move (and poll) is enough
    const uint64_t gm = game_state();
    if (!gm) return 0;
    const uint32_t index = static_cast<uint32_t>(c->x[1]);
    const uint32_t w2 = static_cast<uint32_t>(c->x[2]);
    const uint8_t stage = static_cast<uint8_t>(guest::call(c, base() + kIndexStage, index, 0, 0, 0, 0, scratch(c)));
    const uint8_t current = current_stage(gm);
    if (stage == 0 || stage > kMaxStage || stage == current) return 0;   // the game refuses or moves nowhere; let it decide
    if (is_host() && c->x[30] - base() == kGateExitCall) {
        // A gate's passage exit: polled at its door (on_gate_entry), and past the point of no return (see the header). The host's
        // move takes every member along.
        if (s.walk_stage != stage)
            NXLOG_W(kTag, "Co-op travel: the host walked through a gate to stage %u that had no poll at its door (entered before a "
                          "member joined); the move goes on and takes everyone, a refusal here would leave the screen black", stage);
        s.walk_stage = 0;
        return 0;
    }
    if (is_host() && passed_recently(s, stage)) return 0;                // a script's own poll for it passed: the group goes
    c->x[0] = 0;
    // Refused. A new poll only when none is open and this stage was not just asked about (a step may try every frame).
    if (passed_recently(s, stage) || busy(s, gm) || asked_recently(s, stage)) return 1;
    s.asked_stage = stage;
    s.asked_at = qpc::seconds();
    if (!is_host()) {
        // A member never moves alone: the host's move brings it, after a poll the host opens in its name.
        ask_host(stage);
        NXLOG_I(kTag, "Co-op travel: this member's move to stage %u (index %u) is held; the host is asked for a poll", stage, index);
        return 1;
    }
    s.held = Held{true, Kind::Index, 0, index, w2, stage, current, qpc::seconds(), false};
    open_poll(c, gm, own_slot(), stage);
    NXLOG_I(kTag, "Co-op travel: the host's move to stage %u (index %u) waits for the group's poll", stage, index);
    return 1;
}

int on_gate_entry(::Ctx* c) {
    State& s = st();
    if (s.replaying || !in_session()) return 0;
    const uint64_t gm = game_state();
    if (!gm) return 0;
    const uint64_t trigger = c->x[0];
    uint32_t index = 0;
    const uint8_t stage = gate_stage(c, trigger, &index);
    const uint8_t current = current_stage(gm);
    if (stage == 0 || stage == current) return 0;   // no area move behind this gate (or not readable): the game's own entry
    if (link::member_count() == 0) {
        if (is_host()) s.walk_stage = stage;        // entered alone: its exit is this player's own move
        return 0;
    }
    // Held: the gate stays closed and nothing fades until the group said yes. 0x7b0d80 returns nothing its caller (0x7b0690) uses.
    c->x[0] = 0;
    if (busy(s, gm)) return 1;
    s.asked_stage = stage;
    s.asked_at = qpc::seconds();
    if (!is_host()) {
        ask_host(stage);
        NXLOG_I(kTag, "Co-op travel: this member's gate to stage %u (index %u) waits; the host is asked for a poll", stage, index);
        return 1;
    }
    s.held = Held{true, Kind::Gate, trigger, index, 0, stage, current, qpc::seconds(), false};
    open_poll(c, gm, own_slot(), stage);
    NXLOG_I(kTag, "Co-op travel: the host's gate to stage %u (index %u) waits for the group's poll", stage, index);
    return 1;
}

void on_ask_message(::Ctx* c, int from, uint8_t origin, wire::Reader& r) {
    const uint8_t slot = r.u8();
    const uint8_t stage = r.u8();
    if (!r.ok() || !c) return;
    const uint64_t gm = in_session() && is_host() ? game_state() : 0;
    if (!gm || slot > kMaxSlot || stage == 0 || stage > kMaxStage || stage == current_stage(gm)) {
        NXLOG_W(kTag, "Co-op travel: node %d (player %u) asked for a poll to stage %u; not here (host %d, stage %u)", from, slot, stage,
                is_host() ? 1 : 0, gm ? current_stage(gm) : 0);
        return;
    }
    if (busy(st(), gm)) {
        NXLOG_I(kTag, "Co-op travel: player %u asked for a poll to stage %u while another runs; not opened", slot, stage);
        return;
    }
    open_poll(c, gm, slot, stage);
    NXLOG_I(kTag, "Co-op travel: player %u (node %d, origin %u) asked to go to stage %u; the poll opens in its name%s", slot, from, origin,
            stage, poll_open(gm) ? "" : ", but the game did not open it");
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
        NXLOG_W(kTag, "Co-op travel: the poll for the host's %s to stage %u did not open; %s", s.held.kind == Kind::Gate ? "gate" : "move",
                s.held.stage, s.held.kind == Kind::Gate ? "the gate stays closed" : "the move is off");
        s.held = Held{};
    }
    if (s.walk_stage && current_stage(gm) == s.walk_stage) s.walk_stage = 0;   // arrived: the walk is over
    make_move(c, s, gm);
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::travel
