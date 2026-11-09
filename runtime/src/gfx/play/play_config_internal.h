// SPDX-License-Identifier: GPL-3.0-or-later
// Settings file code shared by play_config*.cpp: the reader (play_config.cpp, play_config_apply.cpp), the factory defaults and their
// migration (play_config_migrate.cpp) and the writer (play_config_format.cpp). Not for use outside those files.
#pragma once
#include <string>
#include <vector>

#include "ini_text.h"
#include "nxrt/extras.h"
#include "play_types.h"

namespace nxrt::play::cfg {

// The words the file uses for the display extras enums, indexed by the enum value (the first word of each list is the default's).
const std::vector<const char*> kHdrModeWords = {"off", "auto", "on", "native"};
const std::vector<const char*> kHdrFormatWords = {"auto", "hdr10", "scrgb"};
const std::vector<const char*> kUpscaleWords = {"off", "fsr1", "fsr3", "dlss", "xess", "nis", "dlaa", "nearest"};
const std::vector<const char*> kUpscaleQualityWords = {"native", "quality", "balanced", "performance", "ultra_performance", "custom"};
const std::vector<const char*> kLatencyWords = {"off", "auto", "on", "boost"};
// The upscale words accepted when reading (spatial is an alias of fsr1) and the enum value each one means.
const std::vector<const char*> kUpscaleReadWords = {"off", "fsr1", "spatial", "fsr3", "dlss", "xess", "nis", "dlaa", "nearest"};
const extras::UpscaleMode kUpscaleFromWord[] = {extras::UpscaleMode::Off, extras::UpscaleMode::Fsr1, extras::UpscaleMode::Fsr1,
                                                extras::UpscaleMode::Fsr3, extras::UpscaleMode::Dlss, extras::UpscaleMode::Xess, extras::UpscaleMode::Nis,
                                                extras::UpscaleMode::Dlaa, extras::UpscaleMode::Nearest};

enum class Sec { None, Unknown, Meta, Mouse, Pad, PadMap, Timing, Display, Audio, Hdr, Upscale, Latency, Picture, Prompts, Fps, Quit, Overlay, Switch, Aim, Camera, Tweaks, Bindings };

// play_config.cpp
Settings builtin_settings();
bool parse_onto(const std::string& text, Settings* s, std::string* err, std::vector<std::string>* warnings, bool with_default_bindings);

// play_config_apply.cpp
const char* sec_label(Sec s);
bool apply_fixed(Sec sec, const std::string& k, const std::string& v, Settings* s, bool* known, std::string* msg);
Sec fixed_sec(const std::string& key);

// play_config_migrate.cpp
int defaults_rev();
void apply_default_changes(Settings* s, int file_rev, std::string* added);
void apply_default_additions(Settings* s, int file_rev, std::string* added);

}  // namespace nxrt::play::cfg
