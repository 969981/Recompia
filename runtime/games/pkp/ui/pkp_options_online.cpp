// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options, the Online tab (pkp_options_model.h): the switch for the launcher's online sharing ([pkp] online_presence), and
// Online events ([pkp] online_events, read by the network services at start through game::Info::report_online). online_presence: the
// runtime does not read the key and has no network code: the launcher (launcher/src-tauri/src/online.rs) reads it every few seconds while
// the game runs. The online name and the reset of the online ID stay in the launcher. A plain table of rows, like the other PC tabs.
#include "ui/pkp_options_model.h"

namespace nxrt::games::pkp::options_detail {
namespace {

// clang-format off
constexpr Row kOnline[] = {
    key_row("Share that I am online", "While the game runs, the launcher tells the Recompia online server that you are playing: your online name from the launcher (empty: anonymous), the version, Windows or Linux, and how many Pokemon have a home in your save. Never your save or your user ID. Off unless you switch it on.", settings::Key::OnlinePresence),
    key_row("Online events", "Tells the game it has an internet connection, so events that need one the first time, such as Apples to Applin, can start. Nothing is sent, and the game's other online services still do not work.", settings::Key::OnlineEvents),
};
// clang-format on

StaticTab g_online{rows_of(kOnline)};

}  // namespace

TabModel& online_tab() { return g_online; }

}  // namespace nxrt::games::pkp::options_detail
