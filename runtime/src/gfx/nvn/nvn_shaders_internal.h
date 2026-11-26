// SPDX-License-Identifier: GPL-3.0-or-later
// The shader cache's file helpers (defined in nvn_shaders.cpp), shared by nvn_shaders.cpp and the reflection catalogue
// (nvn_shaders_refl.cpp). Not for other files.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace nxrt::nvn::be::shader_files {

bool read_file(const std::string& path, std::string& out);
std::string pack_path(std::string dir, const char* suffix);   // <dir>.preload.pack<suffix>, beside the cache directory
uint64_t fnv1a64(std::string_view s);
std::wstring widen(const std::string& s);
uint64_t now_ms();

}  // namespace nxrt::nvn::be::shader_files
