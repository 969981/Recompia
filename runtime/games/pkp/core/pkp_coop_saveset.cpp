// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the co-op save folder on disk (pkp_coop_saveset.h). Every function here may run on a worker thread: it touches files
// only, under the two folders fixed at install.
//
// The game writes its save through the runtime's journalled save volume (runtime/src/services/fs_volume.h): the account folder holds
// the committed files, changes land in a work folder and one rename decides a commit, which then moves the changed files over the
// committed ones while <account>.commit exists (save_files::commit_pending). A copy is read only while no commit is pending and its
// files did not change while read. A received file is moved over a committed one only outside a commit too; on a member the game
// never writes the world files of a whole copy (the hold, pkp_coop_savecopy.cpp), so the two never move the same file.
#include "core/pkp_coop_saveset.h"
#include "core/pkp_save_files.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>

#include "miniz.h"
#include "nxrt/file_write.h"

namespace nxrt::games::pkp::coop::saveset {

namespace {

namespace stdfs = std::filesystem;

constexpr uint32_t kSaveMagic = 0x31227687;
constexpr size_t kEndAt = 0x10;                 // u32: end of the zlib stream (header + compressed length), 0 when stored whole
constexpr size_t kMinHeader = 0x40;             // the smallest header (SYDAT, SCSHDAT)
constexpr const char* kStamp = "stamp.json";    // coop_save.rs STAMP
constexpr const char* kPeers = "peers.json";    // coop_save.rs PEERS
constexpr const char* kIncoming = "incoming";
constexpr const char* kReady = "ready.txt";     // a received copy is whole and checked: its moves may run
constexpr int kCommitWaitMs = 10000;            // apply() waits this long at most for a pending commit of the game to finish

struct Paths {
    stdfs::path save, account, incoming;   // incoming: <save>/incoming/account/<key>, the mirror of account/
};
Paths& paths() {
    static Paths p;
    return p;
}

stdfs::path in_save(const char* name) { return paths().save / name; }
stdfs::path slot_path(const stdfs::path& root, const std::string& rel) { return root / save_files::from_u8(rel); }
std::string backup_of(const std::string& rel) { return "B00" + rel.substr(3); }   // "G00/x" -> "B00/x"

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

int64_t unix_mtime(const stdfs::path& p) {
    std::error_code ec;
    const auto t = stdfs::last_write_time(p, ec);
    if (ec) return 0;
    // file time -> wall clock by the two clocks' current offset: std::chrono::clock_cast is missing from the Linux and macOS
    // standard libraries the port builds with; the stamp's seconds need no more than this.
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - stdfs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

bool read_file(const stdfs::path& p, std::vector<uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

// Moves a over b (b replaced). A few short retries: an antivirus scan or the game's own read may hold b for a moment (the runtime opens
// save files with FILE_SHARE_DELETE, so a rename over them is allowed).
bool move_over(const stdfs::path& a, const stdfs::path& b, std::string* why) {
    std::error_code ec;
    stdfs::create_directories(b.parent_path(), ec);
    for (int tries = 0; tries < 8; ++tries) {
        if (tries) std::this_thread::sleep_for(std::chrono::milliseconds(50 * tries));
        if (MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    }
    *why = "cannot move " + save_files::u8(a.filename()) + " into place (error " + std::to_string(GetLastError()) + ")";
    return false;
}

bool wait_no_commit() {
    for (int waited = 0; save_files::commit_pending(); waited += 100) {
        if (waited >= kCommitWaitMs) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

// ---- the small text files ------------------------------------------------------------------------------------------------------

// The unsigned number after "key": in a JSON object's text (the files written here and by the launcher); 0 when absent.
uint64_t json_u64(std::string_view text, std::string_view key) {
    const std::string k = "\"" + std::string(key) + "\"";
    const size_t at = text.find(k);
    if (at == std::string_view::npos) return 0;
    size_t i = text.find(':', at + k.size());
    if (i == std::string_view::npos) return 0;
    ++i;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    uint64_t v = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) v = v * 10 + static_cast<uint64_t>(text[i++] - '0');
    return v;
}

bool read_text(const stdfs::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool hex_id(std::string_view s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// ---- the ready list (incoming/ready.txt): "seq N", "saved_at N", "by ID", then "all NAME" and "recv NAME" lines ----

struct Ready {
    Stamp stamp;
    std::string by;
    std::vector<std::string> all, received;
};

bool write_ready(const Ready& r, std::string* why) {
    std::string t = "seq " + std::to_string(r.stamp.seq) + "\nsaved_at " + std::to_string(r.stamp.saved_at) + "\nby " + r.by + "\n";
    for (const std::string& n : r.all) t += "all " + n + "\n";
    for (const std::string& n : r.received) t += "recv " + n + "\n";
    std::string err;
    if (nxrt::write_file_atomic(save_files::u8(paths().save / kIncoming / kReady), t, &err)) return true;
    *why = err;
    return false;
}

bool read_ready(Ready* r) {
    std::string t;
    if (!read_text(paths().save / kIncoming / kReady, &t)) return false;
    size_t at = 0;
    bool have_seq = false;
    while (at < t.size()) {
        size_t end = t.find('\n', at);
        if (end == std::string::npos) end = t.size();
        std::string line = t.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string key = line.substr(0, sp), val = line.substr(sp + 1);
        if (key == "seq") {
            r->stamp.seq = std::strtoull(val.c_str(), nullptr, 10);
            have_seq = true;
        } else if (key == "saved_at") {
            r->stamp.saved_at = std::strtoull(val.c_str(), nullptr, 10);
        } else if (key == "by") {
            r->by = val;
        } else if ((key == "all" || key == "recv") && world_name(val, false)) {
            (key == "all" ? r->all : r->received).push_back(val);
        }
    }
    return have_seq && r->stamp.seq > 0;
}

// The moves of a ready copy, each safe to run twice (see apply in the header).
bool run_moves(const Ready& r, std::string* why) {
    const Paths& p = paths();
    std::error_code ec;
    for (const std::string& n : r.received) {
        const stdfs::path from = slot_path(p.incoming, n), to = slot_path(p.account, n);
        if (!stdfs::exists(from, ec)) continue;   // moved already
        if (stdfs::exists(to, ec) && !move_over(to, slot_path(p.account, backup_of(n)), why)) return false;
        if (!move_over(from, to, why)) return false;
    }
    for (const Seen& s : scan()) {
        if (std::find(r.all.begin(), r.all.end(), s.name) != r.all.end()) continue;
        if (!move_over(slot_path(p.account, s.name), slot_path(p.account, backup_of(s.name)), why)) return false;
    }
    if (!write_stamp(r.stamp, r.by, why)) return false;
    drop_incoming();
    return true;
}

}  // namespace

bool world_name(std::string_view rel, bool backup_too) {
    if (rel.size() < 5 || rel[3] != '/') return false;
    const std::string_view slot = rel.substr(0, 3), file = rel.substr(4);
    if (!iequal(slot, "G00") && !(backup_too && iequal(slot, "B00"))) return false;
    if (iequal(file, "SCSHDAT.BIN")) return true;
    return file.size() == 12 && iequal(file.substr(0, 6), "STGDAT") && std::isdigit(static_cast<unsigned char>(file[6])) &&
           std::isdigit(static_cast<unsigned char>(file[7])) && iequal(file.substr(8), ".BIN");
}

void install() {
    Paths& p = paths();
    p.save = save_files::save_dir();
    p.account = save_files::account_dir();
    p.incoming = p.save / kIncoming / "account" / save_files::account_key();
}

stdfs::path account_dir() { return paths().account; }

std::vector<Seen> scan() {
    std::vector<Seen> out;
    std::error_code ec;
    for (stdfs::directory_iterator it(paths().account / "G00", ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        const std::string rel = "G00/" + save_files::u8(it->path().filename());
        if (!world_name(rel, false)) continue;
        out.push_back(Seen{rel, static_cast<uint64_t>(it->file_size(e2)), unix_mtime(it->path())});
    }
    std::sort(out.begin(), out.end(), [](const Seen& a, const Seen& b) { return a.name < b.name; });
    return out;
}

uint64_t hash(const uint8_t* p, size_t n) {
    const uint64_t crc = mz_crc32(MZ_CRC32_INIT, p, n);
    const uint64_t adler = mz_adler32(MZ_ADLER32_INIT, p, n);
    return crc << 32 | (adler & 0xffffffffu);
}

bool header_ok(const uint8_t* p, size_t n) {
    if (n < kMinHeader) return false;
    uint32_t magic, end;
    std::memcpy(&magic, p, 4);
    std::memcpy(&end, p + kEndAt, 4);
    return magic == kSaveMagic && end <= n;
}

bool read(Snapshot* out, std::string* why) {
    *out = Snapshot{};
    if (save_files::commit_pending()) {
        *why = "a save of the game is being committed";
        return false;
    }
    const std::vector<Seen> before = scan();
    if (before.empty()) {
        *why = "no world files";
        return false;
    }
    if (before.size() > kMaxFiles) {
        *why = "more world files than a copy carries";
        return false;
    }
    for (const Seen& s : before) {
        std::vector<uint8_t> b;
        if (!read_file(slot_path(paths().account, s.name), &b) || b.size() != s.size || b.size() > kMaxFileBytes) {
            *why = s.name + " changed or could not be read";
            return false;
        }
        if (!header_ok(b.data(), b.size())) {
            *why = s.name + " is not a game save file";
            return false;
        }
        out->files.push_back(File{s.name, b.size(), hash(b.data(), b.size())});
        out->bytes.push_back(std::move(b));
        out->newest = std::max(out->newest, s.mtime);
    }
    if (save_files::commit_pending() || scan() != before) {
        *why = "the game saved while the copy was read";
        *out = Snapshot{};
        return false;
    }
    return true;
}

bool hashes(std::vector<File>* out, std::string* why) {
    Snapshot s;
    out->clear();
    if (scan().empty()) return true;   // no world on this PC yet: an empty list
    if (!read(&s, why)) return false;
    *out = std::move(s.files);
    return true;
}

bool has_stamp() {
    std::string t;
    return read_text(in_save(kStamp), &t) && t.find("\"seq\"") != std::string::npos;
}

Stamp own_stamp() {
    std::string t;
    if (!read_text(in_save(kStamp), &t)) return Stamp{};
    return Stamp{json_u64(t, "seq"), json_u64(t, "saved_at")};
}

bool write_stamp(const Stamp& s, const std::string& by, std::string* why) {
    const std::string t = "{ \"seq\": " + std::to_string(s.seq) + ", \"saved_at\": " + std::to_string(s.saved_at) + ", \"by\": \"" +
                          (hex_id(by) ? by : std::string()) + "\" }\n";
    std::string err;
    if (nxrt::write_file_atomic(save_files::u8(in_save(kStamp)), t, &err)) return true;
    *why = err;
    return false;
}

std::map<std::string, Stamp> read_peers() {
    std::map<std::string, Stamp> out;
    std::string t;
    if (!read_text(in_save(kPeers), &t)) return out;
    // { "peers": { "<id>": { "seq": n, "saved_at": n }, ... } }: every quoted 32-hex key followed by an object
    for (size_t at = t.find('"'); at != std::string::npos; at = t.find('"', at + 1)) {
        const size_t close = t.find('"', at + 1);
        if (close == std::string::npos) break;
        const std::string key = t.substr(at + 1, close - at - 1);
        at = close;
        if (!hex_id(key)) continue;
        const size_t open = t.find('{', close), end = t.find('}', close);
        if (open == std::string::npos || end == std::string::npos || open > end) continue;
        const std::string_view body(t.data() + open, end - open);
        std::string id = key;
        for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out[id] = Stamp{json_u64(body, "seq"), json_u64(body, "saved_at")};
        at = end;
    }
    return out;
}

bool write_peers(const std::map<std::string, Stamp>& peers, std::string* why) {
    std::string t = "{ \"peers\": {";
    bool first = true;
    for (const auto& [id, s] : peers) {
        if (!hex_id(id)) continue;
        t += (first ? "\n  \"" : ",\n  \"") + id + "\": { \"seq\": " + std::to_string(s.seq) + ", \"saved_at\": " + std::to_string(s.saved_at) + " }";
        first = false;
    }
    t += first ? "} }\n" : "\n} }\n";
    std::string err;
    if (nxrt::write_file_atomic(save_files::u8(in_save(kPeers)), t, &err)) return true;
    *why = err;
    return false;
}

void drop_incoming() {
    std::error_code ec;
    stdfs::remove_all(paths().save / kIncoming, ec);
}

bool incoming_create(const std::string& name, std::string* why) {
    if (!world_name(name, false)) {
        *why = "not a world file name";
        return false;
    }
    const stdfs::path p = slot_path(paths().incoming, name);
    std::error_code ec;
    stdfs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (f) return true;
    *why = "cannot create " + name + " in incoming/";
    return false;
}

bool incoming_append(const std::string& name, const uint8_t* p, size_t n, std::string* why) {
    std::ofstream f(slot_path(paths().incoming, name), std::ios::binary | std::ios::app);
    f.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
    f.flush();
    if (f) return true;
    *why = "cannot write " + name + " in incoming/ (disk full?)";
    return false;
}

bool apply(const Stamp& s, const std::string& by, const std::vector<File>& all, const std::vector<std::string>& received,
           bool* ready, std::string* why) {
    const Paths& p = paths();
    *ready = false;
    Ready r{s, by, {}, received};
    for (const File& f : all) {
        if (!world_name(f.name, false)) {
            *why = f.name + " is not a world file name";
            return false;
        }
        r.all.push_back(f.name);
        const bool got = std::find(received.begin(), received.end(), f.name) != received.end();
        std::vector<uint8_t> b;
        if (!read_file(slot_path(got ? p.incoming : p.account, f.name), &b) || b.size() != f.size || hash(b.data(), b.size()) != f.hash ||
            !header_ok(b.data(), b.size())) {
            *why = f.name + (got ? " did not arrive whole" : " on this PC is not the copy's");
            return false;
        }
    }
    if (!wait_no_commit()) {
        *why = "a save of the game stayed in its commit";
        return false;
    }
    if (!write_ready(r, why)) return false;
    *ready = true;
    return run_moves(r, why);
}

bool ready_pending() {
    Ready r;
    return read_ready(&r);
}

bool finish(std::string* why) {
    Ready r;
    if (!read_ready(&r)) {
        drop_incoming();
        return false;
    }
    if (save_files::commit_pending()) {   // the game's own commit first (the runtime finishes an interrupted one when the save opens)
        *why = "the game's save has a commit pending";
        return false;
    }
    return run_moves(r, why);
}

}  // namespace nxrt::games::pkp::coop::saveset
