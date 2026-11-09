// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode settings page: the Controls tab (mouse, keyboard and mouse buttons, gamepad, Switch layout, status).
#include "play_ui_page.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "nxrt/audio_sink.h"
#include "nxrt/extras.h"
#include "extras/temporal.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/host_device.h"
#include "nxrt/input_source.h"
#include "nxrt/lowlat.h"
#include "nxrt/overlay.h"
#include "nxrt/platform.h"
#include "play_game.h"

namespace nxrt::play {
namespace ui {

constexpr Trig kTrigChoices[] = {Trig::Press, Trig::Tap, Trig::Hold, Trig::Toggle, Trig::DoubleTap};
// The Button prompts combo on the Controller tab, in PromptStyle order.
constexpr const char* kPromptLabels[kPromptStyleCount] = {"Switch (the game's own)", "Xbox", "PlayStation", "Steam Controller", "Steam Deck", "Keyboard and mouse",
                                                          "Auto (follows the last used device)"};

struct SwitchButton {
    const char* name;
    hid::NpadButtonBit bit;
};
constexpr SwitchButton kSwitchButtons[] = {
    {"A", hid::kBtnA},         {"B", hid::kBtnB},         {"X", hid::kBtnX},         {"Y", hid::kBtnY},
    {"L", hid::kBtnL},         {"R", hid::kBtnR},         {"ZL", hid::kBtnZL},       {"ZR", hid::kBtnZR},
    {"PLUS", hid::kBtnPlus},   {"MINUS", hid::kBtnMinus}, {"UP", hid::kBtnUp},       {"DOWN", hid::kBtnDown},
    {"LEFT", hid::kBtnLeft},   {"RIGHT", hid::kBtnRight}, {"LSTICK", hid::kBtnStickL}, {"RSTICK", hid::kBtnStickR},
};
constexpr int kSwitchButtonCount = static_cast<int>(sizeof kSwitchButtons / sizeof kSwitchButtons[0]);
static_assert(kSwitchButtonCount == 16, "the Switch pad has 16 buttons in the status view");

// ---- Mouse --------------------------------------------------------------------------------------------------------------------------

void sensitivity_help(double rad) {
    if (rad <= 0) {
        ImGui::TextDisabled("= %.6f rad per count", rad);
        return;
    }
    ImGui::TextDisabled("= %.6f rad per count, %.3f degrees per count, 360 degrees in %.0f counts", rad, rad * 180.0 / kPi, 2.0 * kPi / rad);
}

// The game's own entries of the Mouse tab (play_game.h GamePlay::mouse_tab): where = 0 after the sensitivity, 1 after the invert boxes, 2 at the end.
void game_mouse_section(int where, Page& p) {
    const GamePlay* gp = game_play();
    if (gp && gp->mouse_tab && gp->mouse_tab(where, p.edit)) mark(p);
}

void mouse_tab(Page& p) {
    MouseSettings& m = p.edit.mouse;
    double src = source_sens(m.yaw_rad);
    if (slider("Horizontal sensitivity", &src, 0.1, 20.0, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        m.yaw_rad = rad_per_count(src);
        if (p.link_pitch) m.pitch_rad = m.yaw_rad;
        mark(p);
    }
    sensitivity_help(m.yaw_rad);

    src = source_sens(m.pitch_rad);
    if (slider("Vertical sensitivity", &src, 0.1, 20.0, "%.2f", ImGuiSliderFlags_Logarithmic)) {
        m.pitch_rad = rad_per_count(src);
        mark(p);
    }
    sensitivity_help(m.pitch_rad);
    ImGui::Checkbox("Link vertical to horizontal", &p.link_pitch);
    game_mouse_section(0, p);

    ImGui::Spacing();
    if (ImGui::Checkbox("Invert vertical", &m.invert_y)) mark(p);
    if (ImGui::Checkbox("Invert horizontal", &m.invert_x)) mark(p);
    game_mouse_section(1, p);
    if (ImGui::Checkbox("Mouse pointer in menus (cursor, hover, click)", &m.menu_cursor)) mark(p);
    game_mouse_section(2, p);
}

// ---- Controller ---------------------------------------------------------------------------------------------------------------------

// The 22 controller buttons, in PadSrc order.
constexpr const char* kPadSrcLabels[kPadSrcCount] = {
    "A / Cross (bottom)", "B / Circle (right)", "X / Square (left)", "Y / Triangle (top)",
    "LB / L1",            "RB / R1",            "LT / L2",            "RT / R2",
    "Menu / Options",     "View / Create",      "Left stick click",   "Right stick click",
    "D-pad up",           "D-pad down",         "D-pad left",         "D-pad right",
    "Right grip R4 (upper)", "Left grip L4 (upper)", "Right grip R5 (lower)", "Left grip L5 (lower)",
    "Touchpad / left trackpad click", "Right trackpad click",
};
static_assert(sizeof kPadSrcLabels / sizeof kPadSrcLabels[0] == 22, "one label per controller button");

// The choices of a controller button: Default, Nothing, then the single Switch buttons in kSwitchButtons order.
constexpr const char* kPadChoiceNames[] = {
    "Default", "Nothing",
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus",
    "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Left stick click", "Right stick click",
};
constexpr int kPadChoiceCount = static_cast<int>(sizeof kPadChoiceNames / sizeof kPadChoiceNames[0]);
static_assert(kPadChoiceCount == 2 + kSwitchButtonCount, "one choice per Switch button, plus Default and Nothing");
constexpr const char* kPadCustomLabel = "Custom (edit in controls.ini)";

// The value a choice stores.
uint64_t pad_choice_value(int c) {
    if (c == 0) return kPadDefault;
    if (c == 1) return 0;
    return static_cast<uint64_t>(hid::btn(kSwitchButtons[c - 2].bit));
}

// The choice a stored value shows as, or -1 when no choice holds it (a mask such as two buttons, set in the file).
int pad_choice_of(uint64_t v) {
    for (int c = 0; c < kPadChoiceCount; ++c)
        if (pad_choice_value(c) == v) return c;
    return -1;
}

void pad_buttons_group(Page& p) {
    ImGui::SeparatorText("Pad buttons");
    ImGui::TextWrapped("What each controller button presses on the Switch pad. Default keeps the built-in layout.");
    if (ImGui::BeginTable("pad_buttons", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Controller button");
        ImGui::TableSetupColumn("Presses on the Switch pad");
        ImGui::TableHeadersRow();
        for (int i = 0; i < kPadSrcCount; ++i) {
            uint64_t& v = p.edit.pad.remap[i];
            const int sel = pad_choice_of(v);
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(kPadSrcLabels[i]);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##remap", sel >= 0 ? kPadChoiceNames[sel] : kPadCustomLabel)) {
                if (sel < 0) {   // a mask no choice holds: shown as the current entry, not offered
                    ImGui::BeginDisabled();
                    ImGui::Selectable(kPadCustomLabel, true);
                    ImGui::EndDisabled();
                }
                for (int c = 0; c < kPadChoiceCount; ++c) {
                    if (ImGui::Selectable(kPadChoiceNames[c], c == sel) && c != sel) {
                        v = pad_choice_value(c);
                        mark(p);
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset pad buttons")) {
        bool changed = false;
        for (uint64_t& v : p.edit.pad.remap) {
            if (v != kPadDefault) {
                v = kPadDefault;
                changed = true;
            }
        }
        if (changed) mark(p);
    }
}

// The quit combination ([quit] pad): two controller buttons held together open the "Close the game?" prompt; both none = off. A file may
// name more buttons: the combos then show the first two, and changing either keeps only what they show.
void quit_pad_group(Page& p) {
    ImGui::SeparatorText("Quit shortcut");
    uint32_t& bits = p.edit.quit.pad;
    int sel[2] = {-1, -1}, n = 0;
    for (int i = 0; i < kPadSrcCount; ++i) {
        if (!(bits & pad_src_bit(static_cast<PadSrc>(i)))) continue;
        if (n < 2) sel[n] = i;
        ++n;
    }
    for (int k = 0; k < 2; ++k) {
        ImGui::PushID(k);
        if (ImGui::BeginCombo(k == 0 ? "Quit: button 1" : "Quit: button 2", sel[k] < 0 ? "none" : kPadSrcLabels[sel[k]])) {
            for (int c = -1; c < kPadSrcCount; ++c) {
                if (ImGui::Selectable(c < 0 ? "none" : kPadSrcLabels[c], c == sel[k]) && c != sel[k]) {
                    int next[2] = {sel[0], sel[1]};
                    next[k] = c;
                    bits = 0;
                    for (int v : next)
                        if (v >= 0) bits |= pad_src_bit(static_cast<PadSrc>(v));
                    mark(p);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    tip("Holding these controller buttons together asks \"Close the game?\" (A quits, B cancels). They are the physical buttons, whatever they "
        "press on the Switch pad. Both none turns the shortcut off.");
    if (n > 2) ImGui::TextColored(kOrange, "controls.ini names %d buttons (%s); changing one here keeps only the two shown.", n, pad_combo_text(bits).c_str());
}

// The button prompt style: which icons the game shows (Auto follows the last device used). Shared by the keyboard and the pad.
void prompts_row(const LiveStatus& live, Page& p) {
    PromptSettings& prompts = p.edit.prompts;
    const int prompt_sel = static_cast<int>(prompts.style);
    if (ImGui::BeginCombo("Button prompts", kPromptLabels[prompt_sel])) {
        for (int i = 0; i < kPromptStyleCount; ++i) {
            if (ImGui::Selectable(kPromptLabels[i], i == prompt_sel) && i != prompt_sel) {
                prompts.style = static_cast<PromptStyle>(i);
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    tip("The button icons in the game's prompts: the game's own Switch icons, Xbox, PlayStation, Steam Controller, Steam Deck, keyboard and mouse, or Auto to follow the device you used last.");
    if (prompts.style == PromptStyle::Auto) {
        ImGui::SameLine();
        ImGui::TextDisabled("showing %s", cstr(live.prompt_now));
    }
}

void pad_tab(const LiveStatus& live, Page& p) {   // the Gamepad tab of the Controls page
    PadSettings& pad = p.edit.pad;
    const bool by_label = pad.label_abxy;
    if (ImGui::RadioButton("Switch layout (right = A, bottom = B)", !by_label) && by_label) {
        pad.label_abxy = false;
        mark(p);
    }
    tip("Face buttons by position, as on a Switch pad: the right button confirms (Switch A), the bottom one goes back (Switch B).");
    if (ImGui::RadioButton("Xbox layout (A = A, B = B)", by_label) && !by_label) {
        pad.label_abxy = true;
        mark(p);
    }
    tip("Face buttons by the letter printed on them: the pad's A is Switch A wherever it sits (on an Xbox pad, the bottom button confirms).");

    ImGui::SeparatorText("Sticks and triggers");
    if (slider("Left stick dead zone", &pad.stick_deadzone, 0.0, 0.5, "%.2f")) mark(p);
    tip("The stick must move this far from the centre before it counts (a fraction of its travel). Applies before the game's own dead zone.");
    if (slider("Right stick dead zone", &pad.right_deadzone, 0.0, 0.5, "%.2f")) mark(p);
    tip("The same for the right stick (looking).");
    if (slider("Left stick max", &pad.stick_max, 0.5, 1.0, "%.2f")) mark(p);
    tip("How far the left stick must be pushed to count as fully pushed (a fraction of its travel). Lower it if your stick never reaches full speed (for example it tops out around 0.95). 1.00 turns it off.");
    if (slider("Right stick max", &pad.right_stick_max, 0.5, 1.0, "%.2f")) mark(p);
    tip("The same for the right stick (looking): lower it if the camera never turns at full speed.");
    if (ImGui::Checkbox("Invert look (right stick up looks down)", &pad.invert_y)) mark(p);
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", "Applies in play only; adds to the game's own invert option.");
    if (slider("Look sensitivity", &pad.look_sens, 0.25, 3.0, "%.2f", ImGuiSliderFlags_Logarithmic)) mark(p);
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", "Multiplies the right stick while playing. Below 1 the top turning speed is lower; above 1 you reach it with less stick.");
    if (slider("Trigger threshold", &pad.trigger_threshold, 0.05, 1.0, "%.2f")) mark(p);
    if (ImGui::Checkbox("Gyro (six-axis sensor)", &pad.gyro)) mark(p);
    if (slider("Gyro scale", &pad.gyro_scale, 0.1, 4.0, "%.2f", ImGuiSliderFlags_Logarithmic)) mark(p);
    if (ImGui::Checkbox("Rumble", &pad.rumble)) mark(p);
    if (slider("Rumble scale", &pad.rumble_scale, 0.0, 2.0, "%.2f")) mark(p);

    pad_buttons_group(p);
    quit_pad_group(p);

    ImGui::SeparatorText("Gamepad");
    if (live.pads == 0) ImGui::TextUnformatted("No gamepad open.");
    else ImGui::Text("%d open: %s", live.pads, live.pad_name.c_str());
    ImGui::Text("Gyro available: %s    Rumble available: %s", yes_no(live.pad_gyro), yes_no(live.pad_rumble));
}

// ---- Bindings -----------------------------------------------------------------------------------------------------------------------

std::string input_button_text(const Binding& b, bool waiting) {
    if (waiting) return "press a key...";
    if (b.in.kind == InKind::None) return "<none>";
    const std::string name = input_name(b.in);
    return name.empty() ? "?" : name;
}

// The quit key ([quit] key): opens the "Close the game?" prompt and never reaches the game. Part of the working copy.
void quit_key_group(UiHost& host, Page& p) {
    ImGui::SeparatorText("Quit shortcut");
    const std::string key = p.edit.quit.key.kind == InKind::Key ? input_name(p.edit.quit.key) : std::string();
    ImGui::Text("Quit key: %s", key.empty() ? "none" : key.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Change##quit_key")) {
        stop_rebind(host, p);
        stop_fps_capture(host, p);
        set_notice(p, "", false);
        p.quit_capture = true;
        host.begin_capture();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(p.edit.quit.key.kind == InKind::None);
    if (ImGui::Button("Clear##quit_key")) {
        if (p.quit_capture) stop_rebind(host, p);
        p.edit.quit.key = {};
        mark(p);
    }
    ImGui::EndDisabled();
    if (p.quit_capture) {
        ImGui::SameLine();
        ImGui::TextDisabled("press a keyboard key...");
    }
    tip("A keyboard key that asks \"Close the game?\". The key never reaches the game. The gamepad's combination is on the Gamepad tab.");
}

void bindings_tab(UiHost& host, Page& p) {
    if (ImGui::BeginCombo("Context", ctx_name(p.ctx))) {
        for (int c = 0; c < kCtxCount; ++c) {
            const Ctx ctx = static_cast<Ctx>(c);
            if (ImGui::Selectable(ctx_name(ctx), ctx == p.ctx) && ctx != p.ctx) {
                stop_rebind(host, p);
                p.ctx = ctx;
            }
        }
        ImGui::EndCombo();
    }

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;
    int remove = -1;
    if (ImGui::BeginTable("bindings", 5, kFlags, ImVec2(0.0f, px(320.0f)))) {
        ImGui::TableSetupColumn("Input");
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Trigger", ImGuiTableColumnFlags_WidthFixed, px(110.0f));
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, px(90.0f));
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, px(32.0f));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < p.edit.bindings.size(); ++i) {
            Binding& b = p.edit.bindings[i];
            if (b.ctx != p.ctx) continue;
            const int row = static_cast<int>(i);
            ImGui::PushID(row);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            const bool waiting = p.rebind == row;
            const std::vector<int> clash = conflicts_of(p, row);
            const std::string label = input_button_text(b, waiting) + "##in";
            if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, kPressed);
            else if (!clash.empty()) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.38f, 0.10f, 1.0f));
            const bool clicked = ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0.0f));
            if (waiting || !clash.empty()) ImGui::PopStyleColor();
            if (!clash.empty() && ImGui::IsItemHovered()) {
                std::string who;
                for (int c : clash) who += std::string(who.empty() ? "" : ", ") + act_label(p.edit.bindings[static_cast<std::size_t>(c)].act);
                ImGui::SetItemTooltip("Also bound here to: %s", who.c_str());
            }
            if (clicked) {
                stop_fps_capture(host, p);
                p.quit_capture = false;
                set_notice(p, "", false);
                p.rebind = row;
                host.begin_capture();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##act", act_label(b.act))) {
                for (int a = 0; a < kActCount; ++a) {
                    const Act act = static_cast<Act>(a);
                    if (ImGui::Selectable(act_label(act), act == b.act) && act != b.act) {
                        b.act = act;
                        mark(p);
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##trig", trig_name(b.trig))) {
                for (Trig t : kTrigChoices) {
                    if (ImGui::Selectable(trig_name(t), t == b.trig) && t != b.trig) {
                        b.trig = t;
                        mark(p);
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(3);
            const bool timed = b.trig == Trig::Tap || b.trig == Trig::Hold || b.trig == Trig::DoubleTap;
            ImGui::BeginDisabled(!timed);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (int_field("##ms", &b.ms, 0, 5000, 0, 0)) mark(p);
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("x")) remove = row;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (remove >= 0) {
        if (p.rebind == remove) stop_rebind(host, p);
        else if (p.rebind > remove) --p.rebind;
        p.edit.bindings.erase(p.edit.bindings.begin() + remove);
        mark(p);
    }

    if (ImGui::Button("Add binding")) {
        Binding b;
        b.in = InputId{};
        b.ctx = p.ctx;
        b.act = Act::Primary;
        b.trig = Trig::Press;
        b.ms = 0;
        p.edit.bindings.push_back(b);
        stop_fps_capture(host, p);
        p.quit_capture = false;
        set_notice(p, "", false);
        p.rebind = static_cast<int>(p.edit.bindings.size()) - 1;
        host.begin_capture();
        mark(p);
    }
    // Inputs used twice in one context with the same trigger (their rows are orange): both actions happen when it is pressed.
    int shown = 0;
    for (std::size_t i = 0; i < p.edit.bindings.size() && shown < 6; ++i) {
        const Binding& b = p.edit.bindings[i];
        if (b.ctx != p.ctx) continue;
        for (int c : conflicts_of(p, static_cast<int>(i))) {
            if (c < static_cast<int>(i)) continue;   // each pair once
            ImGui::TextColored(kOrange, "%s is bound to %s and to %s", input_name(b.in).c_str(), act_label(b.act),
                               act_label(p.edit.bindings[static_cast<std::size_t>(c)].act));
            if (++shown >= 6) break;
        }
    }
    ImGui::TextWrapped("Click an input and press the key or mouse button (or turn the wheel) to bind it, Escape cancels. "
                       "Bindings of a context win over the 'all' layer for the same input. Action 'none' switches an input off in that context.");
    quit_key_group(host, p);
}

// ---- Switch layout ------------------------------------------------------------------------------------------------------------------

void switch_tab(Page& p) {
    // The cycling actions by the game's names (mpr: beam_next and beam_prev ... beam_1 to beam_4).
    const std::string help = std::string("What each action presses on the Switch pad: buttons A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LSTICK RSTICK "
                                         "and stick directions LS_UP LS_DOWN LS_LEFT LS_RIGHT RS_UP RS_DOWN RS_LEFT RS_RIGHT, joined by +. ") +
                             act_info(Act::SetANext).name + " and " + act_info(Act::SetAPrev).name + " have no fixed output (they pick " +
                             act_info(Act::SetA1).name + " to " + act_info(Act::SetA4).name + ").";
    ImGui::TextWrapped("%s", help.c_str());
    if (ImGui::BeginTable("switch_out", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, px(150.0f));
        ImGui::TableSetupColumn("Output");
        ImGui::TableHeadersRow();
        for (int a = 1; a < kActCount; ++a) {
            const bool bad = !p.out_err[a].empty();
            ImGui::PushID(a);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(act_info(static_cast<Act>(a)).name);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (bad) {
                ImGui::PushStyleColor(ImGuiCol_Border, kRed);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            }
            if (ImGui::InputText("##out", p.out_text[a], sizeof p.out_text[a])) {
                Output o;
                std::string err;
                if (parse_output(p.out_text[a], &o, &err)) {
                    p.edit.out[a] = o;
                    p.out_err[a].clear();
                    mark(p);
                } else {
                    p.out_err[a] = err;   // the previous valid output stays in the working copy
                }
            }
            if (bad) {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
            if (!p.out_err[a].empty()) ImGui::TextColored(kRed, "%s", p.out_err[a].c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset this table to the defaults")) {
        for (int a = 1; a < kActCount; ++a) {
            Output o;
            if (parse_output(act_info(static_cast<Act>(a)).default_out, &o)) p.edit.out[a] = o;
        }
        refresh_out_text(p);
        mark(p);
    }
}

// ---- Status -------------------------------------------------------------------------------------------------------------------------

// Live view only: nothing here touches the working copy.
void status_tab(const LiveStatus& live) {
    ImGui::Text("Context: %s", ctx_name(live.ctx));
    ImGui::Text("Aim: %s", cstr(live.aim));
    ImGui::Text("Mouse captured: %s", yes_no(live.mouse_captured));
    ImGui::Text("Pitch: %.1f degrees", live.pitch_deg);
    if (live.input_calls == 0) ImGui::TextDisabled("ProcessInput calls: 0 (the game has not reached gameplay yet)");
    else ImGui::Text("ProcessInput calls: %llu", static_cast<unsigned long long>(live.input_calls));

    ImGui::SeparatorText("Switch pad output");
    for (int i = 0; i < kSwitchButtonCount; ++i) {
        const bool on = (live.out.buttons & hid::btn(kSwitchButtons[i].bit)) != 0;
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? kPressed : kReleased);
        ImGui::Button(kSwitchButtons[i].name, ImVec2(px(56.0f), 0.0f));
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (i % 8 != 7) ImGui::SameLine();
    }
    ImGui::Text("LS (%5.2f, %5.2f)   RS (%5.2f, %5.2f)", live.out.lx, live.out.ly, live.out.rx, live.out.ry);

    ImGui::SeparatorText("Gamepad");
    if (live.pads == 0) ImGui::TextUnformatted("No gamepad open.");
    else ImGui::Text("%d open: %s", live.pads, live.pad_name.c_str());
    ImGui::Text("Gyro: %s    Rumble: %s", yes_no(live.pad_gyro), yes_no(live.pad_rumble));
}

// ---- Controls and Game --------------------------------------------------------------------------------------------------------------

// The Controls page: the button prompt style on top, then mouse, keyboard bindings, gamepad, the Switch layout and the live status.
void controls_page(UiHost& host, const LiveStatus& live, Page& p) {
    prompts_row(live, p);
    ImGui::Spacing();
    if (ImGui::BeginTabBar("controls_tabs")) {
        if (ImGui::BeginTabItem("Mouse")) {
            mouse_tab(p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Keyboard and mouse buttons")) {
            bindings_tab(host, p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Gamepad")) {
            pad_tab(live, p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Switch layout")) {
            if (!p.out_tab_open) refresh_out_text(p);
            p.out_tab_open = true;
            switch_tab(p);
            ImGui::EndTabItem();
        } else {
            p.out_tab_open = false;
        }
        if (ImGui::BeginTabItem("Status")) {
            status_tab(live);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}


}  // namespace ui
}  // namespace nxrt::play
