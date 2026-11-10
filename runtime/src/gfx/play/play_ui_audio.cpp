// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode settings page: the Audio tab.
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

// ---- Audio --------------------------------------------------------------------------------------------------------------------------

void load_audio_devices(Page& p) {
    p.audio_devices.clear();
    for (const audio::OutputDevice& d : audio::list_output_devices())
        if (!d.name.empty()) p.audio_devices.push_back(d.name);
    p.audio_loaded = true;
}

void audio_page(Page& p) {
    AudioSettings& a = p.edit.audio;
    ImGui::SeparatorText("Volume");
    float pct = static_cast<float>(a.volume * 100.0);
    if (ImGui::SliderFloat("Master volume", &pct, 0.0f, 100.0f, "%.0f%%")) {
        a.volume = std::clamp(static_cast<double>(pct), 0.0, 100.0) / 100.0;
        mark(p);
    }
    tip("The volume of everything the game plays, applied as a gain in the audio output. [audio] volume in the ini or NXRT_AUDIO_VOLUME wins at the start.");
    if (ImGui::Checkbox("Mute when the window is not in focus", &a.mute_unfocused)) mark(p);

    ImGui::SeparatorText("Output");
    if (!p.audio_loaded) load_audio_devices(p);
    const bool is_default = a.device.empty() || a.device == "default";
    const char* preview = is_default ? "System default" : a.device.c_str();
    if (ImGui::BeginCombo("Output device", preview)) {
        if (ImGui::Selectable("System default", is_default) && !is_default) {
            a.device = "default";
            mark(p);
        }
        for (const std::string& name : p.audio_devices) {
            if (ImGui::Selectable(name.c_str(), !is_default && a.device == name) && a.device != name) {
                a.device = name;
                mark(p);
            }
        }
        ImGui::EndCombo();
    }
    restart_badge();
    tip("Where the sound plays. The game opens its audio device once, so a new choice takes effect at the next start. [audio] device in the ini wins over this.");
    if (ImGui::Button("Refresh the device list")) load_audio_devices(p);
    ImGui::SameLine();
    ImGui::TextDisabled("%d devices", static_cast<int>(p.audio_devices.size()));
    if (!is_default && std::find(p.audio_devices.begin(), p.audio_devices.end(), a.device) == p.audio_devices.end())
        ImGui::TextColored(kOrange, "'%s' is not connected: the system default is used.", a.device.c_str());
}


}  // namespace ui
}  // namespace nxrt::play
