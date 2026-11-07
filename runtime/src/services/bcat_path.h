// SPDX-License-Identifier: GPL-3.0-or-later
// Name check for the BCAT delivery cache served from a host folder ([bcat] cache_dir, network.cpp). Standard library only.
#pragma once

// True for a delivery cache directory or file name that may become part of a host path: 1 to 31 characters from A-Z a-z 0-9 _ - . and no "..".
static inline bool bcat_name_ok(const char* s) {
    if (!s) return false;
    unsigned n = 0;
    for (; s[n]; ++n) {
        const char c = s[n];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok || n >= 31) return false;
        if (c == '.' && s[n + 1] == '.') return false;
    }
    return n >= 1;
}
