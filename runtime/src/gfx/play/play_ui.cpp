// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode settings page: the "Controls" window of the F1 overlay. The contract is play_ui.h. The page edits a working copy of the
// settings: Apply and save hands it to the host, Revert and Factory defaults replace the copy, and nothing else reaches the game.
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

using namespace ui;

namespace {

// Changes made by the page that must hold before the copy is handed to the host: no bindings without an input, values in range.
void sanitize(Settings& s) {
    std::vector<Binding>& v = s.bindings;
    v.erase(std::remove_if(v.begin(), v.end(), [](const Binding& b) { return b.in.kind == InKind::None; }), v.end());
    for (Binding& b : v) b.ms = std::clamp(b.ms, 0, 5000);
    s.display.width = std::clamp(s.display.width, 320, 16384);
    s.display.height = std::clamp(s.display.height, 320, 16384);
    // A fixed render size is 0 (follow the window) or 320..16384 per side; the settings file rejects anything else.
    s.display.render_width = s.display.render_width <= 0 ? 0 : std::clamp(s.display.render_width, 320, 16384);
    s.display.render_height = s.display.render_height <= 0 ? 0 : std::clamp(s.display.render_height, 320, 16384);
    // Extras: peak and paper white are 0 (auto) or in range; the upscale mode is kept as it is, even for the modes not yet offered.
    extras::HdrSettings& h = s.extras.hdr;
    h.peak_nits = h.peak_nits <= 0 ? 0 : std::clamp(h.peak_nits, 200.0, 10000.0);
    h.paper_white_nits = h.paper_white_nits <= 0 ? 0 : std::clamp(h.paper_white_nits, 80.0, 500.0);
    h.highlight_boost = std::clamp(h.highlight_boost, 0.0, 2.0);
    s.extras.upscale.sharpness = std::clamp(s.extras.upscale.sharpness, 0.0, 1.0);
    s.extras.picture = extras::picture_clamped(s.extras.picture);
    s.audio.volume = std::clamp(s.audio.volume, 0.0, 1.0);
    s.display.monitor = std::clamp(s.display.monitor, 0, 32);
    if (s.audio.device.empty()) s.audio.device = "default";
}

// The frame rate counter is live (the hotkey changes it in the game), so the working copy always takes it from the host, never from a
// stale copy: Apply and save, Revert and Factory defaults leave the counter as it is.
void apply_now(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit.fps = host.settings().fps;
    sanitize(p.edit);
    host.apply(p.edit);
    p.dirty = false;
}

void revert_to_saved(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit = host.settings();
    p.edit.fps = host.settings().fps;
    extras::set_picture_live(p.edit.extras.picture);   // the colour sliders preview live: back to the saved values
    refresh_out_text(p);
    p.dirty = false;
}

void load_factory(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit = host.factory();
    p.edit.fps = host.settings().fps;
    extras::set_picture_live(p.edit.extras.picture);
    refresh_out_text(p);
    mark(p);
}

}  // namespace

void draw_settings_page(UiHost& host) {
    static Page page;
    Page& p = page;
    static uint64_t seen_game_edits = 0;
    if (const uint64_t n = game_menu_edits(); n != seen_game_edits && !p.dirty) {   // changed in the game's own menu: F1's Apply must not put the old values back
        seen_game_edits = n;
        p.loaded = false;
    }
    if (!p.loaded) {
        p.edit = host.settings();
        p.edit.fps = host.settings().fps;
        refresh_out_text(p);
        p.loaded = true;
    }
    poll_capture(host, p);
    const LiveStatus live = host.status();

    if (host.capturing()) ImGui::TextColored(kYellow, "Press the key or mouse button for the binding, Escape cancels");
    else if (!p.notice.empty()) ImGui::TextColored(p.notice_warn ? kOrange : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", p.notice.c_str());
    ImGui::TextDisabled("Settings file: %s", host.settings_path().c_str());
    if (p.dirty) {
        ImGui::SameLine();
        ImGui::TextColored(kOrange, "unsaved changes");
    }
    if (ImGui::Button("Apply and save")) apply_now(host, p);
    tip("Applies the changes to the running game (items marked 'restart' wait for the next start) and writes them to the settings file.");
    ImGui::SameLine();
    if (ImGui::Button("Revert")) revert_to_saved(host, p);
    tip("Drops the changes made since the last Apply and save.");
    ImGui::SameLine();
    if (ImGui::Button("Factory defaults")) load_factory(host, p);
    tip("Loads the factory settings into the page. Nothing changes until Apply and save.");
    ImGui::SameLine();
    if (ImGui::Button("Quit game")) host.request_quit_prompt();
    tip("Asks before closing the game. Unsaved settings changes are lost; the game's own save is not touched - save in the game first.");
    ImGui::TextDisabled("Controller: D-pad or left stick move, A choose, B back, + close.");
    ImGui::Separator();

    ImGui::PushItemWidth(std::max(px(200.0f), ImGui::GetContentRegionAvail().x * 0.42f));
    if (ImGui::BeginTabBar("settings_pages")) {
        if (ImGui::BeginTabItem("Display")) {
            display_page(host, live, p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Graphics")) {
            graphics_page(p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            controls_page(host, live, p);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            audio_page(p);
            ImGui::EndTabItem();
        }
        if (const GamePlay* gp = game_play(); gp && gp->tweaks_tab && ImGui::BeginTabItem("Game")) {   // the game's optional looks and start-up tweaks
            if (gp->tweaks_tab(p.edit)) mark(p);
            if (fastboot_ui(p.edit.fastboot)) mark(p);   // start-up and menu tweaks (play_ui_fastboot.cpp)
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("About")) {
            draw_about_page(host.settings_path());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
}

}  // namespace nxrt::play
