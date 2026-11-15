// SPDX-License-Identifier: GPL-3.0-or-later
// Play as Pokemon, the copied Pokemon (pkp_play_as_list.h). The file is one line per Pokemon, `number<TAB>species`, in the order they
// were copied; a line that is not two numbers in range is skipped, so a hand-edited file cannot break the list. The names are not
// stored: they come from the game in its language each time (layout::kSpeciesName, the table the game names a Pokemon from).
#include "input/pkp_play_as_list.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "nxrt/log.h"

namespace nxrt::games::pkp::play_as {
namespace {

constexpr const char* kTag = "pkp";
constexpr const char* kFile = "play_as_pokemon.tsv";
constexpr size_t kMost = 256;   // what one slider of the Settings screen can show

std::vector<Copied> g_list;
bool g_read = false;
uint64_t g_base = 0;

void read() {
    g_read = true;
    std::ifstream file(kFile);
    unsigned number = 0, species = 0;
    for (std::string line; g_list.size() < kMost && std::getline(file, line);) {
        if (std::sscanf(line.c_str(), "%u\t%u", &number, &species) != 2) continue;
        if (number <= layout::kFormHuman || number > layout::kFormMax || species > layout::kSpeciesMax) continue;
        if (std::none_of(g_list.begin(), g_list.end(), [&](const Copied& e) { return e.number == number; }))
            g_list.push_back({static_cast<uint16_t>(number), static_cast<uint16_t>(species)});
    }
    NXLOG_I(kTag, "play as Pokemon: %zu copied Pokemon in %s", g_list.size(), kFile);
}

void write() {
    std::ofstream file(kFile, std::ios::trunc);
    for (const Copied& e : g_list) file << e.number << '\t' << e.species << '\n';
    if (!file) NXLOG_W(kTag, "play as Pokemon: %s could not be written, the copied Pokemon are kept until the game closes", kFile);
}

}  // namespace

void start(uint64_t base) { g_base = base; }
void stop() { g_base = 0; }

const std::vector<Copied>& copied() {
    if (!g_read) read();
    return g_list;
}

void remember(Copied c) {
    if (!g_read) read();
    const auto it = std::find_if(g_list.begin(), g_list.end(), [&](const Copied& e) { return e.number == c.number; });
    if (it != g_list.end()) {
        if (it->species == c.species || !c.species) return;
        it->species = c.species;
    } else {
        if (g_list.size() >= kMost) g_list.erase(g_list.begin());   // the oldest makes room
        g_list.push_back(c);
    }
    write();
}

std::string species_name(::Ctx* c, uint16_t species) {
    std::string out;
    if (!c || !g_base || !species || species > layout::kSpeciesMax) return out;
    const uint64_t p = guest::call(c, g_base + layout::kSpeciesName, species);
    for (uint64_t a = p; p && out.size() < 64 && guest::ok(a, 1); ++a) {
        const char ch = guest::rd<char>(a);
        if (!ch) break;
        out += ch;
    }
    return out;
}

}  // namespace nxrt::games::pkp::play_as
