// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the player's save on the host's disk, for the modules that work on its files while the game is not using them (the
// UUID repair, Story co-op's copy and journal). The save is <save dir>/account/<key>/ ([fs] save_dir; fs.cpp names the folder after the
// profile's user) with <key>.meta next to it.
#pragma once
#include <cstddef>
#include <filesystem>
#include <string>

namespace nxrt::games::pkp::save_files {

std::filesystem::path from_u8(const std::string& s);
std::string u8(const std::filesystem::path& p);

// [fs] save_dir.
std::filesystem::path save_dir();
// The account save's folder name: the profile user's Uid, its 16 bytes in memory order, as hex.
std::string account_key();
// <save dir>/account/<key>.
std::filesystem::path account_dir();
// True while the save has a commit the game did not finish (<key>.commit): its files are then not one consistent save.
bool commit_pending();

// Copies the account save (and <key>.meta) to <save dir>/<folder>/<yyyymmdd-hhmmss>/<key>. With keep above 0 only the newest `keep`
// copies of that folder stay. False when there is no save, a commit is pending or the copy failed (*error says which, empty for no save).
bool copy_account(const char* folder, size_t keep, std::filesystem::path* where, std::string* error);

}  // namespace nxrt::games::pkp::save_files
