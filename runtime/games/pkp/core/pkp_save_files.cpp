// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the player's save on the host's disk (pkp_save_files.h).
#include "core/pkp_save_files.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <system_error>
#include <vector>

#include "nxrt/services.h"

namespace nxrt::games::pkp::save_files {

namespace stdfs = std::filesystem;

stdfs::path from_u8(const std::string& s) { return stdfs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
std::string u8(const stdfs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

stdfs::path save_dir() { return from_u8(services::save_dir()); }

std::string account_key() {
    const services::TitleProfile& p = services::profile();
    uint8_t u[16];
    std::memcpy(u, &p.user_uid_hi, 8);
    std::memcpy(u + 8, &p.user_uid_lo, 8);
    std::string k;
    char b[3];
    for (uint8_t c : u) {
        std::snprintf(b, sizeof b, "%02x", c);
        k += b;
    }
    return k;
}

stdfs::path account_dir() { return save_dir() / "account" / account_key(); }

bool commit_pending() {
    std::error_code ec;
    return stdfs::exists(stdfs::path(account_dir().wstring() + L".commit"), ec);
}

bool copy_account(const char* folder, size_t keep, stdfs::path* where, std::string* error) {
    error->clear();
    const stdfs::path root = account_dir();
    std::error_code ec;
    if (!stdfs::is_directory(root, ec)) return false;
    if (commit_pending()) {
        *error = "the save has an interrupted commit the game finishes first";
        return false;
    }
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    const stdfs::path copies = save_dir() / folder;
    const stdfs::path dir = copies / stamp;
    *where = dir / account_key();
    stdfs::create_directories(*where, ec);
    if (!ec) stdfs::copy(root, *where, stdfs::copy_options::recursive | stdfs::copy_options::overwrite_existing, ec);
    if (ec) {
        *error = ec.message();
        return false;
    }
    const stdfs::path meta(root.wstring() + L".meta");
    std::error_code e2;
    if (stdfs::is_regular_file(meta, e2)) stdfs::copy_file(meta, dir / meta.filename(), stdfs::copy_options::overwrite_existing, e2);

    if (keep > 0) {   // the folder names sort by time
        std::vector<stdfs::path> all;
        for (stdfs::directory_iterator it(copies, e2), end; !e2 && it != end; it.increment(e2))
            if (it->is_directory(e2)) all.push_back(it->path());
        std::sort(all.begin(), all.end());
        for (size_t i = 0; i + keep < all.size(); ++i) stdfs::remove_all(all[i], e2);
    }
    return true;
}

}  // namespace nxrt::games::pkp::save_files
