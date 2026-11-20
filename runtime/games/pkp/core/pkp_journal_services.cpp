// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's services watcher (pkp_journal_watch.h): creature work and NPC services, Link Play's request queue,
// requests, the shop's stock and the discovery setters. The hooks and polls live in pkp_journal_services_{queue,work,requests,shop,
// discovery}.cpp (one job each, sharing pkp_journal_services_internal.h); this file is the table that registers them. Every site's
// word is the first instruction of its function (a midhook's: the instruction it runs before), checked on its own at start.
#include "core/pkp_journal_services_internal.h"

namespace nxrt::games::pkp::journal {

namespace {

using namespace services;

constexpr WatchSite kSites[] = {
    // Link Play's request queue
    {{0x2148a0, 0xb0019a68u, true}, &on_queue_receive, "queue-receive 0x2148a0"},   // adrp x8, 0x3561000
    {{0x214974, kPacibsp, true}, &on_queue_send, "queue-send 0x214974"},
    {{0x2159c0, 0x90019a68u, true}, &on_record_send, "talk-end-send 0x2159c0"},      // adrp x8, 0x3561000 (co-op's site too)
    {{0x2197a0, kPacibsp, true}, &on_host_apply, "talk-end-apply 0x2197a0"},
    // creature work
    {{0x480b60, kPacibsp, true}, &on_talk_end, "talk-end 0x480b60"},
    {{0x48fdac, 0x79400008u, true}, &on_service_offer, "service-offer 0x48fdac"},    // ldrh w8, [x0]
    {{0x4984a0, 0x79400008u, true}, &on_service_start, "service-start 0x4984a0"},    // ldrh w8, [x0]
    {{0x34dc64, kPacibsp, true}, &on_job_start, "job-start 0x34dc64"},
    // requests
    {{0x629004, 0x39002115u, false}, &on_general_state, "general-state 0x629004"},   // strb w21, [x8, #8]
    {{0x6290ac, 0x39002115u, false}, &on_general_state, "general-state 0x6290ac"},   // strb w21, [x8, #8] (stage 0x11 copy)
    {{0x61858c, 0xb4000040u, false}, &on_goal_unique, "goal-count 0x61858c"},        // cbz x0, 0x618594
    {{0x621be8, 0xb4000040u, false}, &on_goal_general, "goal-count 0x621be8"},       // cbz x0, 0x621bf0
    {{0x619410, kPacibsp, true}, &on_friendship, "friendship 0x619410"},
    // the shop
    {{0x646864, kPacibsp, true}, &on_stock_get, "stock-get 0x646864"},
    {{0x646ae8, kPacibsp, true}, &on_stock_set, "stock-set 0x646ae8"},
    {{0xe889f8, kPacibsp, true}, &on_shop_grant, "shop-grant 0xe889f8"},
    {{0xe87260, kPacibsp, true}, &on_offline_row, "shop-row 0xe87260"},
    // discovery setters
    {{0xa982b0, 0x51000408u, true}, &on_dex_bit0, "dex-bit0 0xa982b0"},              // sub w8, w0, #1
    {{0xa983d4, kPacibsp, true}, &on_dex_bit1, "dex-bit1 0xa983d4"},
    {{0xa984e4, kPacibsp, true}, &on_dex_bit2, "dex-bit2 0xa984e4"},
    {{0xa985c8, kPacibsp, true}, &on_dex_bit3, "dex-bit3 0xa985c8"},
    {{0xa96f5c, kPacibsp, true}, &on_habitat_found, "habitat-found 0xa96f5c"},
    {{0xa9802c, kPacibsp, true}, &on_habitat_scene, "habitat-scene 0xa9802c"},
    {{0x2e7240, kPacibsp, true}, &on_recipe_learn, "recipe-learn 0x2e7240"},
    {{0x3bcca8, kPacibsp, true}, &on_move_learn, "move-learn 0x3bcca8"},
    {{0x2f5860, kPacibsp, true}, &on_emote_learn, "emote-learn 0x2f5860"},
    {{0x5c04d4, 0x37380160u, true}, &on_building_set, "building-set 0x5c04d4"},      // tbnz w0, #7, 0x5c0500
    // wild creature appearance, placement notice
    {{0x1240dc, kPacibsp, true}, &on_wild_pop, "wild-pop 0x1240dc"},
    {{0x13596c, 0x12003c29u, true}, &on_placed_notice, "placement-notice 0x13596c"},   // and w9, w1, #0xffff
};

void game_frame(::Ctx* c, bool in_game) {
    queue_frame(c, in_game);
    requests_frame(c, in_game);
    shop_frame(c, in_game);
    discovery_frame(c, in_game);
}

void reset() {
    queue_reset();
    requests_reset();
    shop_reset();
    discovery_reset();
}

}  // namespace

const Watcher kServicesWatcher = {"services", kSites, sizeof kSites / sizeof kSites[0], &game_frame, &reset};

}  // namespace nxrt::games::pkp::journal
