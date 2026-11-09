// SPDX-License-Identifier: GPL-3.0-or-later
// Writes the docs of controls.ini from the settings table (src/gfx/play/play_config_table.h) with the factory defaults of one game:
//   nxrt_settings_doc_<game> --markdown docs/CONFIG.md --schema docs/controls_schema.json
// --markdown replaces the text between the two marker lines of the file (its line endings kept); --schema writes the launcher's JSON.
// runtime/scripts/gen_settings_doc.ps1 builds and runs it.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "play_config_table.h"
#include "setting_doc.h"

#ifndef NXRT_SETTINGS_DOC_REGISTER
#error "NXRT_SETTINGS_DOC_REGISTER names the game's data registration function"
#endif
void NXRT_SETTINGS_DOC_REGISTER();

namespace {

constexpr const char* kBegin = "<!-- controls.ini keys: generated from src/gfx/play/play_config_table.cpp by scripts/gen_settings_doc.ps1, do not edit -->";
constexpr const char* kEnd = "<!-- end of the generated controls.ini keys -->";

bool read_file(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const std::string& path, const std::string& text) {
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

}  // namespace

int main(int argc, char** argv) {
    NXRT_SETTINGS_DOC_REGISTER();
    const std::vector<nxrt::setting_doc::Row> rows = nxrt::play::settings_doc_rows();
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string opt = argv[i], path = argv[i + 1];
        if (opt == "--schema") {
            if (!write_file(path, nxrt::setting_doc::schema_json("controls.ini", rows))) {
                std::fprintf(stderr, "cannot write %s\n", path.c_str());
                return 1;
            }
        } else if (opt == "--markdown") {
            std::string doc;
            if (!read_file(path, &doc)) {
                std::fprintf(stderr, "cannot read %s\n", path.c_str());
                return 1;
            }
            const size_t b = doc.find(kBegin), e = doc.find(kEnd);
            if (b == std::string::npos || e == std::string::npos || e < b) {
                std::fprintf(stderr, "%s has no marker lines:\n%s\n%s\n", path.c_str(), kBegin, kEnd);
                return 1;
            }
            const bool crlf = doc.find("\r\n") != std::string::npos;
            const size_t body = b + std::string(kBegin).size();
            const std::string text = with_newlines("\n\n" + nxrt::setting_doc::markdown(rows) + "\n", crlf);
            doc.replace(body, e - body, text);
            if (!write_file(path, doc)) {
                std::fprintf(stderr, "cannot write %s\n", path.c_str());
                return 1;
            }
        } else {
            std::fprintf(stderr, "usage: %s [--markdown CONFIG.md] [--schema controls_schema.json]\n", argv[0]);
            return 1;
        }
    }
    return 0;
}
