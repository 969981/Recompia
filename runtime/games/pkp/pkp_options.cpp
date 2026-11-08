// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options, prototype: one extra row ("FPS Counter", Off / On) at the end of the Game tab, backed by [fps] show in controls.ini.
// [pkp] options_test_row, off by default. It tests whether settings can live in the game's own menu; no game file or save data changes.
//
// How the game's Options screen works (main 2.0.1a, static reading):
//  * Rows come from a data table of 51 records of 24 bytes (ids 0x25..0x32 are empty). A record names its tab, type, value count and text.
//  * 0x7100e81b3c (player, id, mode, item) reads (mode 1) or applies (2, 3) the value at item + 0x358; unknown ids do nothing.
//  * 0x710100fce0 (widget, value count - 1, id) names the values from a table that ends at id 0x24: a new choice row crashes without a hook.
//  * Text ids above 800 are skipped by the game's guards; text 0xa8 is a bare "show this string" entry in every language.
//
// Hooks (sites in recomp/games/pkp/main.sites): fill the record when the screen opens, get/set the value, and draw the three texts.
#include "pkp_options.h"

#include <cstring>

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "play_game.h"
#include "play_types.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr uint64_t kOpen = 0x1006628;         // Options screen, open (vtable +0x38)
constexpr uint64_t kOption = 0xe81b3c;        // option get/set
constexpr uint64_t kValueNames = 0x100fce0;   // choice widget: value names
constexpr uint64_t kRowDrawn = 0x100b294;     // inline, after the row's draw called its value widget (x19 = the row)
constexpr uint64_t kTextById = 0xeba100;      // (widget, slot, text id): sets the description
constexpr uint64_t kTextString = 0xf35f20;    // (widget, slot, text id, const char*): text id with a string argument
constexpr uint64_t kTables = 0x35618f0;       // qword: the data-table manager
constexpr uint64_t kOptionTable = 0x10 + 0x828, kRowId = 0x420, kItemValue = 0x358;
constexpr uint32_t kCustomText = 801;         // past the game's guards (1..800): the row's label and description
constexpr uint32_t kStringText = 0xa8;        // "<string;0(lstr1)>"
constexpr uint8_t kId = 0x25;
constexpr uint8_t kOrder = 10;                // after the Game tab's last row (9)
constexpr uint32_t kFirstValueSlot = 7;       // text slots of the value names

uint64_t g_base = 0;

template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }

uint64_t call(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t sp = 0) {
    const ::Ctx saved = *c;
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    if (sp) c->x[31] = sp;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}

// The string is copied below the guest stack pointer: the game reads it as guest memory.
void set_text(::Ctx* c, uint64_t widget, uint32_t slot, const char* s) {
    const uint64_t buf = (c->x[31] - 0x100) & ~uint64_t{0xf};
    std::memcpy(reinterpret_cast<void*>(buf), s, std::strlen(s) + 1);
    call(c, g_base + kTextString, widget, slot, kStringText, buf, buf - 0x10);
}

int hook_open(::Ctx* c, uint64_t, void*) {
    const uint64_t mgr = rd<uint64_t>(g_base + kTables);
    uint64_t t = mgr ? rd<uint64_t>(mgr + kOptionTable) : 0;
    if (t) t = call(c, rd<uint64_t>(rd<uint64_t>(t) + 0x38), t);
    const uint64_t entry = t ? call(c, rd<uint64_t>(rd<uint64_t>(t) + 0x30), t, kId) : 0;
    const uint64_t rec = entry ? rd<uint64_t>(entry + 8) : 0;
    if (!rec || !mem::is_mapped(rec, 24)) {
        static bool logged = false;
        if (!logged) NXLOG_W(kTag, "options: no record for id 0x%x, the test row stays hidden", kId);
        logged = true;
        return 0;
    }
    auto* r = reinterpret_cast<uint8_t*>(rec);
    if (r[0x15]) return 0;                           // already filled
    const uint16_t text = kCustomText;
    std::memcpy(r, &text, 2);                        // description
    std::memcpy(r + 2, &text, 2);                    // label
    r[4] = 1;                                        // choice
    r[5] = 0;                                        // default: first value
    r[6] = 1;                                        // two values
    std::memset(r + 8, 1, 10);                       // shown in every screen mode
    r[0x12] = 1;                                     // Game tab
    r[0x13] = 2;                                     // general options
    r[0x16] = 3;                                     // as every other row
    r[0x15] = kOrder;                                // last: a nonzero order is what makes the row exist
    NXLOG_I(kTag, "options: test row added (id 0x%x)", kId);
    return 0;
}

int hook_option(::Ctx* c, uint64_t, void*) {
    if ((c->x[1] & 0xff) != kId) return 0;
    const uint32_t mode = static_cast<uint32_t>(c->x[2]);
    const uint64_t value = c->x[3] + kItemValue;
    play::Settings s = play::current_settings();
    if (mode == 1) {
        const uint32_t v = s.fps.show ? 1 : 0;
        std::memcpy(reinterpret_cast<void*>(value), &v, 4);
    } else if (mode == 2 || mode == 3) {
        const bool on = rd<uint8_t>(value) != 0;
        if (s.fps.show != on) {
            s.fps.show = on;
            play::apply_settings(s);                 // main thread, next frame; also writes controls.ini
            NXLOG_I(kTag, "options: FPS counter %s", on ? "on" : "off");
        }
    }
    return 1;
}

int hook_value_names(::Ctx* c, uint64_t, void*) {
    if ((c->x[2] & 0xff) != kId) return 0;
    set_text(c, c->x[0], kFirstValueSlot, "Off");
    set_text(c, c->x[0], kFirstValueSlot + 1, "On");
    return 1;
}

void hook_row_drawn(::Ctx* c, uint64_t, void*) {
    if (rd<uint8_t>(c->x[19] + kRowId) == kId) set_text(c, c->x[19], 4, "FPS Counter");
}

int hook_text_by_id(::Ctx* c, uint64_t, void*) {
    if ((c->x[2] & 0xffff) != kCustomText) return 0;
    set_text(c, c->x[0], static_cast<uint32_t>(c->x[1]), "Show the frame rate on screen.");
    return 1;
}

}  // namespace

void options_install(uint64_t base) {
    if (!config_get_bool("pkp", "options_test_row", false)) return;
    g_base = base;
    hook_add_entry(base + kOpen, &hook_open);
    hook_add_entry(base + kOption, &hook_option);
    hook_add_entry(base + kValueNames, &hook_value_names);
    hook_add_inline(base + kRowDrawn, &hook_row_drawn);
    hook_add_entry(base + kTextById, &hook_text_by_id);
    NXLOG_I(kTag, "options: test row on");
}

}  // namespace nxrt::games::pkp
