// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui overlay: the "Close the game?" prompt.
#include "overlay_impl.h"

namespace nxrt::gfx {
using namespace overlay_detail;

bool Overlay::Impl::quit_key_event(const input::Event& e) {
    if (e.type == input::EventType::KeyDown && !(e.flags & input::kFlagRepeat) && !quit_confirmed) {
        switch (e.u.key.scancode) {
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
            case SDL_SCANCODE_SPACE: quit_answer(true); break;
            case SDL_SCANCODE_ESCAPE: quit_answer(false); break;
            default: break;
        }
    }
    return true;
}

void Overlay::Impl::draw_quit_prompt() {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float k = ui_scale > 0.f ? ui_scale : 1.f;
    const float w = std::max(280.f, std::min(440.f * k, disp.x - 32.f * k));
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), disp, IM_COL32(0, 0, 0, 110));
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;
    bool cancelled = false;
    if (ImGui::Begin("Close the game?###nxrt_quit", nullptr, wf)) {
        const bool done = quit_confirmed.load();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(done ? "Closing ..." : "Progress since the game's last save is lost.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const ImVec2 bs((ImGui::GetContentRegionAvail().x - gap) * 0.5f, std::round(ImGui::GetFontSize() * 2.2f));
        ImGui::BeginDisabled(done);
        if (ImGui::Button("Quit", bs)) quit_answer(true);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", bs)) {
            quit_answer(false);
            cancelled = true;
        }
        if (ImGui::Button("Settings", ImVec2(bs.x * 2.f + gap, bs.y))) {
            quit_to_settings();
            cancelled = true;
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        // the face buttons are Switch buttons after the player's mapping, named as in the on-screen keyboard's help line
        const bool lab = kb_by_label.load(std::memory_order_relaxed);
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Pad: %s quit, %s cancel, %s settings", lab ? "A" : "A (right)", lab ? "B" : "B (bottom)", lab ? "X" : "X (top)");
        ImGui::TextDisabled("Keyboard: Enter quit, Esc cancel");
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    if (cancelled) clear_imgui_input(imgui);
}

void Overlay::open_quit_prompt() {
    if (!impl_->quit_open.exchange(true)) NXLOG_I(kTag, "quit prompt opened");
}
bool Overlay::quit_prompt_open() const { return impl_->quit_open; }
bool Overlay::take_quit_confirmed() { return impl_->quit_confirmed.exchange(false); }
bool Overlay::take_settings_wanted() { return impl_->settings_wanted.exchange(false); }


}  // namespace nxrt::gfx
