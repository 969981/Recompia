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
//   - the authority's own Save and Quit closes the session for everyone (0xbdc68 from the pause menu's state 0xf, or 0xbd45c reason 0
//     from its state 0x10): it says so first (DropClosing), so no member takes the close for the host's drop, and the authority
//     waits for no one that leaves after it;
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
//
// A member never plays outside the group (owner rule: the group's world is one; a member alone in its own copy of it is a world that
// cannot exist). So a member's game closes whenever its session ended and it is not back in one:
//   - its own Save and Quit, and the host's Save and Quit that closes the session for everyone: the game's trip home runs (it is the
//     path that ends the session cleanly and makes the game's arrival save, which keeps the player's own data: CMNDAT; the world
//     files are held, pkp_coop_savecopy.cpp), then the process ends;
//   - a session lost without either (the host's drop or this member's own: a member cannot tell them apart): the trip home runs as
//     well (the one path that clears the game's session bits, below), then the session part searches (pkp_coop_session.cpp). It
//     finds the host's session: this member's own drop, it joins again and the group, which waits for it, plays on. kNoHost
//     searches in a row find none (the host dropped, or this PC's network is still down), or kRejoin passes without a session: the
//     process ends. The host dropped: every member's game closes this way.
//   While a close is decided the session part starts no search (closing()): its Save and Quit would otherwise join the host again.
//   The process ends once nothing is in flight (the trip record idle, no save running, the player able to walk, all for kCloseSettle),
//   or kCloseMax after it was due. The bag record and the copies' stamps are written at the session's end (their own parts), before
//   that. A player who wants back in starts co-op again from the launcher. The authority's game is never closed here: it holds the
//   group's world, and the members join it again.
//
// The dropped process. Its own session is gone (the channel leaves on it at once: the ldn station loses the network), and a member
// cannot tell its own drop from the host's. Its game handles that as before: error 3103 (0x80401103, KTGL SESSIONP2P_NETWORK_LOST;
// shown by 0x547278 through the error applet), and the drop check 0xbca8c files the trip home (kind 0xE; the host's own: 0xbd548,
// kind 4, a reload of its stage). That trip is kept: it is the one path that clears the game's session bits (GM+0x90 stays 05 until
// the trip's stage entry, work/droprig peeks), and only with them clear does pkp_coop_session.cpp search and join again, the normal
// join that gives the bag back (pkp_coop_bags.cpp) and catches the player up. So nobody waits on the dropped process: for kLimit it
// says it is rejoining, with a countdown on its own clock (it hears no one; a member kRejoin, the authority kLimit), and error 3103 is
// held back (its applet shows nothing on PC, the log keeps it). At the end without a rejoin a member's game closes (above); the
// authority says so in plain words and plays on, its session part still open to joins. On a process that still has the session the game's drop check does not file
// anything (0xbca8c is entered every frame the trip record is idle, from 0xbaf54's tail call; its own tests decide, so it is not
// hooked: this process's own channel leave is what ends a wait here).
//
// The trip filers, every caller (extract calls.tsv; modding-docs coop-session/LEAVE-DOORS-RE.md 2..4):
//   0xbd45c(record, reason, member, flag): the pause menu (0xfb9ea0 state 0x10, return 0xfba740: reason 0 the host disbands, 4 a teammate
//     leaves, 2 the host sends a member home, which pkp_coop_doors.cpp refuses at the menu), Go Back Home of the Link Play screen
//     (0xe800ac, tail call; its state 0x15 is refused at the screen), and the kick handler 0x5483a0 (RequestRejectMember: reason 1,
//     kind 8 home; return 0x54841c). The kick is the game's own "go home" after a host's refusal: refused here in a session, the way the
//     filer itself refuses (w0 = 0, as when the record is busy), so 0x5483a0 answers the host "not done" (its 0x5484f4 path).
//   0xbdc68(record): the pause menu's state 0xf (the host's Save and Quit, close for everyone) and the Link Play screen's 0x14 (refused at
//     the screen). Allowed.
//   0xbdd28(record): the pause menu's 0x12 and the screen's 0x17 (0xe81788, tail call): the GameShare end. Allowed.
//   0xbe5f0(record, .., kind): the StageMove message 0x54f1f4, the host's trip on a member (the group's travel, and the close). Allowed.
//     It files only with an idle record; with a busy one it self-disconnects (0x5373a4(3)); a paused member's record is idle.
//   The drop's own filings (kind 0xE / 10 / 4) are the drop check's, never a filer's (above).
//
// The return. The dropped player comes back through the normal join (a relaunch, or the session part's own retries after its trip
// home), as a new node. Its channel joins at the ldn connect, before the game's join sync and the host's world load: from then until
// it says it is in the world (DropHere ready, or kArrivalMax) the simulation runs on every process again (the join's sync, its
// arrival and the warp next to the host need the game's step; never pause the arriving process or the one it arrives in), the input
// stays held and the countdown says it is joining. Its DropHere with an awaited key ends that wait: the bag and the catch-up are the
// join's own (pkp_coop_bags.cpp and the other parts' on_member_joined).
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_strings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/notices.h"
#include "nxrt/play.h"
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
constexpr double kArrivalMax = 60.0;    // a node that joined during a wait runs the world this long at most until it is in
constexpr double kFiledMax = 3.0;       // a leave or close filed whose trip has not started by then was refused
constexpr double kRejoin = 90.0;        // a member that lost its session has this long at most to be back in one before its game closes
constexpr unsigned kNoHost = 2;         // ... and closes at once after this many searches in a row that found no session
constexpr double kCloseSettle = 3.0;    // a member's game closes once nothing was in flight for this long
constexpr double kCloseMax = 45.0;      // ... or this long after the close was due, whatever is in flight
constexpr uint8_t kWaiting = 1, kJoining = 2, kBack = 3, kGivenUp = 4;
enum class Close : uint8_t { None, Left, Ended, Lost, Asked };   // why a member's game closes (Asked: the quit was requested)

// Player-visible text: the game layer's string table (pkp_strings.h), keys CoopDrop*; every line is drawn by the overlay (notices::post),
// so Japanese, Korean and Chinese keep the English text.
using strings::Str;
std::string say(Str key, const std::string& a = {}, const std::string& b = {}) { return strings::fmt(base(), key, a, b); }

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
    bool joining = false;   // the authority: its player's process said DropHere again (arriving)
    unsigned left = 0;      // seconds left as the authority last said
};

struct State {
    // every process
    bool in = false;            // this process is in a session's channel
    bool member = false;        // ... as a member (not the authority)
    bool here_sent = false, ready_sent = false;
    double walk_since = -1.0;   // the player can walk since (negative: not now)
    bool leaving = false;       // this member filed its own leave
    bool closing = false;       // the authority closes the session for everyone (it said so, or this process is it)
    double filed_at = 0.0;      // this process's own leave or close was filed at (cleared if the trip never starts)
    bool filed_ran = false;     // ... and its trip record went busy since
    bool loaded = false;        // a member: its world loaded (play mode left the game) since its session join
    double joined_at = 0.0;
    // the authority
    Node nodes[kNodes];
    std::vector<Awaited> awaited;   // the authority's waits; a member's copy of the authority's word
    std::vector<Awaited> ended;     // the authority: ends (back / given up) not sent yet
    double next_send = 0.0, next_log = 0.0;
    double arrival_at[kNodes] = {};   // a node joined during a wait at (0: none arriving)
    // a member
    double heard_at = 0.0;
    // the hold on this process
    bool holding = false, paused = false, let_go = false;
    uint64_t buttons = 0;
    bool showing = false;   // the countdown notice is up (an end's notice is not cleared by the release)
    double posted = 0.0;
    // this process lost its session
    bool ever_in = false;       // it was in a session this launch
    bool lost = false;          // its session is gone, not by its own leave: it says it is rejoining
    bool lost_authority = false;
    double lost_until = 0.0;
    bool error_held = false;    // error 3103 was held back during this loss
    // a member whose session ended: its game closes
    Close close = Close::None;
    double close_at = 0.0, close_by = 0.0;   // due at; at the latest
    double settled_since = -1.0;             // nothing is in flight since (negative: something is)
    unsigned empty_searches = 0;             // Lost: searches in a row that found no session
    bool decided = false;                    // Lost: no session to go back to, the close is decided
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
    notices::post(kNotice, how == kBack ? say(Str::CoopDropBack, a.name) : say(Str::CoopDropGivenUp, a.name), 4.0);
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
    std::string text = a.state == kJoining ? say(Str::CoopDropJoining, a.name, clock_text(a.left))
                                           : say(Str::CoopDropWaiting, a.name, clock_text(a.left));
    if (s.awaited.size() > 1) text += " " + say(Str::CoopDropMore, std::to_string(s.awaited.size() - 1));
    text += " " + say(Str::CoopDropStop);
    notices::post(kNotice, text, kNoticeSeconds);
}

// The authority's clock: deadlines, the countdown sent and logged.
void authority_tick(State& s, double now) {
    for (size_t i = s.awaited.size(); i-- > 0;)
        if (now >= s.awaited[i].until) end_wait(s, i, kGivenUp, "did not come back in time: the group plays on");
    bool arrival = false;
    for (int n = 0; n < kNodes; ++n) {
        if (s.arrival_at[n] > 0.0 && (now - s.arrival_at[n] >= kArrivalMax || s.nodes[n].ready)) s.arrival_at[n] = 0.0;
        if (s.arrival_at[n] > 0.0) arrival = true;
    }
    for (Awaited& a : s.awaited) {
        a.left = seconds_left(a, now);
        const uint8_t state = a.joining || arrival ? kJoining : kWaiting;
        if (state != a.state) s.next_send = 0.0;
        a.state = state;
    }
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

// The rejoin window on a process whose session is gone: the countdown, then the plain end.
void lost_tick(State& s, double now) {
    if (!s.lost) return;
    if (now >= s.lost_until) {
        s.lost = false;
        s.showing = false;
        NXLOG_W(kTag, "Co-op drop: not back in a session within %.0f s%s; %s", s.lost_authority ? kLimit : kRejoin,
                s.error_held ? " (error 3103 was held back)" : "",
                s.lost_authority ? "the session part keeps trying" : "this member's game closes");
        if (s.lost_authority) notices::post(kNotice, say(Str::CoopDropGaveUpAuthority), 10.0);   // a member: close_tick says it
        return;
    }
    if (now - s.posted < kNoticeEvery) return;
    s.posted = now;
    s.showing = true;
    const unsigned left = static_cast<unsigned>(s.lost_until - now + 0.999);
    notices::post(kNotice,
                  s.lost_authority ? say(Str::CoopDropLostWait, clock_text(left)) : say(Str::CoopDropLostRejoin, clock_text(left)),
                  kNoticeSeconds);
}

// A member whose session ended and that is not back in one: the notice, then the process ends once nothing is in flight.
void close_tick(State& s, bool in_game, double now) {
    if (s.close == Close::None || s.close == Close::Asked) return;
    if (s.close == Close::Lost && !s.decided) {
        if (now < s.close_at) return;   // the rejoin window: lost_tick shows its countdown
        s.decided = true;
    }
    if (now - s.posted >= kNoticeEvery) {
        s.posted = now;
        s.showing = true;
        notices::post(kNotice,
                      s.close == Close::Left ? say(Str::CoopDropCloseLeft)
                                             : s.close == Close::Ended ? say(Str::CoopDropCloseEnded) : say(Str::CoopDropGaveUpRejoin),
                      kNoticeSeconds);
    }
    if (now < s.close_at) return;
    if (busy() || !session::can_walk(in_game)) s.settled_since = -1.0;
    else if (s.settled_since < 0.0) s.settled_since = now;
    const bool settled = s.settled_since >= 0.0 && now - s.settled_since >= kCloseSettle;
    if (!settled && now < s.close_by) return;
    NXLOG_W(kTag, "Co-op drop: this member's game closes (%s; %s): a member does not play outside the group",
            s.close == Close::Left ? "its own Save and Quit" : s.close == Close::Ended ? "the host closed the session" : "the session was lost and not joined again",
            settled ? "nothing in flight" : "the wait for the trip and the save ran out");
    s.close = Close::Asked;
    play::request_quit("co-op: this member's session ended");
}

}  // namespace

void on_member(int node, bool joined, bool self) {
    State& s = st();
    if (self) {
        if (joined) {
            if (s.lost) {
                NXLOG_I(kTag, "Co-op drop: back in a session %.0f s after the connection was lost", kLimit - (s.lost_until - qpc::seconds()));
                notices::post(kNotice, say(Str::CoopDropBackGroup), 4.0);
                s.lost = false;
                s.showing = false;
            }
            if (s.close == Close::Lost) {
                NXLOG_I(kTag, "Co-op drop: back in a session: this member's game stays open");
                s.close = Close::None;
            }
            s.in = true;
            s.ever_in = true;
            s.member = !link::is_authority();
            s.here_sent = s.ready_sent = false;
            s.leaving = s.closing = false;
            s.walk_since = -1.0;
            s.loaded = false;
            s.joined_at = qpc::seconds();
            s.error_held = false;
            for (Node& n : s.nodes) n = Node{};
            return;
        }
        if (s.in && s.member)
            NXLOG_W(kTag, "Co-op drop: this process's session is gone%s", s.leaving   ? " (its own leave)"
                                                                       : s.closing ? " (the host closed it)"
                                                                                   : ": the connection to the group was lost");
        else if (s.in)
            NXLOG_W(kTag, "Co-op drop: the authority's channel left its session (every member is gone); no one is awaited");
        if (s.in && !s.leaving && !s.closing) {
            s.lost = true;
            s.lost_authority = !s.member;
            s.lost_until = qpc::seconds() + (s.member ? kRejoin : kLimit);
            s.posted = 0.0;
            NXLOG_I(kTag, "Co-op drop: rejoin window of %.0f s (the game's own trip runs; the session part joins again)",
                    s.member ? kRejoin : kLimit);
        }
        if (s.in && s.member && s.close == Close::None) {
            s.close = s.leaving ? Close::Left : s.closing ? Close::Ended : Close::Lost;
            s.close_at = qpc::seconds() + (s.close == Close::Lost ? kRejoin : kCloseSettle);
            s.close_by = s.close_at + kCloseMax;
            s.settled_since = -1.0;
            s.posted = 0.0;
            s.empty_searches = 0;
            s.decided = s.close != Close::Lost;
        }
        s.in = s.member = false;
        s.walk_since = -1.0;
        for (Node& n : s.nodes) n = Node{};
        s.awaited.clear();
        s.ended.clear();
        release(s, "this process is out of the session");
        return;
    }
    if (node < 0 || node >= kNodes || !s.in || s.member) return;
    if (joined) {
        if (!s.awaited.empty()) {
            s.arrival_at[node] = qpc::seconds();
            NXLOG_I(kTag, "Co-op drop: node %d joined during the wait; the world runs for its arrival", node);
        }
        return;
    }
    s.arrival_at[node] = 0.0;
    Node& n = s.nodes[node];
    for (Awaited& a : s.awaited)
        if (n.known && a.joining && std::memcmp(a.key, n.key, kMemberKeyBytes) == 0) {
            a.joining = false;   // it dropped again mid-join: still awaited, the world may pause again
            NXLOG_I(kTag, "Co-op drop: %s left again while joining; still awaited", a.name.c_str());
        }
    if (!n.known)
        NXLOG_I(kTag, "Co-op drop: node %d left; it never said it waits for drops (an older build): no wait", node);
    else if (n.leaving || s.closing)
        NXLOG_I(kTag, "Co-op drop: node %d (%s) left with Save and Quit%s", node, notice::name_of(static_cast<uint8_t>(node)).c_str(),
                s.closing ? " (the host closes the session)" : "");
    else
        start_wait(s, n, node, qpc::seconds());
    n = Node{};
}

bool closing() {
    const State& s = st();
    return s.close != Close::None && s.decided;
}

void on_search(bool found) {
    State& s = st();
    if (s.close != Close::Lost || s.decided) return;
    if (found) {
        s.empty_searches = 0;
        return;
    }
    if (++s.empty_searches < kNoHost) return;
    NXLOG_W(kTag, "Co-op drop: %u searches found no session after the loss (the host is gone, or this PC's network is down)", s.empty_searches);
    s.decided = true;
    s.lost = false;
    s.close_at = qpc::seconds();
    s.close_by = s.close_at + kCloseMax;
    s.posted = 0.0;
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
    State& s = st();
    if (ready) s.arrival_at[from] = 0.0;
    for (size_t i = 0; i < s.awaited.size(); ++i) {
        Awaited& a = s.awaited[i];
        if (std::memcmp(a.key, key, sizeof key) != 0) continue;
        if (ready) {
            end_wait(s, i, kBack, "is back in the world");
        } else if (!a.joining) {
            a.joining = true;
            NXLOG_I(kTag, "Co-op drop: %s is joining again as node %d", a.name.c_str(), from);
        }
        break;
    }
}

void on_leaving_message(::Ctx*, int from, uint8_t, wire::Reader& r) {
    if (!r.ok() || from < 0 || from >= kNodes) return;
    st().nodes[from].leaving = true;
    NXLOG_I(kTag, "Co-op drop: node %d files its own leave", from);
}

void on_closing_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    if (!r.ok()) return;
    st().closing = true;
    NXLOG_I(kTag, "Co-op drop: the host closes the session for everyone (its Save and Quit): no drop");
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
        a.name = name.empty() ? say(Str::CoopDropTeammate) : name;
        if (std::memcmp(a.key, member_key_bytes(), kMemberKeyBytes) == 0) continue;   // this player itself, coming back
        if (a.state == kBack || a.state == kGivenUp) {
            NXLOG_I(kTag, "Co-op drop: the authority says %s %s", a.name.c_str(), a.state == kBack ? "is back" : "is no longer awaited");
            notices::post(kNotice, a.state == kBack ? say(Str::CoopDropBack, a.name) : say(Str::CoopDropGivenUp, a.name), 4.0);
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

// Entry 0xbd45c(record, reason, member, flag) and the other filers. Read here: x0 the record, w1 the reason, x30 the caller (0xbd45c is
// a leaf: x30 is its caller's return). The kick's filing is refused (returns 1, w0 = 0); everything else runs (returns 0).
int on_trip_file(::Ctx* c, uint64_t site) {
    State& s = st();
    if (!link::in_session()) return 0;
    const uint64_t from = c->x[30] - base();
    if (link::is_authority()) {
        const bool closes = site == layout::kCloseTrip ||
                            (site == layout::kFileTrip && from == layout::kPauseFileReturn && static_cast<uint32_t>(c->x[1]) == layout::kTripDisband);
        if (closes && record_phase() >= layout::kTripRecordReady && !s.closing) {
            s.closing = true;
            s.filed_at = qpc::seconds();
            s.filed_ran = false;
            link::to_members(wire::Type::DropClosing, wire::Writer(link::self()).take());
            NXLOG_I(kTag, "Co-op drop: the host closes the session with Save and Quit; the members are told it is no drop");
            s.awaited.clear();
            s.ended.clear();
            release(s, "the host closes the session");
        }
        return 0;
    }
    if (site != layout::kFileTrip) return 0;
    if (from == layout::kKickFileReturn) {
        NXLOG_I(kTag, "Co-op drop: the host's rejection would send this member home (kind 8): refused in the co-op session");
        c->x[0] = 0;
        return 1;
    }
    if (from == layout::kPauseFileReturn && static_cast<uint32_t>(c->x[1]) == layout::kTripLeave &&
        record_phase() >= layout::kTripRecordReady && !s.leaving) {
        s.leaving = true;
        s.filed_at = qpc::seconds();
        s.filed_ran = false;
        link::to_authority(wire::Type::DropLeaving, wire::Writer(link::self()).take());
        NXLOG_I(kTag, "Co-op drop: this member leaves with Save and Quit; the authority is told it is no drop");
    }
    return 0;
}

// Entry 0x547278(NM, error), the online error display; *(u32*)error is the KTGL result. Error 3103 is held back while this process is in
// a co-op session or its rejoin window (the error comes about 30 ms after the network is gone, maybe before the channel's leave is
// drained): the window says what happened instead (returns 1, w0 = 1 as the display's own end). After the window it is shown.
int on_error(::Ctx* c) {
    State& s = st();
    if (!(s.in || s.lost) || !guest::ok(c->x[1], 4) || guest::rd<uint32_t>(c->x[1]) != layout::kErrorSessionLost) return 0;
    if (!s.error_held) NXLOG_I(kTag, "Co-op drop: error 3103 (the session's network was lost) held back; the rejoin window says it");
    s.error_held = true;
    c->x[0] = 1;
    return 1;
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
        if (!in_game) s.loaded = true;
        if ((s.leaving || s.closing) && s.filed_at > 0.0) {
            if (record_phase() < layout::kTripRecordReady) {
                s.filed_ran = true;
            } else if (!s.filed_ran && now - s.filed_at > kFiledMax) {
                NXLOG_W(kTag, "Co-op drop: the %s filed %.0f s ago never started its trip; a leave from now on is a drop again",
                        s.leaving ? "leave" : "close", kFiledMax);
                s.leaving = s.closing = false;
                s.filed_at = 0.0;
            }
        }
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
            // In the world: walking kSettle after the join's world load (a rejoin can start in the home world, still walking);
            // without a load seen, kArrivalMax after the join.
            const bool walked = s.walk_since >= 0.0 && now - s.walk_since >= kSettle;
            if (!s.ready_sent && walked && (s.loaded || now - s.joined_at >= kArrivalMax)) {
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
    lost_tick(s, now);
    close_tick(s, in_game, now);
}

void reset() {
    State& s = st();
    if (s.holding) lock::release(lock::Holder::Drop);
    if (s.showing) notices::post(kNotice, "", 0.0);
    s = State{};
}

}  // namespace nxrt::games::pkp::coop::drop
