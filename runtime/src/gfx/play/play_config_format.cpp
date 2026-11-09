// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the settings file writer (format_settings). The file format is described in play_types.h.
#include <string>

#include "nxrt/game.h"
#include "play_config_internal.h"

namespace nxrt::play {

using namespace cfg;

std::string format_settings(const Settings& s) {
    std::string o;
    auto kv = [&o](const char* key, const std::string& value) { o += std::string(key) + " = " + value + "\n"; };
    auto flag = [](bool b) { return std::string(b ? "1" : "0"); };

    const char* title = game::info().title;
    o += (title && *title) ? std::string("# ") + title + ", play mode: controls and display.\n" : std::string("# Play mode: controls and display.\n");
    o += "\n[meta]\n";
    kv("defaults_rev", std::to_string(s.defaults_rev));
    o += "\n[mouse]\n";
    kv("yaw", num(s.mouse.yaw_rad));
    kv("pitch", num(s.mouse.pitch_rad));
    kv(key_alt_camera(), flag(s.mouse.alt_camera));
    kv(key_alt_sens(), num(source_sens(s.mouse.alt_yaw_rad)));
    if (s.mouse.alt_pitch_rad != s.mouse.alt_yaw_rad) kv(key_alt_pitch(), num(s.mouse.alt_pitch_rad));   // a vertical of its own, after the pair
    kv("invert_y", flag(s.mouse.invert_y));
    kv("invert_x", flag(s.mouse.invert_x));
    kv("menu_cursor", flag(s.mouse.menu_cursor));
    kv(key_wheel_cycles(), flag(s.mouse.wheel_cycles));

    o += "\n[tweaks]\n";
    kv("language", s.language.empty() ? "auto" : s.language);

    o += "\n[pad]\n";
    kv("label_abxy", flag(s.pad.label_abxy));
    kv("deadzone", num(s.pad.stick_deadzone));
    kv("right_deadzone", num(s.pad.right_deadzone));
    kv("invert_y", flag(s.pad.invert_y));
    kv("look_sens", num(s.pad.look_sens));
    kv("trigger_threshold", num(s.pad.trigger_threshold));
    kv("stick_max", num(s.pad.stick_max));
    kv("right_stick_max", num(s.pad.right_stick_max));
    kv("gyro", flag(s.pad.gyro));
    kv("gyro_scale", num(s.pad.gyro_scale));
    kv("rumble", flag(s.pad.rumble));
    kv("rumble_scale", num(s.pad.rumble_scale));
    kv(key_alt_orbit(), num(s.pad.alt_orbit));

    o += "\n[padmap]\n";
    for (int i = 0; i < kPadSrcCount; ++i) {
        Output out;
        out.buttons = s.pad.remap[i];
        kv(pad_src_name(static_cast<PadSrc>(i)), s.pad.remap[i] == kPadDefault ? std::string("default") : output_text(out));
    }

    o += "\n[timing]\n";
    kv("tap_ms", std::to_string(s.timing.tap_ms));
    kv("hold_ms", std::to_string(s.timing.hold_ms));
    kv("double_ms", std::to_string(s.timing.double_ms));
    kv("pulse_ms", std::to_string(s.timing.pulse_ms));

    o += "\n[display]\n";
    kv("window_mode", s.display.window_mode);
    kv("vsync", flag(s.display.vsync));
    kv("fps_limit", s.display.fps_cap < 0 ? std::string("auto") : std::to_string(s.display.fps_cap));
    kv("fps_snap", s.display.fps_snap);
    kv("width", std::to_string(s.display.width));
    kv("height", std::to_string(s.display.height));
    kv("keep_aspect", flag(s.display.keep_aspect));
    kv("vram_cap", flag(s.display.vram_cap));
    kv("render_scale", s.display.render_scale <= 0 ? std::string("auto") : std::to_string(s.display.render_scale));
    kv("render_width", std::to_string(s.display.render_width));
    kv("render_height", std::to_string(s.display.render_height));
    kv("aspect", s.display.aspect <= 0 ? std::string("auto") : num_exact(s.display.aspect));
    kv("fov_offset", num_exact(s.display.fov_offset));
    kv("dynamic_resolution", flag(s.display.dynamic_resolution));
    kv("anisotropy", std::to_string(s.display.anisotropy));
    kv("lod_bias", num_exact(s.display.lod_bias));
    kv("bloom", flag(s.display.bloom));
    kv("chromatic_aberration", flag(s.display.chromatic_aberration));
    kv("quality_preset", s.display.quality_preset);
    kv("fast_gpu_memory", flag(s.display.fast_gpu_memory));
    kv("fewer_gpu_waits", flag(s.display.fewer_gpu_waits));
    kv("shader_prewarm", flag(s.display.shader_prewarm));
    kv("astc_quality", s.display.astc_quality);
    kv("monitor", s.display.monitor <= 0 ? std::string("auto") : std::to_string(s.display.monitor));

    o += "\n[audio]\n";
    kv("volume", num_exact(s.audio.volume));
    kv("mute_unfocused", flag(s.audio.mute_unfocused));
    kv("device", s.audio.device.empty() ? std::string("default") : s.audio.device);

    o += "\n[hdr]\n";
    kv("mode", kHdrModeWords[static_cast<int>(s.extras.hdr.mode)]);
    kv("format", kHdrFormatWords[static_cast<int>(s.extras.hdr.format)]);
    kv("peak_nits", num_exact(s.extras.hdr.peak_nits));
    kv("paper_white_nits", num_exact(s.extras.hdr.paper_white_nits));
    kv("highlight_boost", num_exact(s.extras.hdr.highlight_boost));

    o += "\n[upscale]\n";
    kv("mode", kUpscaleWords[static_cast<int>(s.extras.upscale.mode)]);
    kv("quality", kUpscaleQualityWords[static_cast<int>(s.extras.upscale.quality)]);
    kv("sharpness", num_exact(s.extras.upscale.sharpness));
    kv("scene_scale", std::to_string(s.extras.upscale.scene_scale));
    kv("fsr_dll", s.extras.upscale.fsr_dll);
    kv("dlss_dll", s.extras.upscale.dlss_dll);
    kv("xess_dll", s.extras.upscale.xess_dll);
    kv("frame_generation", flag(s.extras.upscale.frame_generation));

    o += "\n[latency]\n";
    kv("mode", kLatencyWords[static_cast<int>(s.extras.latency.mode)]);
    kv("frames_ahead", std::to_string(s.extras.latency.frames_ahead));
    kv("log_timings", flag(s.extras.latency.log_timings));

    o += "\n[picture]\n";
    kv("contrast", num_exact(s.extras.picture.contrast));
    kv("saturation", num_exact(s.extras.picture.saturation));
    kv("black", num_exact(s.extras.picture.black));
    kv("gamma", num_exact(s.extras.picture.gamma));
    o += "\n[prompts]\n";
    kv("style", prompt_style_name(s.prompts.style));

    o += "\n[fps]\n";
    kv("show", flag(s.fps.show));
    kv("key", input_name(s.fps.key));
    kv("corner", corner_name(s.fps.corner));
    kv("detail", std::to_string(s.fps.detail));
    kv("in_screenshots", flag(s.fps.in_screenshots));

    o += "\n[quit]\n";
    kv("pad", pad_combo_text(s.quit.pad));
    kv("key", s.quit.key.kind == InKind::Key ? input_name(s.quit.key) : std::string("none"));

    o += "\n[overlay]\n";
    kv("pad", pad_combo_text(s.overlay.pad));

    o += "\n[switch]\n";
    for (int a = 1; a < kActCount; ++a) {   // every action except none
        kv(act_info(static_cast<Act>(a)).name, output_text(s.out[a]));
    }

    // Always written, even when empty, so that a file with every binding removed does not fall back to the defaults.
    for (int c = 0; c < kCtxCount; ++c) {
        o += std::string("\n[bindings.") + ctx_name(static_cast<Ctx>(c)) + "]\n";
        for (const Binding& b : s.bindings) {
            if (static_cast<int>(b.ctx) != c) continue;
            const std::string key = input_name(b.in);
            if (key.empty()) continue;   // an input with no name cannot be written; it cannot be read back either
            o += key + " = " + act_info(b.act).name;
            if (b.trig != Trig::Press || b.ms != 0) o += std::string(" ") + trig_name(b.trig);
            if (b.ms != 0) o += " " + std::to_string(b.ms);
            o += "\n";
        }
    }
    return o;
}

}  // namespace nxrt::play
