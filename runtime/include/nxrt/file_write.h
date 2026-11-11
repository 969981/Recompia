// SPDX-License-Identifier: GPL-3.0-or-later
// Writing a settings file whole: the bytes go to <path>.tmp first, which is then renamed over the file, so a crash or a full disk never
// leaves half a file behind. The folder is created when missing. Used for controls.ini (play_config_io.cpp) and the ini editor
// (nxrt/ini_edit.h).
#pragma once
#include <string>
#include <string_view>

namespace nxrt {

// false with a message in *err (when given) if the folder, the temp file or the rename failed; the temp file is removed then.
bool write_file_atomic(const std::string& path, std::string_view data, std::string* err);

}  // namespace nxrt
