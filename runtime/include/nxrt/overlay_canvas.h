// SPDX-License-Identifier: GPL-3.0-or-later
// A layer the game draws over its own picture (debug views): lines, rectangle outlines, filled panels and text. Coordinates are pixels of the
// game's picture rectangle as it is on screen (the letterbox bars left out): 0, 0 is its top left corner, width() x height() its size, so a
// point the game projects to its picture lands where the picture shows it whatever the window shape. Drawing outside the rectangle is
// clipped to it. Colours are 0xRRGGBBAA.
//
// The overlay owns the canvas (overlay.h Overlay::set_game_layer): it asks the layer's active() once per presented frame and, when true, calls
// draw() with the canvas inside its frame, under the menu and the prompts. Both run on the overlay's presenting thread, not the game's: the
// layer reads what it draws from state it published from the game thread itself. The layer is drawn into screenshots too.
#pragma once
#include <cstdint>

namespace nxrt::gfx {

class OverlayCanvas {
public:
    virtual ~OverlayCanvas() = default;
    virtual float width() const = 0;          // the picture rectangle, pixels
    virtual float height() const = 0;
    virtual float line_height() const = 0;    // a line of text, pixels (the overlay's text scales with the frame)
    virtual void line(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness = 1.f) = 0;
    virtual void rect(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness = 1.f) = 0;   // outline
    virtual void fill(float x0, float y0, float x1, float y1, uint32_t rgba) = 0;
    virtual void text(float x, float y, uint32_t rgba, const char* s) = 0;   // x, y = the top left of the first line; '\n' starts a line
    virtual float text_width(const char* s) const = 0;
};

// The game's layer: active() says whether there is anything to draw this frame (false or null: the overlay does no work for it).
struct GameLayer {
    bool (*active)() = nullptr;
    void (*draw)(OverlayCanvas& c) = nullptr;
};

}  // namespace nxrt::gfx
