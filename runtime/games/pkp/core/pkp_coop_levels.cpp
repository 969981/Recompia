// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's Environment Levels (pkp_coop_internal.h levels). The authority's 32 area-level bytes reach every member's
// own save, so a member's levels do not wait for the game's own join fill.
//
// The game, 2.0.1a (modding-docs coop-v2/MEMBER-PC-WAIT-RE.md sections 3 and 4):
//   the getter 0x58042c(stage) reads, on a visitor, the mirror R+0x1718490+stage (Cloud Island: its own byte R+0xfd84a7); on the host
//   the own levels R+0x741494+stage. The PC's tick (0x7d41f4) opens a pressed PC only while that level is 1..10.
//   The mirror is filled by Link Play's GameReplication kind 0x16 (applier 0x548e60 -> 0x57f1ac), a bulk after the join whose delay
//   depends on the host (26 s and 77 s in two logs). Until it lands every level reads 0, and nothing ends the PC press that waits on it.
// Rebuilt here (PROJECT.md 6.7): the authority sends its 32 own-level bytes to every member when one joins, when they change and every
// kAllEvery seconds, with no play-mode or can-walk gate (they are bytes of its save, read at any time); a member writes each byte into
// its mirror only where the mirror still holds 0 (fill-only), so the game's own kind-0x16 write stays on, overwrites with the same
// values and still raises its level-up notice for a real rise. The member's Cloud Island level is its own byte and is not touched.
// The digest (pkp_coop_digest.cpp) keeps comparing the levels the game reads and logs a persistent difference.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <array>
#include <cstring>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::levels {

namespace {

constexpr const char* kTag = "pkp";
constexpr size_t kStages = layout::kAreaStages;
constexpr double kLookEvery = 1.0;   // the authority's look for a change
constexpr double kAllEvery = 30.0;   // every level again
constexpr double kFillEvery = 0.5;   // a member's look for mirror bytes still 0

using Bytes = std::array<uint8_t, kStages>;

struct State {
    // The authority.
    Bytes sent{};
    bool sent_have = false;
    bool send_all = true;
    double look_at = -1e9, sent_at = -1e9;
    // A member.
    Bytes theirs{};
    bool have = false;
    double fill_at = -1e9;
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() { return guest::rd<uint64_t>(base() + layout::kSaveRoot); }

// The authority: its own levels, to every member when due.
void send() {
    State& s = st();
    if (!link::is_authority() || !(session_bits() & layout::kSessionOnline) || !link::member_count() || !local_player()) return;
    const double now = qpc::seconds();
    if (now - s.look_at < kLookEvery) return;
    s.look_at = now;
    const uint64_t root = save_root();
    Bytes own{};
    if (!root || !guest::read(root + layout::kSaveAreaLevel, own.data(), own.size())) return;
    const bool changed = !s.sent_have || own != s.sent;
    if (!s.send_all && !changed && now - s.sent_at < kAllEvery) return;
    link::to_members(wire::Type::AreaLevels, wire::Writer(link::self()).u8(static_cast<uint8_t>(kStages)).bytes(own.data(), own.size()).take());
    if (s.send_all || changed)
        NXLOG_I(kTag, "Co-op levels: the authority's area levels sent to the members (%s)", s.send_all ? "a join" : "a change");
    s.sent = own;
    s.sent_have = true;
    s.send_all = false;
    s.sent_at = now;
}

bool member_here() {
    return role() == CoopRole::Visit && !link::is_authority() && link::in_session() && (session_bits() & layout::kSessionVisitor);
}

}  // namespace

size_t fill_now() {
    State& s = st();
    if (!s.have || !member_here()) return 0;
    const uint64_t root = save_root();
    Bytes mirror{};
    if (!root || !guest::read(root + layout::kSaveAreaLevelMirror, mirror.data(), mirror.size())) return 0;
    size_t filled = 0;
    for (size_t i = 1; i < kStages; ++i) {
        if (i == layout::kCloudStage || mirror[i] || !s.theirs[i]) continue;
        guest::wr<uint8_t>(root + layout::kSaveAreaLevelMirror + i, s.theirs[i]);
        ++filled;
    }
    if (filled) NXLOG_I(kTag, "Co-op levels: %zu area levels were 0 here and are filled from the authority's copy", filled);
    return filled;
}

void on_levels_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    const uint8_t n = r.u8();
    if (!r.ok() || link::is_authority() || n != kStages) return;   // another build's table size: skipped
    Bytes got{};
    r.bytes(got.data(), got.size());
    if (!r.ok()) return;
    State& s = st();
    s.theirs = got;
    s.have = true;
    s.fill_at = -1e9;   // looked at once
}

void tick() {
    send();
    State& s = st();
    if (!s.have) return;
    const double now = qpc::seconds();
    if (now - s.fill_at < kFillEvery) return;
    s.fill_at = now;
    fill_now();
}

void on_session_change() {
    State& s = st();
    s.sent_have = false;
    s.send_all = true;
    s.have = false;
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::levels
