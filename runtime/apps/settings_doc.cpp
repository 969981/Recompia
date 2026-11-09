// SPDX-License-Identifier: GPL-3.0-or-later
// Writes the docs of the settings from their tables, with the factory defaults of one game:
//   nxrt_settings_doc_<game> --markdown docs/CONFIG.md --schema docs/controls_schema.json --game-schema docs/<game>_ini_schema.json
// controls.ini: the play-mode settings table (src/gfx/play/play_config_table.h). The game ini: the keys of the game's own section, the
// rows game::Info::settings_doc gives (none when it is null). The game is only registered (its pure-data library), never booted.
// --markdown replaces the text between each pair of marker lines of the file (its line endings kept); --schema and --game-schema write the
// launcher's JSON of each file. --check compares instead of writing and fails when a file would change. runtime/scripts/gen_settings_doc.ps1
// builds and runs it.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "nxrt/game.h"
#include "nxrt/setting_doc.h"
#include "play_config_table.h"

#ifndef NXRT_SETTINGS_DOC_REGISTER
#error "NXRT_SETTINGS_DOC_REGISTER names the game's data registration function"
#endif
void NXRT_SETTINGS_DOC_REGISTER();

namespace {

namespace doc = nxrt::setting_doc;

struct Block {
    const char* begin;
    const char* end;
};
constexpr Block kControlsBlock = {
    "<!-- controls.ini keys: generated from src/gfx/play/play_config_table.cpp by scripts/gen_settings_doc.ps1, do not edit -->",
    "<!-- end of the generated controls.ini keys -->"};
constexpr Block kGameBlock = {
    "<!-- game ini keys: generated from the game module's settings table (game::Info::settings_doc) by scripts/gen_settings_doc.ps1, do not edit -->",
    "<!-- end of the generated game ini keys -->"};

bool g_check = false;   // --check: report the files that would change, write nothing
int g_stale = 0;

bool read_file(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// The text without carriage returns: a checkout may have either line ending (git normalises them), only the content counts.
std::string without_cr(const std::string& s) {
    std::string r;
    for (char c : s)
        if (c != '\r') r += c;
    return r;
}

// Writes the file, or with --check only compares it. False on a write error.
bool put_file(const std::string& path, const std::string& text) {
    if (g_check) {
        std::string old;
        if (!read_file(path, &old) || without_cr(old) != without_cr(text)) {
            std::fprintf(stderr, "out of date: %s\n", path.c_str());
            ++g_stale;
        }
        return true;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

std::string with_newlines(const std::string& text, bool crlf) {
    if (!crlf) return text;
    std::string r;
    for (char c : text) {
        if (c == '\n') r += '\r';
        r += c;
    }
    return r;
}

// Replaces the text between the block's marker lines. False (and a message) when the markers are missing.
bool fill_block(std::string* doc_text, const Block& blk, const std::string& body, const std::string& path) {
    const size_t b = doc_text->find(blk.begin), e = doc_text->find(blk.end);
    if (b == std::string::npos || e == std::string::npos || e < b) {
        std::fprintf(stderr, "%s has no marker lines:\n%s\n%s\n", path.c_str(), blk.begin, blk.end);
        return false;
    }
    const bool crlf = doc_text->find("\r\n") != std::string::npos;
    const size_t start = b + std::string(blk.begin).size();
    doc_text->replace(start, e - start, with_newlines("\n\n" + body + "\n", crlf));
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    NXRT_SETTINGS_DOC_REGISTER();
    const nxrt::game::Info& game = nxrt::game::info();
    const std::vector<doc::Row> controls = nxrt::play::settings_doc_rows();
    const std::vector<doc::Row> game_rows = game.settings_doc ? game.settings_doc() : std::vector<doc::Row>{};
    const std::string game_ini = *game.game_ini ? game.game_ini : "nxrt.ini";

    std::vector<std::string> args(argv + 1, argv + argc);
    for (const std::string& a : args) g_check = g_check || a == "--check";
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& opt = args[i];
        if (opt == "--check") continue;
        if (i + 1 >= args.size()) {
            std::fprintf(stderr, "%s needs a file\n", opt.c_str());
            return 1;
        }
        const std::string path = args[++i];
        bool ok = true;
        if (opt == "--schema") {
            ok = put_file(path, doc::schema_json("controls.ini", controls));
        } else if (opt == "--game-schema") {
            ok = put_file(path, doc::schema_json(game_ini, game_rows, doc::Store::GameIni));
        } else if (opt == "--markdown") {
            std::string text;
            if (!read_file(path, &text)) {
                std::fprintf(stderr, "cannot read %s\n", path.c_str());
                return 1;
            }
            if (!fill_block(&text, kControlsBlock, doc::markdown(controls), path)) return 1;
            // A game without a table of its own needs no game block.
            if ((!game_rows.empty() || text.find(kGameBlock.begin) != std::string::npos) &&
                !fill_block(&text, kGameBlock, doc::markdown(game_rows, doc::Store::GameIni), path))
                return 1;
            ok = put_file(path, text);
        } else {
            std::fprintf(stderr, "usage: %s [--check] [--markdown CONFIG.md] [--schema controls_schema.json] [--game-schema <file>]\n", argv[0]);
            return 1;
        }
        if (!ok) {
            std::fprintf(stderr, "cannot write %s\n", path.c_str());
            return 1;
        }
    }
    return g_stale ? 2 : 0;
}
