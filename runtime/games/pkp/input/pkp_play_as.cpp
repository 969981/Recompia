// SPDX-License-Identifier: GPL-3.0-or-later
// Play as Pokemon: ZL + D-pad Up copies the Pokemon in front of the player (turns into it and remembers it), ZL + D-pad Left turns the
// player into the chosen Pokemon and keeps the form, ZL + D-pad Right turns back; a Pokemon that flies takes off and rises with ZR and
// sinks and lands with ZL. [pkp] play_as_pokemon (on by default: the buttons work, and nothing changes until they are pressed),
// [pkp] play_as_pokemon_form (the chosen form's character number, Ditto by default; read at each press, set by a copy and by the
// Play as tab of the game's Settings screen, which lists the copied Pokemon: pkp_play_as_list.h).
//
// A port of the Play as Pokemon patch of cycalo/Pokopia-Editor (GPL-3.0, mods/play-as-pokemon) to site hooks. The patch keeps "the form
// held" (0 = none) in a spare word of the game's bss, branches from eight instructions of main into code caves that read it, and has a
// cheat file write it on the button combos. Here the word is State::form, the caves are the hooks below and the cheat is the two frame
// callbacks.
//
// What the game does (main 2.0.1a, read from the patch and the code around its sites):
//  * A player's form is a character number (layout::kFormHuman = 1 the human form, 70 Ditto). The form the player is in is the u16 at
//    player + layout::kFormNow; the form asked for is the u16 at player + layout::kFormWanted, read and written through two virtuals:
//    0x71006671b0 (x0 = player -> w0) and 0x71006671bc (x0 = player, w1 = form).
//  * A change of form is asked for through the request block at player + layout::kFormRequest. The game's own writer is 0x71002f14e0
//    (x0 = the block, w1 = form, w2 = the go flag, w5 = the motion played for the change): it stores w1 at +0x10, w5 at +0x12 and w2 at
//    +0x14. The game calls it with form 1 to bring the player back to the human form.
//  * A player's ground state is the block at player + layout::kGroundState, a state machine (state at +0xaf4, its function at +0xb00).
//    0x7100288b3c runs it for each of the player manager's four players: 0x71002de8d4 (x0 = block, x1 = player) calls the update
//    0x71004050a4, and the block's state function 0x7100407edc calls 0x710040a12c. Both store the block's flag word at +0xbe2. Bit 16
//    of it is what the patch sets: its notes call it "in a small space", where the game lets the player be Ditto.
//  * The idle motion calls the check 0x71006d5374 (x0 = its context; the u16 at [x0 + 8] + 0x1c is the actor's index, which the
//    patch takes to be 0 for the local player) at 0x71006f232c; the patch's notes: "something is in the way", and when the check
//    does not pass Ditto's idle starts its stand-up motion.
//  * Motions. A character's table row names the character number whose motions it plays (u16 at row + 0xd8); the character setup
//    0x710027cad0 (x0 = character) stores it in the character at 0x710027cbd0. The motions of a number come in a motion pack
//    (layout::kMotionPacks: the pack id of a number, and per pack a count of requests and of loads) and are played through the
//    number's motion set (layout::kMotionSets). The patch's notes: the game loads a Pokemon's motions only while it lives in the town,
//    and a model without motions is drawn as nothing. The functions: 0x71008d20a0 (packs, id) requests a pack, 0x71008d2420 (packs,
//    id, flush) gives the request back, 0x71008d1130 (packs, id) says whether it is requested and loaded, 0x71008d11c0 (-, number)
//    adds a user to the number's set (the first one binds its motions, when the set is loaded) and 0x71008d1380 (-, number, all)
//    takes one away (the last one unbinds them).
//  * Flying. A Pokemon that hovers or flies in the game has a hover motion: motion 7 of its motion set, of kind 34 (the first byte
//    of the motion's data). The patch's notes: the game applies no gravity while that motion plays. A movement component plays the
//    motion at +layout::kMoveMotion; its controller rises with the speed at +layout::kControllerRise.
//
// The hooks (all need their sites in recomp/games/pkp/main.sites), each acting only while a form is held and only on the local player
// ([main + layout::kPlayerManager] + layout::kManagerPlayer); the patch does not look at whose block it is, so there the manager's
// other players get them too:
//  * entry 0x71006671b0: answers the form held. Entry 0x71006671bc: stores the form held instead of w1.
//  * inline 0x7100405178 (x20 = the ground state, w10 = the flags about to be stored) and inline 0x710040a388 (x19 = the ground state,
//    w9 = the flags): set bit 16.
//  * entry 0x71002f14e0: drops a request for the human form.
//  * entry 0x71006d5374, for the call from 0x71006f232c and actor 0: answers 1.
//  * inline 0x710027cbd0 (x19 = character, w8 = the number whose motions it plays): while a form is held that number's pack is
//    requested; when the number changes or the form is let go, the pack and the set are given back.
//  * inline 0x71002face0, in the player's virtual 0x71002faca0, the patch's every-frame point (x0 = player): once the requested pack
//    is loaded the number's set gets its user. When that user is the set's first, the motions were not bound when the game changed
//    the form, so the change is asked for again from the human form (the patch's test for "first" is w8 == 1 after 0x71008d11c0,
//    the user count that function leaves there; here the count is read before the call). Then the fly buttons, for a form with a
//    hover motion, on the local player's movement component: ZR alone starts the hover motion and, while it plays, rises (speed 7);
//    ZL alone sinks (speed -7) and, once the height has changed by less than 0.5 for 4 calls in a row, lands: motion 1 (the
//    patch's notes: the idle, from which the game does its own fall and landing) and a rise of 4 for the next 3 calls; ZR + ZL
//    drops out of the air (motion 1 at once); neither hovers in place. With a D-pad direction held the buttons are left alone
//    (ZL + D-pad are this module's combos). The counts are per call, as in the patch.
// Turning into the form and back is asked for as the patch's cheat does it: the wanted form, then the request block's motion (0x3b2),
// go flag and form, the three stores of the game's own writer.
//
// Copying is not in the patch. The Pokemon in the world are the actors of the pool (layout::kActorPool); each is a character, so it
// has a position, says its character number through the virtual every character has (layout::kCharacterNumber: the number its table
// row and its motions are looked up with, and a player's form) and carries the id of its species name (layout::kActorSpecies). The
// copy takes the nearest one within kCopyReach of the player, one behind the player counting as twice as far, turns the player into
// its number and remembers number and species.
//
// Link Play: the form is not turned on in a session, and a session that starts while it is held turns it back (layout::kOnline), so
// no form is held in a session.
#include "input/pkp_play_as.h"
#include "input/pkp_play_as_list.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"

#include <atomic>
#include <cmath>
#include <string>

#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/notices.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kFormGet = 0x6671b0, kFormSet = 0x6671bc;       // the form virtuals (x0 = player)
constexpr uint64_t kGroundStore = 0x405178, kGroundStore2 = 0x40a388;   // the ground state's flag stores (x20 / w10, x19 / w9)
constexpr uint32_t kGroundKeepForm = 0x10000;                      // ground state flags: the player keeps Ditto's form
constexpr uint64_t kSetRequest = 0x2f14e0;                         // (request block, form, go, ..., w5 = motion): the block's writer
constexpr uint64_t kObstacleCheck = 0x6d5374;                      // (context) -> 1: the idle's check passes
constexpr uint64_t kIdleCall = 0x6f2330;                           // return address of the idle's call of kObstacleCheck
constexpr uint64_t kCheckActor = 0x8, kActorIndex = 0x1c;          // context: its actor (pointer); actor: its index (u16)
constexpr uint16_t kChangeMotion = 0x3b2;                          // the motion the patch asks for with a change
constexpr uint64_t kMotionStore = 0x27cbd0;                        // the character setup's store of the motions' number (x19 / w8)
constexpr uint64_t kPlayerTick = 0x2face0;                         // the player's every-frame virtual, at its virtual +0x200 call (x0)
constexpr uint64_t kPackRequest = 0x8d20a0, kPackRelease = 0x8d2420, kPackReady = 0x8d1130;   // (packs, id[, flush])
constexpr uint64_t kSetUse = 0x8d11c0, kSetRelease = 0x8d1380;     // (-, number[, all])
constexpr uint32_t kHoverMotion = 7, kRestMotion = 1;              // motion set: the hover motion's place, and the motion landing goes to
constexpr float kCopyReach = 600.0f;                               // world units (6 blocks) to the Pokemon a copy takes
constexpr int kHoverKind = 34;                                     // a motion's kind: hover
constexpr float kRiseSpeed = 7.0f, kLandSpeed = 4.0f;              // controller rise: flying up (down: the negative), the calls after landing
constexpr float kStill = 0.5f;                                     // sinking moved the height by less than this: on the ground
constexpr uint8_t kStillCalls = 4, kLandCalls = 3;

using guest::rd;
using guest::wr;

enum Ask : int { kNone = 0, kTurn, kBack, kCopy };

struct State {
    uint64_t base = 0;
    std::atomic<uint16_t> form{0};      // the form held (0: none); the hooks read it on the game's threads
    std::atomic<int> ask{kNone};        // main thread -> game thread: an Ask
    uint64_t prev_buttons = 0;          // main thread: the pads' buttons last frame
    uint16_t motions = 0;               // the two motion hooks: the number whose pack this module requested (0: none)
    uint16_t bound = 0;                 // the two motion hooks: the number whose set this module is a user of (0: none)
    float sink_y = 0;                   // the player's tick: the height at the last sinking call
    uint8_t still = 0;                  // the player's tick: sinking calls in a row that did not move the height
    uint8_t landing = 0;                // the player's tick: calls left that rise with kLandSpeed
};
State g;

uint64_t local_player() {
    const uint64_t mgr = rd<uint64_t>(g.base + layout::kPlayerManager);
    return guest::ok(mgr + layout::kManagerPlayer, 8) ? rd<uint64_t>(mgr + layout::kManagerPlayer) : 0;
}

// The form held when `at` is the local player's block at `off` (the player itself: 0), else 0.
uint16_t held_at(uint64_t at, uint64_t off = 0) {
    const uint16_t form = g.form.load(std::memory_order_relaxed);
    if (!form) return 0;
    const uint64_t player = local_player();
    return player && at == player + off ? form : 0;
}

int hook_form_get(::Ctx* c, uint64_t, void*) {
    const uint16_t form = held_at(c->x[0]);
    if (!form) return 0;
    c->x[0] = form;
    return 1;
}

int hook_form_set(::Ctx* c, uint64_t, void*) {
    if (const uint16_t form = held_at(c->x[0])) c->x[1] = form;
    return 0;
}

void hook_ground_store(::Ctx* c, uint64_t, void*) {
    if (held_at(c->x[20], layout::kGroundState)) c->x[10] |= kGroundKeepForm;
}

void hook_ground_store2(::Ctx* c, uint64_t, void*) {
    if (held_at(c->x[19], layout::kGroundState)) c->x[9] |= kGroundKeepForm;
}

int hook_set_request(::Ctx* c, uint64_t, void*) {
    return held_at(c->x[0], layout::kFormRequest) && static_cast<uint32_t>(c->x[1]) == layout::kFormHuman;
}

int hook_obstacle_check(::Ctx* c, uint64_t, void*) {
    if (c->x[30] != g.base + kIdleCall || !g.form.load(std::memory_order_relaxed)) return 0;
    const uint64_t context = c->x[0];
    if (!guest::ok(context + kCheckActor, 8)) return 0;
    const uint64_t actor = rd<uint64_t>(context + kCheckActor);
    if (!guest::ok(actor + kActorIndex, 2) || rd<uint16_t>(actor + kActorIndex) != 0) return 0;
    c->x[0] = 1;
    return 1;
}

// The request block's three fields: the game changes the player into `to`.
void request(uint64_t player, uint16_t to) {
    const uint64_t block = player + layout::kFormRequest;
    wr<uint16_t>(block + layout::kRequestMotion, kChangeMotion);
    wr<uint8_t>(block + layout::kRequestGo, 1);
    wr<uint16_t>(block + layout::kRequestForm, to);
}

// Holds `hold` (0: nothing) and asks the game to change the local player into `to`. False while there is no player (not in the world).
bool change(uint16_t hold, uint16_t to) {
    const uint64_t player = local_player();
    if (!guest::ok(player, layout::kFormWanted + 2)) return false;
    g.form.store(hold, std::memory_order_relaxed);
    wr<uint16_t>(player + layout::kFormWanted, to);
    request(player, to);
    return true;
}

// ---- motions ----
uint64_t motion_packs() {
    const uint64_t owner = rd<uint64_t>(g.base + layout::kPackOwner);
    return guest::ok(owner + layout::kMotionPacks, 8) ? owner + layout::kMotionPacks : 0;
}

// The pack that holds the motions of character `number` (0: none).
uint16_t pack_of(uint64_t packs, uint16_t number) {
    const uint64_t at = packs + layout::kPackOfCharacter + uint64_t{number} * 2;
    return guest::ok(at, 2) ? rd<uint16_t>(at) : 0;
}

// The motion set of character `number` (0: none): the list walk of 0x71008d11c0.
uint64_t motion_set(uint16_t number) {
    const uint64_t list = rd<uint64_t>(g.base + layout::kMotionSets);
    if (!guest::ok(list, layout::kSetsUsed + 2)) return 0;
    const uint32_t slots = rd<uint16_t>(list + layout::kSetsSlots), used = rd<uint16_t>(list + layout::kSetsUsed);
    const uint64_t entries = rd<uint64_t>(list + layout::kSetsEntries);
    if (!slots || !used || !guest::ok(entries, uint64_t{slots} * 8)) return 0;
    for (uint32_t i = 0, seen = 0; i < slots && seen < used; ++i) {
        const uint64_t entry = rd<uint64_t>(entries + uint64_t{i} * 8);
        if (!guest::ok(entry, layout::kSetEntryNumber + 4)) return 0;
        const int32_t of = rd<int32_t>(entry + layout::kSetEntryNumber);
        if (of < 0) continue;   // a free slot
        if (of == number) return rd<uint64_t>(entry);
        ++seen;
    }
    return 0;
}

void hook_motion_store(::Ctx* c, uint64_t, void*) {
    if (!c->x[19] || c->x[19] != local_player()) return;
    const uint16_t want = g.form.load(std::memory_order_relaxed) ? static_cast<uint16_t>(c->x[8]) : 0;
    const uint16_t old = g.motions;
    if (old == want) return;
    g.motions = want;
    if (old) {
        if (g.bound == old) {
            g.bound = 0;
            guest::call(c, g.base + kSetRelease, 0, old, 0);
        }
        const uint64_t packs = motion_packs();
        if (!packs) return;
        guest::call(c, g.base + kPackRelease, packs, pack_of(packs, old), 0);
    }
    if (!want) return;
    if (const uint64_t packs = motion_packs()) guest::call(c, g.base + kPackRequest, packs, pack_of(packs, want));
}

// Once the pack this module requested is loaded: a user for the number's set, and the change again when the motions were not bound.
void bind_motions(::Ctx* c, uint64_t player) {
    const uint16_t number = g.motions;
    if (!number || number == g.bound) return;
    const uint64_t packs = motion_packs();
    if (!packs) return;
    const uint16_t pack = pack_of(packs, number);
    if (pack && !static_cast<uint32_t>(guest::call(c, g.base + kPackReady, packs, pack))) return;   // still loading
    const uint64_t set = motion_set(number);
    const bool first = guest::ok(set, layout::kSetLoaded + 1) && rd<uint8_t>(set + layout::kSetLoaded) && rd<uint16_t>(set + layout::kSetUsers) == 0;
    guest::call(c, g.base + kSetUse, 0, number);
    g.bound = number;
    const uint16_t form = g.form.load(std::memory_order_relaxed);
    if (!first || !form || !guest::ok(player, layout::kFormWanted + 2)) return;
    wr<uint16_t>(player + layout::kFormNow, layout::kFormHuman);
    request(player, form);
    NXLOG_I(kTag, "play as Pokemon: the motions of %u are loaded, the change to form %u is asked for again", number, form);
}

// ---- flying ----
// A motion's kind, -1 when there is no motion or it has no data.
int motion_kind(uint64_t motion) {
    if (!guest::ok(motion + layout::kMotionData, 8)) return -1;
    const uint64_t data = rd<uint64_t>(motion + layout::kMotionData);
    return guest::ok(data, 1) ? rd<uint8_t>(data) : -1;
}

void fly() {
    if (!g.form.load(std::memory_order_relaxed)) return;
    const uint64_t owner = rd<uint64_t>(g.base + layout::kMovements);
    if (!guest::ok(owner + layout::kMovementSlots, 8)) return;
    const uint64_t move = rd<uint64_t>(owner + layout::kMovementSlots);
    if (!guest::ok(move, layout::kController + layout::kControllerY + 4)) return;
    const uint64_t set = rd<uint64_t>(move + layout::kMoveMotions);
    if (!guest::ok(set, layout::kSetMotionCount + 4) || rd<uint32_t>(set + layout::kSetMotionCount) <= kHoverMotion) return;
    const uint64_t motions = rd<uint64_t>(set + layout::kSetMotions);
    if (!guest::ok(motions, (kHoverMotion + 1) * 8)) return;
    const uint64_t hover = rd<uint64_t>(motions + uint64_t{kHoverMotion} * 8);
    if (motion_kind(hover) != kHoverKind) return;   // not a Pokemon that flies
    const uint64_t rise = move + layout::kController + layout::kControllerRise;
    if (g.landing) {
        --g.landing;
        wr<float>(rise, kLandSpeed);
        return;
    }
    const uint64_t buttons = hid::sampled_buttons();
    const bool zr = buttons & hid::btn(hid::kBtnZR), zl = buttons & hid::btn(hid::kBtnZL);
    if (buttons & (hid::btn(hid::kBtnLeft) | hid::btn(hid::kBtnUp) | hid::btn(hid::kBtnRight) | hid::btn(hid::kBtnDown))) {
        g.still = 0;
        return;
    }
    const int playing = motion_kind(rd<uint64_t>(move + layout::kMoveMotion));
    if (playing < 0) return;
    if (playing != kHoverKind) {   // on the ground: ZR alone takes off
        if (zr && !zl) wr<uint64_t>(move + layout::kMoveMotion, hover);
        g.still = 0;
        return;
    }
    const uint64_t rest = rd<uint64_t>(motions + uint64_t{kRestMotion} * 8);
    if (zr && zl) {   // drop out of the air
        if (rest) wr<uint64_t>(move + layout::kMoveMotion, rest);
        g.still = 0;
        return;
    }
    if (zr || !zl) {   // rise, or hover in place
        if (zr) wr<float>(rise, kRiseSpeed);
        g.still = 0;
        return;
    }
    wr<float>(rise, -kRiseSpeed);
    const float y = rd<float>(move + layout::kController + layout::kControllerY);
    const bool moved = std::fabs(y - g.sink_y) >= kStill;
    g.sink_y = y;
    if (moved) {
        g.still = 0;
        return;
    }
    if (++g.still < kStillCalls) return;
    g.still = 0;
    if (!rest) return;
    wr<uint64_t>(move + layout::kMoveMotion, rest);   // on the ground: land
    g.landing = kLandCalls;
}

void hook_player_tick(::Ctx* c, uint64_t, void*) {
    const uint64_t player = c->x[0];
    if (!player || player != local_player()) return;
    bind_motions(c, player);
    fly();
}

void install(const FeatureInstall& in) {
    if (!settings::get_bool(settings::Key::PlayAsPokemon)) return;
    const uint64_t base = in.base;
    g.base = base;
    play_as::start(base);
    in.hooks.add_entry(base + kFormGet, &hook_form_get);
    in.hooks.add_entry(base + kFormSet, &hook_form_set);
    in.hooks.add_inline(base + kGroundStore, &hook_ground_store);
    in.hooks.add_inline(base + kGroundStore2, &hook_ground_store2);
    in.hooks.add_entry(base + kSetRequest, &hook_set_request);
    in.hooks.add_entry(base + kObstacleCheck, &hook_obstacle_check);
    in.hooks.add_inline(base + kMotionStore, &hook_motion_store);
    in.hooks.add_inline(base + kPlayerTick, &hook_player_tick);
    NXLOG_I(kTag, "play as Pokemon: on (ZL + D-pad Up copies the Pokemon in front, ZL + D-pad Left turns the player into form %lld, ZL + D-pad Right "
                  "turns back; [pkp] play_as_pokemon, play_as_pokemon_form)",
            static_cast<long long>(settings::get_int(settings::Key::PlayAsPokemonForm)));
}

void remove() {
    play_as::stop();
    g.base = 0;
    g.form.store(0);
    g.ask.store(kNone);
    g.prev_buttons = 0;
    g.motions = 0;
    g.bound = 0;
    g.sink_y = 0;
    g.still = 0;
    g.landing = 0;
}

// Main thread, once per frame: the two button combos, on the press that completes them, while the game is in play.
void frame(bool in_game, double, play::GameFrame*) {
    if (!g.base) return;
    const uint64_t buttons = hid::sampled_buttons();
    const uint64_t zl = hid::btn(hid::kBtnZL), turn = zl | hid::btn(hid::kBtnLeft), back = zl | hid::btn(hid::kBtnRight), copy = zl | hid::btn(hid::kBtnUp);
    const auto pressed = [&](uint64_t combo) { return (buttons & combo) == combo && (g.prev_buttons & combo) != combo; };
    if (in_game) {
        if (pressed(turn)) g.ask.store(kTurn, std::memory_order_release);
        else if (pressed(back)) g.ask.store(kBack, std::memory_order_release);
        else if (pressed(copy)) g.ask.store(kCopy, std::memory_order_release);
    }
    g.prev_buttons = buttons;
}

// ---- copying ----
// The Pokemon a copy takes (number 0: none in reach).
play_as::Copied copy_target(::Ctx* c) {
    play_as::Copied best;
    const uint64_t player = local_player();
    const uint64_t holder = rd<uint64_t>(g.base + layout::kActorPool);
    const uint64_t pool = guest::ok(holder, 8) ? rd<uint64_t>(holder) : 0;
    if (!guest::ok(player, layout::kFormWanted + 2) || !guest::ok(pool + layout::kPoolSlots, layout::kPoolSize * 8)) return best;
    const float px = rd<float>(player + layout::kPosition), pz = rd<float>(player + layout::kPosition + 8);
    const float heading = rd<float>(player + layout::kHeading), fx = std::sin(heading), fz = std::cos(heading);
    float nearest = kCopyReach;
    for (uint32_t i = 0; i < layout::kPoolSize; ++i) {
        const uint64_t actor = rd<uint64_t>(pool + layout::kPoolSlots + uint64_t{i} * 8);
        if (actor == player || !guest::ok(actor, layout::kActorSpecies + 2)) continue;
        const float dx = rd<float>(actor + layout::kPosition) - px, dz = rd<float>(actor + layout::kPosition + 8) - pz;
        float far = std::hypot(dx, dz);
        if (dx * fx + dz * fz < 0) far *= 2;   // behind the player
        if (!(far < nearest)) continue;
        const uint64_t vtable = rd<uint64_t>(actor);
        if (!guest::ok(vtable + layout::kCharacterNumber, 8)) continue;
        const uint64_t number = guest::call(c, rd<uint64_t>(vtable + layout::kCharacterNumber), actor) & 0xffff;
        if (number <= layout::kFormHuman || number > layout::kFormMax) continue;
        nearest = far;
        best = {static_cast<uint16_t>(number), rd<uint16_t>(actor + layout::kActorSpecies)};
    }
    return best;
}

// Turns the player into the Pokemon in front and makes it the chosen form.
void copy(::Ctx* c) {
    const play_as::Copied target = copy_target(c);
    if (!target.number) {
        notices::post("pkp.play_as", "No Pokémon close enough to copy", 2.0);
        return;
    }
    if (!change(target.number, target.number)) return;
    play_as::remember(target);
    settings::set_int(settings::Key::PlayAsPokemonForm, target.number);
    const std::string name = play_as::species_name(c, target.species);
    notices::post("pkp.play_as", name.empty() ? "Copied" : "Copied: " + name, 2.5);
    NXLOG_I(kTag, "play as Pokemon: copied form %u (species %u, %s)", target.number, target.species, name.c_str());
}

// The game thread, once a frame: what the main thread asked for, and the turn back when a Link Play session starts.
void game_frame(::Ctx* c) {
    if (!g.base) return;
    const int ask = g.ask.exchange(kNone, std::memory_order_acq_rel);
    const bool held = g.form.load(std::memory_order_relaxed) != 0;
    if (ask == kNone && !held) return;
    const bool online = guest::call(c, g.base + layout::kOnline, 0) & 1;
    if (ask == kTurn && !online) {
        const auto form = static_cast<uint16_t>(settings::get_int(settings::Key::PlayAsPokemonForm));
        if (change(form, form)) NXLOG_I(kTag, "play as Pokemon: the player turns into form %u and keeps it", form);
    } else if (ask == kCopy && !online) {
        copy(c);
    } else if (ask == kTurn || ask == kCopy) {
        notices::post("pkp.play_as", "Not during Link Play", 2.0);
    } else if (held && (ask == kBack || online)) {
        if (change(0, layout::kFormHuman)) NXLOG_I(kTag, "play as Pokemon: the player turns back%s", online ? " (a Link Play session started)" : "");
    }
}

}  // namespace

const Feature kPlayAsFeature = {.name = "play_as", .install = &install, .remove = &remove, .frame = &frame, .game_frame = &game_frame};

}  // namespace nxrt::games::pkp
