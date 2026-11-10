// SPDX-License-Identifier: GPL-3.0-or-later
// The button prompt preview of the F1 Controller tab: see play_ui_prompt_preview.h.
#include "play_ui_prompt_preview.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"   // RegisterUserTexture
#include "prompt_resolve.h"
#include "prompts.h"

namespace nxrt::play::ui {

namespace {

constexpr int kAtlasCols = 32;

// Every entry of the icon pack in one RGBA texture, which the renderer backend uploads at the next frame (ImGui's user texture list). Built
// once, on the first draw of a preview; tied to the ImGui context it was registered with. The overlay makes one context per run: should a
// new one appear, a new texture is built and the old one is left alone (its backend destroyed the GPU copy at shutdown).
struct Atlas {
    ImGuiContext* ctx = nullptr;
    ImTextureData* tex = nullptr;
    std::map<std::string, ImVec4> uv;   // entry -> (u0, v0, u1, v1)
};

Atlas& atlas() {
    static Atlas a;
    return a;
}

const Atlas* ready_atlas() {
    Atlas& a = atlas();
    ImGuiContext* const cur = ImGui::GetCurrentContext();
    if (a.tex && a.ctx == cur) return &a;
    const IconPack* pack = prompts_pack();
    if (!pack || pack->size() == 0) return nullptr;
    const std::vector<std::string> names = pack->names();
    const int iw = static_cast<int>(pack->width()), ih = static_cast<int>(pack->height());
    const int rows = (static_cast<int>(names.size()) + kAtlasCols - 1) / kAtlasCols;
    auto* tex = IM_NEW(ImTextureData)();
    tex->Create(ImTextureFormat_RGBA32, kAtlasCols * iw, rows * ih);
    tex->UseColors = true;
    a.uv.clear();
    for (size_t i = 0; i < names.size(); ++i) {
        const int x = static_cast<int>(i % kAtlasCols) * iw, y = static_cast<int>(i / kAtlasCols) * ih;
        const uint8_t* px = pack->find(names[i]);
        for (int row = 0; row < ih; ++row)
            std::memcpy(tex->GetPixelsAt(x, y + row), px + static_cast<size_t>(row) * iw * 4, static_cast<size_t>(iw) * 4);
        a.uv[names[i]] = ImVec4(static_cast<float>(x) / tex->Width, static_cast<float>(y) / tex->Height,
                                static_cast<float>(x + iw) / tex->Width, static_cast<float>(y + ih) / tex->Height);
    }
    ImGui::RegisterUserTexture(tex);
    a.tex = tex;
    a.ctx = cur;
    return &a;
}

// The atlas cell of an entry; a key without an icon of its own shows the plain key, as in the game (prompts.cpp).
const ImVec4* cell_of(const Atlas& a, const std::string& name) {
    auto it = a.uv.find(name);
    if (it == a.uv.end() && name.compare(0, 4, "kbm/") == 0) it = a.uv.find("kbm/any");
    return it == a.uv.end() ? nullptr : &it->second;
}

}  // namespace

void prompt_preview_strip(PromptStyle style, const Settings& s) {
    if (style == PromptStyle::Switch) {
        ImGui::TextDisabled("Preview: the game's own Switch icons.");
        return;
    }
    const std::vector<PromptPreviewItem> items = prompt_preview(style, s);
    if (items.empty()) return;
    const Atlas* a = ready_atlas();
    if (!a) {
        ImGui::TextDisabled("Preview: no icon pack (prompt_icons.pak) was found.");
        return;
    }
    ImGui::TextDisabled("Preview");
    const ImGuiStyle& gs = ImGui::GetStyle();
    const float icon = ImGui::GetFrameHeight() * 1.4f;
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::PushID("prompt_preview");
    for (size_t i = 0; i < items.size(); ++i) {
        const PromptPreviewItem& it = items[i];
        const float cell = std::max(icon, ImGui::CalcTextSize(it.button).x);
        if (i > 0) {
            ImGui::SameLine(0.0f, gs.ItemSpacing.x * 2.0f);
            if (ImGui::GetCursorPosX() + cell > right) ImGui::NewLine();
        }
        ImGui::BeginGroup();
        const float x0 = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(x0 + (cell - icon) * 0.5f);
        const ImVec4* uv = it.icon.empty() ? nullptr : cell_of(*a, it.icon);
        if (uv) {
            ImGui::Image(a->tex->GetTexRef(), ImVec2(icon, icon), ImVec2(uv->x, uv->y), ImVec2(uv->z, uv->w));
        } else {
            // the game keeps its own icon for this button (the controller has none, or nothing presses it)
            ImGui::Dummy(ImVec2(icon, icon));
            const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(mn, mx, ImGui::GetColorU32(ImGuiCol_TextDisabled), icon * 0.2f);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", uv ? ("Switch " + std::string(it.button) + " shows " + it.icon).c_str()
                                       : ("Switch " + std::string(it.button) + " keeps the game's own icon").c_str());
        ImGui::SetCursorPosX(x0 + (cell - ImGui::CalcTextSize(it.button).x) * 0.5f);
        ImGui::TextUnformatted(it.button);
        ImGui::EndGroup();
    }
    ImGui::PopID();
}

}  // namespace nxrt::play::ui
