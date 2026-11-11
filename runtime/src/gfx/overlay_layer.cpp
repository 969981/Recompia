// SPDX-License-Identifier: GPL-3.0-or-later
// The game's layer over its picture (nxrt/overlay_canvas.h): the canvas over ImGui's background draw list, clipped to the picture rectangle,
// so the menu and the prompts drawn in the same frame stay on top of it.
#include "overlay_impl.h"

namespace nxrt::gfx {
namespace {

ImU32 im_color(uint32_t rgba) { return IM_COL32(rgba >> 24, (rgba >> 16) & 0xff, (rgba >> 8) & 0xff, rgba & 0xff); }

class Canvas final : public OverlayCanvas {
public:
    Canvas(ImDrawList* dl, float x, float y, float w, float h, float scale) : dl_(dl), x_(x), y_(y), w_(w), h_(h), scale_(scale) {}
    float width() const override { return w_; }
    float height() const override { return h_; }
    float line_height() const override { return ImGui::GetTextLineHeight(); }
    void line(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness) override {
        dl_->AddLine(ImVec2(x_ + x0, y_ + y0), ImVec2(x_ + x1, y_ + y1), im_color(rgba), thickness * scale_);
    }
    void rect(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness) override {
        dl_->AddRect(ImVec2(x_ + x0, y_ + y0), ImVec2(x_ + x1, y_ + y1), im_color(rgba), 0.f, 0, thickness * scale_);
    }
    void fill(float x0, float y0, float x1, float y1, uint32_t rgba) override {
        dl_->AddRectFilled(ImVec2(x_ + x0, y_ + y0), ImVec2(x_ + x1, y_ + y1), im_color(rgba));
    }
    void text(float x, float y, uint32_t rgba, const char* s) override { dl_->AddText(ImVec2(x_ + x, y_ + y), im_color(rgba), s); }
    float text_width(const char* s) const override { return ImGui::CalcTextSize(s).x; }

private:
    ImDrawList* dl_;
    float x_, y_, w_, h_, scale_;
};

}  // namespace

void Overlay::set_game_layer(const GameLayer& layer) {
    impl_->layer_active.store(layer.active);
    impl_->layer_draw.store(layer.draw);
}

void Overlay::set_picture_rect(float x, float y, float w, float h) {
    Impl& s = *impl_;
    if (!(w > 0.f) || !(h > 0.f)) return;
    s.pic_x.store(x, std::memory_order_relaxed);
    s.pic_y.store(y, std::memory_order_relaxed);
    s.pic_w.store(w, std::memory_order_relaxed);
    s.pic_h.store(h, std::memory_order_relaxed);
}

bool Overlay::Impl::layer_wanted() const {
    bool (*active)() = layer_active.load(std::memory_order_relaxed);
    return active && layer_draw.load(std::memory_order_relaxed) && active();
}

void Overlay::Impl::draw_game_layer(const FrameInfo& f) {
    void (*draw)(OverlayCanvas&) = layer_draw.load(std::memory_order_relaxed);
    if (!draw) return;
    const float fw = static_cast<float>(f.extent.width), fh = static_cast<float>(f.extent.height);
    const float x = pic_x.load(std::memory_order_relaxed) * fw, y = pic_y.load(std::memory_order_relaxed) * fh;
    const float w = pic_w.load(std::memory_order_relaxed) * fw, h = pic_h.load(std::memory_order_relaxed) * fh;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);
    Canvas c(dl, x, y, w, h, std::max(1.f, fh / 1080.f));   // line thickness grows with the frame like the overlay's text
    draw(c);
    dl->PopClipRect();
}

}  // namespace nxrt::gfx
