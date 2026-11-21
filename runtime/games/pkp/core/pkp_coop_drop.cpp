// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: a dropped player is waited for (pkp_coop_internal.h; RE: modding-docs coop-session/DISCONNECT-RE.md, LEAVE-DOORS-RE.md,
// coop-v2/P0-RE.md P0RE-8, coop-v2/role-census/build/session-hooks.md 2).
//
// Who dropped. The game has no event for a lost member that every process sees: its drop check 0xbca8c fires only on a process whose
// own session is unusable (0x24347c false), and on the host it files nothing for a member's drop (KTGL keeps the member as a ghost,
// then MemberDelete 0x535c80 deletes its character). The co-op channel does see it on every process: a member silent for
// kPeerTimeoutMs leaves (nxrt/coop_channel.h). The channel cannot tell a drop from a leave, so:
//   - a member says it speaks this part (DropHere, its member key) at each session join, and again once its player is in the world;
//     the authority waits only for a member that said it (a member on an older build leaves as it did before: nobody waits for it);
//   - a member's own leave (the pause menu's Save and Quit, 0xbd45c reason 4 from 0xfba740) is announced first (DropLeaving), on the
//     same stream, so it reaches the authority before the leave;
//   - the authority decides: a member that said it and left without announcing it dropped.
//
// The wait (the players still connected stay in the world; nobody is sent home). The authority keeps each dropped member for kLimit
// from the drop, on its own clock, and tells every member who is awaited and how many seconds are left (DropWait, every second and at
// each change), so every process shows the same countdown. While anyone is awaited, on the authority and on every member that heard it:
//   - the player's input is held: the game's input lock (pkp_coop_lock.cpp; it also keeps the menus shut, Run P0RE-8 B);
//   - the world's simulation is paused: the simulation step 0x231890 (game state 0x11; its only caller 0x230b00 tail-calls it, and it
//     returns 1) does not run. Before its switch 0x230b00 runs the network update 0x531aa4 (KTGL, the NM machines), and the UI frame
//     0x9154a8 (the session manager, the menus) and the feature registry's game-frame tick run after it, so the network and this part
//     go on. What stops is in 0x231890: the pad, the trip manager 0x113b4dc, the GM frame 0xb80c0 (story, trip record 0xbaf54, drop
//     check, autosave, clock), the save task runner and the characters (Link Play's snapshots). The game's own self-disconnect on bad
//     quality (0x53ca60 adds dt to NM+0xb3374 while a member's ping is over 1499; at 20 s it disconnects, reason 0) still counts, so its
//     timer is cleared every paused frame;
//   - the pause starts only when nothing is in flight that a skipped step would leave half done: the player can walk (in the world, no
//     event running, the autosave idle), the trip record is idle (phase >= 5) and no save was asked for (the autosave's "now" bit).
//     Once on it holds until the wait ends, the player lets go, or one of those turns busy (a Save and Quit filed from the menu, a save
//     asked for, the host's group move filed by its StageMove): the step runs again at once so that work finishes.
//   - (+) lets go of this player's hold: its lock and pause end (the menu opens, Save and Quit is there). On the authority that also
//     ends the wait for everyone (the world runs on). A member that stops hearing the authority for kSilence lets go by itself.
//   After kLimit the authority gives up: the others play on without the dropped player (it can still join later, as anyone can).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/notices.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::drop {

namespace {

constexpr const char* kTag = "pkp";
constexpr const char* kNotice = "pkp.coop_drop";
constexpr int kNodes = 8;
constexpr double kSettle = 3.0;         // the player walks this long after a session join before it counts as in the world
constexpr double kLimit = 120.0;        // how long the group waits for a dropped player
constexpr double kSendEvery = 1.0;      // the authority's DropWait while anyone is awaited
constexpr double kSilence = 5.0;        // a member that heard no DropWait for this long lets go
constexpr double kNoticeSeconds = 2.5, kNoticeEvery = 1.0;
constexpr double kLogEvery = 30.0;      // the countdown in the log
constexpr uint8_t kWaiting = 1, kJoining = 2, kBack = 3, kGivenUp = 4;

// Player-visible text: English here, one place (the co-op string table takes these lines at the merge).
std::string say(const char* english) { return english; }

struct Node {
    bool known = false;     // it said DropHere this session
    bool leaving = false;   // it announced its own leave
    bool ready = false;     // its player is in the world
    uint8_t key[kMemberKeyBytes] = {};
};

// One awaited player: on the authority, its own record; on a member, the authority's last word.
struct Awaited {
    uint8_t key[kMemberKeyBytes] = {};
    std::string name;
    uint8_t state = kWaiting;
    double until = 0.0;     // the authority: its deadline on the authority's clock
    unsigned left = 0;      // seconds left as the authority last said
};

struct State {
    // every process
    bool in = false;            // this process is in a session's channel
    bool member = false;        // ... as a member (not the authority)
    bool here_sent = false, ready_sent = false;
    double walk_since = -1.0;   // the player can walk since (negative: not now)
    bool leaving = false;       // this member filed its own leave
    // the authority
    Node nodes[kNodes];
    std::vector<Awaited> awaited;   // the authority's waits; a member's copy of the authority's word
    std::vector<Awaited> ended;     // the authority: ends (back / given up) not sent yet
    double next_send = 0.0, next_log = 0.0;
    // a member
    double heard_at = 0.0;
    // the hold on this process
    bool holding = false, paused = false, let_go = false;
    uint64_t buttons = 0;
    bool showing = false;   // the countdown notice is up (an end's notice is not cleared by the release)
    double posted = 0.0;
};
State& st() {
    static State s;
    return s;
}

std::string hex_key(const uint8_t* k) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < kMemberKeyBytes; ++i) {
        out += d[k[i] >> 4];
        out += d[k[i] & 15];
    }
    return out;
}

std::string clock_text(unsigned seconds) {
    char b[16];
    std::snprintf(b, sizeof b, "%u:%02u", seconds / 60, seconds % 60);
    return b;
}

void send_here(bool ready) {
    link::to_authority(wire::Type::DropHere, wire::Writer(link::self()).bytes(member_key_bytes(), kMemberKeyBytes).u8(ready ? 1 : 0).take());
}

uint64_t game_state() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(gm, layout::kTripRecord + 4) ? gm : 0;
}

uint32_t record_phase() {
    const uint64_t gm = game_state();
    return gm ? guest::rd<uint32_t>(gm + layout::kTripRecord) : 0;
}

// Work a skipped simulation step would leave half done: a trip filed (the record not idle), a save running or asked for.
bool busy() {
    const uint64_t gm = game_state();
    if (!gm) return true;
    if (guest::rd<uint32_t>(gm + layout::kTripRecord) < layout::kTripRecordReady) return true;
    if (guest::rd<uint32_t>(gm + layout::kStateAutosave) != 0) return true;
    return guest::rd<uint32_t>(gm + layout::kStateAutosaveFlags) & layout::kAutosaveAskedNow;
}

unsigned seconds_left(const Awaited& a, double now) { return static_cast<unsigned>(std::max(0.0, a.until - now) + 0.999); }

// The authority's word to every member: who is awaited, in what state, how long is left; then the ends not sent yet.
void send_wait(State& s, double now) {
    wire::Writer w(link::self());
    w.u8(static_cast<uint8_t>(s.awaited.size() + s.ended.size()));
    auto entry = [&w](const Awaited& a, unsigned left) {
        const size_t n = std::min(a.name.size(), wire::kNameMax);
        w.bytes(a.key, kMemberKeyBytes).u8(a.state).u16(static_cast<uint16_t>(left)).u8(static_cast<uint8_t>(n));
        w.bytes(reinterpret_cast<const uint8_t*>(a.name.data()), n);
    };
    for (const Awaited& a : s.awaited) entry(a, seconds_left(a, now));
    for (const Awaited& a : s.ended) entry(a, 0);
    s.ended.clear();
    link::to_members(wire::Type::DropWait, w.take());
    s.next_send = now + kSendEvery;
}

// The authority ends one wait (back or given up): said in the log and the notice here, sent to the members with the next DropWait.
void end_wait(State& s, size_t i, uint8_t how, const char* why) {
    Awaited a = s.awaited[i];
    s.awaited.erase(s.awaited.begin() + static_cast<std::ptrdiff_t>(i));
    a.state = how;
    NXLOG_I(kTag, "Co-op drop: %s (member %s) %s; %zu still awaited", a.name.c_str(), hex_key(a.key).c_str(), why, s.awaited.size());
    notices::post(kNotice, how == kBack ? a.name + say(" is back") : say("Playing on without ") + a.name, 4.0);
    s.showing = false;
    s.ended.push_back(a);
    s.next_send = 0.0;
}

void start_wait(State& s, const Node& n, int node, double now) {
    for (const Awaited& a : s.awaited)
        if (std::memcmp(a.key, n.key, kMemberKeyBytes) == 0) return;
    Awaited a;
    std::memcpy(a.key, n.key, kMemberKeyBytes);
    a.name = notice::name_of(static_cast<uint8_t>(node));
    a.until = now + kLimit;
    s.awaited.push_back(a);
    s.next_send = 0.0;
    s.next_log = now + kLogEvery;
    s.let_go = false;
    NXLOG_W(kTag, "Co-op drop: node %d (%s, member %s) dropped; the group waits up to %.0f s for it", node, a.name.c_str(),
            hex_key(a.key).c_str(), kLimit);
}

// This process lets go of its hold (lock, pause, notice).
void release(State& s, const char* why) {
    if (s.paused) NXLOG_I(kTag, "Co-op drop: the world runs again (%s)", why);
    if (s.holding) {
        lock::release(lock::Holder::Drop);
        NXLOG_I(kTag, "Co-op drop: this player's input is free again (%s)", why);
    }
    s.holding = s.paused = false;
    if (s.showing) notices::post(kNotice, "", 0.0);
    s.showing = false;
}

// The notice over the picture: who is awaited, the countdown, the way out.
void show(State& s, double now) {
    if (now - s.posted < kNoticeEvery) return;
    s.posted = now;
    s.showing = true;
    const Awaited& a = s.awaited.front();
    std::string text = a.state == kJoining ? a.name + say(" is joining the group again: ")
                                           : say("Waiting for ") + a.name + say(" to come back: ");
    text += clock_text(a.left);
    if (s.awaited.size() > 1) text += say(" (and ") + std::to_string(s.awaited.size() - 1) + say(" more)");
    text += say(". Press (+) to stop waiting");
    notices::post(kNotice, text, kNoticeSeconds);
}

// The authority's clock: deadlines, the countdown sent and logged.
void authority_tick(State& s, double now) {
    for (size_t i = s.awaited.size(); i-- > 0;)
        if (now >= s.awaited[i].until) end_wait(s, i, kGivenUp, "did not come back in time: the group plays on");
    for (Awaited& a : s.awaited) a.left = seconds_left(a, now);
    if ((!s.awaited.empty() || !s.ended.empty()) && now >= s.next_send) send_wait(s, now);
    if (!s.awaited.empty() && now >= s.next_log) {
        s.next_log = now + kLogEvery;
        NXLOG_I(kTag, "Co-op drop: waiting for %s, %u s left (%zu awaited)", s.awaited.front().name.c_str(), s.awaited.front().left,
                s.awaited.size());
    }
}

// The hold on this process while anyone is awaited: input, the simulation, the notice; (+) lets go.
void hold_tick(State& s, bool in_game, double now) {
    const bool waiting = !s.awaited.empty();
    if (!waiting) s.let_go = false;
    const uint64_t me = local_player();
    if (!waiting || s.let_go || !me || !link::in_session()) {
        if (s.holding || s.paused) release(s, !waiting ? "no one is awaited" : s.let_go ? "the player stopped waiting" : "no session or character");
        return;
    }
    if (!s.holding) {
        if (!session::can_walk(in_game)) return;   // the hold starts once the player walks (a talk or a load ends first)
        s.holding = true;
        s.buttons = hid::sampled_buttons();
        NXLOG_I(kTag, "Co-op drop: this player's input is held while %s is awaited", s.awaited.front().name.c_str());
    }
    lock::hold(lock::Holder::Drop, me);
    const uint64_t buttons = hid::sampled_buttons(), plus = hid::btn(hid::kBtnPlus);
    const bool pressed = (buttons & plus) && !(s.buttons & plus);
    s.buttons = buttons;
    if (pressed) {
        s.let_go = true;
        NXLOG_I(kTag, "Co-op drop: (+): this player stops waiting");
        if (!s.member)
            while (!s.awaited.empty()) end_wait(s, 0, kGivenUp, "is no longer awaited: the host stopped waiting");
        release(s, "the player stopped waiting");
        return;
    }
    const bool arriving = std::any_of(s.awaited.begin(), s.awaited.end(), [](const Awaited& a) { return a.state == kJoining; });
    const bool may_pause = !arriving && !busy() && s.walk_since >= 0.0 && now - s.walk_since >= kSettle;
    // Once on, the pause holds while nothing is in flight (play mode's in_game and the walk test are not read again: they may follow
    // the skipped step); it starts only from a walking player.
    const bool pause = s.paused ? !arriving && !busy() : may_pause;
    if (pause != s.paused) {
        s.paused = pause;
        NXLOG_I(kTag, "Co-op drop: the world's simulation is %s", pause ? "paused" : arriving ? "running for the arrival" : "running (a trip or save is in flight)");
    }
    if (s.paused) {
        const uint64_t nm = guest::rd<uint64_t>(base() + layout::kNetManager);
        if (guest::ok(nm, layout::kNetQualityTimer + 4)) guest::wr<uint32_t>(nm + layout::kNetQualityTimer, 0);   // 0.0f
    }
    show(s, now);
}

}  // namespace

void on_member(int node, bool joined, bool self) {
    State& s = st();
    if (self) {
        if (joined) {
            s.in = true;
            s.member = !link::is_authority();
            s.here_sent = s.ready_sent = false;
            s.leaving = false;
            for (Node& n : s.nodes) n = Node{};
            return;
        }
        if (s.in && s.member)
            NXLOG_W(kTag, "Co-op drop: this process's session is gone%s", s.leaving ? " (its own leave)" : ": the connection to the group was lost");
        else if (s.in)
            NXLOG_W(kTag, "Co-op drop: the authority's channel left its session (every member is gone); no one is awaited");
        s.in = s.member = false;
        for (Node& n : s.nodes) n = Node{};
        s.awaited.clear();
        s.ended.clear();
        release(s, "this process is out of the session");
        return;
    }
    if (joined || node < 0 || node >= kNodes || !s.in || s.member) return;
    Node& n = s.nodes[node];
    if (!n.known)
        NXLOG_I(kTag, "Co-op drop: node %d left; it never said it waits for drops (an older build): no wait", node);
    else if (n.leaving)
        NXLOG_I(kTag, "Co-op drop: node %d (%s) left with Save and Quit", node, notice::name_of(static_cast<uint8_t>(node)).c_str());
    else
        start_wait(s, n, node, qpc::seconds());
    n = Node{};
}

void on_here_message(::Ctx*, int from, uint8_t, wire::Reader& r) {
    uint8_t key[kMemberKeyBytes];
    r.bytes(key, sizeof key);
    const bool ready = r.u8() != 0;
    if (!r.ok() || from < 0 || from >= kNodes) return;
    Node& n = st().nodes[from];
    if (!n.known || std::memcmp(n.key, key, sizeof key) != 0 || ready != n.ready)
        NXLOG_I(kTag, "Co-op drop: node %d is member %s%s", from, hex_key(key).c_str(), ready ? ", in the world" : ", arriving");
    n.known = true;
    n.ready = ready;
    std::memcpy(n.key, key, sizeof key);
}

void on_leaving_message(::Ctx*, int from, uint8_t, wire::Reader& r) {
    if (!r.ok() || from < 0 || from >= kNodes) return;
    st().nodes[from].leaving = true;
    NXLOG_I(kTag, "Co-op drop: node %d files its own leave", from);
}

void on_wait_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    State& s = st();
    const uint8_t n = r.u8();
    std::vector<Awaited> now_awaited;
    for (uint8_t i = 0; i < n && r.ok(); ++i) {
        Awaited a;
        r.bytes(a.key, kMemberKeyBytes);
        a.state = r.u8();
        a.left = r.u16();
        const uint8_t len = r.u8();
        std::string name(len, '\0');
        r.bytes(reinterpret_cast<uint8_t*>(name.data()), len);
        if (!r.ok()) break;
        a.name = name.empty() ? say("Your teammate") : name;
        if (std::memcmp(a.key, member_key_bytes(), kMemberKeyBytes) == 0) continue;   // this player itself, coming back
        if (a.state == kBack || a.state == kGivenUp) {
            NXLOG_I(kTag, "Co-op drop: the authority says %s %s", a.name.c_str(), a.state == kBack ? "is back" : "is no longer awaited");
            notices::post(kNotice, a.state == kBack ? a.name + say(" is back") : say("Playing on without ") + a.name, 4.0);
            s.showing = false;
            continue;
        }
        if (a.state == kWaiting || a.state == kJoining) now_awaited.push_back(a);
    }
    if (!r.ok()) return;
    if (s.awaited.empty() && !now_awaited.empty())
        NXLOG_I(kTag, "Co-op drop: the authority waits for %s (%u s left)", now_awaited.front().name.c_str(), now_awaited.front().left);
    s.awaited = std::move(now_awaited);
    s.heard_at = qpc::seconds();
}

// Entry 0xbd45c(record, reason, member, flag) and the other filers. Read here: x0 the record, w1 the reason, x30 the caller.
int on_trip_file(::Ctx* c, uint64_t site) {
    State& s = st();
    if (!link::in_session() || link::is_authority() || site != layout::kFileTrip) return 0;
    const uint64_t from = c->x[30] - base();
    if (from == layout::kPauseFileReturn && static_cast<uint32_t>(c->x[1]) == layout::kTripLeave &&
        record_phase() >= layout::kTripRecordReady && !s.leaving) {
        s.leaving = true;
        link::to_authority(wire::Type::DropLeaving, wire::Writer(link::self()).take());
        NXLOG_I(kTag, "Co-op drop: this member leaves with Save and Quit; the authority is told it is no drop");
    }
    return 0;
}

// Entry 0x231890, the simulation step: skipped while paused (it returns 1, as its own end does).
int on_sim_step(::Ctx* c) {
    if (!st().paused) return 0;
    c->x[0] = 1;
    return 1;
}

void tick(::Ctx*, bool in_game) {
    State& s = st();
    const double now = qpc::seconds();
    if (s.in && link::in_session()) {
        if (session::can_walk(in_game)) {
            if (s.walk_since < 0.0) s.walk_since = now;
        } else if (!s.paused) {
            s.walk_since = -1.0;
        }
        if (s.member) {
            if (!s.here_sent) {
                s.here_sent = true;
                send_here(false);
            }
            if (!s.ready_sent && s.walk_since >= 0.0 && now - s.walk_since >= kSettle) {
                s.ready_sent = true;
                send_here(true);
            }
            if (!s.awaited.empty() && now - s.heard_at > kSilence) {
                NXLOG_W(kTag, "Co-op drop: no word from the authority for %.0f s; this player stops waiting", kSilence);
                s.awaited.clear();
            }
        } else {
            authority_tick(s, now);
        }
    }
    hold_tick(s, in_game, now);
}

void reset() {
    State& s = st();
    if (s.holding) lock::release(lock::Holder::Drop);
    s = State{};
}

}  // namespace nxrt::games::pkp::coop::drop
