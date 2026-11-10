// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the settings file on disk (load_settings, save_settings).
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "play_types.h"

namespace nxrt::play {

bool load_settings(const std::string& path, Settings* out, std::string* err, std::vector<std::string>* warnings) {
    namespace fs = std::filesystem;
    if (err) err->clear();
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) {
        *out = default_settings();
        if (err) *err = "cannot check " + path + ": " + ec.message();
        return false;
    }
    if (!exists) {   // a first run: no file yet, not an error
        *out = default_settings();
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *out = default_settings();
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);   // a UTF-8 byte order mark, written by some editors

    Settings s;
    std::string perr;
    if (!parse_settings(text, &s, &perr)) {
        *out = default_settings();
        if (err) *err = path + ": " + perr;
        return false;
    }
    *out = s;
    return true;
}

bool save_settings(const std::string& path, const Settings& s, std::string* err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            if (err) *err = "cannot create folder for " + path + ": " + ec.message();
            return false;
        }
    }
    // Write the whole file to a temp name first, then rename it over the target, so a crash never leaves half a file.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err) *err = "cannot write " + tmp;
            return false;
        }
        out << format_settings(s);
        out.flush();
        if (!out) {
            if (err) *err = "cannot write " + tmp;
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, target, ec);   // replaces an existing file on Windows
    if (ec) {
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        if (err) *err = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace nxrt::play
