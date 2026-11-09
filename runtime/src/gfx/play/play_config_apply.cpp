// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the settings file reader, one `key = value` of a fixed section at a time (apply_fixed). The file format is described in play_types.h.
#include <string>

#include "nxrt/game.h"
#include "play_config_internal.h"

namespace nxrt::play {
namespace cfg {

const char* sec_label(Sec s) {
    switch (s) {
        case Sec::Meta: return "[meta]";
        case Sec::Mouse: return "[mouse]";
        case Sec::Pad: return "[pad]";
        case Sec::PadMap: return "[padmap]";
        case Sec::Timing: return "[timing]";
        case Sec::Display: return "[display]";
        case Sec::Audio: return "[audio]";
        case Sec::Hdr: return "[hdr]";
        case Sec::Upscale: return "[upscale]";
        case Sec::Latency: return "[latency]";
        case Sec::Picture: return "[picture]";
        case Sec::Prompts: return "[prompts]";
        case Sec::Fps: return "[fps]";
        case Sec::Quit: return "[quit]";
        case Sec::Overlay: return "[overlay]";
        case Sec::Switch: return "[switch]";
        case Sec::Tweaks: return "[tweaks]";
        default: return "";
    }
}

// Keys that older builds wrote and nothing reads any more: accepted and ignored, so an old file loads without a warning. The next save
// leaves them out.
static bool retired_key(Sec sec, const std::string& k) {
    struct Retired {
        Sec sec;
        const char* key;
    };
    static const Retired kRetired[] = {
        {Sec::Tweaks, "glow_color"}, {Sec::Tweaks, "suit_glow"}, {Sec::Tweaks, "ball_glow"}, {Sec::Tweaks, "gun_glow"},
        {Sec::Tweaks, "suit_color"}, {Sec::Tweaks, "gun_color"}, {Sec::Tweaks, "skip_intro"}, {Sec::Tweaks, "skip_boot_logos"},
        {Sec::Tweaks, "fast_menus"}, {Sec::Tweaks, "menu_speed"},
    };
    for (const Retired& r : kRetired)
        if (r.sec == sec && k == r.key) return true;
    return false;
}

// Applies one `key = value` of a fixed section. On a bad value returns false with *msg set (no line prefix).
// *known is false for a key the section does not have.
bool apply_fixed(Sec sec, const std::string& k, const std::string& v, Settings* s, bool* known, std::string* msg) {
    *known = true;
    auto set_num = [&](double* dst) {
        double x = 0;
        if (!to_double(v, &x)) {
            *msg = k + " must be a number, got '" + v + "'";
            return false;
        }
        *dst = x;
        return true;
    };
    auto set_real = [&](double lo, double hi, double* dst) {
        double x = 0;
        if (!to_double(v, &x)) {
            *msg = k + " must be a number, got '" + v + "'";
            return false;
        }
        if (x < lo || x > hi) {
            *msg = k + " must be " + num(lo) + ".." + num(hi) + ", got " + v;
            return false;
        }
        *dst = x;
        return true;
    };
    // A real that is either 0 (the "auto" value) or within lo..hi.
    auto set_zero_or_real = [&](double lo, double hi, double* dst) {
        double x = 0;
        if (!to_double(v, &x)) {
            *msg = k + " must be a number, got '" + v + "'";
            return false;
        }
        if (x != 0 && (x < lo || x > hi)) {
            *msg = k + " must be 0 (auto) or " + num(lo) + ".." + num(hi) + ", got " + v;
            return false;
        }
        *dst = x;
        return true;
    };
    auto set_int = [&](long long lo, long long hi, int* dst) {
        long long x = 0;
        if (!to_int(v, &x)) {
            *msg = k + " must be a whole number, got '" + v + "'";
            return false;
        }
        if (x < lo || x > hi) {
            *msg = k + " must be " + std::to_string(lo) + ".." + std::to_string(hi) + ", got " + v;
            return false;
        }
        *dst = static_cast<int>(x);
        return true;
    };
    auto set_bool = [&](bool* dst) {
        if (!to_bool(v, dst)) {
            *msg = k + " must be 1/0 (or true/false, yes/no, on/off), got '" + v + "'";
            return false;
        }
        return true;
    };
    // A fixed render size: 0 (follow the window) or 320..16384.
    auto set_size = [&](int* dst) {
        long long x = 0;
        if (!to_int(v, &x)) {
            *msg = k + " must be a whole number, got '" + v + "'";
            return false;
        }
        if (x != 0 && (x < 320 || x > 16384)) {
            *msg = k + " must be 0 or 320..16384, got " + v;
            return false;
        }
        *dst = static_cast<int>(x);
        return true;
    };
    // The forced aspect ratio: auto (0), a ratio W:H (16:9, 21:9, 32:9, 16:10) or a number 1.0..5.0.
    auto set_aspect = [&](double* dst) {
        double x = 0;
        const size_t colon = v.find(':');
        double num = 0, den = 0;
        if (lower(v) == "auto") {
            x = 0;
        } else if (colon != std::string::npos && to_double(v.substr(0, colon), &num) && to_double(v.substr(colon + 1), &den) && num > 0 && den > 0) {
            x = num / den;
        } else if (!to_double(v, &x)) {
            *msg = k + " must be auto, W:H or a number, got '" + v + "'";
            return false;
        }
        if (x != 0 && (x < 1.0 || x > 5.0)) {
            *msg = k + " must be 0 (auto) or 1.0..5.0, got " + v;
            return false;
        }
        *dst = x;
        return true;
    };

    switch (sec) {
        case Sec::Meta:
            if (k == "defaults_rev") return set_int(0, 1000, &s->defaults_rev);
            break;
        case Sec::Mouse:
            if (k == "yaw") return set_num(&s->mouse.yaw_rad);
            if (k == "pitch") return set_num(&s->mouse.pitch_rad);
            // the ball camera's speed: 0.1 to 20 on the Source scale (the F1 slider's range), in radians per count for the older keys
            if (k == key_alt_yaw()) return set_real(rad_per_count(0.1), rad_per_count(20.0), &s->mouse.alt_yaw_rad);
            if (k == key_alt_pitch()) return set_real(rad_per_count(0.1), rad_per_count(20.0), &s->mouse.alt_pitch_rad);
            if (k == key_alt_camera()) return set_bool(&s->mouse.alt_camera);
            if (k == key_alt_sens()) {   // Source scale, both directions
                double x = 0;
                if (!set_real(0.1, 20.0, &x)) return false;
                s->mouse.alt_yaw_rad = s->mouse.alt_pitch_rad = rad_per_count(x);
                return true;
            }
            if (k == "yaw_source" || k == "pitch_source") {   // Source scale, converted to radians per count
                double x = 0;
                if (!set_num(&x)) return false;
                (k == "yaw_source" ? s->mouse.yaw_rad : s->mouse.pitch_rad) = rad_per_count(x);
                return true;
            }
            if (k == "invert_y") return set_bool(&s->mouse.invert_y);
            if (k == "invert_x") return set_bool(&s->mouse.invert_x);
            if (k == "menu_cursor") return set_bool(&s->mouse.menu_cursor);
            if (k == "menu_touch") return true;   // legacy key, dead since it never did anything: accepted and ignored
            if (k == key_wheel_cycles()) return set_bool(&s->mouse.wheel_cycles);
            break;
        case Sec::Tweaks:
            if (k == "language") {
                s->language = lower(v) == "auto" ? std::string() : v;
                return true;
            }
            break;
        case Sec::Pad:
            if (k == "label_abxy") return set_bool(&s->pad.label_abxy);
            if (k == "deadzone") return set_real(0.0, 0.5, &s->pad.stick_deadzone);
            if (k == "right_deadzone") return set_real(0.0, 0.5, &s->pad.right_deadzone);
            if (k == "stick_max") return set_real(0.5, 1.0, &s->pad.stick_max);
            if (k == "right_stick_max") return set_real(0.5, 1.0, &s->pad.right_stick_max);
            if (k == "invert_y") return set_bool(&s->pad.invert_y);
            if (k == "look_sens") return set_real(0.25, 3.0, &s->pad.look_sens);
            if (k == "trigger_threshold") return set_real(0.05, 1.0, &s->pad.trigger_threshold);
            if (k == "gyro") return set_bool(&s->pad.gyro);
            if (k == "gyro_scale") return set_num(&s->pad.gyro_scale);
            if (k == "rumble") return set_bool(&s->pad.rumble);
            if (k == "rumble_scale") return set_num(&s->pad.rumble_scale);
            if (k == key_alt_orbit()) return set_real(0.0, 6.0, &s->pad.alt_orbit);
            break;
        case Sec::Timing:
            if (k == "tap_ms") return set_int(1, 5000, &s->timing.tap_ms);
            if (k == "hold_ms") return set_int(1, 5000, &s->timing.hold_ms);
            if (k == "double_ms") return set_int(1, 5000, &s->timing.double_ms);
            if (k == "pulse_ms") return set_int(1, 5000, &s->timing.pulse_ms);
            break;
        case Sec::Prompts:
            if (k == "style") {
                if (!parse_prompt_style(v, &s->prompts.style)) {
                    *msg = "style must be switch, xbox, playstation, steam, keyboard or auto, got '" + v + "'";
                    return false;
                }
                return true;
            }
            break;
        case Sec::Fps:
            if (k == "show") return set_bool(&s->fps.show);
            if (k == "key") {
                InputId id;
                if (!parse_input(v, &id) || id.kind != InKind::Key) {
                    *msg = "key must be a keyboard key name (F2, Tab, Left Ctrl ...), got '" + v + "'";
                    return false;
                }
                s->fps.key = id;
                return true;
            }
            if (k == "corner") {
                if (!parse_corner(v, &s->fps.corner)) {
                    *msg = "corner must be top_left, top_right, bottom_left or bottom_right, got '" + v + "'";
                    return false;
                }
                return true;
            }
            if (k == "detail") return set_int(0, 2, &s->fps.detail);
            if (k == "in_screenshots") return set_bool(&s->fps.in_screenshots);
            break;
        case Sec::Quit:
            if (k == "pad") {
                if (!parse_pad_combo(v, &s->quit.pad)) {
                    *msg = "pad must be none or pad buttons joined by '+' (back+start, left_shoulder+right_shoulder ...), got '" + v + "'";
                    return false;
                }
                return true;
            }
            if (k == "key") {
                InputId id;
                if (lower(v) == "none" || v.empty()) {
                    s->quit.key = {};
                    return true;
                }
                if (!parse_input(v, &id) || id.kind != InKind::Key) {
                    *msg = "key must be none or a keyboard key name (F4, End, Pause ...), got '" + v + "'";
                    return false;
                }
                s->quit.key = id;
                return true;
            }
            break;
        case Sec::Overlay:
            if (k == "pad") {
                if (!parse_pad_combo(v, &s->overlay.pad)) {
                    *msg = "pad must be none or pad buttons joined by '+' (left_shoulder+right_shoulder, back+start ...), got '" + v + "'";
                    return false;
                }
                return true;
            }
            break;
        case Sec::PadMap: {
            PadSrc src;
            if (!parse_pad_src(k, &src)) break;   // unknown pad button name: a warning
            uint64_t& dst = s->pad.remap[static_cast<int>(src)];
            if (lower(v) == "default") {
                dst = kPadDefault;
                return true;
            }
            Output o;
            std::string perr;
            if (!parse_output(v, &o, &perr)) {
                *msg = perr;
                return false;
            }
            if (o.lx != 0 || o.ly != 0 || o.rx != 0 || o.ry != 0) {
                *msg = k + " can press Switch buttons only (no stick directions), got '" + v + "'";
                return false;
            }
            dst = o.buttons;
            return true;
        }
        case Sec::Display:
            if (k == "window_mode") {
                const std::string l = lower(v);
                if (l != "windowed" && l != "borderless" && l != "exclusive") {
                    *msg = "window_mode must be windowed, borderless or exclusive, got '" + v + "'";
                    return false;
                }
                s->display.window_mode = l;
                return true;
            }
            if (k == "vsync") return set_bool(&s->display.vsync);
            if (k == "fps_limit" || k == "fps_cap") {     // fps_cap: the older name
                if (lower(v) == "auto") { s->display.fps_cap = -1; return true; }
                return set_int(0, 1000, &s->display.fps_cap);
            }
            if (k == "fps_snap") {
                const std::string l = lower(v);
                if (l == "auto") s->display.fps_snap = "auto";
                else if (l == "true" || l == "on" || l == "1" || l == "yes") s->display.fps_snap = "true";
                else if (l == "false" || l == "off" || l == "0" || l == "no") s->display.fps_snap = "false";
                else {
                    *msg = "fps_snap must be auto, true or false, got '" + v + "'";
                    return false;
                }
                return true;
            }
            if (k == "width") return set_int(320, 16384, &s->display.width);
            if (k == "height") return set_int(320, 16384, &s->display.height);
            if (k == "keep_aspect") return set_bool(&s->display.keep_aspect);
            if (k == "vram_cap") return set_bool(&s->display.vram_cap);
            if (k == "render_scale") {
                if (lower(v) == "auto") { s->display.render_scale = 0; return true; }
                return set_int(25, 400, &s->display.render_scale);
            }
            if (k == "render_width") return set_size(&s->display.render_width);
            if (k == "render_height") return set_size(&s->display.render_height);
            if (k == "aspect") return set_aspect(&s->display.aspect);
            if (k == "fov_offset") return set_real(-30.0, 40.0, &s->display.fov_offset);
            if (k == "dynamic_resolution") return set_bool(&s->display.dynamic_resolution);
            if (k == "anisotropy") {
                long long x = 0;
                if (!to_int(v, &x)) {
                    *msg = k + " must be a whole number, got '" + v + "'";
                    return false;
                }
                if (x != 0 && x != 2 && x != 4 && x != 8 && x != 16) {
                    *msg = "anisotropy must be 0, 2, 4, 8 or 16, got " + v;
                    return false;
                }
                s->display.anisotropy = static_cast<int>(x);
                return true;
            }
            if (k == "lod_bias") return set_real(-2.0, 2.0, &s->display.lod_bias);
            if (k == "bloom") return set_bool(&s->display.bloom);
            if (k == "chromatic_aberration") return set_bool(&s->display.chromatic_aberration);
            if (k == "view_distance") return true;   // removed (#249): the view distance is the console's; old files still load
            if (k == "quality_preset") {
                const QualityPresetInfo* q = find_quality_preset(v);
                if (!q) {
                    *msg = "quality_preset must be switch2, high, ultra, steamdeck, legion_go_s, laptop, desktop or custom, got '" + v + "'";
                    return false;
                }
                s->display.quality_preset = q->id;
                return true;
            }
            if (k == "fast_gpu_memory") return set_bool(&s->display.fast_gpu_memory);
            if (k == "fewer_gpu_waits") return set_bool(&s->display.fewer_gpu_waits);
            if (k == "shader_prewarm") return set_bool(&s->display.shader_prewarm);
            if (k == "astc_quality") {
                const std::string q = lower(v);
                if (q != "fast" && q != "high" && q != "uncompressed") {
                    *msg = "astc_quality must be fast, high or uncompressed, got " + v;
                    return false;
                }
                s->display.astc_quality = q;
                return true;
            }
            if (k == "monitor") {
                if (lower(v) == "auto") { s->display.monitor = 0; return true; }
                return set_int(0, 32, &s->display.monitor);
            }
            break;
        case Sec::Audio:
            if (k == "volume") return set_real(0.0, 1.0, &s->audio.volume);
            if (k == "mute_unfocused") return set_bool(&s->audio.mute_unfocused);
            if (k == "device") {
                s->audio.device = v.empty() ? "default" : v;
                return true;
            }
            break;
        case Sec::Hdr: {
            extras::HdrSettings& h = s->extras.hdr;
            if (k == "mode") {
                int i = 0;
                if (!pick_word(k, v, kHdrModeWords, &i, msg)) return false;
                h.mode = static_cast<extras::HdrMode>(i);
                return true;
            }
            if (k == "format") {
                int i = 0;
                if (!pick_word(k, v, kHdrFormatWords, &i, msg)) return false;
                h.format = static_cast<extras::HdrFormat>(i);
                return true;
            }
            if (k == "peak_nits") return set_zero_or_real(200.0, 10000.0, &h.peak_nits);
            if (k == "paper_white_nits") return set_zero_or_real(80.0, 500.0, &h.paper_white_nits);
            if (k == "highlight_boost") return set_real(0.0, 2.0, &h.highlight_boost);
            break;
        }
        case Sec::Upscale: {
            extras::UpscaleSettings& u = s->extras.upscale;
            if (k == "mode") {
                int i = 0;
                if (!pick_word(k, v, kUpscaleReadWords, &i, msg)) return false;
                u.mode = extras::upscale_mode_enabled(kUpscaleFromWord[i]);
                return true;
            }
            if (k == "quality") {   // the temporal modes' render scale preset
                int i = 0;
                if (!pick_word(k, v, kUpscaleQualityWords, &i, msg)) return false;
                u.quality = static_cast<extras::UpscaleQuality>(i);
                return true;
            }
            if (k == "sharpness") return set_real(0.0, 1.0, &u.sharpness);
            if (k == "fsr_dll") {   // a path: kept as written
                u.fsr_dll = v;
                return true;
            }
            if (k == "dlss_dll") {
                u.dlss_dll = v;
                return true;
            }
            if (k == "xess_dll") {
                u.xess_dll = v;
                return true;
            }
            if (k == "scene_scale") return set_int(25, 100, &u.scene_scale);
            if (k == "frame_generation") return set_bool(&u.frame_generation);
            break;
        }
        case Sec::Latency: {
            extras::LatencySettings& l = s->extras.latency;
            if (k == "mode") {
                int i = 0;
                if (!pick_word(k, v, kLatencyWords, &i, msg)) return false;
                l.mode = static_cast<extras::LatencyMode>(i);
                return true;
            }
            if (k == "frames_ahead") return set_int(-1, 3, &l.frames_ahead);
            if (k == "log_timings") return set_bool(&l.log_timings);
            break;
        }
        case Sec::Picture: {
            extras::PictureSettings& c = s->extras.picture;
            if (k == "preset") {   // read only (the file keeps the four values): fills them in, the keys after it win
                if (!extras::picture_preset(lower(v), &c)) {
                    *msg = "preset must be neutral or vivid";
                    return false;
                }
                return true;
            }
            if (k == "contrast") return set_real(0.5, 1.5, &c.contrast);
            if (k == "saturation") return set_real(0.0, 2.0, &c.saturation);
            if (k == "black") return set_real(-0.5, 0.25, &c.black);
            if (k == "gamma") return set_real(0.5, 2.0, &c.gamma);
            break;
        }
        case Sec::Switch: {
            Act a = Act::None;
            if (!parse_act(k, &a) || a == Act::None) break;   // unknown action name: a warning
            Output o;
            std::string perr;
            if (!parse_output(v, &o, &perr)) {
                *msg = perr;
                return false;
            }
            s->out[static_cast<int>(a)] = o;
            return true;
        }
        default:
            break;
    }
    if (retired_key(sec, k)) return true;
    *known = false;
    return true;
}

// Adds the factory bindings newer than `file_rev` to a file that has bindings sections. A binding is added only when no binding of the
// same context already uses that input. Sets *added to a description of what was added (for the log line).
// A fixed section by its lower-case name ([bindings.*] is not one); Sec::Unknown when there is none.
Sec fixed_sec(const std::string& key) {
    if (key == "meta") return Sec::Meta;
    if (key == "mouse") return Sec::Mouse;
    if (key == "pad") return Sec::Pad;
    if (key == "padmap") return Sec::PadMap;
    if (key == "timing") return Sec::Timing;
    if (key == "display") return Sec::Display;
    if (key == "audio") return Sec::Audio;
    if (key == "hdr") return Sec::Hdr;
    if (key == "upscale") return Sec::Upscale;
    if (key == "latency") return Sec::Latency;
    if (key == "picture") return Sec::Picture;
    if (key == "prompts") return Sec::Prompts;
    if (key == "fps") return Sec::Fps;
    if (key == "quit") return Sec::Quit;
    if (key == "overlay") return Sec::Overlay;
    if (key == "aim" || key == "camera") return Sec::Retired;   // [aim] lock_mode free_lock_margin, [camera] gun_drives_view
    if (key == "tweaks") return Sec::Tweaks;
    if (key == "switch") return Sec::Switch;
    return Sec::Unknown;
}

}  // namespace cfg
}  // namespace nxrt::play
