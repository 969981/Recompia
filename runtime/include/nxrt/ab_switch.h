// SPDX-License-Identifier: GPL-3.0-or-later
// The developer A/B switch: one named choice that can be flipped while the game runs, for paired measurements at one spot.
// [diag] ab_name = <name> and [diag] ab_file = <path>: ab(name, def) answers what the file holds ("1": on, "0": off), read at most every
// 250 ms; any other name, no file or an unreadable one answers def. Every change is logged ("ab: <name> on|off") so the perf lines can be
// split by it. Off by default: without ab_file a call is one load and a compare.
#pragma once

namespace nxrt::diag {
bool ab(const char* name, bool def);
}
