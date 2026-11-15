// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options, the Play as tab (pkp_options_model.h): the Play as Pokemon mod's switch and its form (input/pkp_play_as.cpp,
// [pkp] play_as_pokemon and play_as_pokemon_form). The form is a character number from 2 to 403, more positions than a slider has
// (256), so two sliders hold it: the hundreds and the rest. The key takes the sum when the screen commits, clamped to its range. The
// mod reads the form at each press of its buttons, so a new form needs no restart; the switch does.
#include "ui/pkp_options_model.h"

#include <cstdio>

#include "core/pkp_settings.h"

namespace nxrt::games::pkp::options_detail {
namespace {

using settings::Key;

constexpr int kSwitchRow = 0, kHundredsRow = 1, kRestRow = 2;

int form() { return static_cast<int>(settings::get_int(Key::PlayAsPokemonForm)); }

const char* hundreds_text(int pos, char* out, size_t n) {
    std::snprintf(out, n, "%d", pos * 100);
    return out;
}
const char* rest_text(int pos, char* out, size_t n) {
    std::snprintf(out, n, "+%d", pos);
    return out;
}
int hundreds_max() { return static_cast<int>(settings::spec(Key::PlayAsPokemonForm).hi) / 100; }
int rest_max() { return 99; }

// clang-format off
constexpr Row kPlayAs[] = {
    key_row("Play as Pokémon", "Experimental: hold ZL and press Left on the D-pad to turn into the Pokémon below and stay that way; ZL and Right on the D-pad turns you back. Pokémon that fly take off with ZR and land with ZL. Off during Link Play.", Key::PlayAsPokemon),
    dynamic("Pokémon number: hundreds", "The number of the Pokémon you turn into is this plus the row below. 70 is Ditto. Numbers run from 2 to 403; not every number is a Pokémon.", &hundreds_text, &hundreds_max,
            [](const S&) { return form() / 100; }, [](S&, int) {}),
    dynamic("Pokémon number: rest", "Added to the hundreds above. The next ZL and Left on the D-pad uses the new number.", &rest_text, &rest_max,
            [](const S&) { return form() % 100; }, [](S&, int) {}),
};
// clang-format on

class PlayAsTab final : public StaticTab {
public:
    PlayAsTab() : StaticTab(rows_of(kPlayAs)) {}
    // The two number rows are one key, and nothing here is a play-mode setting, so the rows' own (empty) setters are not run.
    void commit(Apply&) override {
        if (cur[kSwitchRow] != orig[kSwitchRow]) settings::set_bool(Key::PlayAsPokemon, cur[kSwitchRow] != 0);
        if (cur[kHundredsRow] != orig[kHundredsRow] || cur[kRestRow] != orig[kRestRow])
            settings::set_int(Key::PlayAsPokemonForm, cur[kHundredsRow] * 100 + cur[kRestRow]);
    }
};

PlayAsTab g_play_as;

}  // namespace

TabModel& play_as_tab() { return g_play_as; }

}  // namespace nxrt::games::pkp::options_detail
