// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's one shared save (pkp_coop_internal.h). A co-op save is one world for the group: the authority's save
// point is the group's, and every member keeps a whole copy of it, so whoever has the newest copy can host the next session
// (launcher/src-tauri/src/coop_save.rs: the folder contract, choose_host). What a copy is: pkp_coop_saveset.h (the G00 world files;
// CMNDAT and SYDAT stay each player's own).
//
// The authority (the session's authority, or before and outside a session the process the launcher started as host):
//   - a save point: the game commits its save and the world files on disk change (polled every kPollEvery; nothing hooks the save
//     writers 0x241324 / 0x245200 / 0x5a8a34, the runtime's commit is where the files are whole). Once they held still for kStable
//     and no commit is pending, they are read (a worker thread) and, when their hashes differ from the last copy, stamped: seq =
//     the highest seq it knows (its own, every member's) + 1, saved_at = now, stamp.json written after the files (they are on disk
//     already). The first read of a launch whose files are not newer than its stamp keeps that stamp (nothing changed since).
//   - each member that said hello (SaveHave: its member key, its copy's stamp and file hashes) is sent SaveOffer (the stamp, every
//     file with its hash and whether its bytes follow, and every member's stamp it knows); then the files it lacks as SaveChunks,
//     kChunk bytes each, the next one only when less than half a chunk is still queued for it, so the session's other messages
//     never wait long behind a copy. A member whose copy is as new as the authority's or newer (it hosted a session this one did
//     not see) gets the authority's world stamped above it: the session's world is the group's.
// A member (every other process):
//   - says hello at session up and again every kHelloEvery until an offer came (an older authority never answers: nothing changes).
//   - an offer newer than its copy: incoming/ is emptied, the files whose bytes follow are written there as they arrive (in order,
//     each chunk at the offset it says). Once all are in, a worker thread checks them (size, hash, a game save header) together with
//     the files the copy shares with this PC's, then moves them over account/ and writes stamp.json last (saveset::apply). A copy
//     cut short (the session ends, no chunk for kChunkWait, a write fails) is dropped: this PC keeps its previous whole copy and
//     stamp. A copy that fails its check is asked for once more (SaveHave status 2), then given up until the next one.
//   - each applied copy is told back (SaveHave status 1), so the authority knows every member's stamp.
// What every PC knows of the copies (peers.json, read by the launcher's choose_host): the authority learns each member's stamp from
// its SaveHave, and every offer carries all the stamps the authority knows (an announce of a new copy reaches every member before
// its bytes, so a session cut short mid-copy still leaves every launcher knowing who has the newest). Each PC keeps the stamps of the
// others (its own is stamp.json), the ones it knew before this session merged in, and writes peers.json whenever one changes (not
// only at the session's end: a closed window or a crash ends a session too), and once more at the end.
// The hold (a member): its game must not write its own world over the group's copy. Its world writes are dropped at the runtime's
// commit (services fsapi::set_save_commit_hold), at the file layer rather than at the three save writers: the commit sees exactly
// the files, whichever writer and whatever kind of request made them (autosave kind 3 writes SCSHDAT, the arrival save at home after
// a leave or a drop writes STGDAT, the backup job writes B00), and the game still sees its save done (no error notice, the
// take-out's save after a take still completes). CMNDAT and SYDAT are never held: the player's own data in them (coins, claims,
// the boxes a take-out emptied, the look) reaches disk as before. Held: the world files of G00 and B00, while this process is a
// member (in a session or not: the writes that would overwrite the copy are the ones outside it) and this PC has a whole copy of
// the group's world (stamp.json), or is moving one into place. With no copy yet (a new member, or a group from before this build)
// nothing is held, so the member's first world still reaches disk.
// Fail safe: an older build on either side never sends or answers these types; a member then keeps today's behaviour.
#include "core/pkp_coop_internal.h"
#include "core/pkp_coop_saveset.h"
#include "core/pkp_save_files.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <future>
#include <map>
#include <string>
#include <vector>

#include "nxrt/coop_channel.h"
#include "nxrt/log.h"
#include "nxrt/qpc.h"
#include "nxrt/services_api.h"

namespace nxrt::games::pkp::coop::savecopy {

namespace {

namespace ch = nxrt::coop_channel;
namespace ss = saveset;
constexpr const char* kTag = "pkp";

constexpr double kPollEvery = 1.0;      // seconds between two looks at the world files' sizes and times
constexpr double kStable = 2.0;         // seconds the world files hold still before a save point is read (one save commits several)
constexpr double kHelloEvery = 5.0;     // seconds between two hellos of a member no offer answered yet
constexpr double kChunkWait = 60.0;     // seconds without a chunk after which a member gives the copy up
constexpr size_t kChunk = 256 * 1024;   // bytes of a file per SaveChunk
constexpr uint32_t kMaxFails = 2;       // failed checks of one copy before a member stops asking for it

enum class HaveStatus : uint8_t { Hello = 0, Applied = 1, Failed = 2 };

std::string hex(const uint8_t* k) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < kMemberKeyBytes; ++i) {
        s += d[k[i] >> 4];
        s += d[k[i] & 15];
    }
    return s;
}

void put_files(wire::Writer& w, const std::vector<ss::File>& files, const std::vector<uint8_t>* sent) {
    w.u8(static_cast<uint8_t>(files.size()));
    for (size_t i = 0; i < files.size(); ++i) {
        const ss::File& f = files[i];
        w.u8(static_cast<uint8_t>(f.name.size())).bytes(reinterpret_cast<const uint8_t*>(f.name.data()), f.name.size());
        w.u32(static_cast<uint32_t>(f.size)).u64(f.hash);
        if (sent) w.u8((*sent)[i]);
    }
}

bool get_files(wire::Reader& r, std::vector<ss::File>* files, std::vector<uint8_t>* sent) {
    const uint8_t n = r.u8();
    if (n > ss::kMaxFiles) return false;
    for (uint8_t i = 0; i < n && r.ok(); ++i) {
        ss::File f;
        const uint8_t len = r.u8();
        if (len == 0 || len > ss::kMaxName) return false;
        f.name.resize(len);
        r.bytes(reinterpret_cast<uint8_t*>(f.name.data()), len);
        f.size = r.u32();
        f.hash = r.u64();
        if (!ss::world_name(f.name, false) || f.size > ss::kMaxFileBytes) return false;
        files->push_back(std::move(f));
        if (sent) sent->push_back(r.u8() ? 1 : 0);
    }
    return r.ok();
}

bool has(const std::vector<ss::File>& files, const ss::File& f) { return std::find(files.begin(), files.end(), f) != files.end(); }

// A worker's result, taken on the game thread.
struct JobResult {
    bool ok = false;
    bool ready = false;  // an apply: the copy is ready on disk, its moves wait (finish)
    std::string why;
    ss::Snapshot snap;   // a save point's read
};
enum class Job : uint8_t { None, Read, Apply, Finish };

// The authority's view of one member.
struct Node {
    std::string key;                // its member id (hex)
    ss::Stamp stamp;                // its copy, as it last said
    std::vector<ss::File> files;    // its copy's world files, as it last said
    bool hello = false;
    uint64_t stream_seq = 0;        // the copy being sent to it (0: none yet)
    std::vector<uint8_t> sent;      // per file of that copy: its bytes are sent
    size_t file = 0;                // the next file to send (index into the copy) and its offset
    uint64_t off = 0;
    bool streaming = false;
};

// A copy a member is receiving.
struct Transfer {
    bool active = false;
    ss::Stamp stamp;
    std::string by;
    std::vector<ss::File> files;
    std::vector<uint8_t> sent;
    std::vector<uint64_t> got;      // bytes received per file
    double last_at = 0;
};

struct State {
    bool installed = false;
    bool stamped = false;           // stamp.json: a whole copy of the group's world is on disk
    bool authority_mode = false;    // the side the last tick acted as
    std::future<JobResult> job;
    Job job_kind = Job::None;
    // shared: this PC's copy and what it knows of the others' (member id -> stamp; this PC's own included)
    ss::Stamp own;
    std::vector<ss::File> own_files;
    std::map<std::string, ss::Stamp> table;
    std::map<std::string, ss::Stamp> peers_written;   // what peers.json holds (the table without this PC)
    // the authority
    std::vector<ss::Seen> seen;
    bool seen_init = false, dirty = false;
    double changed_at = 0, polled_at = -1e9;
    bool have = false;              // a copy to offer
    ss::Snapshot copy;
    std::map<int, Node> nodes;
    // a member
    bool offered = false;           // an offer came this session
    double hello_at = -1e9;
    Transfer t;
    uint64_t failed_seq = 0;
    uint32_t fails = 0;
    bool pending = false;           // a ready copy in incoming/ whose moves did not all run: only finish() touches incoming/ now
    double finish_at = -1e9;
    uint32_t held_logged = 0;
};
State& st() {
    static State s;
    return s;
}

bool acting_authority() { return link::in_session() ? link::is_authority() : role() == CoopRole::Host; }

// The hold's state, read on the committing thread (not part of State: the filter may run while a reset clears State).
struct Hold {
    std::atomic<bool> on{false};
    std::atomic<uint32_t> count{0};   // file changes held since the launch (the log shows the hold matches)
    std::string root;   // the account folder, norm()ed; fixed before the filter is registered
};
Hold& hold() {
    static Hold h;
    return h;
}

// A host folder (UTF-8) as compared here: normalized, '/' separated, lower case, no trailing '/'.
std::string norm(const std::string& u8path) {
    std::string p = save_files::u8(std::filesystem::path(save_files::from_u8(u8path).lexically_normal().generic_u8string()));
    while (!p.empty() && p.back() == '/') p.pop_back();
    for (char& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return p;
}

// fsapi::set_save_commit_hold's filter: a world file of this co-op save's account folder, while the hold is on.
bool held(const std::string& root, const std::string& rel) {
    Hold& h = hold();
    if (!h.on.load(std::memory_order_acquire) || !ss::world_name(rel, true) || norm(root) != h.root) return false;
    h.count.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool job_busy() { return st().job_kind != Job::None; }

void start_read() {
    State& s = st();
    s.job_kind = Job::Read;
    s.job = std::async(std::launch::async, [] {
        JobResult r;
        r.ok = ss::read(&r.snap, &r.why);
        return r;
    });
}

void start_apply() {
    State& s = st();
    const Transfer& t = s.t;
    std::vector<std::string> received;
    for (size_t i = 0; i < t.files.size(); ++i)
        if (t.sent[i]) received.push_back(t.files[i].name);
    s.job_kind = Job::Apply;
    hold().on.store(true, std::memory_order_release);   // before the first file moves: the game's next commit leaves them alone
    s.job = std::async(std::launch::async, [stamp = t.stamp, by = t.by, files = t.files, received] {
        JobResult r;
        r.ok = ss::apply(stamp, by, files, received, &r.ready, &r.why);
        return r;
    });
}

constexpr double kFinishEvery = 5.0;   // seconds between two tries to complete a ready copy

void start_finish() {
    State& s = st();
    s.job_kind = Job::Finish;
    s.finish_at = qpc::seconds();
    s.job = std::async(std::launch::async, [] {
        JobResult r;
        r.ok = ss::finish(&r.why);
        r.ready = !r.ok && ss::ready_pending();
        return r;
    });
}

uint64_t now_unix() { return static_cast<uint64_t>(std::time(nullptr)); }

uint64_t highest_seq() {
    uint64_t m = st().own.seq;
    for (const auto& [id, stamp] : st().table) m = std::max(m, stamp.seq);
    return m;
}

// peers.json follows the table (the other members' stamps) whenever they changed.
void save_peers() {
    State& s = st();
    std::map<std::string, ss::Stamp> peers = s.table;
    peers.erase(member_key());
    if (peers.empty() || peers == s.peers_written) return;
    std::string why;
    if (ss::write_peers(peers, &why)) s.peers_written = std::move(peers);
    else NXLOG_W(kTag, "Co-op save: peers.json was not written: %s", why.c_str());
}

// ---- the authority -----------------------------------------------------------------------------------------------------------

void send_offer(int node, Node& n) {
    State& s = st();
    const uint64_t seq = s.own.seq;
    if (n.stream_seq != seq) {   // a new copy for this member: what it lacks follows
        n.stream_seq = seq;
        n.sent.assign(s.copy.files.size(), 0);
        for (size_t i = 0; i < s.copy.files.size(); ++i) n.sent[i] = has(n.files, s.copy.files[i]) ? 0 : 1;
        n.file = 0;
        n.off = 0;
        n.streaming = std::find(n.sent.begin(), n.sent.end(), 1) != n.sent.end();
    }
    wire::Writer w(link::self());
    w.u64(s.own.seq).u64(s.own.saved_at).bytes(member_key_bytes(), kMemberKeyBytes);
    put_files(w, s.copy.files, &n.sent);
    w.u8(static_cast<uint8_t>(std::min<size_t>(s.table.size(), 0xff)));
    size_t k = 0;
    for (const auto& [id, stamp] : s.table) {
        if (k++ == 0xff) break;
        uint8_t key[kMemberKeyBytes] = {};
        for (size_t i = 0; i < kMemberKeyBytes && 2 * i + 1 < id.size(); ++i)
            key[i] = static_cast<uint8_t>(std::stoi(id.substr(2 * i, 2), nullptr, 16));
        w.bytes(key, kMemberKeyBytes).u64(stamp.seq).u64(stamp.saved_at);
    }
    if (!link::to_node(node, wire::Type::SaveOffer, w.take())) n.streaming = false;
}

void offer_all() {
    for (auto& [node, n] : st().nodes)
        if (n.hello) send_offer(node, n);
}

// The world on disk becomes a new save point: stamped above every copy known, then offered.
void stamp_new() {
    State& s = st();
    s.own = ss::Stamp{highest_seq() + 1, now_unix()};
    std::string why;
    if (ss::write_stamp(s.own, member_key(), &why)) s.stamped = true;
    else NXLOG_W(kTag, "Co-op save: stamp.json of copy %llu was not written: %s", static_cast<unsigned long long>(s.own.seq), why.c_str());
    s.table[member_key()] = s.own;
    s.own_files = s.copy.files;
}

void on_read_done(JobResult r) {
    State& s = st();
    if (!r.ok) {
        NXLOG_D(kTag, "Co-op save: the save point is read again later: %s", r.why.c_str());
        s.changed_at = qpc::seconds();   // dirty stays: retried once the files hold still again
        return;
    }
    s.dirty = false;
    if (s.have && r.snap.files == s.copy.files) return;   // the same world (a save of CMNDAT or SYDAT only)
    const bool first = !s.have;
    s.copy = std::move(r.snap);
    s.have = true;
    if (first && ss::has_stamp() && s.copy.newest <= static_cast<int64_t>(s.own.saved_at) && highest_seq() == s.own.seq) {
        s.own_files = s.copy.files;   // nothing saved since this copy was stamped
        s.table[member_key()] = s.own;
    } else {
        stamp_new();
    }
    uint64_t bytes = 0;
    for (const ss::File& f : s.copy.files) bytes += f.size;
    NXLOG_I(kTag, "Co-op save: copy %llu of the group's world (%zu files, %llu KB) to %zu member(s)", static_cast<unsigned long long>(s.own.seq),
            s.copy.files.size(), static_cast<unsigned long long>(bytes / 1024), s.nodes.size());
    offer_all();
}

void poll_world() {
    State& s = st();
    const double now = qpc::seconds();
    if (now - s.polled_at >= kPollEvery) {
        s.polled_at = now;
        std::vector<ss::Seen> cur = ss::scan();
        if (!s.seen_init || cur != s.seen) {
            if (!s.seen_init) s.changed_at = now - kStable;   // the launch's first look: read it at once
            else s.changed_at = now;
            s.seen_init = true;
            s.seen = std::move(cur);
            s.dirty = !s.seen.empty();
        }
    }
    if (s.dirty && !job_busy() && now - s.changed_at >= kStable) start_read();
}

void stream(int node, Node& n) {
    State& s = st();
    while (n.streaming && n.stream_seq == s.own.seq && ch::queued_bytes(node) < kChunk / 2) {
        while (n.file < n.sent.size() && !n.sent[n.file]) ++n.file;
        if (n.file >= n.sent.size()) {
            n.streaming = false;
            break;
        }
        const std::vector<uint8_t>& b = s.copy.bytes[n.file];
        const size_t len = static_cast<size_t>(std::min<uint64_t>(kChunk, b.size() - n.off));
        wire::Writer w(link::self());
        w.u64(n.stream_seq).u8(static_cast<uint8_t>(n.file)).u32(static_cast<uint32_t>(n.off)).u32(static_cast<uint32_t>(len));
        w.bytes(b.data() + n.off, len);
        if (!link::to_node(node, wire::Type::SaveChunk, w.take())) {
            n.streaming = false;
            break;
        }
        n.off += len;
        if (n.off >= b.size()) {
            ++n.file;
            n.off = 0;
        }
    }
}

void tick_authority() {
    State& s = st();
    if (s.pending) return;   // the world on disk is not whole until the ready copy is completed
    poll_world();
    for (auto& [node, n] : s.nodes) stream(node, n);
}

// ---- a member ------------------------------------------------------------------------------------------------------------------

void send_have(HaveStatus status) {
    State& s = st();
    wire::Writer w(link::self());
    w.bytes(member_key_bytes(), kMemberKeyBytes).u64(s.own.seq).u64(s.own.saved_at).u8(static_cast<uint8_t>(status));
    put_files(w, s.own_files, nullptr);
    link::to_authority(wire::Type::SaveHave, w.take());
}

void drop_transfer(const char* why) {
    State& s = st();
    if (!s.t.active) return;
    NXLOG_W(kTag, "Co-op save: copy %llu not taken (%s); this PC keeps copy %llu", static_cast<unsigned long long>(s.t.stamp.seq), why,
            static_cast<unsigned long long>(s.own.seq));
    s.t = Transfer{};
    ss::drop_incoming();
}

// This PC's world files and their hashes, read again (a member's offer is compared with them).
void refresh_own_files() {
    State& s = st();
    std::string why;
    if (!ss::hashes(&s.own_files, &why)) {
        s.own_files.clear();
        NXLOG_W(kTag, "Co-op save: this PC's copy could not be read (%s); every file of the next copy is asked for", why.c_str());
    }
}

void on_apply_done(const JobResult& r) {
    State& s = st();
    const Transfer t = s.t;
    s.t = Transfer{};
    if (r.ok) {
        s.own = t.stamp;
        s.own_files = t.files;
        s.stamped = true;
        s.table[member_key()] = s.own;
        NXLOG_I(kTag, "Co-op save: copy %llu of the group's world is this PC's copy now", static_cast<unsigned long long>(s.own.seq));
        send_have(HaveStatus::Applied);
        return;
    }
    if (r.ready) {   // nothing may drop it now: the moves are completed from incoming/ (here, or at the next start)
        s.pending = true;
        NXLOG_W(kTag, "Co-op save: copy %llu is ready but not all in place (%s); it is completed as soon as it can be",
                static_cast<unsigned long long>(t.stamp.seq), r.why.c_str());
        return;
    }
    ss::drop_incoming();
    s.fails = s.failed_seq == t.stamp.seq ? s.fails + 1 : 1;
    s.failed_seq = t.stamp.seq;
    NXLOG_W(kTag, "Co-op save: copy %llu not taken (%s); this PC keeps copy %llu%s", static_cast<unsigned long long>(t.stamp.seq),
            r.why.c_str(), static_cast<unsigned long long>(s.own.seq), s.fails < kMaxFails ? ", asked for again" : "");
    refresh_own_files();
    if (s.fails < kMaxFails) send_have(HaveStatus::Failed);
}

void on_finish_done(const JobResult& r) {
    State& s = st();
    if (!r.ok) {
        s.pending = r.ready;   // still ready: tried again later; none left (it was not ready and is dropped): nothing waits
        if (!s.pending) refresh_own_files();
        return;
    }
    s.pending = false;
    s.own = ss::own_stamp();
    s.stamped = true;
    refresh_own_files();
    s.table[member_key()] = s.own;
    NXLOG_I(kTag, "Co-op save: copy %llu of the group's world is this PC's copy now (completed)", static_cast<unsigned long long>(s.own.seq));
    if (link::in_session() && !link::is_authority()) send_have(HaveStatus::Applied);
}

void tick_member() {
    State& s = st();
    if (!link::in_session()) return;
    const double now = qpc::seconds();
    if (!s.offered && now - s.hello_at >= kHelloEvery) {
        s.hello_at = now;
        send_have(HaveStatus::Hello);
    }
    if (s.t.active && !job_busy() && now - s.t.last_at > kChunkWait) {
        drop_transfer("no part of it came for a minute");
        s.offered = false;   // hello again: the authority sends it anew
    }
}

}  // namespace

// ---- messages --------------------------------------------------------------------------------------------------------------------

void on_have_message(::Ctx*, int from, uint8_t, wire::Reader& r) {
    State& s = st();
    if (!s.installed || !link::is_authority()) return;
    uint8_t key[kMemberKeyBytes];
    r.bytes(key, kMemberKeyBytes);
    ss::Stamp stamp{r.u64(), 0};
    stamp.saved_at = r.u64();
    const auto status = static_cast<HaveStatus>(r.u8());
    std::vector<ss::File> files;
    if (!get_files(r, &files, nullptr) || !r.ok()) {
        NXLOG_W(kTag, "Co-op save: a malformed SaveHave from node %d was dropped", from);
        return;
    }
    Node& n = s.nodes[from];
    n.key = hex(key);
    n.stamp = stamp;
    n.files = std::move(files);
    n.hello = true;
    s.table[n.key] = stamp;
    if (!s.have) return;   // offered once this process has a copy
    if (status != HaveStatus::Applied && !(stamp < s.own)) {
        // the member's copy is as new or newer than the one this session plays: the session's world is stamped above it
        stamp_new();
        NXLOG_I(kTag, "Co-op save: node %d has copy %llu; this session's world is copy %llu", from, static_cast<unsigned long long>(stamp.seq),
                static_cast<unsigned long long>(s.own.seq));
        offer_all();
        return;
    }
    // Behind: what it lacks is sent (anew, unless a copy is on its way to it now: a hello that crossed the offer).
    if (stamp < s.own && !n.streaming) n.stream_seq = 0;
    send_offer(from, n);
    if (status != HaveStatus::Applied) return;
    for (auto& [node, other] : s.nodes)   // every other member learns the new stamp; a copy on its way to one is not restarted
        if (node != from && other.hello) send_offer(node, other);
}

void on_offer_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    State& s = st();
    if (!s.installed || link::is_authority()) return;
    Transfer t;
    t.stamp.seq = r.u64();
    t.stamp.saved_at = r.u64();
    uint8_t by[kMemberKeyBytes];
    r.bytes(by, kMemberKeyBytes);
    t.by = hex(by);
    std::map<std::string, ss::Stamp> table;
    if (!get_files(r, &t.files, &t.sent)) {
        NXLOG_W(kTag, "Co-op save: a malformed SaveOffer was dropped");
        return;
    }
    const uint8_t m = r.u8();
    for (uint8_t i = 0; i < m && r.ok(); ++i) {
        uint8_t key[kMemberKeyBytes];
        r.bytes(key, kMemberKeyBytes);
        ss::Stamp stamp{r.u64(), 0};
        stamp.saved_at = r.u64();
        table[hex(key)] = stamp;
    }
    if (!r.ok()) {
        NXLOG_W(kTag, "Co-op save: a malformed SaveOffer was dropped");
        return;
    }
    s.offered = true;
    for (const auto& [id, stamp] : table)
        if (id != member_key()) s.table[id] = stamp;
    if (!(s.own < t.stamp)) return;                                   // not newer than this PC's copy
    if (s.t.active && s.t.stamp == t.stamp) return;                   // being received
    if (job_busy() || s.pending) return;                              // a copy is being put in place: its answer asks for this one
    if (s.failed_seq == t.stamp.seq && s.fails >= kMaxFails) return;  // given up
    drop_transfer("a newer copy came");
    ss::drop_incoming();
    for (size_t i = 0; i < t.files.size(); ++i) {
        std::string why;
        if (t.sent[i] ? !ss::incoming_create(t.files[i].name, &why) : !has(s.own_files, t.files[i])) {
            NXLOG_W(kTag, "Co-op save: copy %llu not taken: %s", static_cast<unsigned long long>(t.stamp.seq),
                    t.sent[i] ? why.c_str() : "this PC's files are not the ones the authority knows; it says so");
            ss::drop_incoming();
            refresh_own_files();
            s.offered = false;   // hello again (now, and until an offer answers it)
            s.hello_at = qpc::seconds();
            send_have(HaveStatus::Hello);
            return;
        }
    }
    t.got.assign(t.files.size(), 0);
    t.active = true;
    t.last_at = qpc::seconds();
    s.t = std::move(t);
    if (std::find(s.t.sent.begin(), s.t.sent.end(), 1) == s.t.sent.end()) start_apply();   // only the stamp is new
}

void on_chunk_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    State& s = st();
    if (!s.installed || link::is_authority()) return;
    const uint64_t seq = r.u64();
    const uint8_t idx = r.u8();
    const uint32_t off = r.u32(), len = r.u32();
    Transfer& t = s.t;
    if (!r.ok() || !t.active || job_busy() || seq != t.stamp.seq) return;   // a copy this PC is not receiving (any more)
    if (idx >= t.files.size() || !t.sent[idx] || off != t.got[idx] || off + uint64_t{len} > t.files[idx].size) {
        drop_transfer("its parts came out of order");
        s.offered = false;   // hello again: the authority sends it anew
        return;
    }
    std::vector<uint8_t> b(len);
    r.bytes(b.data(), len);
    std::string why;
    if (!r.ok() || !ss::incoming_append(t.files[idx].name, b.data(), len, &why)) {
        drop_transfer(r.ok() ? why.c_str() : "a part was cut short");
        s.offered = false;
        return;
    }
    t.got[idx] += len;
    t.last_at = qpc::seconds();
    for (size_t i = 0; i < t.files.size(); ++i)
        if (t.sent[i] && t.got[i] < t.files[i].size) return;
    start_apply();
}

// ---- the part's entry points ---------------------------------------------------------------------------------------------------

void install() {
    State& s = st();
    ss::install();
    std::string why;
    if (ss::finish(&why)) NXLOG_I(kTag, "Co-op save: a received copy left half moved by the last run was completed");
    else if (!why.empty()) NXLOG_W(kTag, "Co-op save: a received copy is still waiting: %s", why.c_str());
    s.pending = ss::ready_pending();
    s.own = ss::own_stamp();
    s.stamped = ss::has_stamp();
    s.table = ss::read_peers();
    s.peers_written = s.table;
    s.table[member_key()] = s.own;
    refresh_own_files();
    hold().root = norm(save_files::u8(ss::account_dir()));
    services::fsapi::set_save_commit_hold(&held);
    s.installed = true;
    NXLOG_I(kTag, "Co-op save: this PC has copy %llu (%s), %zu world file(s)", static_cast<unsigned long long>(s.own.seq),
            ss::has_stamp() ? "stamped" : "no stamp yet", s.own_files.size());
}

void tick(::Ctx*) {
    State& s = st();
    if (!s.installed) return;
    if (s.job_kind != Job::None && s.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const Job kind = s.job_kind;
        s.job_kind = Job::None;
        JobResult r = s.job.get();
        if (kind == Job::Read) on_read_done(std::move(r));
        else if (kind == Job::Apply) on_apply_done(r);
        else on_finish_done(r);
    }
    if (s.pending && !job_busy() && qpc::seconds() - s.finish_at >= kFinishEvery) start_finish();
    const bool authority = acting_authority();
    if (authority != s.authority_mode) {   // the side changed (a launch role the session did not keep)
        s.authority_mode = authority;
        s.nodes.clear();
        if (!job_busy() && !s.pending) drop_transfer("this process is the authority now");
        s.offered = false;
    }
    if (authority) tick_authority();
    else tick_member();
    hold().on.store(!authority && (s.stamped || s.pending || s.job_kind == Job::Apply || s.job_kind == Job::Finish),
                    std::memory_order_release);
    if (const uint32_t n = hold().count.load(std::memory_order_relaxed); n != s.held_logged) {
        s.held_logged = n;
        NXLOG_I(kTag, "Co-op save: this member's own world writes kept off the group's copy: %u file change(s) so far", n);
    }
    save_peers();
}

void on_member(int node, bool joined, bool self) {
    State& s = st();
    if (self) {   // this process joined or left a session: the members say hello again
        s.nodes.clear();
        s.offered = false;
        s.hello_at = -1e9;
        if (!joined && !job_busy() && !s.pending) drop_transfer("the session ended");
        if (!joined) save_peers();
        return;
    }
    if (!joined) s.nodes.erase(node);
}

void reset() {
    State& s = st();
    if (s.job.valid()) s.job.wait();   // a copy being put in place finishes (or stays ready for the next start)
    hold().on.store(false);
    if (s.installed) services::fsapi::set_save_commit_hold(nullptr);
    s = State{};
}

}  // namespace nxrt::games::pkp::coop::savecopy
