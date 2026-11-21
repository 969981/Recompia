// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the co-op save folder on this PC's disk (launcher/src-tauri/src/coop_save.rs has the folder contract): the group's
// world files of the account save, this copy's stamp, what is known of the other members' copies, and a received copy's way from
// incoming/ over account/. Files only: no game memory, no channel. Used by pkp_coop_savecopy.cpp (the copy over the channel and the
// hold of a member's world writes).
//
// Which files are the group's (modding-docs coop-save/SAVE-FILES-RE.md 1, SAVE-SYNC-RE.md 1): the account save is G00/ (the save),
// B00/ (the game's backup copy of it) and S00/:
//   G00/STGDATnn.BIN  one per stage: the island's world (buildings, placed objects, the stage characters, storage boxes, photo slots).
//                     Its m_player (data 0x10..0x3f, the save owner's position) is per player but cannot be split from the file.
//   G00/SCSHDAT.BIN   the photos the STGDAT photo slots point into: travels with the STGDAT.
//   G00/CMNDAT.BIN    NOT the group's: it holds the story and the residents' relations, but in the same file the player's look
//                     (m_CharacterCreate), name, Life Coins (layout::kSaveMoney), challenge claims, the Ordered Items / Gifts boxes
//                     and the bags. A member's take-out must reach disk in it (build/takeout.md). It stays each player's own; its story
//                     part is kept in step during a session by the co-op parts (quests, records, unlocks ...), not by the save copy.
//   S00/SYDAT.BIN     the player's own: user id and name, controls, options.
// So the group's world is the set of G00 world files (world_name); the B00 world files are this PC's previous copy of it.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace nxrt::games::pkp::coop::saveset {

constexpr size_t kMaxFiles = 32;                        // world files in one copy (stages + the photos)
constexpr size_t kMaxName = 40;                         // "G00/STGDAT01.BIN" is 16
constexpr uint64_t kMaxFileBytes = uint64_t{256} << 20; // one file (a played STGDAT is about 3 MB compressed)

// A copy's stamp (coop_save.rs Stamp): ordered by seq, then saved_at (unix seconds).
struct Stamp {
    uint64_t seq = 0, saved_at = 0;
    bool operator<(const Stamp& o) const { return seq != o.seq ? seq < o.seq : saved_at < o.saved_at; }
    bool operator==(const Stamp& o) const { return seq == o.seq && saved_at == o.saved_at; }
};

// One world file: its name relative to the account folder ('/' separated), size and hash().
struct File {
    std::string name;
    uint64_t size = 0, hash = 0;
    bool operator==(const File& o) const { return name == o.name && size == o.size && hash == o.hash; }
};

// A world file as listed, without reading it.
struct Seen {
    std::string name;
    uint64_t size = 0;
    int64_t mtime = 0;   // unix seconds
    bool operator==(const Seen& o) const { return name == o.name && size == o.size && mtime == o.mtime; }
};

// A whole copy read from account/: its files in name order and their bytes.
struct Snapshot {
    std::vector<File> files;
    std::vector<std::vector<uint8_t>> bytes;
    int64_t newest = 0;   // the newest file time (unix seconds)
};

// rel ('/' separated, relative to the account folder) is a world file: "G00/" (or "B00/" with backup_too) then "STGDATnn.BIN" or
// "SCSHDAT.BIN", any case.
bool world_name(std::string_view rel, bool backup_too);

// Fixed at install: the co-op folder ([fs] save_dir) and the account folder in it. Every other call works on these.
void install();
std::filesystem::path account_dir();

// The world files of G00/ as they are on disk, in name order (none when the folder is missing).
std::vector<Seen> scan();
// Reads the G00 world files whole. False (why says it) when a commit of the game is pending, a file changes while read, or one is
// not a game save file (header_ok).
bool read(Snapshot* out, std::string* why);
// The hashes of the G00 world files (read and hashed), for this PC's copy.
bool hashes(std::vector<File>* out, std::string* why);

// crc32 in the high half, adler32 in the low (miniz): a transfer check, not a signature.
uint64_t hash(const uint8_t* p, size_t n);
// A game save file's header (SAVE-FILES-RE.md 1): magic 0x31227687, and the end of the compressed stream at +0x10 inside the file
// (0 for a file stored whole).
bool header_ok(const uint8_t* p, size_t n);

// stamp.json: this copy's stamp and who wrote it. has_stamp: the file reads (a whole copy of the group's world is on disk).
bool has_stamp();
Stamp own_stamp();
bool write_stamp(const Stamp& s, const std::string& by, std::string* why);

// peers.json: what this PC knows of the other members' copies, by member id (32 lower-case hex digits).
std::map<std::string, Stamp> read_peers();
bool write_peers(const std::map<std::string, Stamp>& peers, std::string* why);

// ---- a copy being received (incoming/) ----
// Empties incoming/ (a copy cut short, or one this PC gives up).
void drop_incoming();
// The file of `name` inside incoming/ (created with its folders, empty) for writing a received copy into.
bool incoming_create(const std::string& name, std::string* why);
bool incoming_append(const std::string& name, const uint8_t* p, size_t n, std::string* why);
// Checks every received file (size, hash, header), then makes the copy whole on disk: incoming/ready.json (the copy's stamp, its
// writer and every file of it) first; then, file by file, the old G00 file goes to B00 (this PC's previous copy) and the received
// one takes its place; G00 world files the copy does not have go to B00 too; stamp.json last; then incoming/ is emptied. A process
// stopped half way leaves ready.json, and finish() at the next start completes the same steps (each one can run twice). `received`:
// the files whose bytes are in incoming/; the others of `all` must be in G00 as they are. False with *ready false: nothing was moved
// (incoming/ may be dropped); with *ready true: the copy is ready but its moves did not all run (incoming/ must stay: finish()).
bool apply(const Stamp& s, const std::string& by, const std::vector<File>& all, const std::vector<std::string>& received,
           bool* ready, std::string* why);
// Completes a ready copy (apply's moves, then stamp.json); with no ready copy, drops whatever is in incoming/. True when a copy was
// completed. At install (before the game opens its save), and again while a ready copy waits.
bool finish(std::string* why);
// incoming/ holds a ready copy whose moves have not all run.
bool ready_pending();

}  // namespace nxrt::games::pkp::coop::saveset
