// SPDX-License-Identifier: GPL-3.0-or-later
// See pkp_time.h.
#include "pkp_time.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace nxrt::games::pkp {
namespace {

// "2026-11-16" -> 20261116; 0 when it is not a date.
int parse_date(const std::string& s) {
    int y = 0, m = 0, d = 0;
    char tail = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d%c", &y, &m, &d, &tail) != 3) return 0;
    if (y < 1970 || y > 2100 || m < 1 || m > 12 || d < 1 || d > days_in_month(y, m)) return 0;
    return y * 10000 + m * 100 + d;
}

const char* const kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string day_text(int date, bool with_year) {
    char buf[32];
    std::snprintf(buf, sizeof buf, with_year ? "%d %s %d" : "%d %s", date % 100, kMonths[(date / 100) % 100 - 1], date / 10000);
    return buf;
}

}  // namespace

int days_in_month(int year, int month) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 31;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return kDays[month - 1] + (month == 2 && leap ? 1 : 0);
}

int clamp_pos(int pos, int count) { return std::clamp(pos, 0, std::max(count - 1, 0)); }

std::vector<TimeEvent> parse_events(const std::string& text, int today, size_t limit) {
    std::vector<TimeEvent> all;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        TimeEvent e;
        e.start = parse_date(line.substr(0, a));
        e.end = parse_date(line.substr(a + 1, b - a - 1));
        e.name = line.substr(b + 1);
        if (!e.start || !e.end || e.end < e.start || e.name.empty()) continue;
        all.push_back(std::move(e));
    }
    // Over (end before today) after the others; among the others soonest start first, among the over ones latest first.
    std::stable_sort(all.begin(), all.end(), [today](const TimeEvent& x, const TimeEvent& y) {
        const bool xo = x.end < today, yo = y.end < today;
        if (xo != yo) return !xo;
        return xo ? x.start > y.start : x.start < y.start;
    });
    if (all.size() > limit) all.resize(limit);
    return all;
}

std::string event_dates(const TimeEvent& e, int today) {
    std::string out = today >= e.start && today <= e.end ? "Now: " : "";
    if (e.start == e.end) return out + day_text(e.start, true);
    const bool same_year = e.start / 10000 == e.end / 10000;
    return out + day_text(e.start, !same_year) + " to " + day_text(e.end, true);
}

}  // namespace nxrt::games::pkp
