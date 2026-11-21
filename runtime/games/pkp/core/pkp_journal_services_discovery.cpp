// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's look at the discovery setters and the two world notices that co-op replays (pkp_journal_services_internal.h;
// RE: modding-docs coop-v2/DISCOVERY-RE.md, UNLOCK-LISTS-RE.md, RECIPES-RE.md, BULBASAUR-POP-RE.md, TANGROWTH-CAMPFIRE-RE.md). Every
// setter writes the CMNDAT of the process that runs it (the talker, the builder): nothing replicates a bit by itself, so the line names
// who asked (LR) and what the record held before. All entry hooks:
//   0xa982b0 / 0xa983d4 / 0xa984e4 / 0xa985c8 (creature)   the dex byte's bits 0 / 1 / 2 / 3 (R+0x6fa1a4 + dex number)
//   0xa96f5c(hab) found, 0xa9802c(hab) scene shown        the habitat record R+0x6f89a4 + hab*0xc, byte +0 bits 0 / 1
//   0x2e7240(player, recipe, quiet, ..)                   a recipe learned (R+0x6cfdec + id*6, +4 bit 1)
//   0x3bcca8(R+0x108, move, notice) / 0x2f5860(player, emote, force, quiet, alt) / 0x5c04d4(building, on)   the unlock lists
//   0x1240dc(habitat, creature, add; s0 delay)            a wild creature's appearance (script command 0xc1e, quest effect 0x33)
//   0x13596c(scheduler, object, x, y, z, dir)             the contents scheduler's "object placed" notice (kind 5)
// The dex number of a creature is a row of the creature table (a virtual lookup), so the dex setters print the creature only.
#include "core/pkp_journal_services_internal.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::journal::services {

namespace {

constexpr uint64_t kHabitats = 0x6f89a4, kHabitatSize = 0xc;     // save root +: byte +0 bit 0 found, bit 1 scene shown, bit 2 new; u32 +4 trace bits
constexpr uint64_t kMoveKnown = 0x6d82f0, kMoveSize = 8;         // save root + id * 8: u8 bit 0 learned (ids 1..0x3c)
constexpr uint64_t kEmoteKnown = 0x6d84f4;                       // save root + id: u8 bit 0 learned (ids 1..0x64)
constexpr uint64_t kBuildingBuilt = 0x73b1d4;                    // save root + id: u8 bit 0 built (ids 0..0x7f)
constexpr uint32_t kCreatureMax = 0x3ff, kHabitatMax = 500, kMoveMax = 0x3c, kEmoteMax = 0x64;
constexpr double kFlushEvery = 30.0;

Seen g_recipes;
double g_flushed = 0;

void dex(::Ctx* c, int bit, const char* setter) {
    const uint32_t creature = static_cast<uint32_t>(c->x[0]) & 0xffff;
    journal::Rec r(journal::Area::Discovery, "dex-set");
    r.in("creature=%u bit=%d setter=%s from=%s sess=%s", creature, bit, setter, lr_text(c).c_str(), session_text(session()).c_str());
    if (creature - 1u >= kCreatureMax) r.note("creature outside 1..0x3ff: the game ignores it");
}

int record_byte(uint64_t at) {
    return guest::ok(at, 1) ? guest::rd<uint8_t>(at) : -1;
}

void habitat(::Ctx* c, const char* what, int bit) {
    const uint32_t hab = static_cast<uint32_t>(c->x[0]) & 0xffff;
    const uint64_t r = save_root();
    journal::Rec rc(journal::Area::Discovery, what);
    rc.in("hab=%u bit=%d from=%s sess=%s", hab, bit, lr_text(c).c_str(), session_text(session()).c_str());
    if (r && hab - 1u < kHabitatMax) {
        const int byte = record_byte(r + kHabitats + hab * kHabitatSize);
        rc.out("byte before=0x%02x (found %d, scene shown %d, new %d)", byte, byte & 1, byte >> 1 & 1, byte >> 2 & 1);
    } else {
        rc.note("hab outside 1..500: the game ignores it");
    }
}

}  // namespace

void on_dex_bit0(::Ctx* c) { dex(c, 0, "0xa982b0"); }
void on_dex_bit1(::Ctx* c) { dex(c, 1, "0xa983d4"); }
void on_dex_bit2(::Ctx* c) { dex(c, 2, "0xa984e4"); }
void on_dex_bit3(::Ctx* c) { dex(c, 3, "0xa985c8"); }

void on_habitat_found(::Ctx* c) { habitat(c, "habitat-found", 0); }
void on_habitat_scene(::Ctx* c) { habitat(c, "habitat-scene-shown", 1); }

void on_recipe_learn(::Ctx* c) {
    const uint32_t id = static_cast<uint32_t>(c->x[1]) & 0xffff;
    const uint64_t r = save_root();
    const int flags = r && id - 1u < layout::kRecipeMax ? record_byte(r + layout::kSaveRecipes + id * layout::kRecipeSize + layout::kRecipeFlags) : -1;
    const bool known = flags >= 0 && (flags & layout::kRecipeKnown);
    if (!g_recipes.hit(static_cast<uint64_t>(id) << 48 | ((c->x[30] - base()) & 0xffffff) << 8 | known)) return;
    journal::Rec rc(journal::Area::Discovery, "recipe-learn");
    rc.in("recipe=%u quiet=%u player=0x%llx from=%s sess=%s", id, static_cast<unsigned>(c->x[2] & 1), static_cast<unsigned long long>(c->x[0]), lr_text(c).c_str(),
          session_text(session()).c_str());
    rc.out("known before=%s", flags < 0 ? "?" : known ? "yes" : "no");
}

void on_move_learn(::Ctx* c) {
    const uint32_t id = static_cast<uint32_t>(c->x[1]) & 0xff;
    const uint64_t r = save_root();
    const int b = r && id - 1u < kMoveMax ? record_byte(r + kMoveKnown + id * kMoveSize) : -1;
    journal::Rec rc(journal::Area::Discovery, "move-learn");
    rc.in("move=%u notice=%u from=%s sess=%s", id, static_cast<unsigned>(c->x[2] & 1), lr_text(c).c_str(), session_text(session()).c_str());
    rc.out("known before=%s", b < 0 ? "?" : b & 1 ? "yes" : "no");
}

void on_emote_learn(::Ctx* c) {
    const uint32_t id = static_cast<uint32_t>(c->x[1]) & 0xff;
    const uint64_t r = save_root();
    const int b = r && id - 1u < kEmoteMax ? record_byte(r + kEmoteKnown + id) : -1;
    journal::Rec rc(journal::Area::Discovery, "emote-learn");
    rc.in("emote=%u force=%u quiet=%u alt=%u from=%s sess=%s", id, static_cast<unsigned>(c->x[2] & 1), static_cast<unsigned>(c->x[3] & 1),
          static_cast<unsigned>(c->x[4] & 1), lr_text(c).c_str(), session_text(session()).c_str());
    rc.out("known before=%s", b < 0 ? "?" : b & 1 ? "yes" : "no");
}

void on_building_set(::Ctx* c) {
    const uint32_t id = static_cast<uint32_t>(c->x[0]) & 0xff;
    const uint64_t r = save_root();
    const int b = r && id < 0x80 ? record_byte(r + kBuildingBuilt + id) : -1;
    journal::Rec rc(journal::Area::Discovery, "building-set");
    rc.in("building=%u on=%u from=%s sess=%s", id, static_cast<unsigned>(c->x[1] & 1), lr_text(c).c_str(), session_text(session()).c_str());
    rc.out("built before=%s", b < 0 ? "?" : b & 1 ? "yes" : "no");
    if (id >= 0x80) rc.note("id bit 7 set: the game ignores it");
}

void on_wild_pop(::Ctx* c) {
    const uint8_t sess = session();
    journal::Rec r(journal::Area::Service, "wild-pop");
    r.in("habitat=%u creature=%u delay=%.1f add=%u from=%s sess=%s", static_cast<unsigned>(c->x[0] & 0xffff), static_cast<unsigned>(c->x[1] & 0xffff),
         c->v[0].f[0], static_cast<unsigned>(c->x[2] & 1), lr_text(c).c_str(), session_text(sess).c_str());
    // The game writes the caller's own STGDAT wild-pop event field (R+0x972ba0+0xb4): on a visitor that is not the authority's.
    r.out("written to this process's own STGDAT");
    if (is_visitor(sess)) r.expect("the authority's STGDAT");
}

void on_placed_notice(::Ctx* c) {
    journal::Rec r(journal::Area::Service, "placement-notice");
    r.in("kind=5 scheduler=0x%llx object=%u cell=(%d,%d,%d) dir=%u from=%s sess=%s stage=%u", static_cast<unsigned long long>(c->x[0]), static_cast<unsigned>(c->x[1] & 0xffff),
         static_cast<int16_t>(c->x[2]), static_cast<int16_t>(c->x[3]), static_cast<int16_t>(c->x[4]), static_cast<unsigned>(c->x[5] & 0xff), lr_text(c).c_str(),
         session_text(session()).c_str(), stage());
    if (is_visitor(session())) r.note("visitor: the placement only went to the host (0x214974); the local grid may not hold the object yet");
}

void discovery_frame(::Ctx*, bool) {
    const double now = qpc::seconds();
    if (now - g_flushed < kFlushEvery) return;
    g_flushed = now;
    const std::vector<std::string> rep = g_recipes.grown([](uint64_t k) {
        return journal::fmt("recipe=%u from=main+0x%llx known=%u", static_cast<unsigned>(k >> 48), static_cast<unsigned long long>(k >> 8 & 0xffffff), static_cast<unsigned>(k & 1));
    });
    if (!rep.empty()) journal::list(journal::Area::Discovery, "recipe-learn-repeats", rep);
}

void discovery_reset() {
    g_recipes.clear();
    g_flushed = 0;
}

}  // namespace nxrt::games::pkp::journal::services
