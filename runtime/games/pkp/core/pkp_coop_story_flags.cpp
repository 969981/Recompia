// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's story flags (pkp_coop_internal.h): the 20 flags the story's content records set, clear and test are
// the group's, not each player's.
//
// The game, 2.0.1. m_storyCtrlFlag is 4 bytes of the save (layout::kSaveStoryFlags): flag n (1..20) is bit n & 7 of byte n >> 3.
// Contents condition 0x32 reads a flag; the only code that writes one is contents action 0xbbd, 0x12c46c(x0 = the action's words:
// u8 n at +4, u32 value at +8): value 1 sets the bit, any other value clears it. It runs on the process whose own content
// scheduler runs the record, so a flag set by a record that waits for one player's scene end or talk existed in that player's
// save alone, and the records of every other player that test it went another way (another hint scene at a gate, a creature's
// arc one step behind).
//
// The rule. A flag set by any player's record is set for the group. A flag is cleared for the group only by the authority's
// records: the clears are stage-entry and content-start records, which the authority runs for the group's move, and a member
// that joins late runs its own stage-entry record, which must not clear what the group has. Every member's four bytes are the
// authority's:
//   a member's set    runs as the game runs it and is sent to the authority (StoryFlagSet), which sets its own bit. The member
//                     keeps the bit over the authority's copy until that copy shows it (or kForget seconds: the authority
//                     cleared it since)
//   a member's clear  does not run (the entry hook returns 1): the authority's bytes decide
//   the authority     sends its four bytes (StoryFlags) when they change, when a member joins and every kAllEvery seconds
//   a member          writes the authority's bytes, with the sets it still waits for, into its save wherever its save differs,
//                     once a frame
// Before the authority's bytes first arrive (the frames between a member's world load and the first message) a member's records
// read and write its own save's flags as the game does; the arrival then replaces them, and a set made in that window is not
// sent: a record whose own guard is that flag runs again against the group's flags.
// A record another player's set closes does not run its other actions on this process: for the flags set with a creature's
// release the escort's own rule decides who may release (pkp_coop_follow.cpp); for the others the set is the record's only
// story action.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::storyflags {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kActionFlag = 4, kActionValue = 8;   // contents action 0xbbd's words: u8 the flag, u32 1 to set
constexpr uint32_t kFlagBits = 0x001ffffeu;             // flags 1..20 as bits of the four bytes read as one u32
constexpr double kAllEvery = 30.0;   // the authority: its bytes again
constexpr double kAgain = 5.0;       // a member: a set the authority's copy does not show is sent again
constexpr double kForget = 20.0;     // and given up (the authority cleared the flag since)

struct State {
    // The authority.
    uint32_t sent = 0;
    bool sent_have = false, send_all = true;
    double sent_at = -1e9;
    // A member.
    uint32_t theirs = 0;    // the authority's flags
    bool have = false;
    uint32_t waiting = 0;   // sets sent that the authority's copy does not show yet
    double waiting_since = -1e9, asked_at = -1e9;
    uint32_t refused = 0;   // clears not run, logged once per flag
};
State& st() {
    static State s;
    return s;
}

uint64_t flags_at() {
    const uint64_t root = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return root && guest::ok(root + layout::kSaveStoryFlags, layout::kStoryFlagBytes) ? root + layout::kSaveStoryFlags : 0;
}

void ask(uint32_t bits) {
    for (uint32_t n = 1; n <= layout::kStoryFlagMax; ++n)
        if (bits >> n & 1) link::to_authority(wire::Type::StoryFlagSet, wire::Writer(link::self()).u8(static_cast<uint8_t>(n)).take());
}

// The authority: its own flags, to every member when due.
void send() {
    State& s = st();
    if (!link::is_authority() || !link::member_count()) return;
    const uint64_t at = flags_at();
    if (!at) return;
    const uint32_t own = guest::rd<uint32_t>(at) & kFlagBits;
    const double now = qpc::seconds();
    const bool changed = !s.sent_have || own != s.sent;
    if (!s.send_all && !changed && now - s.sent_at < kAllEvery) return;
    link::to_members(wire::Type::StoryFlags, wire::Writer(link::self()).u32(own).take());
    if (s.send_all || changed)
        NXLOG_I(kTag, "Co-op story flags: the authority's flags 0x%06x sent to the members (%s)", own, s.send_all ? "a join" : "a change");
    s.sent = own;
    s.sent_have = true;
    s.send_all = false;
    s.sent_at = now;
}

// A member: its save's flags become the authority's, with the sets it still waits for.
void apply() {
    State& s = st();
    if (!s.have || !quests::member_rules()) return;
    const double now = qpc::seconds();
    s.waiting &= ~s.theirs;
    if (s.waiting && now - s.waiting_since >= kForget) {
        NXLOG_W(kTag, "Co-op story flags: sets 0x%06x never showed in the authority's flags; let go", s.waiting);
        s.waiting = 0;
    }
    if (s.waiting && now - s.asked_at >= kAgain) {
        s.asked_at = now;
        ask(s.waiting);
    }
    const uint64_t at = flags_at();
    if (!at) return;
    const uint32_t here = guest::rd<uint32_t>(at), want = (here & ~kFlagBits) | s.theirs | s.waiting;
    if (here == want) return;
    guest::wr<uint32_t>(at, want);
    NXLOG_I(kTag, "Co-op story flags: this save's flags 0x%06x -> 0x%06x (the group's)", here & kFlagBits, want & kFlagBits);
}

}  // namespace

int on_flag_action(::Ctx* c) {
    if (!quests::member_rules()) return 0;   // the authority and a game outside a session: the game's own write
    State& s = st();
    const uint64_t words = c->x[0];
    if (!s.have || !guest::ok(words, kActionValue + 4)) return 0;
    const uint32_t n = guest::rd<uint8_t>(words + kActionFlag);
    if (n < 1 || n > layout::kStoryFlagMax) return 0;
    const uint32_t bit = 1u << n;
    if (guest::rd<uint32_t>(words + kActionValue) == 1) {
        if (!((s.theirs | s.waiting) & bit)) {
            if (!s.waiting) s.waiting_since = qpc::seconds();
            s.waiting |= bit;
            s.asked_at = qpc::seconds();
            ask(bit);
            NXLOG_I(kTag, "Co-op story flags: flag %u set by this player's record, asked of the authority", n);
        }
        return 0;
    }
    if (!(s.refused & bit)) {
        s.refused |= bit;
        NXLOG_I(kTag, "Co-op story flags: this player's record would clear flag %u; a clear is the authority's (not run here)", n);
    }
    return 1;   // the action's whole job is the write
}

void on_set_message(::Ctx*, int from, uint8_t, wire::Reader& r) {
    const uint32_t n = r.u8();
    if (!r.ok() || !link::is_authority() || n < 1 || n > layout::kStoryFlagMax) return;
    const uint64_t at = flags_at();
    if (!at) return;
    const uint32_t here = guest::rd<uint32_t>(at);
    if (here >> n & 1) return;
    guest::wr<uint32_t>(at, here | 1u << n);
    NXLOG_I(kTag, "Co-op story flags: flag %u set for the group (node %d's record)", n, from);
}

void on_flags_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    const uint32_t flags = r.u32();
    if (!r.ok() || link::is_authority()) return;
    State& s = st();
    s.theirs = flags & kFlagBits;
    s.have = true;
}

void tick() {
    send();
    apply();
}

void on_session_change() {
    State& s = st();
    s.sent_have = false;
    s.send_all = true;
    s.have = false;
    s.waiting = 0;
    s.refused = 0;
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::storyflags
