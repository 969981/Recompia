// SPDX-License-Identifier: GPL-3.0-or-later
// write_file_atomic (nxrt/file_write.h).
#include "nxrt/file_write.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace nxrt {

bool write_file_atomic(const std::string& path, std::string_view data, std::string* err) {
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
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (err) *err = "cannot write " + tmp;
            return false;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) {
            if (err) *err = "cannot write " + tmp;
            out.close();
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

}  // namespace nxrt
