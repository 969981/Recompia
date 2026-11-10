// SPDX-License-Identifier: GPL-3.0-or-later
// The pure part of the in-game Options "Time" and "Events" tabs (pkp_options.cpp): the event list file, its order and its text, and the
// calendar arithmetic of the date rows. No game or runtime calls in here, so it can be read and tested without a running game.
#pragma once
#include <string>
#include <vector>

namespace nxrt::games::pkp {

// One game event from pokopia_events.tsv. Dates are y * 10000 + m * 100 + d.
struct TimeEvent {
    int start = 0, end = 0;
    std::string name;
};

// The file's lines `start<TAB>end<TAB>name` (ISO dates 2026-11-16; blank lines and lines starting with # are skipped; a bad line is
// skipped). Ordered for the list: events that run now or come next first, soonest first, then the ones that are over, latest first.
// `today` is y * 10000 + m * 100 + d; at most `limit` events are kept.
std::vector<TimeEvent> parse_events(const std::string& text, int today, size_t limit);

// "16 Nov to 1 Dec 2026", or "16 Nov 2026" for a one-day event; "Now: " in front while `today` falls inside it.
std::string event_dates(const TimeEvent& e, int today);

int days_in_month(int year, int month);

// The Time tab's date rows keep a position per row; these turn a position into the value and back, clamped to the row's range.
constexpr int kFirstYear = 2025, kYearCount = 16;
inline int year_of(int pos) { return kFirstYear + pos; }
int clamp_pos(int pos, int count);

}  // namespace nxrt::games::pkp
