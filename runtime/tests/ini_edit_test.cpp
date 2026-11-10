// SPDX-License-Identifier: GPL-3.0-or-later
// Build and run: cmake --build <build> --target nxrt_ini_edit_test && <build>/nxrt_ini_edit_test
// Changing one ini value (nxrt/ini_edit.h, config_save_key): the rest of the file stays, and config.h reads back what was written.
#include "nxrt/config.h"
#include "nxrt/ini_edit.h"
#include "nxrt/log.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using nxrt::ini_with_value;

// config.cpp logs through nxrt::logf; the test links config.cpp alone, so a silent logf stands in for the real one (src/diag/log.cpp, Windows only).
namespace nxrt {
void logf(LogLevel, const char*, const char*, ...) {}
}  // namespace nxrt

static int checks = 0;
static void same(const std::string& got, const std::string& want, const char* what) {
    ++checks;
    if (got != want) {
        std::cerr << "FAIL: " << what << "\n--- got:\n" << got << "\n--- want:\n" << want << "\n---\n";
        std::exit(1);
    }
}
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        std::exit(1);
    }
}

static std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int main() {
    // replace in place: the comment, the spacing before it and every other line stay
    same(ini_with_value("[pkp]\ncamera_aim = 0 ; try it\njump_momentum = 0\n", "pkp", "camera_aim", "1"),
         "[pkp]\ncamera_aim = 1 ; try it\njump_momentum = 0\n", "value replaced, trailing comment kept");
    same(ini_with_value("[pkp]\nCamera_Aim=0\n", "PKP", "camera_aim", "1"), "[pkp]\nCamera_Aim=1\n", "section and key are case-insensitive, spelling kept");
    same(ini_with_value("[pkp]\nx = 1 # note\n", "pkp", "x", "2"), "[pkp]\nx = 2 # note\n", "# comment kept");
    same(ini_with_value("[a]\nk = 1\n[b]\nk = 1\n", "b", "k", "9"), "[a]\nk = 1\n[b]\nk = 9\n", "only the named section changes");
    same(ini_with_value("[pkp]\nk = 1\nk = 2\n", "pkp", "k", "3"), "[pkp]\nk = 1\nk = 3\n", "the last line wins, so it is the one changed");
    same(ini_with_value("[pkp]\nk = \"a;b\"\n", "pkp", "k", "c"), "[pkp]\nk = c\n", "a quoted value is replaced with its quotes");
    same(ini_with_value("[pkp]\nk =\nz = 1\n", "pkp", "k", "5"), "[pkp]\nk =5\nz = 1\n", "an empty value takes the new one");
    same(ini_with_value("[pkp]\n; k = 1\nk = 2\n", "pkp", "k", "3"), "[pkp]\n; k = 1\nk = 3\n", "a commented-out line is not the key");
    same(ini_with_value("[pkp]\nk = 1", "pkp", "k", "2"), "[pkp]\nk = 2", "no final newline stays absent");

    // a key that is missing goes at the end of its section
    same(ini_with_value("[pkp]\na = 1\n\n[net]\nb = 2\n", "pkp", "c", "3"), "[pkp]\na = 1\nc = 3\n\n[net]\nb = 2\n", "added after the last line of the section, before the blank line");
    same(ini_with_value("[pkp]\na = 1\n", "pkp", "c", "3"), "[pkp]\na = 1\nc = 3\n", "added at the end of the last section");
    same(ini_with_value("[pkp]\na = 1", "pkp", "c", "3"), "[pkp]\na = 1\nc = 3\n", "added after a last line with no newline");
    same(ini_with_value("[pkp]\n[net]\nb = 2\n", "pkp", "c", "3"), "[pkp]\nc = 3\n[net]\nb = 2\n", "an empty section gets the key under its header");
    same(ini_with_value("[pkp]\na = 1\n[net]\nb = 2\n[PKP]\nd = 4\n", "pkp", "c", "3"), "[pkp]\na = 1\n[net]\nb = 2\n[PKP]\nd = 4\nc = 3\n", "a section met twice: the key goes to its last block");

    // a missing section goes at the end, after a blank line
    same(ini_with_value("", "pkp", "k", "1"), "[pkp]\nk = 1\n", "empty file");
    same(ini_with_value("[net]\nb = 2\n", "pkp", "k", "1"), "[net]\nb = 2\n\n[pkp]\nk = 1\n", "new section after a blank line");
    same(ini_with_value("[net]\nb = 2", "pkp", "k", "1"), "[net]\nb = 2\n\n[pkp]\nk = 1\n", "new section after a last line with no newline");
    same(ini_with_value("[net]\nb = 2\n\n", "pkp", "k", "1"), "[net]\nb = 2\n\n[pkp]\nk = 1\n", "no second blank line");

    // CRLF files stay CRLF
    same(ini_with_value("[pkp]\r\na = 1\r\n", "pkp", "c", "3"), "[pkp]\r\na = 1\r\nc = 3\r\n", "CRLF: key added");
    same(ini_with_value("[pkp]\r\na = 1 ; x\r\n", "pkp", "a", "0"), "[pkp]\r\na = 0 ; x\r\n", "CRLF: value replaced");
    same(ini_with_value("[net]\r\nb = 2\r\n", "pkp", "k", "1"), "[net]\r\nb = 2\r\n\r\n[pkp]\r\nk = 1\r\n", "CRLF: section added");

    // values that need quotes
    same(ini_with_value("", "s", "k", ""), "[s]\nk = \"\"\n", "empty value is quoted");
    same(ini_with_value("", "s", "k", "a b"), "[s]\nk = a b\n", "inner space needs no quote");
    same(ini_with_value("", "s", "k", " a"), "[s]\nk = \" a\"\n", "edge white space is quoted");
    same(ini_with_value("", "s", "k", "a;b"), "[s]\nk = \"a;b\"\n", "';' is quoted");

    // idempotent, and the second change of a file keeps the first
    const std::string once = ini_with_value("[pkp]\nx = 1\n", "pkp", "k", "1");
    same(ini_with_value(once, "pkp", "k", "1"), once, "setting the same value again changes nothing");
    same(ini_with_value(once, "pkp", "x", "2"), "[pkp]\nx = 2\nk = 1\n", "a second key edit keeps the first");

    // the file route: atomic replace, a missing file is created, nothing .tmp is left, and config.h reads back what was written
    const auto dir = std::filesystem::temp_directory_path() / ("nxrt-ini-edit-test-" + std::to_string(std::time(nullptr)));
    std::filesystem::remove_all(dir);   // a run that failed before cleaning up must not leak into this one
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "pkp.ini").string();
    std::string err;
    check(nxrt::config_save_key(path.c_str(), "pkp", "camera_aim", "1", &err), "save into a missing file");
    same(slurp(path), "[pkp]\ncamera_aim = 1\n", "the new file");
    check(nxrt::config_get_bool("pkp", "camera_aim", false), "the value is in force at once");
    {
        std::ofstream f(path, std::ios::binary | std::ios::app);
        f << "; my notes\n[fs]\nsave_dir = save\n";
    }
    check(nxrt::config_save_key(path.c_str(), "pkp", "jump_momentum", "1", &err), "save a second key");
    same(slurp(path), "[pkp]\ncamera_aim = 1\njump_momentum = 1\n; my notes\n[fs]\nsave_dir = save\n", "the file keeps its comment and other section");
    check(nxrt::config_load_file(path.c_str()), "the file loads");
    check(nxrt::config_get_bool("pkp", "jump_momentum", false) && nxrt::config_get_bool("pkp", "camera_aim", false), "read back after loading");
    check(nxrt::config_get_str("fs", "save_dir") == "save", "other sections read back too");
    check(!std::filesystem::exists(path + ".tmp"), "no temp file left");
    const std::string unwritable = (dir / "no-such-folder" / "pkp.ini").string();
    check(!nxrt::config_save_key(unwritable.c_str(), "pkp", "x", "1", &err) && !err.empty(), "an unwritable place fails with a reason");
    check(!nxrt::config_has("pkp", "x"), "a failed save sets nothing");
    std::filesystem::remove_all(dir);

    std::cout << "ini_edit tests passed (" << checks << " checks)\n";
    return 0;
}
