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
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstring>
#include <string>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::drop {

namespace {

constexpr const char* kTag = "pkp";
constexpr int kNodes = 8;
constexpr double kSettle = 3.0;   // the player walks this long after a session join before it counts as in the world

struct Node {
    bool known = false;     // it said DropHere this session
    bool leaving = false;   // it announced its own leave
    bool ready = false;     // its player is in the world
    uint8_t key[kMemberKeyBytes] = {};
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

void send_here(bool ready) {
    link::to_authority(wire::Type::DropHere, wire::Writer(link::self()).bytes(member_key_bytes(), kMemberKeyBytes).u8(ready ? 1 : 0).take());
}

uint32_t record_phase() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(gm, layout::kTripRecord + 4) ? guest::rd<uint32_t>(gm + layout::kTripRecord) : 0;
}

}  // namespace

void on_member(int node, bool joined, bool self) {
    State& s = st();
    if (self) {
        if (joined) {
            s.in = true;
            s.member = !link::is_authority();
            s.here_sent = s.ready_sent = false;
            s.walk_since = -1.0;
            s.leaving = false;
            for (Node& n : s.nodes) n = Node{};
            return;
        }
        if (s.in && s.member)
            NXLOG_W(kTag, "Co-op drop: this process's session is gone%s", s.leaving ? " (its own leave)" : ": the connection to the group was lost");
        else if (s.in)
            NXLOG_W(kTag, "Co-op drop: the authority's channel left its session (every member is gone)");
        s.in = s.member = false;
        for (Node& n : s.nodes) n = Node{};
        return;
    }
    if (joined || node < 0 || node >= kNodes || !s.in || s.member) return;
    Node& n = s.nodes[node];
    if (!n.known)
        NXLOG_I(kTag, "Co-op drop: node %d left; it never said it waits for drops (an older build): no wait", node);
    else if (n.leaving)
        NXLOG_I(kTag, "Co-op drop: node %d (%s) left with Save and Quit", node, notice::name_of(static_cast<uint8_t>(node)).c_str());
    else
        NXLOG_W(kTag, "Co-op drop: node %d (%s, member %s) dropped", node, notice::name_of(static_cast<uint8_t>(node)).c_str(),
                hex_key(n.key).c_str());
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

void tick(::Ctx*, bool in_game) {
    State& s = st();
    if (!s.in || !s.member || !link::in_session()) return;
    if (!s.here_sent) {
        s.here_sent = true;
        send_here(false);
    }
    const double now = qpc::seconds();
    if (!session::can_walk(in_game)) {
        s.walk_since = -1.0;
        return;
    }
    if (s.walk_since < 0.0) s.walk_since = now;
    if (!s.ready_sent && now - s.walk_since >= kSettle) {
        s.ready_sent = true;
        send_here(true);
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::drop
