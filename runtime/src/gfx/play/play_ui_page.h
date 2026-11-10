// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to the settings page (play_ui*.cpp): the working copy and editing state of the page, and the helpers every tab shares.
// One file per page: play_ui_display.cpp, play_ui_graphics.cpp, play_ui_controls.cpp, play_ui_audio.cpp; play_ui.cpp is the shell around them.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

#include "imgui.h"
#include "nxrt/input_source.h"
#include "play_ui.h"

namespace nxrt::play::ui {

namespace hid = nxrt::hid;

inline const ImVec4 kRed(1.0f, 0.35f, 0.35f, 1.0f);
inline const ImVec4 kYellow(1.0f, 0.85f, 0.3f, 1.0f);
inline const ImVec4 kOrange(1.0f, 0.65f, 0.25f, 1.0f);
inline const ImVec4 kPressed(0.20f, 0.60f, 0.30f, 1.0f);
inline const ImVec4 kReleased(0.25f, 0.25f, 0.28f, 1.0f);

// The aspect ratio combo: Auto stores 0, the presets store their ratio, Custom shows a slider (index kAspectCustom, no value of its own).
inline constexpr const char* kAspectNames[] = {"Auto", "16:9", "16:10", "21:9", "32:9", "Custom"};
inline constexpr double kAspectValues[] = {0.0, 16.0 / 9.0, 1.6, 21.0 / 9.0, 32.0 / 9.0};
inline constexpr int kAspectCustom = 5;
inline constexpr int kAspectNameCount = static_cast<int>(sizeof kAspectNames / sizeof kAspectNames[0]);

// The working copy and the editing state of the page. One page per process.
struct Page {
    bool loaded = false;
    bool dirty = false;                    // the working copy differs from the settings in force (set by every change)
    Settings edit;
    Ctx ctx = Ctx::Play;               // the context shown in the Bindings tab
    int rebind = -1;                       // index into edit.bindings of the row waiting for an input; -1 = none
    bool link_pitch = true;                // Mouse tab: moving the horizontal sensitivity moves the vertical one too (local, not saved)
    bool out_tab_open = false;             // the Switch layout tab was open last frame; its fields are refilled when it opens
    char out_text[kActCount][128] = {};    // the text of each output field
    std::string out_err[kActCount];        // parse error of each field; empty when its text is valid
    bool aspect_custom = false;            // Display tab: the Custom aspect entry is picked (local, not saved)
    bool fps_capture = false;              // Display tab: the frame rate counter's hotkey waits for a key (local, not saved)
    bool quit_capture = false;             // Keyboard tab: the quit key waits for a key (local, not saved)
    std::string notice;                    // a line under the buttons: a refused or conflicting binding (cleared by the next rebind)
    bool notice_warn = false;              // the notice is a warning (drawn in orange), not information
    std::vector<std::string> monitors;     // Display page: "1: name (w x h @ hz)", refreshed every couple of seconds while the page is open
    double monitors_at = -1e9;
    std::vector<std::string> audio_devices;   // Audio page: output device names, read when the page opens and by its Refresh button
    bool audio_loaded = false;
};

void mark(Page& p);

const char* yes_no(bool b);
const char* cstr(const char* s);
const char* act_label(Act a);

void stop_rebind(UiHost& host, Page& p);
void stop_fps_capture(UiHost& host, Page& p);
std::vector<int> conflicts_of(const Page& p, int row);
void set_notice(Page& p, const std::string& text, bool warn);
void poll_capture(UiHost& host, Page& p);
void refresh_out_text(Page& p);

bool slider(const char* label, double* v, double lo, double hi, const char* format, ImGuiSliderFlags flags = 0);
bool int_field(const char* label, int* v, int lo, int hi, int step, int step_fast);
bool render_size_field(const char* label, int* v);
float px(float v);
void tip(const char* text);
void restart_badge();
int aspect_index(double aspect, bool custom);

// The pages (play_ui_<page>.cpp), drawn by draw_settings_page in the tab bar of play_ui.cpp.
void display_page(UiHost& host, const LiveStatus& live, Page& p);
void graphics_page(Page& p);
void controls_page(UiHost& host, const LiveStatus& live, Page& p);
void audio_page(Page& p);

}  // namespace nxrt::play::ui
