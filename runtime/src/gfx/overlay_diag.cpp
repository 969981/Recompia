// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui overlay: the Diagnostics window and the menu panels.
#include "overlay_impl.h"

namespace nxrt::gfx {
using namespace overlay_detail;

namespace {
const double kFpsChoices[] = {0, 30, 60, 90, 120, 144, 165, 240, 360};

}  // namespace

void Overlay::Impl::build_ui() {
    Renderer& r = *ctx.renderer;
    // First-use layout, in units of the UI scale (update_scale): the panels (the settings menu) sit at the top left, the diagnostics window
    // at the top right and folded up. The player moves them; nothing is remembered between runs.
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float k = ui_scale > 0.f ? ui_scale : 1.f;
    const float diag_w = std::min(420.f * k, std::max(200.f, disp.x - 32.f * k));
    ImGui::SetNextWindowPos(ImVec2(std::max(16.f * k, disp.x - diag_w - 16.f * k), 16.f * k), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(diag_w, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Diagnostics")) {
        const GpuInfo& g = ctx.device->gpu();
        ImGui::Text("GPU: %s", g.name.c_str());
        ImGui::Text("Vulkan 1.3, driver 0x%08x, validation %s%s", g.driver_version, ctx.device->validation_enabled() ? "on" : "off",
                    ctx.device->headless() ? ", headless" : "");
        const VkExtent2D e = r.extent();
        ImGui::Text("Target: %ux%u, %u images, %u frames in flight", e.width, e.height, r.image_count(), r.frames_in_flight());

        ImGui::SeparatorText("Presentation");
        const PresentMode modes[] = {PresentMode::Fifo, PresentMode::FifoRelaxed, PresentMode::Mailbox, PresentMode::Immediate};
        int cur = static_cast<int>(r.requested_present_mode());
        if (ImGui::BeginCombo("Present mode", present_mode_name(modes[cur]))) {
            for (int i = 0; i < 4; ++i) {
                const bool ok = r.present_mode_supported(modes[i]);
                ImGui::BeginDisabled(!ok);
                if (ImGui::Selectable(present_mode_name(modes[i]), i == cur)) { want_mode = true; new_mode = modes[i]; }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (r.present_mode() != r.requested_present_mode()) ImGui::TextDisabled("in use: %s (requested mode unavailable)", present_mode_name(r.present_mode()));

        if (ctx.limiter) {
            char label[32];
            const double t = ctx.limiter->target_fps();
            std::snprintf(label, sizeof label, t > 0 ? "%.0f fps" : "unlimited", t);
            if (ImGui::BeginCombo("Frame cap", label)) {
                for (double v : kFpsChoices) {
                    std::snprintf(label, sizeof label, v > 0 ? "%.0f fps" : "unlimited", v);
                    if (ImGui::Selectable(label, v == t)) ctx.limiter->set_target_fps(v);
                }
                ImGui::EndCombo();
            }
            const float hz = platform::has_window() ? platform::display_refresh_hz() : 0.f;
            if (hz > 0) {
                ImGui::TextDisabled("display %.2f Hz; VRR-friendly cap %.1f fps", hz, suggest_vrr_cap(hz));
                if (ImGui::SmallButton("Use VRR cap")) ctx.limiter->set_target_fps(suggest_vrr_cap(hz));
            }
        }

        if (ctx.has_window && platform::has_window()) {
            ImGui::SeparatorText("Window");
            const auto fm = platform::fullscreen_mode();
            if (ImGui::RadioButton("Windowed", fm == platform::FullscreenMode::Windowed)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Windowed; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Borderless", fm == platform::FullscreenMode::Borderless)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Borderless; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Exclusive", fm == platform::FullscreenMode::Exclusive)) { want_fullscreen = true; new_fullscreen = platform::FullscreenMode::Exclusive; }
            bool rel = platform::relative_mouse();
            if (ImGui::Checkbox("Raw mouse (relative mode)", &rel)) platform::set_relative_mouse(rel);
            ImGui::SameLine();
            ImGui::TextDisabled(platform::relative_mouse_is_raw() ? "(unaccelerated)" : "(system scaling!)");
        }

        ImGui::SeparatorText("Timing");
        const RendererStats rs = r.stats();
        const float dt_ms = ImGui::GetIO().DeltaTime * 1000.f;
        frame_ms[frame_ms_pos] = dt_ms;
        frame_ms_pos = (frame_ms_pos + 1) % 120;
        ImGui::Text("CPU frame %.2f ms (%.0f fps)   GPU %.2f ms (avg %.2f)", dt_ms, dt_ms > 0 ? 1000.f / dt_ms : 0.f, rs.gpu_ms_last, rs.gpu_ms_avg);
        ImGui::PlotLines("##ft", frame_ms, 120, frame_ms_pos, nullptr, 0.f, 33.f, ImVec2(0, 48));
        ImGui::Text("frames %llu, swapchains %llu, out-of-date %llu, screenshots %llu", static_cast<unsigned long long>(rs.frames),
                    static_cast<unsigned long long>(rs.swapchain_creates), static_cast<unsigned long long>(rs.out_of_date),
                    static_cast<unsigned long long>(rs.screenshots));
        const ValidationStats vs = ctx.device->validation_stats();
        ImGui::Text("validation: %llu errors, %llu warnings", static_cast<unsigned long long>(vs.errors), static_cast<unsigned long long>(vs.warnings));

        ImGui::SeparatorText("NVN front end");
        ImGui::Text("%zu entry points, %zu implemented; stub calls %llu (%zu distinct)", nvn::entry_count(), nvn::implemented_count(),
                    static_cast<unsigned long long>(nvn::stub_calls_total()), nvn::stubs_called_distinct());
        if (ImGui::TreeNode("Most called stubs")) {
            auto calls = nvn::called_entries();
            int shown = 0;
            for (auto& c : calls) {
                if (c.implemented) continue;
                ImGui::Text("%8llu  %s", static_cast<unsigned long long>(c.calls), c.entry->name);
                if (++shown >= 12) break;
            }
            if (!shown) ImGui::TextDisabled("none called");
            ImGui::TreePop();
        }
    }
    ImGui::End();
    for (auto& p : panels) {
        ImGui::SetNextWindowPos(ImVec2(16.f * k, 16.f * k), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(std::min(760.f * k, std::max(240.f, disp.x - 32.f * k)), std::min(680.f * k, std::max(200.f, disp.y - 32.f * k))),
                                 ImGuiCond_FirstUseEver);
        if (ImGui::Begin(p.title.c_str())) p.fn();
        ImGui::End();
    }
}


}  // namespace nxrt::gfx
