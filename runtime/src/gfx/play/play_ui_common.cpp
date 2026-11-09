// SPDX-License-Identifier: GPL-3.0-or-later
// Shared state helpers and widgets of the settings page tabs, see play_ui_page.h.
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

void mark(Page& p) { p.dirty = true; }

const char* yes_no(bool b) { return b ? "yes" : "no"; }
const char* cstr(const char* s) { return s ? s : ""; }
const char* act_label(Act a) { return a == Act::None ? "none (input off)" : act_info(a).name; }

// Stops our rebind (a binding row or the quit key). The host's capture is cancelled only when it is ours.
void stop_rebind(UiHost& host, Page& p) {
    if ((p.rebind >= 0 || p.quit_capture) && host.capturing()) host.cancel_capture();
    p.rebind = -1;
    p.quit_capture = false;
}

// Ends the frame rate counter's hotkey capture. The host's capture is cancelled only when it is ours.
void stop_fps_capture(UiHost& host, Page& p) {
    if (p.fps_capture && host.capturing()) host.cancel_capture();
    p.fps_capture = false;
}

// The bindings of the working copy that share row `row`'s input in the same context and trigger: pressing that input would do both.
std::vector<int> conflicts_of(const Page& p, int row) {
    std::vector<int> out;
    const Binding& a = p.edit.bindings[static_cast<std::size_t>(row)];
    if (a.in.kind == InKind::None) return out;
    for (std::size_t i = 0; i < p.edit.bindings.size(); ++i) {
        const Binding& b = p.edit.bindings[i];
        if (static_cast<int>(i) != row && b.ctx == a.ctx && b.in == a.in && b.trig == a.trig) out.push_back(static_cast<int>(i));
    }
    return out;
}

void set_notice(Page& p, const std::string& text, bool warn) {
    p.notice = text;
    p.notice_warn = warn;
}

// Takes the captured input, if the host has one. Results are taken every frame, so a stale one never reaches a later rebind.
// Only one capture runs at a time (the binding row or the frame rate hotkey), so a result goes to whichever one is waiting.
// The key that opens the menu cannot be bound (and the frame rate hotkey cannot be bound to an action): the page says so and keeps waiting.
void poll_capture(UiHost& host, Page& p) {
    InputId id;
    if (host.capture_result(&id)) {
        const std::string name = input_name(id);
        const bool reserved = host.reserved_key(id);
        if (p.fps_capture) {
            // The frame rate hotkey takes keyboard keys only; a mouse button or a wheel notch is dropped and the capture ends.
            if (reserved) {
                set_notice(p, name + " opens this menu and cannot be the frame rate hotkey. Press another key, Escape cancels.", true);
                host.begin_capture();
                return;
            }
            p.fps_capture = false;
            if (id.kind == InKind::Key) {
                Settings cur = host.settings();
                cur.fps.key = id;
                host.apply(cur);   // live: not part of the working copy
            }
        } else if (p.quit_capture) {
            // The quit key: keyboard keys only (a mouse button or a wheel notch is dropped and the capture ends), part of the working copy.
            const Settings cur = host.settings();
            if (reserved || (id.kind == InKind::Key && id == cur.fps.key)) {
                set_notice(p, name + (reserved ? " opens this menu" : " is the frame rate counter hotkey") + " and cannot be the quit key. Press another key, Escape cancels.", true);
                host.begin_capture();
                return;
            }
            p.quit_capture = false;
            if (id.kind == InKind::Key) {
                p.edit.quit.key = id;
                mark(p);
                bool used = false;
                for (const Binding& b : p.edit.bindings) used = used || b.in == id;
                if (used) set_notice(p, name + " is the quit key now; its bindings no longer reach the game.", true);
                else set_notice(p, name + " is the quit key.", false);
            }
        } else if (p.rebind >= 0 && p.rebind < static_cast<int>(p.edit.bindings.size())) {
            const Settings cur = host.settings();
            if (reserved || (id.kind == InKind::Key && (id == cur.fps.key || id == p.edit.quit.key))) {
                set_notice(p, name + (reserved ? " opens this menu" : id == cur.fps.key ? " is the frame rate counter hotkey" : " is the quit key") +
                                  " and cannot be bound. Press another input, Escape cancels.", true);
                host.begin_capture();
                return;
            }
            p.edit.bindings[static_cast<std::size_t>(p.rebind)].in = id;
            mark(p);
            const std::vector<int> other = conflicts_of(p, p.rebind);
            if (other.empty()) {
                set_notice(p, name + " bound.", false);
            } else {
                const Binding& o = p.edit.bindings[static_cast<std::size_t>(other[0])];
                set_notice(p, name + " is also bound to " + act_label(o.act) + " (" + ctx_name(o.ctx) + "): both happen when it is pressed.", true);
            }
        }
        p.rebind = -1;
    } else if ((p.rebind >= 0 || p.fps_capture || p.quit_capture) && !host.capturing()) {
        p.rebind = -1;   // Escape, or the host ended the capture without a result
        p.fps_capture = false;
        p.quit_capture = false;
        set_notice(p, "Rebinding cancelled.", false);
    }
}

// Fills the Switch layout fields from the working copy. Also clears their errors.
void refresh_out_text(Page& p) {
    for (int a = 1; a < kActCount; ++a) {   // Act::None has no row
        const std::string text = output_text(p.edit.out[a]);
        std::snprintf(p.out_text[a], sizeof p.out_text[a], "%s", text.c_str());
        p.out_err[a].clear();
    }
}

// A slider on a double; true when the user changed it.
bool slider(const char* label, double* v, double lo, double hi, const char* format, ImGuiSliderFlags flags) {
    float f = static_cast<float>(*v);
    if (!ImGui::SliderFloat(label, &f, static_cast<float>(lo), static_cast<float>(hi), format, flags)) return false;
    *v = f;
    return true;
}

// An int field. The value is clamped when the field is left, not while typing (typing "1280" must not turn into "320" on the way).
bool int_field(const char* label, int* v, int lo, int hi, int step, int step_fast) {
    bool changed = ImGui::InputInt(label, v, step, step_fast);
    if (ImGui::IsItemDeactivated()) {
        const int c = std::clamp(*v, lo, hi);
        changed = changed || c != *v;
        *v = c;
    }
    return changed;
}

// One side of the fixed render size: 0 (follow the window) or 320..16384. A value in between is clamped to 320 when the field is left.
bool render_size_field(const char* label, int* v) {
    bool changed = ImGui::InputInt(label, v, 0, 0);
    if (ImGui::IsItemDeactivated()) {
        const int c = *v <= 0 ? 0 : std::clamp(*v, 320, 16384);
        changed = changed || c != *v;
        *v = c;
    }
    return changed;
}

// Sizes are in units of the 16 px reference font, so the page follows the overlay's UI scale (overlay.cpp).
float px(float v) { return v * ImGui::GetFontSize() / 16.0f; }

// A hover tooltip for the item drawn last.
void tip(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", text);
}

// Marks the item drawn last as one that only takes effect at the next start.
void restart_badge() {
    ImGui::SameLine();
    ImGui::TextColored(kOrange, "restart");
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", "Applies on restart: the game reads this once, when it starts.");
}

// The combo entry for an aspect value. Auto wins for 0; Custom is shown when picked or when the value is no preset.
int aspect_index(double aspect, bool custom) {
    if (aspect <= 0.0) return 0;
    if (custom) return kAspectCustom;
    for (int i = 1; i < kAspectCustom; ++i)
        if (std::fabs(aspect - kAspectValues[i]) < 1e-6) return i;
    return kAspectCustom;
}


}  // namespace ui
}  // namespace nxrt::play
