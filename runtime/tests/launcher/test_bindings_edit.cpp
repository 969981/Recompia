// SPDX-License-Identifier: GPL-3.0-or-later
// Keyboard tab logic in apps/launcher/bindings_edit.cpp: the list rows, slot edits, clears and resets on the factory bindings.
// Only in-memory settings are used; no file, no window.
//
// The factory bindings are the test game's (Pokopia: games/pkp/pkp_data.cpp kSettingsText). Its [bindings.all] layer is raw Switch
// buttons: Mouse1 is the only key of btn_zr, Space the only key of btn_r, and E, Return and Keypad Enter are btn_a. Pokopia names no
// action slot, so a slot such as primary has no factory key.
#include "test_framework.h"

#include <windows.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

#include "bindings_edit.h"
#include "play_test_game.h"

NXTEST_GROUP(launcher_bindings_edit)

namespace {

using mprl::act_label;
using mprl::bind_slot;
using mprl::clear_slot;
using mprl::inputs_for;
using mprl::key_rows;
using mprl::reset_shown_bindings;
using nxrt::play::Act;
using nxrt::play::Binding;
using nxrt::play::Ctx;
using nxrt::play::InKind;
using nxrt::play::InputId;
using nxrt::play::Settings;
using nxrt::play::Trig;

// A keyboard key by its SDL name ("G", "Space").
InputId key(const char* name) {
    InputId id;
    CHECK(nxrt::play::parse_input(name, &id));
    return id;
}

InputId mouse(int code) { return InputId{InKind::Mouse, code}; }

bool same_input(const InputId& a, const InputId& b) { return a == b; }

bool same_binding(const Binding& a, const Binding& b) {
    return same_input(a.in, b.in) && a.ctx == b.ctx && a.act == b.act && a.trig == b.trig && a.ms == b.ms;
}

// The bindings of the factory layer (all_layer true, Ctx::All) or of the other contexts (false), in file order.
std::vector<Binding> layer(const Settings& s, bool all_layer) {
    std::vector<Binding> out;
    for (const Binding& b : s.bindings)
        if ((b.ctx == Ctx::All) == all_layer) out.push_back(b);
    return out;
}

bool same_list(const std::vector<Binding>& a, const std::vector<Binding>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same_binding(a[i], b[i])) return false;
    return true;
}

Binding binding(const char* key_name, Ctx ctx, Act act) {
    Binding b;
    b.in = key(key_name);
    b.ctx = ctx;
    b.act = act;
    b.trig = Trig::Press;
    b.ms = 0;
    return b;
}

}  // namespace

NXTEST(every_row_has_an_action_name) {
    // Pokopia registers no GameVocabulary, so the launcher lists the generic rows (bindings_edit.cpp key_rows): movement, map, pause, d-pad.
    CHECK_EQ(key_rows().size(), 10u);
    for (const mprl::KeyRow& r : key_rows()) {
        CHECK(r.label != nullptr && r.label[0] != L'\0');
        CHECK(r.group != nullptr && r.group[0] != L'\0');
        const char* name = nxrt::play::act_info(r.act).name;
        CHECK(name != nullptr && name[0] != '\0');
    }
}

NXTEST(factory_zr_is_left_mouse_only) {
    // The factory layer has one key for btn_zr: Mouse1.
    const Settings s = nxrt::play::default_settings();
    const std::vector<InputId> in = inputs_for(s, Act::BtnZR);
    CHECK_EQ(in.size(), 1u);
    CHECK(in.size() == 1 && same_input(in[0], mouse(1)));
}

NXTEST(bind_slot_replaces_slot_zero_and_keeps_trigger) {
    Settings s = nxrt::play::default_settings();
    for (Binding& b : s.bindings)
        if (b.ctx == Ctx::All && b.act == Act::BtnZR) {
            b.trig = Trig::Hold;
            b.ms = 300;
        }
    Act displaced = Act::Jump;
    bind_slot(s, Act::BtnZR, 0, key("G"), &displaced);
    CHECK_EQ(displaced, Act::None);

    const std::vector<InputId> in = inputs_for(s, Act::BtnZR);
    CHECK_EQ(in.size(), 1u);
    CHECK(in.size() == 1 && same_input(in[0], key("G")));

    bool found = false;
    for (const Binding& b : s.bindings) {
        if (b.ctx != Ctx::All || b.act != Act::BtnZR || !same_input(b.in, key("G"))) continue;
        found = true;
        CHECK(b.trig == Trig::Hold);
        CHECK_EQ(b.ms, 300);
    }
    CHECK(found);
}

NXTEST(bind_slot_slot_one_appends) {
    Settings s = nxrt::play::default_settings();
    bind_slot(s, Act::BtnZR, 1, key("G"), nullptr);
    const std::vector<InputId> in = inputs_for(s, Act::BtnZR);
    CHECK_EQ(in.size(), 2u);
    CHECK(in.size() == 2 && same_input(in[0], mouse(1)) && same_input(in[1], key("G")));
}

NXTEST(bind_slot_slot_one_without_first_key_fills_slot_zero) {
    // primary has no factory key: slot 1 becomes the first key.
    Settings s = nxrt::play::default_settings();
    bind_slot(s, Act::Primary, 1, key("G"), nullptr);
    const std::vector<InputId> in = inputs_for(s, Act::Primary);
    CHECK_EQ(in.size(), 1u);
    CHECK(in.size() == 1 && same_input(in[0], key("G")));
}

NXTEST(bind_slot_same_key_in_other_slot_is_dropped) {
    // Mouse1 is already the first key of btn_zr. Setting it as the second key leaves one Mouse1 only.
    Settings s = nxrt::play::default_settings();
    Act displaced = Act::Jump;
    bind_slot(s, Act::BtnZR, 1, mouse(1), &displaced);
    CHECK_EQ(displaced, Act::None);
    const std::vector<InputId> in = inputs_for(s, Act::BtnZR);
    CHECK_EQ(in.size(), 1u);
    CHECK(in.size() == 1 && same_input(in[0], mouse(1)));
}

NXTEST(binding_space_to_zr_displaces_r) {
    Settings s = nxrt::play::default_settings();
    Act displaced = Act::None;
    bind_slot(s, Act::BtnZR, 0, key("Space"), &displaced);
    CHECK_EQ(displaced, Act::BtnR);
    CHECK(inputs_for(s, Act::BtnR).empty());
    const std::vector<InputId> in = inputs_for(s, Act::BtnZR);
    CHECK(in.size() == 1 && same_input(in[0], key("Space")));
}

NXTEST(clear_slot_removes_one_key) {
    Settings s = nxrt::play::default_settings();
    bind_slot(s, Act::BtnZR, 1, key("G"), nullptr);
    clear_slot(s, Act::BtnZR, 0);
    const std::vector<InputId> after_first = inputs_for(s, Act::BtnZR);
    CHECK(after_first.size() == 1 && same_input(after_first[0], key("G")));
    clear_slot(s, Act::BtnZR, 0);   // G is slot 0 now
    CHECK(inputs_for(s, Act::BtnZR).empty());
    clear_slot(s, Act::BtnZR, 1);   // empty slot: nothing happens
    CHECK(inputs_for(s, Act::BtnZR).empty());
}

NXTEST(reset_restores_factory_bindings) {
    Settings s = nxrt::play::default_settings();
    bind_slot(s, Act::BtnZR, 0, key("G"), nullptr);
    clear_slot(s, Act::BtnR, 0);
    reset_shown_bindings(s);

    const Settings factory = nxrt::play::default_settings();
    CHECK_EQ(s.bindings.size(), factory.bindings.size());
    CHECK(same_list(s.bindings, factory.bindings));
    const std::vector<InputId> in = inputs_for(s, Act::BtnZR);
    CHECK(in.size() == 1 && same_input(in[0], mouse(1)));
}

NXTEST(reset_shown_bindings_leaves_other_contexts_alone) {
    // Edit the factory layer (the keys this tab shows) and every other context.
    Settings s = nxrt::play::default_settings();
    bind_slot(s, Act::BtnZR, 0, key("G"), nullptr);
    clear_slot(s, Act::BtnR, 0);
    bind_slot(s, Act::BtnR, 1, key("Z"), nullptr);
    s.bindings.push_back(binding("H", Ctx::Menu, Act::Confirm));
    s.bindings.push_back(binding("J", Ctx::Map, Act::Map));
    s.bindings.push_back(binding("K", Ctx::Alt, Act::Jump));
    s.bindings.push_back(binding("L", Ctx::Inspect, Act::SetB2));
    s.bindings.push_back(binding("M", Ctx::Play, Act::Primary));
    s.bindings.push_back(binding("N", Ctx::Play, Act::Jump));
    // Remove the first play binding (the factory Mouse2 = mouse_mode); M and N stay.
    for (auto it = s.bindings.begin(); it != s.bindings.end(); ++it) {
        if (it->ctx != Ctx::Play) continue;
        s.bindings.erase(it);
        break;
    }

    const std::vector<Binding> others_before = layer(s, false);
    reset_shown_bindings(s);

    // The factory layer is back, and the other contexts are the same bindings in the same order.
    const Settings factory = nxrt::play::default_settings();
    CHECK(same_list(layer(s, true), layer(factory, true)));
    CHECK(same_list(layer(s, false), others_before));
    CHECK_EQ(s.bindings.size(), layer(factory, true).size() + others_before.size());
    const std::vector<InputId> zr = inputs_for(s, Act::BtnZR);
    CHECK(zr.size() == 1 && same_input(zr[0], mouse(1)));
    const std::vector<InputId> r = inputs_for(s, Act::BtnR);
    CHECK(r.size() == 1 && same_input(r[0], key("Space")));
}

NXTEST(reset_shown_bindings_fills_an_empty_factory_layer) {
    // No All binding left at all: the reset adds the factory layer and keeps the other contexts.
    Settings s = nxrt::play::default_settings();
    const std::vector<Binding> others = layer(s, false);
    s.bindings = others;
    reset_shown_bindings(s);

    const Settings factory = nxrt::play::default_settings();
    CHECK(!layer(factory, true).empty());
    CHECK(same_list(layer(s, true), layer(factory, true)));
    CHECK(same_list(layer(s, false), others));
}

NXTEST(other_contexts_are_not_touched) {
    Settings s = nxrt::play::default_settings();
    Binding menu;
    menu.in = key("G");
    menu.ctx = Ctx::Menu;
    menu.act = Act::Confirm;
    menu.trig = Trig::Press;
    menu.ms = 0;
    s.bindings.push_back(menu);

    auto menu_count = [&s]() {
        int n = 0;
        for (const Binding& b : s.bindings)
            if (b.ctx == Ctx::Menu && b.act == Act::Confirm && same_input(b.in, key("G"))) ++n;
        return n;
    };

    // G is only a menu key: binding it to btn_zr displaces nothing, and clearing btn_zr's key leaves the menu key alone.
    Act displaced = Act::Jump;
    bind_slot(s, Act::BtnZR, 0, key("G"), &displaced);
    CHECK_EQ(displaced, Act::None);
    CHECK_EQ(menu_count(), 1);
    clear_slot(s, Act::BtnZR, 0);
    CHECK_EQ(menu_count(), 1);
}

NXTEST(inputs_for_without_bindings_is_empty) {
    Settings s = nxrt::play::default_settings();
    for (int i = 0; i < 4 && !inputs_for(s, Act::BtnZR).empty(); ++i) clear_slot(s, Act::BtnZR, 0);
    CHECK(inputs_for(s, Act::BtnZR).empty());
    CHECK(inputs_for(s, Act::Primary).empty());
}

NXTEST(act_label_comes_from_the_rows) {
    CHECK(act_label(Act::MoveForward) == L"Move forward");
    CHECK(act_label(Act::Map) == L"Map");
    CHECK(act_label(Act::BtnA).empty());
}

// key_rows() keeps the rows it built first for the life of the process, so the rows of a game vocabulary are checked in a fresh process:
// a test vocabulary (play_test_game.h) is installed before the first call.
NXTEST_CHILD(launcher_rows_from_a_game_vocabulary) {
    nxtest::TestVocabulary tv;
    nxtest::ScopedGame game(&tv.defaults);
    const std::vector<mprl::KeyRow>& rows = key_rows();
    if (rows.size() != 3) return 1;
    if (rows[0].act != Act::Primary || std::wstring(rows[0].label) != L"Fire" || std::wstring(rows[0].group) != L"Combat") return 2;
    if (rows[2].act != Act::AltForm || std::wstring(rows[2].label) != L"Morph ball") return 3;
    if (act_label(Act::Jump) != L"Jump" || !act_label(Act::MoveForward).empty()) return 4;
    return 0;
}

NXTEST(rows_follow_the_game_vocabulary_in_a_fresh_process) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    // cmd /c strips the outer pair of quotes, so the whole command is wrapped in one more pair.
    const std::string cmd = std::string("\"\"") + exe + "\" --child launcher_rows_from_a_game_vocabulary\"";
    const int rc = std::system(cmd.c_str());
    CHECK_MSG(rc == 0, "child returned " + std::to_string(rc));
}
