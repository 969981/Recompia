// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's placement notice (pkp_coop_internal.h; RE: modding-docs coop-v2/TANGROWTH-CAMPFIRE-RE.md).
// When the player places an object, the game tells the contents scheduler (GM+0xd8) at once: 0x593120 calls 0x13596c(scheduler, object,
// x, y, z, dir), which sets the "object placed" notice (kind 5) and runs every armed content's records against it. Some records then
// look at the world: quest 11's content (#7138, and Tangrowth's own script #7099) asks for "placed object 0x254" AND "an object 0x254
// within 5 cells of Tangrowth, on Tangrowth's level" (condition 0xb63, 0x128e64: Tangrowth's actor from this process's actor table,
// then a scan of this process's world grid, 0x128b80). At home the object is in the grid by then (0x594da0 placed it with 0x211de0).
// On a Link Play visitor (every co-op member is one), 0x594da0 only sends the placement to the host (0x214974) and returns "placed".
// The object reaches the member's grid when the host's answer comes back, after the notice. So the test finds nothing, the "placed
// but not near" record runs (Tangrowth: "It's a tad far away..."), scene 65 is never queued, and quest 11 stays at 3.
//   0x13596c (entry hook), from 0x593120 only (return to main+0x593d5c), on a member in a session: when the object is not at that cell
//   in the local grid yet (0x14ebb8), the notice waits. It is raised (the game's own 0x13596c, same arguments) once the object is
//   there and the player can walk, or after kWait seconds as the game would have raised it. Every other case runs the game's own code.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <cstddef>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::placement {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kPlacedNotice = 0x13596c;      // (scheduler, object, x, y, z, dir) -> 1 when a record ran
constexpr uint64_t kPlaceReturn = 0x593d5c;       // 0x593120's `bl 0x13596c` returns here
constexpr uint64_t kObjectAt = 0x14ebb8;          // (world, x, y, z) -> the placed-object record anchored at that cell, or 0
constexpr uint64_t kObjectWord = 0x4;             // placed-object record: u32, bits 0..11 the object id, bits 30..31 its direction
constexpr uint64_t kScheduler = 0xd8;             // game state: the contents scheduler
constexpr uint64_t kFirstOf = 0x28cc60;           // (actor manager, data id) -> the first live actor of that data id, or 0
constexpr uint32_t kTangrowth = 0x2c;             // data id 44, for the log
constexpr uint32_t kObjectMax = 0xfff;
constexpr double kWait = 10.0;
constexpr size_t kQueue = 8;

struct Pending {
    uint64_t scheduler;
    uint16_t object;
    int16_t x, y, z;
    uint8_t dir;
    double at;
};

struct State {
    std::vector<Pending> waiting;
    bool replaying = false;   // our own call of the notice: the game's code runs
};
State& st() {
    static State s;
    return s;
}

uint64_t scheduler() {
    const uint64_t game = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(game, kScheduler + 8) ? guest::rd<uint64_t>(game + kScheduler) : 0;
}

bool member_here() {
    return role() != CoopRole::Host && link::in_session() && !link::is_authority() && (session_bits() & layout::kSessionVisitor);
}

// The placed-object record at that cell in this process's grid, when it is that object (0: not there yet).
uint64_t object_at(::Ctx* c, const Pending& p) {
    const uint64_t world = guest::rd<uint64_t>(base() + layout::kWorld);
    if (!world) return 0;
    const uint64_t rec = guest::call(c, base() + kObjectAt, world, static_cast<uint32_t>(p.x), static_cast<uint32_t>(p.y),
                                     static_cast<uint32_t>(p.z));
    if (!rec || !guest::ok(rec + kObjectWord, 4)) return 0;
    return (guest::rd<uint32_t>(rec + kObjectWord) & kObjectMax) == p.object ? rec : 0;
}

uint64_t sx(int16_t v) { return static_cast<uint64_t>(static_cast<int64_t>(v)); }

void raise(::Ctx* c, const Pending& p, uint8_t dir, const char* why) {
    State& s = st();
    s.replaying = true;
    const uint64_t ran = guest::call_n<6>(c, base() + kPlacedNotice, {p.scheduler, p.object, sx(p.x), sx(p.y), sx(p.z), dir});
    s.replaying = false;
    // Where the contents' "near a character" test finds Tangrowth here (quest 11's goal), for the testers' logs.
    const uint64_t actors = guest::rd<uint64_t>(base() + layout::kPlayerManager);
    const uint64_t tangrowth = actors ? guest::call(c, base() + kFirstOf, actors, kTangrowth) : 0;
    const double after = qpc::seconds() - p.at;
    if (tangrowth && guest::ok(tangrowth + layout::kPosition, 12)) {
        const float tx = guest::rd<float>(tangrowth + layout::kPosition), ty = guest::rd<float>(tangrowth + layout::kPosition + 4),
                    tz = guest::rd<float>(tangrowth + layout::kPosition + 8);
        NXLOG_I(kTag,
                "Co-op placement: object %u at (%d, %d, %d) dir %u raised, %s, %.1f s after the placement; a record ran: %s; Tangrowth "
                "here at cell (%d, %d, %d)",
                p.object, p.x, p.y, p.z, dir, why, after, (ran & 1) ? "yes" : "no", static_cast<int>((tx + 102400.0f) / 100.0f),
                static_cast<int>(ty / 100.0f), static_cast<int>((tz + 102400.0f) / 100.0f));
    } else {
        NXLOG_I(kTag, "Co-op placement: object %u at (%d, %d, %d) dir %u raised, %s, %.1f s after the placement; a record ran: %s",
                p.object, p.x, p.y, p.z, dir, why, after, (ran & 1) ? "yes" : "no");
    }
}

}  // namespace

int on_placed_notice(::Ctx* c) {
    State& s = st();
    if (s.replaying || c->x[30] - base() != kPlaceReturn || !member_here()) return 0;
    const uint16_t object = static_cast<uint16_t>(c->x[1]);
    const uint8_t dir = static_cast<uint8_t>(c->x[5]);
    if (!c->x[0] || static_cast<uint16_t>(object - 1) >= kObjectMax || dir >= 4) return 0;   // the game's own test refuses these
    const Pending p{c->x[0], object, static_cast<int16_t>(c->x[2]), static_cast<int16_t>(c->x[3]), static_cast<int16_t>(c->x[4]), dir,
                    qpc::seconds()};
    if (object_at(c, p) || s.waiting.size() >= kQueue) return 0;   // already in this world (or too many waiting): as the game does
    s.waiting.push_back(p);
    NXLOG_I(kTag, "Co-op placement: object %u at (%d, %d, %d) is not in this world yet (the host places it); its notice waits", object,
            p.x, p.y, p.z);
    c->x[0] = 0;   // "no record ran" (0x593120 does not read it)
    return 1;
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (s.waiting.empty() || !c) return;
    if (!member_here()) {
        s.waiting.clear();
        return;
    }
    const uint64_t sched = scheduler();
    const bool safe = session::can_walk(in_game) && !talk::talking();
    const double now = qpc::seconds();
    for (size_t i = 0; i < s.waiting.size();) {
        const Pending p = s.waiting[i];
        if (p.scheduler != sched) {   // another area since: its contents are not the ones the notice was for
            NXLOG_I(kTag, "Co-op placement: object %u's notice dropped, the area changed", p.object);
            s.waiting.erase(s.waiting.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const uint64_t rec = object_at(c, p);
        if (!safe || (!rec && now - p.at < kWait)) {
            ++i;
            continue;
        }
        s.waiting.erase(s.waiting.begin() + static_cast<std::ptrdiff_t>(i));
        const uint8_t dir = rec ? static_cast<uint8_t>(guest::rd<uint32_t>(rec + kObjectWord) >> 30) : p.dir;
        raise(c, p, dir, rec ? "now in this world" : "still not in this world, as the game would");
        return;   // one a frame: a record may queue a scene
    }
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::placement
