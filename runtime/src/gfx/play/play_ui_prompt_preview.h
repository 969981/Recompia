// SPDX-License-Identifier: GPL-3.0-or-later
// The F1 Controller tab's preview of a button prompt style: each Switch button with the icon the style shows for it (prompt_preview), drawn
// from the icon pack (prompts_pack) uploaded once as one ImGui texture.
#pragma once

#include "play_types.h"

namespace nxrt::play::ui {

// Draws the preview row for `style` with the pad remap and bindings of `s` (inside an ImGui window, current context). Switch: a line saying
// the game's own icons show; no icon pack: a line saying so.
void prompt_preview_strip(PromptStyle style, const Settings& s);

}  // namespace nxrt::play::ui
