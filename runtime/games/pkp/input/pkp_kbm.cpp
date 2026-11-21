// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer, keyboard and mouse button prompts (pkp_kbm.h). Offsets in `main` of the 2.0.1 update, found in IDA (work/re/pc_features.md).
//
// The game writes button prompts as text tags: "^30~153~" is the glyph of whatever pad button does input action 153 ("pick up"). The text
// layout (0x1f90ca0) parses the tag and calls the tag handler stored in its text system (0xbfbe08 -> 0xe922dc). For type 0x30 the handler asks
// the input manager ([0x3561718]) which pad button does the action (0x1126e18: the remappable action table, with the game's L/R swap applied),
// looks up that button's sprite (0xa8332c: sheet and sprite id; 0x9125e0 the sheet; 0x24ec2c0 the 88-byte sprite record: name, size at
// +66/+68, the quad at +80 with its UVs at +24) and adds the sprite as an inline image. The button codes are the input manager's (its per-pad
// state at +1704 + code): 0 left 1 right 2 up 3 down (d-pad), 4 Y 5 A 6 X 7 B, 8 minus 9 plus, 10 L 11 R 12 ZL 13 ZR, 14 / 15 stick clicks;
// the stick glyphs have higher codes (105 is the right stick, sprite "0000_key_1_rs"); they are told apart by sprite name (..._ls, ..._rs).
// All codes of the action table are looked up at the first tag, because the HUD (bottom left) draws the same atlas sprites without tags.
//
// The mid hook sits on the layout's call of the handler (0x1f90fec: x0 the layout, w1 the tag type, x2 the arguments as UTF-16, x8 the
// handler). While a prompt style other than switch shows ([prompts] style keyboard/xbox/playstation, or auto following the last
// device used) and [pkp] kbm_icons is on:
//   * keyboard and mouse: the button's sprite in the game's UI atlas (a BC7 texture) is overwritten with the icon of the key bound
//     to that button (assets/kbm_icons/<name>.png next to the exe, Kenney's Input Prompts names: keyboard_e.png, mouse_left.png ...),
//     through nxrt/tex_patch.h. The game then draws its own glyph, which now shows the key, in every text that uses it. When the pads
//     are used again (or the binding changes) the saved pixels go back. A button with a bound key but no icon file gets the key's
//     name in the game's own font instead ("[E]"), through the layout's add-character call (vtable +16); the handler call is then
//     pointed at an empty function.
//   * xbox / playstation: the same atlas sprites are overwritten with the pad icons of the prompt icon pack
//     (assets/prompts/prompt_icons.pak: play::prompts_pack(); ps/cross for the button that presses Switch A, following [padmap]
//     and [pad] label_abxy), so the prompts show the controller's own buttons instead of the game's Switch glyphs. The four d-pad
//     glyphs (left, right, up, down) get the d-pad with that arm lit (xbox/dpad_up ...).
// The sprites are also looked up from the frame hook (pkp_play.cpp) once the sprite sheets are loaded, so the title screen and the HUD,
// which draw them without a text tag, show the icons before the first tag.
// The A glyphs drawn into the game's art instead (the title screen's "PRESS (A) BUTTON", the challenges' badge) are pkp_baked_prompts.cpp:
// they show what A shows in the current prompt style (the Enter key's icon in keyboard style when one is bound to A, the pad's button icon
// in a pad style).
#include "input/pkp_kbm.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_settings.h"
#include "core/pkp_sprites.h"
#include "input/pkp_baked_prompts.h"
#include "input/pkp_prompt_icons.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "nxrt/dispatch.h"
#include "nxrt/gfx.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/tex_patch.h"
#include "nxrt/text.h"
#include "prompts.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kTagCall = 0x1f90fec;          // mid: text layout, BLR to the tag handler (w1 type, x2 args, x0 layout)
using layout::kInputMgr;
using layout::kActionButtons;
constexpr uint64_t kActionButton = 0x1126e18;     // u8 (mgr, u16 action, bool remap, bool): the button code of an action
constexpr uint64_t kTagIcon = 0xa8332c;           // (type, id, u32* sprite, u16* sheet, u16* extra): the sprite of an icon tag
using sprites::kSheet;
using sprites::kSheetSprite;
constexpr uint64_t kEmpty = 0x124750;             // a function that only returns
using layout::kTables;
using layout::kUiMgr;

// Glyph slots: the 16 button codes, then the two stick glyphs.
constexpr int kCodes = 16, kSlotLS = 16, kSlotRS = 17, kSlots = 18;   // the sticks are found by their sprite names (..._ls, ..._rs)
constexpr int kMaxCode = 0x7f;                                          // the game's glyph codes are 0..0x7e
constexpr uint64_t kCodeBit[kCodes] = {1ull << 12, 1ull << 14, 1ull << 13, 1ull << 15, 1ull << 3, 1ull << 0, 1ull << 2, 1ull << 1,
                                       1ull << 11, 1ull << 10, 1ull << 6, 1ull << 7, 1ull << 8, 1ull << 9, 1ull << 4, 1ull << 5};
constexpr const char* kSlotName[kSlots] = {"left", "right", "up", "down", "Y", "A", "X", "B", "minus", "plus", "L", "R", "ZL", "ZR",
                                           "LS click", "RS click", "LS", "RS"};

struct Want {
    std::string label;    // "E", "Space", "LMB" ... "" = nothing bound: the game's glyph stays
    std::string icon;     // the icon file's name without .png ("keyboard_e"), "" = none
};

struct Glyph {
    bool tried = false;        // the sprite was looked up (found or not)
    uint64_t tex = 0;          // the game's NVNtexture of the atlas, 0 = not found
    uint32_t x = 0, y = 0, w = 0, h = 0;
    std::string shown;         // the icon written there now, "" = the game's own pixels
    std::vector<uint8_t> saved;
};

constexpr std::array<int8_t, kMaxCode> unknown_codes() {
    std::array<int8_t, kMaxCode> a{};
    for (auto& v : a) v = -2;
    return a;
}

struct State {
    uint64_t base = mem::kMainBase;
    bool enabled = true;                             // [pkp] kbm_icons
    prompt_icons::Cache icons;                       // keyboard icons from <exe folder>/assets/kbm_icons/, pad icons from the pack
    BakedPrompts baked;                              // the A drawn into the game's art (game thread only)
    play::Settings settings;                         // the last applied settings: bindings for the keyboard style, the pad remap for pad styles
    play::PromptStyle eff = play::PromptStyle::Switch;   // the style that shows now (play::prompts_effective_style)
    std::mutex mu;
    std::array<Want, kSlots> want;
    std::array<Glyph, kSlots> glyph;
    std::array<int8_t, kMaxCode> code_slot = unknown_codes();   // glyph code -> slot, -1 = none, -2 = not looked up yet
    bool all_resolved = false;
    uint32_t frames = 0, frame_tries = 0;            // kbm_frame: game frames seen, lookups tried before the first tag
    uint64_t types_logged[4] = {};                   // text tag types logged once (diagnostics)
#if NXRT_TESTTOOLS
    std::vector<uint64_t> dumped;                    // atlases written to NXRT_PKP_KBM_DUMP
#endif
    std::string art_icon;                            // the A drawn into the game's art: the Enter key bound to A (else A's icon)
    std::atomic<uint64_t> tags{0}, texts{0}, patches{0};
};

State& st() {
    static State s;
    return s;
}

using guest::rd;
using guest::call;

// ---- what each glyph should show ----
std::string label_of(const play::InputId& in) {
    if (in.kind == play::InKind::Mouse) {
        static const char* const kMouse[] = {"", "LMB", "MMB", "RMB", "Mouse4", "Mouse5"};
        return in.code >= 1 && in.code <= 5 ? kMouse[in.code] : play::input_name(in);
    }
    if (in.kind == play::InKind::Wheel) return in.code > 0 ? "Wheel Up" : "Wheel Down";
    return play::input_name(in);
}

// Kenney Input Prompts file name (Keyboard & Mouse/Default) of an input, "" when the pack has none.
std::string icon_of(const play::InputId& in) {
    if (in.kind == play::InKind::Mouse) return in.code == 1 ? "mouse_left" : in.code == 2 ? "mouse_scroll" : in.code == 3 ? "mouse_right" : "";
    if (in.kind == play::InKind::Wheel) return in.code > 0 ? "mouse_scroll_up" : "mouse_scroll_down";
    if (in.kind != play::InKind::Key) return "";
    std::string n = play::input_name(in);
    n = nxrt::text::lower(std::move(n));
    if (n.size() == 1 && std::isalnum(static_cast<unsigned char>(n[0]))) return "keyboard_" + n;
    if (n.size() >= 2 && n.size() <= 3 && n[0] == 'f' && std::isdigit(static_cast<unsigned char>(n[1]))) return "keyboard_" + n;   // f1..f12
    static const std::pair<const char*, const char*> kNames[] = {
        {"space", "space"}, {"return", "return"}, {"keypad enter", "numpad_enter"}, {"left shift", "shift"}, {"right shift", "shift"},
        {"left ctrl", "ctrl"}, {"right ctrl", "ctrl"}, {"left alt", "alt"}, {"right alt", "alt"}, {"escape", "escape"},
        {"backspace", "backspace"}, {"tab", "tab"}, {"up", "arrow_up"}, {"down", "arrow_down"}, {"left", "arrow_left"},
        {"right", "arrow_right"}, {"capslock", "capslock"}, {"delete", "delete"}, {"end", "end"}, {"home", "home"}, {"insert", "insert"},
        {"pageup", "page_up"}, {"pagedown", "page_down"}, {"-", "minus"}, {"=", "equals"}, {",", "comma"}, {".", "period"},
        {"/", "slash_forward"}, {"\\", "slash_back"}, {";", "semicolon"}, {"'", "apostrophe"}, {"`", "tilde"}, {"[", "bracket_open"},
        {"]", "bracket_close"}, {"keypad +", "numpad_plus"},
    };
    for (const auto& [k, v] : kNames)
        if (n == k) return std::string("keyboard_") + v;
    return "";
}

// The input bound to an action (play bindings before the `all` layer and the menu), press bindings only.
bool input_for_act(const play::Settings& s, play::Act a, play::InputId* out) {
    for (play::Ctx want : {play::Ctx::Play, play::Ctx::All, play::Ctx::Menu})
        for (const play::Binding& b : s.bindings)
            if (b.act == a && b.ctx == want && b.trig == play::Trig::Press) {
                *out = b.in;
                return true;
            }
    return false;
}

// The input bound to exactly these Switch buttons: the named game actions before the raw btn_ ones.
bool input_for_bits(const play::Settings& s, uint64_t bit, play::InputId* out) {
    for (int pass = 0; pass < 2; ++pass)
        for (int a = 1; a < play::kActCount; ++a) {
            const bool raw = a >= static_cast<int>(play::Act::BtnA);
            if ((pass == 0) == raw) continue;
            const play::Output& o = s.out[a];
            if (o.buttons != bit || o.lx != 0 || o.ly != 0 || o.rx != 0 || o.ry != 0) continue;
            if (input_for_act(s, static_cast<play::Act>(a), out)) return true;
        }
    return false;
}

// ---- the sprite and its atlas ----
#if NXRT_TESTTOOLS   // a dev aid: release packages carry no env-var dumps
// NXRT_PKP_KBM_DUMP=<folder>: the atlas's level 0 as raw BC7 blocks (<folder>/atlas_<id>_<w>x<h>.bc7), once per texture. Caller holds s.mu.
void dump_atlas(State& s, uint64_t tex) {
    const char* dir = std::getenv("NXRT_PKP_KBM_DUMP");
    if (!dir || !*dir || std::find(s.dumped.begin(), s.dumped.end(), tex) != s.dumped.end()) return;
    s.dumped.push_back(tex);
    gfx::GuestTexInfo ti;
    std::vector<uint8_t> blocks;
    uint32_t w = 0, h = 0;
    if (!gfx::guest_texture_info(reinterpret_cast<const void*>(tex), &ti) || !gfx::texture_read_bc7_level(reinterpret_cast<const void*>(tex), 0, &blocks, &w, &h))
        return;
    char path[512];
    std::snprintf(path, sizeof path, "%s/atlas_%u_%ux%u.bc7", dir, ti.id, w, h);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(blocks.data(), 1, blocks.size(), f);
        std::fclose(f);
        NXLOG_I(kTag, "kbm: atlas #%u (%ux%u, %u levels, %s) written to %s", ti.id, w, h, ti.levels, ti.srgb ? "sRGB" : "linear", path);
    }
}
#endif

// Looks up the sprite of a glyph code (through an action that shows it) and, for a code with a slot, where it is in the atlas. Caller holds s.mu.
// False: the sprite sheet is not loaded yet (the code stays unknown, to be looked up again).
bool resolve_code(::Ctx* c, State& s, uint32_t code, uint32_t action) {
    if (code >= static_cast<uint32_t>(kMaxCode)) return true;
    const uint64_t scratch = (c->x[31] - 0x400) & ~0xfull, sp = scratch - 0x100;
    std::memset(reinterpret_cast<void*>(scratch), 0, 16);
    call(c, s.base + kTagIcon, 0x30, action, scratch, scratch + 4, scratch + 6, sp);
    const uint32_t sprite = rd<uint32_t>(scratch);
    const uint64_t sheet = call(c, s.base + kSheet, scratch + 4, 0, 0, 0, 0, sp);
    const uint64_t rec = sheet ? call(c, s.base + kSheetSprite, sheet, sprite, 0, 0, 0, sp) : 0;
    if (!rec || !mem::is_mapped(rec, 88)) return false;
    s.code_slot[code] = -1;
    char name[65] = {};
    std::memcpy(name, reinterpret_cast<const void*>(rec), 64);
    const std::string nm(name);
    auto ends = [&](const char* suf) { const size_t n = std::strlen(suf); return nm.size() >= n && nm.compare(nm.size() - n, n, suf) == 0; };
    const int slot = code < static_cast<uint32_t>(kCodes) ? static_cast<int>(code) : ends("_ls") ? kSlotLS : ends("_rs") ? kSlotRS : -1;
    if (slot < 0) {
        NXLOG_I(kTag, "kbm prompt: code %u is sprite %s (left alone)", code, name);
        return true;
    }
    s.code_slot[code] = static_cast<int8_t>(slot);
    Glyph& g = s.glyph[slot];
    if (g.tried) return true;
    g.tried = true;
    g.tex = sprites::texture_of(rec);
    gfx::GuestTexInfo ti;
    sprites::Rect r;
    if (!g.tex || !gfx::guest_texture_info(reinterpret_cast<const void*>(g.tex), &ti) || !sprites::rect_of(rec, ti.width, ti.height, &r)) {
        NXLOG_W(kTag, "kbm prompt %s: sprite %s, atlas texture not found", kSlotName[slot], name);
        g.tex = 0;
        return true;
    }
#if NXRT_TESTTOOLS
    dump_atlas(s, g.tex);
#endif
    g.x = r.x, g.y = r.y, g.w = r.w, g.h = r.h;
    const bool ok = ((g.x | g.y | g.w | g.h) & 3u) == 0 && g.w && g.h && g.w <= 256 && g.h <= 256;
    NXLOG_I(kTag, "kbm prompt %s (code %u): sprite %s %ux%u at (%u, %u) of texture #%u (%ux%u, %u levels)%s", kSlotName[slot], code, name, g.w,
            g.h, g.x, g.y, ti.id, ti.width, ti.height, ti.levels, ok ? "" : ", not 4x4 aligned: text only");
    if (!ok) g.tex = 0;
    return true;
}

// Every code of the input manager's action table, once: the HUD draws the same atlas sprites without text tags. Caller holds s.mu.
// Stays unresolved (to be tried again) while the sprite sheet is not loaded.
void resolve_all(::Ctx* c, State& s, uint64_t mgr) {
    const uint64_t tab = rd<uint64_t>(mgr + kActionButtons), end = rd<uint64_t>(mgr + kActionButtons + 8);
    if (!tab || end <= tab || end - tab > 4 * 0x400 || !mem::is_mapped(tab, end - tab)) return;
    bool ready = true;
    for (uint32_t action = 1; action < (end - tab) / 4 && action <= 0x1ff && ready; ++action) {
        if (action == 255) continue;
        const uint32_t code = rd<uint32_t>(tab + 4ull * action) & 0xff;
        if (code < static_cast<uint32_t>(kMaxCode) && s.code_slot[code] == -2) ready = resolve_code(c, s, code, action);
    }
    s.all_resolved = ready;
}

// What every glyph should show now, from the current settings and the prompt style that shows. Caller holds s.mu.
void compute_want_locked(State& s) {
    const play::PromptStyle eff = play::prompts_effective_style();
    s.eff = eff;
    std::array<Want, kSlots> want;
    std::string art;
    if (eff == play::PromptStyle::Keyboard) {
        for (int i = 0; i < kCodes; ++i) {
            play::InputId in;
            if (input_for_bits(s.settings, kCodeBit[i], &in)) want[i] = {label_of(in), icon_of(in)};
        }
        // the sticks: the left one moves (the four move keys, "WASD"), the right one turns the camera (the mouse)
        std::string move, move_icon = "@move:";
        for (play::Act a : {play::Act::MoveForward, play::Act::MoveLeft, play::Act::MoveBack, play::Act::MoveRight}) {
            play::InputId in;
            const bool bound = input_for_act(s.settings, a, &in);
            const std::string n = bound ? play::input_name(in) : std::string(), ic = bound ? icon_of(in) : std::string();
            move = n.size() == 1 && (!move.empty() || a == play::Act::MoveForward) ? move + n : std::string();
            move_icon = ic.empty() || move_icon.empty() ? std::string() : move_icon + (a == play::Act::MoveForward ? "" : ",") + ic;
        }
        want[kSlotLS] = {move.size() == 4 ? move : "Move", move_icon};
        want[kSlotRS] = {"Mouse", "mouse_move"};
        // the A drawn into the art (title, menu badges): the Enter key when one is bound to A (the menu key, though E is listed first),
        // else whatever A shows
        art = want[5].icon;
        for (const play::Binding& b : s.settings.bindings) {
            const play::Output& o = s.settings.out[static_cast<int>(b.act)];
            if (b.trig != play::Trig::Press || b.in.kind != play::InKind::Key || o.buttons != kCodeBit[5] || o.lx || o.ly || o.rx || o.ry) continue;
            const std::string n = play::input_name(b.in);
            if ((n == "Return" || n == "Keypad Enter") && !icon_of(b.in).empty()) {
                art = icon_of(b.in);
                break;
            }
        }
    } else if (play::prompt_style_is_pad(eff)) {
        // the pad's own button that presses each Switch button, following [padmap] and [pad] label_abxy; the stick clicks and the
        // stick glyphs have no pad icon and keep the game's own
        for (int i = 0; i < kCodes; ++i)
            want[i].icon = play::pad_pack_name(eff, s.settings.pad, s.settings.pad.label_abxy, kCodeBit[i]);
        art = want[5].icon;
    }
    s.want = want;
    s.art_icon = art;
}

// Puts the wanted icon (or the game's pixels) into every glyph that was found. Caller holds s.mu.
void sync_glyphs_locked(State& s) {
    compute_want_locked(s);
    const bool patch = s.enabled && s.eff != play::PromptStyle::Switch;
    for (int i = 0; i < kSlots; ++i) {
        Glyph& g = s.glyph[i];
        if (!g.tex) continue;
        std::string target = patch ? s.want[i].icon : std::string();
        if (!target.empty() && !s.icons.have(target)) {
            const std::string fb = play::pad_pack_fallback(target);   // an older pack without the d-pad directions: the whole d-pad
            target = !fb.empty() && s.icons.have(fb) ? fb : std::string();
        }
        if (target == g.shown) continue;
        const void* tex = reinterpret_cast<const void*>(g.tex);
        if (!g.shown.empty()) {
            gfx::texture_put_bc7_blocks(tex, g.x, g.y, g.w, g.h, g.saved.data(), g.saved.size());
            g.shown.clear();
        }
        if (!target.empty()) {
            const std::vector<uint8_t> px = s.icons.pixels(target, g.w, g.h);
            if (gfx::texture_patch_bc7(tex, g.x, g.y, g.w, g.h, px.data(), &g.saved)) {
                g.shown = target;
                s.patches.fetch_add(1, std::memory_order_relaxed);
                NXLOG_I(kTag, "kbm prompt %s: icon %s", kSlotName[i], target.c_str());
            } else {
                gfx::GuestTexInfo fi;
                if (gfx::guest_texture_info(tex, &fi))
                    NXLOG_W(kTag, "kbm prompt %s: the atlas patch failed (texture #%u, NVN format %u, %ux%u, %u levels, %s%s)", kSlotName[i], fi.id,
                            fi.format, fi.width, fi.height, fi.levels, fi.bc7 ? "BC7" : "not BC7", fi.srgb ? ", sRGB" : "");
                else
                    NXLOG_W(kTag, "kbm prompt %s: the atlas patch failed (texture is gone)", kSlotName[i]);
                g.tex = 0;
            }
        }
    }
}

int hook_tag(::Ctx* c, uint64_t, void*) {
    State& s = st();
    {
        // every tag type once, with its arguments (which texts carry glyphs other than the action tag 0x30)
        const uint32_t type = static_cast<uint32_t>(c->x[1] & 0xff);
        std::lock_guard<std::mutex> lk(s.mu);
        if (!(s.types_logged[type / 64] & (1ull << (type % 64)))) {
            s.types_logged[type / 64] |= 1ull << (type % 64);
            char arg[40] = {};
            for (int i = 0; i < 39 && c->x[2] && mem::is_mapped(c->x[2] + 2 * i, 2); ++i) {
                const uint16_t ch = rd<uint16_t>(c->x[2] + 2 * i);
                if (!ch) break;
                arg[i] = ch >= 0x20 && ch < 0x7f ? static_cast<char>(ch) : '?';
            }
            NXLOG_I(kTag, "kbm: text tag type 0x%02x first seen, arguments \"%s\"", type, arg);
        }
    }
    if ((c->x[1] & 0xff) != 0x30) return 0;
    if (!s.enabled) return 0;
    s.tags.fetch_add(1, std::memory_order_relaxed);
    uint32_t action = 0;
    if (!c->x[2]) return 0;   // no string: no digits, and the hook would read address 0
    for (uint64_t p = c->x[2]; p < c->x[2] + 16; p += 2) {
        const uint16_t ch = rd<uint16_t>(p);
        if (ch < '0' || ch > '9') break;
        action = action * 10 + (ch - '0');
    }
    if (action == 0 || action > 0x1ff || action == 255) return 0;
    const uint64_t mgr = rd<uint64_t>(s.base + kInputMgr);
    if (!mgr) return 0;
    const uint32_t code = static_cast<uint32_t>(call(c, s.base + kActionButton, mgr, action, 1, 0)) & 0xff;
    std::string label;
    bool icon = false;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.all_resolved) resolve_all(c, s, mgr);
        if (code < static_cast<uint32_t>(kMaxCode) && s.code_slot[code] == -2) resolve_code(c, s, code, action);
        const int slot = code < static_cast<uint32_t>(kMaxCode) ? s.code_slot[code] : -1;
        sync_glyphs_locked(s);
        if (slot >= 0 && s.eff == play::PromptStyle::Keyboard) {   // the text label is only for the keyboard style
            label = s.want[slot].label;
            icon = !s.glyph[slot].shown.empty();
        }
    }
    if (label.empty() || icon) return 0;   // the game's glyph (patched with the key's icon, or a pad button no key is bound to)
    // the key's name in the game's font: the layout's add-character call, then the handler call does nothing
    const uint64_t layout = c->x[0];
    const uint64_t add_char = rd<uint64_t>(rd<uint64_t>(layout) + 16);
    for (char ch : "[" + label + "]") call(c, add_char, layout, static_cast<uint8_t>(ch), 0xffffffffull);
    c->x[8] = s.base + kEmpty;
    s.texts.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

// The glyph sprites are looked up (and patched) once the game's sprite sheets are loaded, without waiting for the first text tag: the title
// screen and the HUD draw the same atlas sprites with no tag. Also follows a change of the shown prompt style between tags.
void frame(::Ctx* c) {
    State& s = st();
    if (!s.enabled || (++s.frames % 30) != 0) return;
    const play::PromptStyle eff = play::prompts_effective_style();
    // the art with an A drawn into it (title, challenge badge) is looked for while a style other than switch shows
    s.baked.scan(s.frames, eff != play::PromptStyle::Switch, static_cast<int>(eff));
    {
        std::lock_guard<std::mutex> lk(s.mu);
        bool ready = s.all_resolved;
        if (!ready && eff != play::PromptStyle::Switch && s.frame_tries < 400) {
            const uint64_t mgr = rd<uint64_t>(s.base + kInputMgr);
            if (mgr && rd<uint64_t>(s.base + kTables) && rd<uint64_t>(s.base + kUiMgr)) {
                ++s.frame_tries;
                resolve_all(c, s, mgr);
                ready = s.all_resolved;
                if (ready) NXLOG_I(kTag, "kbm: glyph sprites found at game frame %u", s.frames);
            }
        }
        // not only once everything is found: the title draws (-) Settings while other sprites (sticks) are not loaded yet, so every
        // glyph already found gets its icon now. Skipped while none was found (nothing to patch, no per-frame work)
        if (ready || std::any_of(s.glyph.begin(), s.glyph.end(), [](const Glyph& g) { return g.tex != 0; })) sync_glyphs_locked(s);
        else compute_want_locked(s);
        s.baked.sync(s.icons, s.eff != play::PromptStyle::Switch ? s.art_icon : std::string());
    }
}

std::string exe_dir() {
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    const size_t cut = p.find_last_of("\\/");
    return cut == std::string::npos ? std::string() : p.substr(0, cut + 1);
}

// main thread: the key names for every pad button
void settings_changed(const play::Settings& set) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.settings = set;
    compute_want_locked(s);
}

void install(const FeatureInstall& in) {
    State& s = st();
    s.base = in.base;
    s.enabled = settings::get_bool(settings::Key::KbmIcons);
    s.icons.set_dir(exe_dir() + "assets/kbm_icons/");
    in.hooks.add_mid(in.base + kTagCall, &hook_tag);
    NXLOG_I(kTag, "button prompts %s (keyboard icons from %s, pad icons from the prompt icon pack; the style is [prompts] style)",
            s.enabled ? "on" : "off ([pkp] kbm_icons = 0)", s.icons.dir().c_str());
    settings_changed(in.settings);
}

void status_text(char* buf, size_t n) {
    std::snprintf(buf, n, " kbm_tags=%llu kbm_texts=%llu kbm_patches=%llu", static_cast<unsigned long long>(st().tags.load()),
                  static_cast<unsigned long long>(st().texts.load()), static_cast<unsigned long long>(st().patches.load()));
}

}  // namespace

const Feature kKbmFeature = {
    .name = "kbm",
    .install = &install,
    .game_frame = &frame,
    .settings_changed = &settings_changed,
    .status_text = &status_text,
};

}  // namespace nxrt::games::pkp
