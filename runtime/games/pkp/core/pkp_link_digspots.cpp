// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: a story dig spot a visitor digs up is still the host's find (pkp_link_digspots.h).
//
// The 2.0.1 main's dig-spot manager (*(main+0x3561900) + 0x2f30e0) holds the 19 story dig spots at +0x820, 10 bytes each (id 1-19):
// +2 the glow's effect handle, +4 u8 the stage, +6 u16 the data row, +8 valid.
// - 0x97d184: the manager's frame; at 0x97d1c4 (`bl 0x97d660`, the glows' refresh; x0 = the manager) the midhook looks at the spots.
// - 0x94c8b4(manager, id): the spot's cell, x | y << 16 | z << 32 (s16 each).
// - 0x17d7c(world, x, y, z): the block id of a cell (0: air, or no data). A block is natural terrain when its id is 1-0x485 and its
//   byte in the table *(main+0x3483c28) is 0: the test the world's set-block 0x137e20 makes before it ends a spot.
// - 0x980ca0(manager, u16 cell[3], 0, breaker[16]): ends the spot at the cell (item drop, found bit, glow); 0x137e20's call at
//   0x138ba4. 0x2fc0bc(out[16]): the local player as the breaker. It does nothing for a visitor.
#include "core/pkp_link_digspots.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_site_check.h"

#include <atomic>

#include "nxrt/hooks.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";
constexpr HookSite kSites[] = {
    {0x97d1c4, 0x94000127u, false},   // bl 0x97d660
};
constexpr uint64_t kWorld = 0x34efc98, kNaturalTable = 0x3483c28;
constexpr uint64_t kSpotCell = 0x94c8b4, kBlockId = 0x17d7c, kLocalBreaker = 0x2fc0bc, kEndSpot = 0x980ca0;
constexpr uint64_t kSpots = 0x820, kSpotSize = 10, kSpotStage = 4, kSpotKey = 6, kSpotValid = 8;
constexpr uint32_t kSpotCount = 19, kNaturalMax = 0x485;

struct State {
    uint64_t base = 0;
    HookSet* hooks = nullptr;
    uint32_t id = 0;                 // 0: off or not installed
    std::atomic<bool> armed{false};
    std::atomic<bool> checked{false};
    // The game thread's:
    DigSpotWatch watch[kSpotCount];
    uint64_t breaker[4] = {};        // 0x2fc0bc's 16 bytes, with room
    uint16_t cell[4] = {};
    bool logged = false;
};
State& st() {
    static State s;
    return s;
}

bool natural(const State& s, uint32_t block) {
    if (block - 1 >= kNaturalMax) return false;
    const uint64_t table = guest::rd<uint64_t>(s.base + kNaturalTable);
    return guest::ok(table, kNaturalMax + 1) && guest::rd<uint8_t>(table + block) == 0;
}

int hook(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.armed.load(std::memory_order_acquire)) return 0;
    const uint64_t manager = c->x[0];
    const uint64_t game = guest::rd<uint64_t>(s.base + layout::kGameState);
    const uint64_t world = guest::rd<uint64_t>(s.base + kWorld);
    const uint8_t host = layout::kSessionOnline | layout::kSessionHost;
    if (!guest::ok(game, layout::kStateSession + 1) || !guest::ok(manager, kSpots + kSpotCount * kSpotSize) || !world ||
        (guest::rd<uint8_t>(game + layout::kStateSession) & host) != host) {
        for (DigSpotWatch& w : s.watch) w = {};
        return 0;
    }
    const uint8_t stage = guest::rd<uint8_t>(game + layout::kStateStage);
    for (uint32_t id = 1; id <= kSpotCount; ++id) {
        const uint64_t e = manager + kSpots + (id - 1) * kSpotSize;
        const bool here = guest::rd<uint8_t>(e + kSpotValid) && guest::rd<uint8_t>(e + kSpotStage) == stage;
        const uint16_t key = here ? guest::rd<uint16_t>(e + kSpotKey) : 0;
        bool natural_now = false;
        uint64_t at = 0;
        if (key) {
            at = guest::call(c, s.base + kSpotCell, manager, id);
            const uint64_t x = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(at)));
            const uint64_t y = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(at >> 16)));
            const uint64_t z = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(at >> 32)));
            natural_now = natural(s, static_cast<uint32_t>(guest::call(c, s.base + kBlockId, world, x, y, z)));
        }
        if (!dig_spot_step(&s.watch[id - 1], key, natural_now)) continue;
        s.cell[0] = static_cast<uint16_t>(at);
        s.cell[1] = static_cast<uint16_t>(at >> 16);
        s.cell[2] = static_cast<uint16_t>(at >> 32);
        guest::call(c, s.base + kLocalBreaker, reinterpret_cast<uint64_t>(s.breaker));
        guest::call(c, s.base + kEndSpot, manager, reinterpret_cast<uint64_t>(s.cell), 0, reinterpret_cast<uint64_t>(s.breaker));
        if (!s.logged) {
            s.logged = true;
            NXLOG_I(kTag, "Link Play dig spots: a story dig spot whose block another player removed was ended for the host (item dropped at the spot)");
        }
    }
    return 0;
}

void install(const FeatureInstall& in) {
    State& s = st();
    if (s.id) return;
    if (!settings::get_bool(settings::Key::LinkInteractEverywhere) && !settings::get_bool(settings::Key::LinkStoryCoop)) return;
    s.base = in.base;
    s.hooks = &in.hooks;
    s.armed.store(false);
    s.checked.store(false);
    s.logged = false;
    for (DigSpotWatch& w : s.watch) w = {};
    s.id = in.hooks.add_mid(in.base + kSites[0].site, &hook);
}

// Play mode's frame (main is loaded): the first call checks the site and arms the hook or removes it.
void frame(bool, double, play::GameFrame*) {
    State& s = st();
    if (!s.id || s.checked.exchange(true)) return;
    uint32_t word = 0;
    if (const HookSite* bad = first_bad_site(s.base, kSites, 1, &word)) {
        s.hooks->remove(s.id);
        s.id = 0;
        NXLOG_W(kTag, "Link Play dig spots: off: main+0x%llx is not the expected hook site (word %08x, expected %08x)",
                static_cast<unsigned long long>(bad->site), word, bad->word);
        return;
    }
    s.armed.store(true, std::memory_order_release);
    NXLOG_I(kTag, "Link Play dig spots: the host's story dig spots are ended when another player digs them up");
}

void remove() {   // the registry has removed the hook
    State& s = st();
    s.armed.store(false);
    s.checked.store(false);
    s.id = 0;
    s.hooks = nullptr;
}

}  // namespace

bool dig_spot_step(DigSpotWatch* w, uint16_t key, bool natural_now) {
    if (key != w->key) *w = {.key = key};
    if (!key || w->ended) return false;
    if (natural_now) {
        w->natural = true;
        w->gone = 0;
        return false;
    }
    if (!w->natural || ++w->gone < kDigSpotGoneFrames) return false;
    w->ended = true;
    return true;
}

const Feature kLinkDigSpotsFeature = {.name = "link_digspots", .install = &install, .remove = &remove, .frame = &frame};

}  // namespace nxrt::games::pkp
