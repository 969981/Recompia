// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's shared discoveries (pkp_coop_internal.h): the Pokedex entries and the habitats any member found are
// found by every member, with the game's own scene. The game keeps both in each player's own save, and registers them on the process
// of the player who was there (the talk with a new creature, the habitat built near the player): the others' Pokedex and habitat list
// stayed empty, and they never saw the "registered" or "new habitat" scene.
//
// The game, 2.0.1 (modding-docs coop-v2/DISCOVERY-RE.md):
//   save root + 0x6fa1a4 + dex no.   m_pokemonDexHistory, one byte per Pokedex number 1..999 (CMNDAT): bits 0..3, each set by one
//                                    setter that takes a creature id (row of the creature table, +0x28 its Pokedex number):
//                                    kDexSet[bit](creature) -> 1 when it set the bit (0 when it was set, or the game refuses now).
//                                    Bits 0 and 1 also learn the creature's habitat hints. The setters are the game's only writers.
//   save root + 0x6f89a4 + hab * 0xc m_habitatHistory, habitat 1..500 (CMNDAT): bit 0 found (kHabitatFind(hab): sets it, learns the
//                                    hints of the habitat's creatures, the habitat list's first-time tip), bit 1 its scene was shown
//                                    (kHabitatSeen(hab)), bit 2 "new" in the list.
//   kDexScene(slot, &actor id)       queues event 0xe7e for the slot: the "registered in the Pokedex" scene of the creature the actor
//                                    is (its creature id from kCreatureOf(&actor id)). The talk-end registration 0x3b88b4 is its one
//                                    caller, after it set the entry's bits.
//   kHabitatScene(slot, hab, placed habitat, flag)
//                                    queues event 0xe7b / 0x106a: the "new habitat" scene at that placed habitat (an index into the
//                                    world's habitat records; a record that is not there queues nothing). Its two callers run
//                                    kHabitatSeen + kHabitatFind first; kHabitatFound (0x957588) is that whole sequence.
// Rules. Every process reads both lists each frame and shares what became set (DexKnown, HabitatKnown), and the whole lists every
// kAllEvery seconds and when it or another member joins, through the authority (a union: a member that is behind never takes one
// away). The two scene sites are hooked on the process where the game queues them, and sent (DexScene, HabitatScene). A receiver
// plays a scene at a safe moment (in the group's world, can walk, talks to no one) only when the entry is new here: for the Pokedex,
// only when the actor is the same creature here (actors are the world's, replicated by the game); otherwise a line in the message
// log says what was found. A bit that arrives without a scene is set kSettle seconds later through the game's own setter, so a scene
// on its way (or the game's own registration here) wins.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_tables.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::discovery {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kDexHistory = 0x6fa1a4;        // save root + dex no.: the entry's bits
constexpr uint32_t kDexMax = 999;                 // dex no. 1..999 (the setters take dex no. - 1 <= 0x3e6)
constexpr uint8_t kDexBits = 0x0f;
constexpr uint64_t kDexSet[4] = {0xa982b0, 0xa983d4, 0xa984e4, 0xa985c8};   // (creature id) -> 1: bit 0, 1, 2, 3 set
constexpr uint8_t kDexMet = 0x02;                 // bit 1: the one the talk-end registration sets before its scene
constexpr uint64_t kCreatureTable = 0x8d0;        // the creature table (slot 0x11a); +0 while the manager holds fewer
constexpr uint64_t kCreatureDexNo = 0x28;         // creature row: u16 its Pokedex number
constexpr uint32_t kCreatureMax = 0x3ff;          // creature ids 1..0x3ff
constexpr uint64_t kCreatureOf = 0x285434;        // (&actor id) -> u16 creature id (0: no such actor here)
constexpr uint64_t kDexScene = 0x3b9060;          // (slot, &actor id): queue the "registered" scene

constexpr uint64_t kHabitatHistory = 0x6f89a4;    // save root + hab * kHabitatStride
constexpr uint64_t kHabitatStride = 0xc;
constexpr uint32_t kHabitatMax = 500;
constexpr uint8_t kHabitatFound = 0x01, kHabitatShown = 0x02, kHabitatBits = 0x03;
constexpr uint64_t kHabitatFind = 0xa96f5c;       // (hab): bit 0, the hints, the list's tip
constexpr uint64_t kHabitatSeen = 0xa9802c;       // (hab): bit 1
constexpr uint64_t kHabitatFoundFn = 0x957588;    // (_, 0, hab, placed habitat, flag): seen + find + the scene, as the game's build
constexpr uint64_t kHabitatListOn = 0xbba2c;      // () -> bit 0: the habitat list is on (permission bit 25)
constexpr uint32_t kPlacedMax = 0x3ff;            // placed habitat index 0..0x3ff

constexpr double kAllEvery = 30.0;
constexpr double kSettle = 8.0;
constexpr unsigned kApplyPerTick = 8;
constexpr size_t kSceneQueue = 8;

struct DexScene {
    uint32_t actor;
    uint16_t creature, dex_no;
    uint8_t origin;
};
struct HabScene {
    uint16_t hab, placed;
    uint8_t flag, origin;
};

struct State {
    uint8_t dex_seen[kDexMax + 1] = {};         // per dex no.: the bits at the last read (and sent, once the baseline went)
    uint8_t dex_want[kDexMax + 1] = {};         // per dex no.: bits another member has, not set here yet
    double dex_at[kDexMax + 1] = {};
    uint8_t dex_origin[kDexMax + 1] = {};
    uint8_t hab_seen[kHabitatMax + 1] = {};
    uint8_t hab_want[kHabitatMax + 1] = {};
    double hab_at[kHabitatMax + 1] = {};
    uint8_t hab_origin[kHabitatMax + 1] = {};
    std::bitset<kDexMax + 1> dex_refused;       // a setter refused the id once: logged once
    uint16_t creature_of[kDexMax + 1] = {};     // dex no. -> a creature id with that number (0: none)
    bool creatures_read = false;
    bool have_seen = false, baseline_sent = false, send_all = true;
    double all_at = 0.0;
    std::vector<DexScene> dex_out, dex_in;      // scenes to send / to play here
    std::vector<HabScene> hab_out, hab_in;
    bool replaying = false;                     // our own call of a scene site: not sent again
    alignas(8) uint32_t actor_arg[2] = {};      // the actor id a scene call reads, guest-readable
    unsigned dex_lines = 0, hab_lines = 0;      // quiet finds in the current batch (one line when it is done)
    int line_origin = -1;
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return r && guest::ok(r + kDexHistory, kDexMax + 1) && guest::ok(r + kHabitatHistory, (kHabitatMax + 1) * kHabitatStride) ? r : 0;
}
uint8_t dex_bits(uint64_t root, uint32_t no) { return guest::rd<uint8_t>(root + kDexHistory + no) & kDexBits; }
uint8_t hab_bits(uint64_t root, uint32_t hab) {
    return guest::rd<uint8_t>(root + kHabitatHistory + hab * kHabitatStride) & kHabitatBits;
}

bool shared_here() { return (session_bits() & layout::kSessionOnline) && link::in_session(); }
bool safe_now(bool in_game) {
    const bool in_world = role() == CoopRole::Host || (session_bits() & layout::kSessionVisitor);
    return in_world && session::can_walk(in_game) && !talk::talking();
}

// The creature table's Pokedex numbers, read once: dex no. -> the first creature id with it (the setters take a creature id).
void read_creatures(::Ctx* c) {
    State& s = st();
    if (s.creatures_read) return;
    unsigned found = 0;
    for (uint32_t id = 1; id <= kCreatureMax; ++id) {
        const uint64_t holder = tables::row_or_first(c, base(), kCreatureTable, id);
        const uint64_t row = holder ? guest::rd<uint64_t>(holder + 8) : 0;
        if (!row || !guest::ok(row + kCreatureDexNo, 2)) continue;
        const uint16_t no = guest::rd<uint16_t>(row + kCreatureDexNo);
        if (no < 1 || no > kDexMax || s.creature_of[no]) continue;
        s.creature_of[no] = static_cast<uint16_t>(id);
        ++found;
    }
    if (!found) return;   // the table is not loaded yet: read again next frame
    s.creatures_read = true;
    NXLOG_I(kTag, "Co-op discovery: %u Pokedex numbers read from the creature table", found);
}

// Sends every entry whose bits grew since the last read (all: every entry with bits), DexKnown then HabitatKnown. false: a send
// failed, and the same entries go at the next read.
bool share(uint64_t root, bool all) {
    State& s = st();
    wire::Writer dw(link::self()), hw(link::self());
    std::vector<uint8_t> dex, habs;
    uint16_t dn = 0, hn = 0;
    for (uint32_t no = 1; no <= kDexMax; ++no) {
        const uint8_t b = dex_bits(root, no);
        if (!b || (!all && s.have_seen && (b & ~s.dex_seen[no]) == 0)) continue;
        dex.push_back(static_cast<uint8_t>(no));
        dex.push_back(static_cast<uint8_t>(no >> 8));
        dex.push_back(b);
        ++dn;
    }
    for (uint32_t hab = 1; hab <= kHabitatMax; ++hab) {
        const uint8_t b = hab_bits(root, hab);
        if (!b || (!all && s.have_seen && (b & ~s.hab_seen[hab]) == 0)) continue;
        habs.push_back(static_cast<uint8_t>(hab));
        habs.push_back(static_cast<uint8_t>(hab >> 8));
        habs.push_back(b);
        ++hn;
    }
    const bool first = !s.have_seen;
    s.have_seen = true;
    if (dn && !link::share(wire::Type::DexKnown, dw.u16(dn).bytes(dex.data(), dex.size()).take())) return false;
    if (hn && !link::share(wire::Type::HabitatKnown, hw.u16(hn).bytes(habs.data(), habs.size()).take())) return false;
    for (uint32_t no = 1; no <= kDexMax; ++no) {
        const uint8_t b = dex_bits(root, no);
        if (s.baseline_sent && !first && (b & ~s.dex_seen[no])) NXLOG_I(kTag, "Co-op discovery: Pokedex no. %u now 0x%x here, shared", no, b);
        s.dex_seen[no] = b;
    }
    for (uint32_t hab = 1; hab <= kHabitatMax; ++hab) {
        const uint8_t b = hab_bits(root, hab);
        if (s.baseline_sent && !first && (b & ~s.hab_seen[hab])) NXLOG_I(kTag, "Co-op discovery: habitat %u now 0x%x here, shared", hab, b);
        s.hab_seen[hab] = b;
    }
    s.baseline_sent = true;
    return true;
}

void send_scenes() {
    State& s = st();
    size_t sent = 0;
    for (const DexScene& d : s.dex_out) {
        if (!link::share(wire::Type::DexScene, wire::Writer(link::self()).u32(d.actor).u16(d.creature).u16(d.dex_no).take())) break;
        ++sent;
    }
    s.dex_out.erase(s.dex_out.begin(), s.dex_out.begin() + static_cast<std::ptrdiff_t>(sent));
    sent = 0;
    for (const HabScene& h : s.hab_out) {
        if (!link::share(wire::Type::HabitatScene, wire::Writer(link::self()).u16(h.hab).u16(h.placed).u8(h.flag).take())) break;
        ++sent;
    }
    s.hab_out.erase(s.hab_out.begin(), s.hab_out.begin() + static_cast<std::ptrdiff_t>(sent));
}

void batch_line() {
    State& s = st();
    if (!s.dex_lines && !s.hab_lines) return;
    char text[192];
    const std::string who = notice::name_of(static_cast<uint8_t>(s.line_origin < 0 ? 0 : s.line_origin));
    if (s.dex_lines && s.hab_lines)
        std::snprintf(text, sizeof text, "%s's finds are yours too: %u Pokedex entr%s and %u habitat%s.", who.c_str(), s.dex_lines,
                      s.dex_lines == 1 ? "y" : "ies", s.hab_lines, s.hab_lines == 1 ? "" : "s");
    else if (s.dex_lines)
        std::snprintf(text, sizeof text, "%s filled in %u Pokedex entr%s. Yours has %s too!", who.c_str(), s.dex_lines,
                      s.dex_lines == 1 ? "y" : "ies", s.dex_lines == 1 ? "it" : "them");
    else
        std::snprintf(text, sizeof text, "%s found %u habitat%s. Your list has %s too!", who.c_str(), s.hab_lines,
                      s.hab_lines == 1 ? "" : "s", s.hab_lines == 1 ? "it" : "them");
    notice::post(text);
    s.dex_lines = s.hab_lines = 0;
    s.line_origin = -1;
}

void want_bits(uint8_t* want, double* at, uint8_t* from, uint32_t id, uint8_t bits, uint8_t here, uint8_t origin);

// One scene a safe moment (each is a queued event: the next waits for it to close in the game's own queue anyway).
bool play_scene(::Ctx* c, uint64_t root) {
    State& s = st();
    if (!s.hab_in.empty()) {
        const HabScene h = s.hab_in.front();
        s.hab_in.erase(s.hab_in.begin());
        if (hab_bits(root, h.hab) & kHabitatShown) return false;   // its scene was shown here meanwhile (the game's own test, 0x95820c)
        if (!(guest::call(c, base() + kHabitatListOn, 0) & 1)) {
            s.hab_want[h.hab] |= kHabitatFound | kHabitatShown;   // the list is off here: found quietly later
            s.hab_at[h.hab] = 0.0;
            s.hab_origin[h.hab] = h.origin;
            return false;
        }
        s.replaying = true;
        guest::call(c, base() + kHabitatFoundFn, 0, 0, h.hab, h.placed, h.flag);
        s.replaying = false;
        s.hab_seen[h.hab] |= hab_bits(root, h.hab);
        s.hab_want[h.hab] = 0;
        NXLOG_I(kTag, "Co-op discovery: habitat %u (placed %u) from node %u found here, with its scene", h.hab, h.placed, h.origin);
        notice::post(notice::name_of(h.origin) + " found a new habitat. It's in your list too!");
        return true;
    }
    if (!s.dex_in.empty()) {
        const DexScene d = s.dex_in.front();
        s.dex_in.erase(s.dex_in.begin());
        s.actor_arg[0] = d.actor;
        const uint16_t here = static_cast<uint16_t>(guest::call(c, base() + kCreatureOf, reinterpret_cast<uint64_t>(s.actor_arg)));
        if (dex_bits(root, d.dex_no) & kDexMet) return false;   // registered here meanwhile
        // The entry first, through the game's own setter with the game's own arguments (creature, 0), as the talk-end registration
        // does before its scene. A setter that refuses now (its gate 0x11672c) leaves the entry to the lists, with no scene.
        guest::call(c, base() + kDexSet[1], d.creature, 0);
        if (!(dex_bits(root, d.dex_no) & kDexMet)) {
            want_bits(s.dex_want, s.dex_at, s.dex_origin, d.dex_no, kDexMet, dex_bits(root, d.dex_no), d.origin);
            NXLOG_I(kTag, "Co-op discovery: Pokedex no. %u from node %u: the game does not register it here now, later", d.dex_no, d.origin);
            return false;
        }
        s.dex_seen[d.dex_no] |= dex_bits(root, d.dex_no);
        s.dex_want[d.dex_no] &= static_cast<uint8_t>(~dex_bits(root, d.dex_no));
        if (here == d.creature) {
            s.replaying = true;
            guest::call(c, base() + kDexScene, 0, reinterpret_cast<uint64_t>(s.actor_arg));
            s.replaying = false;
            NXLOG_I(kTag, "Co-op discovery: Pokedex no. %u (creature %u) from node %u registered here, with its scene", d.dex_no,
                    d.creature, d.origin);
        } else {
            NXLOG_I(kTag, "Co-op discovery: Pokedex no. %u (creature %u) from node %u registered here; actor 0x%x is creature %u here, "
                    "no scene", d.dex_no, d.creature, d.origin, d.actor, here);
        }
        notice::post(notice::name_of(d.origin) + " met a new Pokemon. It's in your Pokedex too!");
        return true;
    }
    return false;
}

// The bits that settled and came with no scene: set through the game's own setters, a few a frame, with one line per batch.
void apply_bits(::Ctx* c, uint64_t root) {
    State& s = st();
    const double now = qpc::seconds();
    unsigned done = 0;
    bool waiting = false;
    for (uint32_t hab = 1; hab <= kHabitatMax && done < kApplyPerTick; ++hab) {
        uint8_t want = s.hab_want[hab] & static_cast<uint8_t>(~hab_bits(root, hab));
        if (!want) {
            s.hab_want[hab] = 0;
            continue;
        }
        if (now - s.hab_at[hab] < kSettle) {
            waiting = true;
            continue;
        }
        s.hab_want[hab] = 0;
        if (want & kHabitatShown) guest::call(c, base() + kHabitatSeen, hab);
        if (want & kHabitatFound) {
            guest::call(c, base() + kHabitatFind, hab);
            if (s.line_origin < 0) s.line_origin = s.hab_origin[hab];
            ++s.hab_lines;
        }
        s.hab_seen[hab] |= hab_bits(root, hab);
        ++done;
        NXLOG_I(kTag, "Co-op discovery: habitat %u bits 0x%x from node %u set here", hab, want, s.hab_origin[hab]);
    }
    for (uint32_t no = 1; no <= kDexMax && done < kApplyPerTick; ++no) {
        uint8_t want = s.dex_want[no] & static_cast<uint8_t>(~dex_bits(root, no));
        if (!want) {
            s.dex_want[no] = 0;
            continue;
        }
        if (now - s.dex_at[no] < kSettle) {
            waiting = true;
            continue;
        }
        s.dex_want[no] = 0;
        const uint16_t creature = s.creature_of[no];
        if (!creature) continue;
        const bool fresh = !(dex_bits(root, no) & kDexMet) && (want & kDexMet);
        for (unsigned bit = 0; bit < 4; ++bit)
            if (want & (1u << bit)) guest::call(c, base() + kDexSet[bit], creature, 0);
        const uint8_t now_bits = dex_bits(root, no);
        s.dex_seen[no] |= now_bits;
        if (want & ~now_bits) {
            if (!s.dex_refused.test(no)) NXLOG_W(kTag, "Co-op discovery: Pokedex no. %u: the game set 0x%x of 0x%x here", no, now_bits & want, want);
            s.dex_refused.set(no);
        }
        if (fresh && (now_bits & kDexMet)) {
            if (s.line_origin < 0) s.line_origin = s.dex_origin[no];
            ++s.dex_lines;
        }
        ++done;
    }
    if (!done && !waiting) batch_line();
}

void want_bits(uint8_t* want, double* at, uint8_t* from, uint32_t id, uint8_t bits, uint8_t here, uint8_t origin) {
    const uint8_t add = bits & static_cast<uint8_t>(~here) & static_cast<uint8_t>(~want[id]);
    if (!add) return;
    if (!want[id]) at[id] = qpc::seconds();
    want[id] |= add;
    from[id] = origin;
}

}  // namespace

int on_dex_scene(::Ctx* c) {
    State& s = st();
    if (s.replaying || !shared_here() || !guest::ok(c->x[1], 4)) return 0;
    const uint32_t actor = guest::rd<uint16_t>(c->x[1]);   // the scene reads a u16 (ldrh [x1])
    const uint16_t creature = static_cast<uint16_t>(guest::call(c, base() + kCreatureOf, c->x[1]));
    const uint64_t holder = creature ? tables::row_or_first(c, base(), kCreatureTable, creature) : 0;
    const uint64_t row = holder ? guest::rd<uint64_t>(holder + 8) : 0;
    const uint16_t no = row && guest::ok(row + kCreatureDexNo, 2) ? guest::rd<uint16_t>(row + kCreatureDexNo) : 0;
    if (static_cast<uint16_t>(creature - 1) >= kCreatureMax || no < 1 || no > kDexMax) return 0;
    if (s.dex_out.size() < kSceneQueue) s.dex_out.push_back({actor, creature, no, link::self()});
    NXLOG_I(kTag, "Co-op discovery: Pokedex scene of creature %u (no. %u, actor 0x%x) here, sent to the group", creature, no, actor);
    return 0;   // the game's own scene runs
}

int on_habitat_scene(::Ctx* c) {
    State& s = st();
    if (s.replaying || !shared_here()) return 0;
    const uint16_t hab = static_cast<uint16_t>(c->x[1]);
    const uint32_t placed = static_cast<uint32_t>(c->x[2]);
    if (static_cast<uint16_t>(hab - 1) >= kHabitatMax || placed > kPlacedMax) return 0;
    if (s.hab_out.size() < kSceneQueue) s.hab_out.push_back({hab, static_cast<uint16_t>(placed), static_cast<uint8_t>(c->x[3] & 1), link::self()});
    NXLOG_I(kTag, "Co-op discovery: new habitat scene of habitat %u (placed %u) here, sent to the group", hab, placed);
    return 0;
}

void on_dex_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint16_t n = r.u16();
    const uint64_t root = save_root();
    if (!r.ok() || !root || !shared_here()) return;
    for (uint16_t k = 0; k < n; ++k) {
        const uint16_t no = r.u16();
        const uint8_t bits = r.u8() & kDexBits;
        if (!r.ok()) return;
        if (no < 1 || no > kDexMax) continue;
        want_bits(s.dex_want, s.dex_at, s.dex_origin, no, bits, dex_bits(root, no), origin);
    }
}

void on_habitat_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint16_t n = r.u16();
    const uint64_t root = save_root();
    if (!r.ok() || !root || !shared_here()) return;
    for (uint16_t k = 0; k < n; ++k) {
        const uint16_t hab = r.u16();
        const uint8_t bits = r.u8() & kHabitatBits;
        if (!r.ok()) return;
        if (hab < 1 || hab > kHabitatMax) continue;
        want_bits(s.hab_want, s.hab_at, s.hab_origin, hab, bits, hab_bits(root, hab), origin);
    }
}

void on_dex_scene_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint32_t actor = r.u32();
    const uint16_t creature = r.u16(), no = r.u16();
    const uint64_t root = save_root();
    if (!r.ok() || !root || !shared_here() || static_cast<uint16_t>(creature - 1) >= kCreatureMax || no < 1 || no > kDexMax || actor > 0xffff)
        return;
    if (dex_bits(root, no) & kDexMet) return;   // registered here already: no scene
    if (s.dex_in.size() < kSceneQueue) s.dex_in.push_back({actor, creature, no, origin});
}

void on_habitat_scene_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint16_t hab = r.u16(), placed = r.u16();
    const uint8_t flag = r.u8();
    const uint64_t root = save_root();
    if (!r.ok() || !root || !shared_here() || hab < 1 || hab > kHabitatMax || placed > kPlacedMax || flag > 1) return;
    if (hab_bits(root, hab) & kHabitatShown) return;   // its scene was shown here already (the game plays it only while bit 1 is clear)
    if (s.hab_in.size() < kSceneQueue) s.hab_in.push_back({hab, placed, flag, origin});
}

void tick(::Ctx* c, bool in_game) {
    State& s = st();
    const uint64_t root = save_root();
    if (!c || !root || !shared_here()) return;
    read_creatures(c);
    send_scenes();
    if (safe_now(in_game) && !play_scene(c, root)) apply_bits(c, root);
    const double now = qpc::seconds();
    const bool all = s.send_all || now - s.all_at >= kAllEvery;
    if (!share(root, all) || !all) return;
    s.send_all = false;
    s.all_at = now;
}

void on_session_change() {
    State& s = st();
    const bool read = s.creatures_read;
    uint16_t creatures[kDexMax + 1];
    std::copy(std::begin(s.creature_of), std::end(s.creature_of), creatures);
    s = State{};
    s.creatures_read = read;
    std::copy(std::begin(creatures), std::end(creatures), s.creature_of);
}

void on_member_joined() { st().send_all = true; }

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::discovery

// The digests' readers (pkp_coop_digest.cpp): the entries as the save has them now.
namespace nxrt::games::pkp::coop::digest {

bool dex(Entries& out) {
    const uint64_t root = coop::discovery::save_root();
    if (!root) return false;
    for (uint32_t no = 1; no <= coop::discovery::kDexMax; ++no)
        if (const uint8_t b = coop::discovery::dex_bits(root, no)) out.push_back({no, b});
    return true;
}

bool habitats(Entries& out) {
    const uint64_t root = coop::discovery::save_root();
    if (!root) return false;
    for (uint32_t hab = 1; hab <= coop::discovery::kHabitatMax; ++hab)
        if (const uint8_t b = coop::discovery::hab_bits(root, hab)) out.push_back({hab, b});
    return true;
}

}  // namespace nxrt::games::pkp::coop::digest
