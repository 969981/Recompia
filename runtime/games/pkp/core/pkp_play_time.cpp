// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia, play time for the launcher (pkp_play_time.h). The save data's play time is read only while play mode says the game is
// in play: before the player loads a save the save data holds no save's time. The file is written off the frame, a thread per write a
// minute apart (so the one before has always ended); write_file_atomic means a process that ends mid-write leaves the old file whole. The
// path is UTF-8, which pkp.exe's manifest makes the process code page (game-build/res/pkp.manifest), so a save folder with accents works.
#include "core/pkp_play_time.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_save_files.h"
#include "nxrt/file_write.h"
#include "nxrt/ini_edit.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp {
namespace {

constexpr const char* kTag = "pkp";
constexpr const char* kFileName = "play_time.ini";   // the launcher reads it under this name (play_time.rs FILE)
constexpr const char* kSection = "play_time";
using Clock = std::chrono::steady_clock;
constexpr auto kEvery = std::chrono::seconds(60);
constexpr double kMaxSeconds = 1e9;   // about 31 years: a larger value is not a play time

struct State {
    uint64_t base = 0;
    std::string path, key;     // the file, and this save's key in it (the account folder's name, save_files::account_key)
    double seconds = -1;       // the play time read last while in the game (-1: none yet)
    int64_t written = -1;      // the whole seconds handed to a write last
    Clock::time_point next{};
    std::thread writer;
} g;

// The save data's play time in seconds, or -1 while there is none to read.
double read_play_time() {
    if (!guest::ok(g.base + layout::kSaveRoot, sizeof(uint64_t))) return -1;
    const uint64_t save = guest::rd<uint64_t>(g.base + layout::kSaveRoot);
    if (!guest::ok(save + layout::kSavePlayTime, sizeof(double))) return -1;
    const double s = guest::rd<double>(save + layout::kSavePlayTime);
    return std::isfinite(s) && s >= 0 && s < kMaxSeconds ? s : -1;
}

// Sets this save's line; the other saves' lines stay. A failure is logged once per run.
void write_line(const std::string& path, const std::string& key, int64_t seconds) {
    static std::atomic<bool> warned{false};
    std::string text;
    if (std::ifstream in{save_files::from_u8(path), std::ios::binary}) text.assign(std::istreambuf_iterator<char>(in), {});
    std::string err;
    if (!write_file_atomic(path, ini_set_value(text, kSection, key, std::to_string(seconds)), &err) && !warned.exchange(true))
        NXLOG_W(kTag, "play time: not written: %s", err.c_str());
}

// Writes the play time read last, on a thread, when its whole seconds changed.
void flush() {
    if (g.seconds < 0 || g.key.empty()) return;
    const int64_t whole = static_cast<int64_t>(g.seconds);
    if (whole == g.written) return;
    g.written = whole;
    if (g.writer.joinable()) g.writer.join();
    g.writer = std::thread(&write_line, g.path, g.key, whole);
}

void install(const FeatureInstall& in) {
    g.base = in.base;
    g.path = save_files::u8(save_files::save_dir() / kFileName);
    g.key = save_files::account_key();
}

void remove() {
    if (g.writer.joinable()) g.writer.join();
    g = State{};
}

void frame(bool in_game, double, play::GameFrame*) {
    if (!in_game) return;
    const double s = read_play_time();
    if (s < 0) return;
    g.seconds = s;
    const auto now = Clock::now();
    if (now < g.next) return;
    g.next = now + kEvery;
    flush();
}

}  // namespace

const Feature kPlayTimeFeature = {.name = "play_time", .install = &install, .remove = &remove, .frame = &frame};

}  // namespace nxrt::games::pkp
