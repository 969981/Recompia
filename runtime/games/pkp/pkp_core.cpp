// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, the core part of the game module (pkp_game.h): the data part plus the title facts. Needs the runtime library,
// not the graphics one.
#include "pkp_game.h"

#include "nxrt/services.h"
#include "nxrt/tt.h"

namespace nxrt::tt::pkp {
const GameProbes& pkp_game_probes();      // pkp_tt_probe.cpp
}

namespace nxrt::games::pkp {

namespace {

// main.npdm of the 2.0.1 update; save sizes are placeholders until control.nacp is known (the boot log shows what the game asks for).
void title_defaults(services::TitleProfile& p) {
    p.program_id = data_info().program_id;
    p.name = data_info().title;
    p.display_version = data_info().version;
    p.supported_languages = data_info().languages;   // control.nacp SupportedLanguage (pkp_data.cpp)
    p.account_save_size = 0x4000000;
    p.account_save_journal = 0x4000000;
    p.device_save_size = 0x4000000;
    p.device_save_journal = 0x4000000;
    p.main_thread_stack = 0x100000;            // main.npdm
    p.main_thread_priority = 44;
    p.main_thread_core = 0;
    p.firmware = "20.5.0";                     // the 2.0.1 update is built with SDK 20.5.25
    p.user_nickname = "Player";
    p.user_uid_hi = 0x0000000000000001ull;
    p.user_uid_lo = 0x41434e4852656370ull;     // "PKPRecmp"
}

}  // namespace

const game::Info& core_info() {
    static const game::Info info = [] {
        game::Info g = data_info();
        g.title_defaults = &title_defaults;
        g.probes = &tt::pkp::pkp_game_probes();
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::pkp

void nxrt_register_game_pkp_core() { nxrt::game::set_current(&nxrt::games::pkp::core_info()); }
