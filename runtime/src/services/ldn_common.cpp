// SPDX-License-Identifier: GPL-3.0-or-later
// ldn:u helpers shared by both backends: the session variant (services_api.h ldnapi), room-code.txt, UTF-8-safe name copies,
// the hex dump of the advertise data and the first-use command log.

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "../net/text_util.h"
#include "ldn_internal.h"

namespace nxrt::services::ldn_impl {

Variant& variant() {
    static Variant v;
    return v;
}
std::atomic<bool>& story_flag() {
    static std::atomic<bool> f{false};
    return f;
}
uint8_t own_variant(std::string* what) {
    Variant& v = variant();
    std::lock_guard<std::mutex> lk(v.mu);
    if (what) *what = v.what;
    return v.value;
}
// The game's version on the wire: its variant in bits 12..14. Only for versions 0..0xfff (the game's own bits never collide); any other is
// sent as it is (logged once while a variant is set).
int16_t wire_version(int16_t game) {
    const uint8_t var = own_variant();
    if (!var) return game;
    if (game < 0 || (static_cast<uint16_t>(game) & ~0x0FFFu)) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            NXLOG_W("net", "ldn: the game's local communication version 0x%04x uses bits 12..15: the session variant is not sent",
                    static_cast<uint16_t>(game));
        return game;
    }
    return static_cast<int16_t>(game | (var << ldnapi::kVariantShift));
}
int16_t game_version(int16_t wire) { return wire < 0 ? wire : static_cast<int16_t>(wire & ~ldnapi::kVariantMask); }
uint8_t variant_of(int16_t wire) { return wire < 0 ? 0 : static_cast<uint8_t>((wire & ldnapi::kVariantMask) >> ldnapi::kVariantShift); }

// room-code.txt beside the log (the game folder for pkp.exe): the code in use, for the launcher to show (launcher room_code_host).
std::string room_code_file() {
    const std::string log = log_path();
    const size_t cut = log.find_last_of("/\\");
    return (cut == std::string::npos ? std::string() : log.substr(0, cut + 1)) + "room-code.txt";
}

// [net] room_code -> the lobby's room key (zero: none). While a code is in use it is logged and written to room_code_file(), else
// that file is removed (a code from an earlier run must not be shown). False: the value is not a code (local wireless stays off).
bool room_code_setup(std::array<uint8_t, 16>* key) {
    std::string v = config_get_str("net", "room_code", "");
    const std::string file = room_code_file();
    std::string lower = textutil::ascii_lower(v), norm;
    if (v.empty()) {
        std::remove(file.c_str());
        return true;
    }
    if (lower == "new") {
        norm = vnet::room_code_new();
        if (norm.empty()) {
            NXLOG_W("net", "ldn: [net] room_code = new: no random source: local wireless off");
            std::remove(file.c_str());
            return false;
        }
    } else if (!vnet::room_code_normalize(v, &norm)) {
        NXLOG_W("net", "ldn: [net] room_code is not a room code (16 letters and digits, XXXX-XXXX-XXXX-XXXX): local wireless off");
        std::remove(file.c_str());
        return false;
    }
    *key = vnet::room_key(norm);
    const std::string shown = vnet::room_code_format(norm);
    if (FILE* f = std::fopen(file.c_str(), "wb")) {
        std::fprintf(f, "%s\n", shown.c_str());
        std::fclose(f);
    }
    NXLOG_I("net", "ldn: room code %s%s: a room hosted here is seen only by players with this code; scans show open rooms and rooms with it",
            shown.c_str(), lower == "new" ? " (new)" : "");
    return true;
}
std::string nickname() {
    std::string n = profile().user_nickname;
    return n.empty() ? "Player" : n;
}

void copy_name(char (&dst)[33], const std::string& s) {
    std::memset(dst, 0, sizeof dst);
    size_t n = s.size() < 32 ? s.size() : 32;
    n = textutil::utf8_cut(s, n);
    std::memcpy(dst, s.data(), n);
}

// The first 128 bytes of the advertise data as hex for the Debug SetAdvertiseData line ("|" every 16 bytes, "..." when cut). Pokopia puts
// the host's KTGL member id in it (work/mp/join-reject-analysis.md section 3). Only built when Debug is on (NXLOG_D).
std::string hex_head(const std::vector<uint8_t>& b) { return textutil::hex_head(b, 128); }

// Logs the first use of a command (one line per command id for the life of the process; network_reset() clears it).
std::mutex g_seen_mu;
std::set<uint32_t> g_seen;
void note(uint32_t id, const char* name) {
    {
        std::lock_guard<std::mutex> lk(g_seen_mu);
        if (!g_seen.insert(id).second) return;
    }
    NXLOG_I("net", "ldn cmd %u %s (first use)", id, name);
}

}  // namespace nxrt::services::ldn_impl
