// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op, the cleaning request of a save that cleaned the beach before co-op counted cleanings
// (pkp_coop_internal.h). Unique request 128 (type 0x2c, the water creature that asks for the sludge to be cleaned) is met when the
// u8 cleaning count (save root + 0x4a4401, m_removeMudBlockNum) reaches its row's target (80). Before the count's gate was opened
// in a session (pkp_coop_requests.cpp on_clean_count) no cleaning done in co-op counted, so a group that cleaned all the sludge
// then has nothing left to clean and the count can never get there. The authority repairs that once per stage load: what is
// missing from the target and no longer in the stage was removed already, so the count is raised to target - (sludge left).
//
// The game, 2.0.1a (modding-docs coop-v2/REQUESTS-RE.md section 9):
//   the count's writers        0x6520e0(new, old, &pos) counts a block turned from mud into something else, mud blocks being
//                              ids 17, 72, 73, 292, 381 (0x652270..0x6522d8); 0x652880(variation, &pos) counts a removed mud part,
//                              variations 55, 56, 1232, 1233, 2775, 2776 (0x652908..0x65292c), both only in stage 2 (game state
//                              +0x18) and both `min(count, 98) + 1`. Nothing else writes the count; no path that puts mud back
//                              was found (section 9.2, inferred).
//   the checker                0x613c9c, type 0x2c (0x6142d0): count >= row +0x5c. The unique manager's frame update 0x6125c0 runs
//                              it for every slot in its state-3 list once the per-type precheck 0x613a38 passes, and that precheck
//                              is true for type 0x2c (jump table 0x280a935 entry 0x25 = 0: the shared `mov w0, #1` exit). So a
//                              raised count is met at the game's next check with no cleaning event, past the checker's own gates
//                              (0xbb8e4, 0xbb778, 0xb530c, and those row +0x63 bits 1 / 3 ask for).
//   the blocks                 every chunk slot of the world's chunk map (pkp_layout.h), block id = cell u16 & 0x7ff.
//   the parts                  the world's part pool P = *(world + 0x6c050b0): u32 P + 0x640020 the count (live parts + 1), u32
//                              P + 0x18 + 4 * h the record index of handle h, records at *(main + 0x3561680), 16 bytes, u32 +4
//                              bits 0..11 the variation. This is the exact chain 0x6529b8 (0x6529f8..0x652ab0) reads for one handle;
//                              the game accepts no handle at or above the count, so handles 1..count-1 hold every live part. A
//                              handle there that is free would only add (an overcount leaves the count where it is, never raises it
//                              too far).
// Rules (a false completion is worse than a request that waits): the authority of our co-op session alone, in stage 2 with the
// world loaded and the player able to walk, while request 128 is taken (3) and its row is type 0x2c; two full scans at least
// kConfirmAfter apart that agree on the sludge left and on the count, the count unchanged during each; the count is only ever
// raised. The members get the raised count as story record 8 (pkp_coop_records.cpp). Single player, a Link Play session that is not
// ours and every member: nothing runs.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <array>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::mud {

namespace {

constexpr const char* kTag = "pkp";
constexpr uint32_t kRequest = 128;
constexpr uint8_t kTaken = 3;
constexpr uint8_t kMudStage = 2;
constexpr uint8_t kCleanType = 0x2c;
constexpr uint64_t kRowTarget = 0x5c, kRowType = 0x5d;   // row + : u8 the count asked for, u8 the request type
constexpr uint8_t kCountMax = 99;                        // the writers' cap
constexpr uint64_t kWorldParts = 0x6c050b0;              // world + : -> the part pool
constexpr uint64_t kPartCount = 0x640020;                // pool + : u32 live parts + 1
constexpr uint64_t kPartRecordOf = 0x18;                 // pool + 4 * handle + : u32 its record index
constexpr uint32_t kPartCapacity = 0xc8000;              // handles and records (0x652a3c: handle - 1 <= 0xc7ffe)
constexpr uint64_t kPartRecords = 0x3561680;             // main + : -> the part records
constexpr uint64_t kRecordSize = 16, kRecordPlace = 4;
constexpr uint32_t kVariation = 0xfff;
constexpr uint32_t kCellsPerChunk = static_cast<uint32_t>(layout::kChunkBytes / 3);
constexpr unsigned kChunksPerFrame = 2;                  // about 0.4 MB of cells a frame: a full island in a few seconds
constexpr uint32_t kPartsPerFrame = 16384;
constexpr double kConfirmAfter = 30.0;                   // the second scan starts this long after the first ended, and a scan
                                                         // that could not be read is tried again after this long

bool mud_block(uint16_t id) { return id == 17 || id == 72 || id == 73 || id == 292 || id == 381; }
bool mud_part(uint32_t v) { return v == 55 || v == 56 || v == 1232 || v == 1233 || v == 2775 || v == 2776; }

struct Pass {
    bool active = false;
    uint64_t world = 0, map = 0, cells = 0, pool = 0, records = 0;
    bool records_ok = false;                      // the whole record table is mapped (no check per record)
    uint8_t count = 0;                            // the cleaning count when the pass started
    int target = 0;                               // the request row's, read at the start
    const char* failed = nullptr;                 // why the pass could not be read (nothing is decided from it)
    std::array<bool, layout::kChunkSlots> slot{};
    uint32_t next_slot = 0, next_part = 1, parts = 0;
    uint32_t blocks = 0, mud_parts = 0, cells_used = 0;
    double work = 0.0, started = 0.0;
};

struct Scan {
    uint32_t blocks = 0, parts = 0;
    uint8_t count = 0;
    double work = 0.0, ended = 0.0;
};

struct State {
    Pass pass;
    bool have_first = false;
    Scan first;
    bool settled = false;   // decided for this stage load (or session)
    double retry_at = 0.0;  // a scan that could not be read: the next one not before this
    const char* logged = nullptr;   // the last reason logged for that (one line per reason and stage load)
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() { return guest::rd<uint64_t>(base() + layout::kSaveRoot); }

int clean_count() {
    const uint64_t r = save_root();
    return r && guest::ok(r + layout::kSaveCleanCount, 1) ? guest::rd<uint8_t>(r + layout::kSaveCleanCount) : -1;
}

uint8_t stage() {
    const uint64_t gm = guest::rd<uint64_t>(base() + layout::kGameState);
    return guest::ok(gm, layout::kStateStage + 1) ? guest::rd<uint8_t>(gm + layout::kStateStage) : 0;
}

// The request's target, or -1 when its row is not the cleaning type (another table than this was made for).
int target(::Ctx* c) {
    const uint64_t row = requests::request_row(c, kRequest);
    if (!row || !guest::ok(row + kRowTarget, 2) || guest::rd<uint8_t>(row + kRowType) != kCleanType) return -1;
    return guest::rd<uint8_t>(row + kRowTarget);
}

// A pass from the world as it is now; false (p.failed says why) when the world is not loaded in a form the scan can read.
bool start(Pass& p, uint8_t count, int target) {
    p = Pass{};
    p.world = guest::rd<uint64_t>(base() + layout::kWorld);
    if (!guest::ok(p.world + kWorldParts, 8) || !guest::ok(p.world + layout::kWorldChunkMap, 8)) {
        p.failed = "no world";
        return false;
    }
    p.map = guest::rd<uint64_t>(p.world + layout::kWorldChunkMap);
    p.cells = guest::rd<uint64_t>(p.world + layout::kWorldCells);
    p.pool = guest::rd<uint64_t>(p.world + kWorldParts);
    p.records = guest::rd<uint64_t>(base() + kPartRecords);
    if (!guest::ok(p.map, layout::kChunkColumns * 2) || !p.cells || !guest::ok(p.pool + kPartCount, 4) || !p.records) {
        p.failed = "the chunk map, the cells or the part pool is not readable";
        return false;
    }
    p.parts = guest::rd<uint32_t>(p.pool + kPartCount);
    if (p.parts < 1 || p.parts > kPartCapacity || !guest::ok(p.pool + kPartRecordOf, uint64_t{p.parts} * 4)) {
        p.failed = "the part count is out of range";
        return false;
    }
    p.records_ok = guest::ok(p.records, uint64_t{kPartCapacity} * kRecordSize);
    bool any = false;
    for (uint32_t i = 0; i < layout::kChunkColumns; ++i) {
        const uint16_t s = guest::rd<uint16_t>(p.map + i * 2);
        if (s < layout::kChunkSlots) p.slot[s] = any = true;
    }
    if (!any) {
        p.failed = "the chunk map has no chunk";
        return false;
    }
    p.count = count;
    p.target = target;
    p.started = qpc::seconds();
    p.active = true;
    return true;
}

// The world the pass started on is still the one loaded.
bool same_world(const Pass& p) {
    const uint64_t w = guest::rd<uint64_t>(base() + layout::kWorld);
    return w == p.world && guest::rd<uint64_t>(w + layout::kWorldChunkMap) == p.map &&
           guest::rd<uint64_t>(w + layout::kWorldCells) == p.cells && guest::rd<uint64_t>(w + kWorldParts) == p.pool;
}

// One frame of the pass: true once it is complete; false with p.active cleared when it failed (nothing is decided from it).
bool step(Pass& p) {
    const double t0 = qpc::seconds();
    unsigned chunks = 0;
    while (p.next_slot < layout::kChunkSlots && chunks < kChunksPerFrame) {
        const uint32_t s = p.next_slot++;
        if (!p.slot[s]) continue;
        const uint64_t at = p.cells + s * layout::kChunkBytes;
        if (!guest::ok(at, layout::kChunkBytes)) {
            p.active = false;
            p.failed = "a chunk's cells are not mapped";
            return false;
        }
        const uint8_t* cell = reinterpret_cast<const uint8_t*>(at);
        for (uint32_t i = 0; i < kCellsPerChunk; ++i, cell += 3) {
            const uint16_t id = static_cast<uint16_t>((cell[0] | cell[1] << 8) & 0x7ff);
            p.cells_used += id != 0;
            p.blocks += mud_block(id);
        }
        ++chunks;
    }
    if (p.next_slot >= layout::kChunkSlots && !chunks) {
        const uint32_t end = p.next_part + kPartsPerFrame < p.parts ? p.next_part + kPartsPerFrame : p.parts;
        for (; p.next_part < end; ++p.next_part) {
            const uint32_t rec = guest::rd<uint32_t>(p.pool + kPartRecordOf + uint64_t{p.next_part} * 4);
            const uint64_t at = p.records + uint64_t{rec} * kRecordSize;
            if (rec >= kPartCapacity || (!p.records_ok && !guest::ok(at, kRecordSize))) {
                p.active = false;
                p.failed = "a part's record is out of range";
                return false;
            }
            p.mud_parts += mud_part(guest::rd<uint32_t>(at + kRecordPlace) & kVariation);
        }
    }
    p.work += qpc::seconds() - t0;
    return p.next_slot >= layout::kChunkSlots && p.next_part >= p.parts;
}

// Both scans agree: the count is raised when what is missing from the target is no longer in the stage.
void decide(const Pass& p, double now) {
    State& s = st();
    const int t = p.target;
    s.settled = true;
    const uint32_t left = p.blocks + p.mud_parts;
    const int c = p.count;
    const bool raise = c < t && left < static_cast<uint32_t>(t - c);
    const int to = raise ? std::min<int>(t - static_cast<int>(left), kCountMax) : c;
    if (raise) guest::wr<uint8_t>(save_root() + layout::kSaveCleanCount, static_cast<uint8_t>(to));
    NXLOG_I(kTag,
            "Co-op requests: the cleaning request's check: %u mud blocks and %u mud parts left in the stage, count %d, target %d "
            "(two scans %.0f s apart that agree, %.1f ms and %.1f ms of work, %.1f s for the last): %s %d", p.blocks, p.mud_parts, c, t,
            p.started - s.first.ended, s.first.work * 1000.0, p.work * 1000.0, now - p.started,
            raise ? "the group removed the rest already, count raised to" : "left as it is at", to);
}

}  // namespace

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    if (!c) return;
    // The request's own facts: outside them the next stage load (or session) is a new start.
    const int count = clean_count();
    if (!link::in_session() || !link::is_authority() || stage() != kMudStage || requests::request_state(kRequest) != kTaken ||
        count < 0) {
        s = State{};
        return;
    }
    if (s.settled) return;
    Pass& p = s.pass;
    // A moment the world is not walkable (a load, an event) or changed: the pass in progress is dropped, a finished one is kept.
    if (!session::can_walk(in_game) || (p.active && (!same_world(p) || count != p.count))) {
        p.active = false;
        return;
    }
    const double now = qpc::seconds();
    if (!p.active) {
        if (now < s.retry_at || (s.have_first && now - s.first.ended < kConfirmAfter)) return;
        const int t = target(c);
        if (t < 0 || count >= t) return;   // not the cleaning row, or already met: the checker finishes it
        start(p, static_cast<uint8_t>(count), t);
    }
    const bool done = p.active && step(p);
    if (!done && !p.failed) return;
    p.active = false;
    if (!p.failed && !p.cells_used) p.failed = "the stage has no block at all (not loaded)";
    if (p.failed) {
        s.retry_at = now + kConfirmAfter;
        if (s.logged != p.failed)
            NXLOG_I(kTag, "Co-op requests: the cleaning request's check: %s; tried again in %.0f s", p.failed, kConfirmAfter);
        s.logged = p.failed;
        p.failed = nullptr;
        return;
    }
    if (s.have_first && s.first.blocks == p.blocks && s.first.parts == p.mud_parts && s.first.count == p.count) {
        decide(p, now);
        return;
    }
    if (s.have_first)
        NXLOG_I(kTag, "Co-op requests: the cleaning request's check: the stage changed between two scans (%u + %u mud left, count %u, "
                "then %u + %u, count %u); scanned again later", s.first.blocks, s.first.parts, s.first.count, p.blocks, p.mud_parts,
                p.count);
    s.have_first = true;
    s.first = Scan{p.blocks, p.mud_parts, p.count, p.work, now};
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::mud
