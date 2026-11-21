// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: our own strings in the game's language (pkp_strings.h). One table per language, checked at compile time: every key, in
// the order of pkp_strings_table.h, with no markup ('<' and '>' start the game's text tags in a log line).
#include "core/pkp_strings.h"

#include "core/pkp_language.h"

namespace nxrt::games::pkp::strings {

namespace {

using language::Lang;

enum class Draw : uint8_t { Overlay, Log };

struct Key {
    Draw draw;
    const char* en;
};
constexpr Key kKeys[] = {
#define S(key, draw, en) {Draw::draw, en},
#include "core/pkp_strings_table.h"
#undef S
};
constexpr size_t kKeyCount = sizeof kKeys / sizeof kKeys[0];

struct Entry {
    Str key;
    const char* text;
};

#define T(key, text) {Str::key, text},
constexpr Entry kJa[] = {
#include "core/pkp_strings_ja.inc"
};
constexpr Entry kFr[] = {
#include "core/pkp_strings_fr.inc"
};
constexpr Entry kIt[] = {
#include "core/pkp_strings_it.inc"
};
constexpr Entry kDe[] = {
#include "core/pkp_strings_de.inc"
};
constexpr Entry kEs[] = {
#include "core/pkp_strings_es.inc"
};
constexpr Entry kZhHant[] = {
#include "core/pkp_strings_zh_hant.inc"
};
constexpr Entry kKo[] = {
#include "core/pkp_strings_ko.inc"
};
constexpr Entry kZhHans[] = {
#include "core/pkp_strings_zh_hans.inc"
};
#undef T

constexpr bool plain(const char* s) {
    for (; *s; ++s)
        if (*s == '<' || *s == '>') return false;
    return true;
}

// Every key once, in the table's order, without markup.
template <size_t N>
constexpr bool complete(const Entry (&e)[N]) {
    if (N != kKeyCount) return false;
    for (size_t i = 0; i < N; ++i)
        if (static_cast<size_t>(e[i].key) != i || !plain(e[i].text)) return false;
    return true;
}
static_assert(complete(kJa), "pkp_strings_ja.inc: one T() per key of pkp_strings_table.h, in its order, no '<' '>'");
static_assert(complete(kFr), "pkp_strings_fr.inc: one T() per key of pkp_strings_table.h, in its order, no '<' '>'");
static_assert(complete(kIt), "pkp_strings_it.inc: one T() per key of pkp_strings_table.h, in its order, no '<' '>'");
static_assert(complete(kDe), "pkp_strings_de.inc: one T() per key of pkp_strings_table.h, in its order, no '<' '>'");
static_assert(complete(kEs), "pkp_strings_es.inc: one T() per key of pkp_strings_table.h, in its order, no '<' '>'");
static_assert(complete(kZhHant), "pkp_strings_zh_hant.inc: one T() per key of pkp_strings_table.h, in its order, no '<' '>'");
static_assert(complete(kKo), "pkp_strings_ko.inc: one T() per key of pkp_strings_table.h, in its order, no '<' '>'");
static_assert(complete(kZhHans), "pkp_strings_zh_hans.inc: one T() per key of pkp_strings_table.h, in its order, no '<' '>'");

constexpr bool english_plain() {
    for (const Key& k : kKeys)
        if (!plain(k.en)) return false;
    return true;
}
static_assert(english_plain(), "pkp_strings_table.h: no '<' '>' in a string");

struct Language {
    const Entry* text;      // null: English
    bool overlay_glyphs;    // the overlay's font (ImGui's default, Basic Latin + Latin-1) draws its letters
};
// In the game's language order (pkp_language.h).
constexpr Language kLanguages[language::kCount] = {
    {kJa, false}, {nullptr, true}, {kFr, true}, {kIt, true}, {kDe, true}, {kEs, true}, {kZhHant, false}, {kKo, false}, {kZhHans, false},
};

}  // namespace

const char* get(uint64_t base, Str key) {
    const size_t k = static_cast<size_t>(key);
    if (k >= kKeyCount) return "";
    const Language& l = kLanguages[language::index(base)];
    if (!l.text || (kKeys[k].draw == Draw::Overlay && !l.overlay_glyphs)) return kKeys[k].en;
    return l.text[k].text;
}

std::string fmt_args(uint64_t base, Str key, const std::string* args, size_t count) {
    const std::string_view t = get(base, key);
    std::string out;
    out.reserve(t.size() + 32);
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '{' && i + 2 < t.size() && t[i + 1] >= '0' && t[i + 1] <= '9' && t[i + 2] == '}') {
            const size_t n = static_cast<size_t>(t[i + 1] - '0');
            if (n < count) out += args[n];
            i += 2;
            continue;
        }
        out += t[i];
    }
    return out;
}

}  // namespace nxrt::games::pkp::strings
