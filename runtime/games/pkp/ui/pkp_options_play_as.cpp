// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options, the Play as tab (pkp_options_model.h): the Play as Pokemon mod's switch, the Pokemon to turn into and the row that
// removes one from the list (input/pkp_play_as.cpp, [pkp] play_as_pokemon and play_as_pokemon_form). The Pokemon row picks from the
// ones the player has copied (input/pkp_play_as_list.h, at most play_as::kMost), named by the game in its language; a form set
// another way (the ini) that is not among them is the row's first position, shown by its number. The list is read when the screen
// opens, and the key takes the pick when the screen commits. With Remove set, the commit takes the Pokemon shown out of the list
// instead, and when it was the chosen form the first of the rest (Ditto when none is left) becomes it. The mod reads the key
// at each press of its buttons, so a new pick needs no restart; the switch does. The screen's hooks run on the game thread, so the
// names are asked for with that thread's Ctx (kernel::current_ctx), as the list asks.
#include "ui/pkp_options_model.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "core/pkp_settings.h"
#include "input/pkp_play_as_list.h"
#include "nxrt/kernel.h"

namespace nxrt::games::pkp::options_detail {
namespace {

using settings::Key;

constexpr int kSwitchRow = 0, kPickRow = 1, kRemoveRow = 2, kPlayAsRows = 3;
constexpr uint16_t kDitto = 70;
constexpr const char* kKeepRemove[4] = {"Keep", "Remove"};

std::vector<play_as::Copied> g_picks;       // the Pokemon row's positions
std::string g_labels[play_as::kMost + 1];   // their texts
Row g_play_as_rows[kPlayAsRows];            // filled by PlayAsTab::open

int pick_count() { return static_cast<int>(std::min(g_picks.size(), std::size(g_labels))); }
int pick_max() { return std::max(pick_count() - 1, 0); }
const char* pick_text(int v, char*, size_t) { return v >= 0 && v < pick_count() ? g_labels[v].c_str() : ""; }

// The position of the form in force.
int picked() {
    const int64_t form = settings::get_int(Key::PlayAsPokemonForm);
    for (int i = 0; i < pick_count(); ++i)
        if (g_picks[i].number == form) return i;
    return 0;
}

void load_picks(::Ctx* c) {
    const auto form = static_cast<uint16_t>(settings::get_int(Key::PlayAsPokemonForm));
    const std::vector<play_as::Copied>& copied = play_as::copied();
    g_picks.clear();
    if (std::none_of(copied.begin(), copied.end(), [&](const play_as::Copied& e) { return e.number == form; })) g_picks.push_back({form, 0});
    g_picks.insert(g_picks.end(), copied.begin(), copied.end());
    for (int i = 0; i < pick_count(); ++i) {
        g_labels[i] = play_as::species_name(c, g_picks[i].species);
        if (g_labels[i].empty()) g_labels[i] = "Number " + std::to_string(g_picks[i].number);
    }
}

class PlayAsTab final : public StaticTab {
public:
    PlayAsTab() : StaticTab(RowList{g_play_as_rows, kPlayAsRows}) {}
    void open() override {
        g_play_as_rows[kSwitchRow] = key_row("Play as Pokémon", "Experimental: ZL + D-pad Left turns you into the Pokémon below, ZL + D-pad Right turns you back.", Key::PlayAsPokemon);
        g_play_as_rows[kPickRow] = dynamic("Pokémon", "Copy a Pokémon with ZL + D-pad Up to add it here. Flying ones take off with ZR and land with ZL.", &pick_text, &pick_max,
                                           [](const S&) { return picked(); }, [](S&, int) {});
        g_play_as_rows[kRemoveRow] = choice("Remove from list", "Remove takes the Pokémon shown above out of your list when you save. The list has 20 places.", kKeepRemove, 2,
                                            [](const S&) { return 0; }, [](S&, int) {});
        load_picks(kernel::current_ctx());
        StaticTab::open();
    }
    // Nothing here is a play-mode setting, so the rows' own (empty) setters are not run. Remove goes back to Keep, as the Items tab's
    // count does, and the Pokemon row follows the list as it is now.
    void commit(Apply&) override {
        if (cur[kSwitchRow] != orig[kSwitchRow]) settings::set_bool(Key::PlayAsPokemon, cur[kSwitchRow] != 0);
        const int at = cur[kPickRow];
        if (at >= 0 && at < pick_count()) {
            const uint16_t number = g_picks[at].number;
            if (cur[kRemoveRow] == 1) {
                play_as::forget(number);
                if (settings::get_int(Key::PlayAsPokemonForm) == number) {
                    const std::vector<play_as::Copied>& rest = play_as::copied();
                    settings::set_int(Key::PlayAsPokemonForm, rest.empty() ? kDitto : rest.front().number);
                }
            } else if (at != orig[kPickRow]) {
                settings::set_int(Key::PlayAsPokemonForm, number);
            }
        }
        load_picks(kernel::current_ctx());
        cur[kPickRow] = picked();
        cur[kRemoveRow] = 0;
    }
};

PlayAsTab g_play_as;

}  // namespace

TabModel& play_as_tab() { return g_play_as; }

}  // namespace nxrt::games::pkp::options_detail
