// SPDX-License-Identifier: GPL-3.0-or-later
// The F1 Graphics page's quality presets (games/pkp/pkp_quality.h quality_preset): the values each one writes (#122). The picture presets
// leave the render scale and the frame limit alone; the device presets set them too; every id of play_types.h quality_presets() but custom
// is known to the hook.
#include "test_framework.h"

#include <string>

#include "pkp_quality.h"
#include "play_types.h"

NXTEST_GROUP(pkp_quality)

namespace {

using nxrt::games::pkp::quality_preset;
using nxrt::play::DisplaySettings;

struct Want {
    const char* id;
    double view_distance, lod_bias;
    int anisotropy;
    int render_scale, fps_cap;   // -99: left as it was
};
constexpr int kKeep = -99;
constexpr Want kWant[] = {
    {"switch2", 1.0, 0.0, 0, kKeep, kKeep},
    {"high", 1.5, -0.5, 8, kKeep, kKeep},
    {"ultra", 2.0, -1.0, 16, kKeep, kKeep},
    {"steamdeck", 1.0, 0.0, 4, 75, 40},
    {"legion_go_s", 1.25, 0.0, 8, 80, 60},
    {"laptop", 1.0, 0.0, 4, 75, 60},
    {"desktop", 2.0, -0.5, 16, 0, -1},
};

}  // namespace

NXTEST(preset_values) {
    for (const Want& w : kWant) {
        DisplaySettings d;
        d.render_scale = 133;    // values no preset uses, to see which ones it writes
        d.fps_cap = 77;
        d.quality_preset = "custom";
        CHECK_MSG(quality_preset(w.id, d), w.id);
        CHECK_EQ(d.quality_preset, std::string(w.id));
        CHECK_MSG(d.view_distance == w.view_distance, w.id);
        CHECK_MSG(d.lod_bias == w.lod_bias, w.id);
        CHECK_EQ(d.anisotropy, w.anisotropy);
        CHECK_EQ(d.render_scale, w.render_scale == kKeep ? 133 : w.render_scale);
        CHECK_EQ(d.fps_cap, w.fps_cap == kKeep ? 77 : w.fps_cap);
    }
}

NXTEST(every_listed_preset_is_known_and_custom_is_not) {
    int count = 0;
    const nxrt::play::QualityPresetInfo* qs = nxrt::play::quality_presets(&count);
    for (int i = 0; i < count; ++i) {
        DisplaySettings d;
        const bool custom = std::string(qs[i].id) == "custom";
        CHECK_MSG(quality_preset(qs[i].id, d) == !custom, qs[i].id);
        if (!custom && !qs[i].sets_frame) {   // a picture preset leaves the render scale and the frame limit as they were
            CHECK_EQ(d.render_scale, DisplaySettings{}.render_scale);
            CHECK_EQ(d.fps_cap, DisplaySettings{}.fps_cap);
        }
    }
    DisplaySettings d;
    CHECK(!quality_preset("potato", d));
    CHECK(!quality_preset(nullptr, d));
    CHECK_EQ(d.quality_preset, std::string("switch2"));   // untouched
}

NXTEST(device_preset_then_edit_is_custom) {
    DisplaySettings d;
    REQUIRE(quality_preset("steamdeck", d));
    d.fps_cap = 60;
    CHECK(nxrt::play::quality_preset_edited(d, nxrt::play::QualityField::FrameLimit));
    CHECK_EQ(d.quality_preset, std::string("custom"));
    REQUIRE(quality_preset("high", d));
    d.fps_cap = 30;   // High never set the frame limit: it stays High
    CHECK(!nxrt::play::quality_preset_edited(d, nxrt::play::QualityField::FrameLimit));
    CHECK_EQ(d.quality_preset, std::string("high"));
}
