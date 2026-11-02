// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: who the game is and what its files are called (nxrt/game.h Info). Pure data, no runtime: the launcher links
// this part alone.
#include "pkp_game.h"

namespace nxrt::games::pkp {

const game::Info& data_info() {
    static const game::Info info = [] {
        game::Info g;
        g.id = "pkp";
        g.title = "Pokémon Pokopia";
        g.product = "Pokémon Pokopia PC";
        g.short_title = "Pokopia";
        g.program_id = 0x0400A3A01EE4C000ull;
        g.version = "2.0.1";
        g.main_base = 0x7100000000ull;
        // control.nacp is not in the dump yet: every language for now (bits of services::Language)
        g.languages = 0x1ffffu;
        g.platform = game::Platform::Switch2;
        g.exe_name = "pkp.exe";
        g.setup_exe_name = "pkp_setup.exe";
        g.launcher_ini = "pkp_launcher.ini";
        g.game_ini = "pkp.ini";
        g.shader_pack = "shader_cache.pkpshc";
        g.save_folder = "PokopiaRecompPC";
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::pkp

void nxrt_register_game_pkp_data() { nxrt::game::set_current(&nxrt::games::pkp::data_info()); }
