// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: co-op stage mode (pkp_coop_internal.h). In a co-op session the game's Cloud Island test 0x77ecb0 (GM+0x18 == 0x11)
// answers yes at the rows of pkp_coop_stage_table.inc, so every process reads and writes Cloud Island's record set
// (m_CloudStageSaveData, save root + a family's `cloud`) for that family, as Cloud Island does for host and visitor alike: the
// authority's game marks a changed record dirty and its replication carries it to the members, a member's writes go the way a Cloud
// Island visitor's do. One family at a time; story and quests stay on their own mechanisms. RE: modding-docs coop-v2/town-mode
// (TALK-PILOT-DESIGN.md, FAMILIES.md, CLOUD-FLOW-RE.md, STAGE-CENSUS.md).
//
// Per family, on each process (game thread unless said):
//   Off     no row answers: the game's own answer (own records offline and on the authority, the copy of the host's on a member)
//   Asking  a member in the session's channel with its visitor bit (GM+0x90 bit 2) set, so the host's world is loading or loaded:
//           it sends StageAsk every kAskEvery seconds until a StageTable arrives (an older authority never answers: rows stay off)
//   On      the rows answer Cloud. The authority got there by copying its own fields into the Cloud set once its area was settled
//           (nothing in the join snapshot carries these records: 0x570bfc / 0x571f40 / 0x572200 read no talk offset), a member by
//           writing the authority's StageTable over its Cloud set (its own Cloud fields kept, written back when the mode ends)
//   The mode ends at the end of this process's channel session, at reset, and at Cloud Island (stage 0x11, which co-op never enters:
//   pkp_coop_doors.cpp): the authority copies the Cloud fields into its own set (the Cloud set keeps the group's), a member writes
//   back what its Cloud set held before the seed.
// The rows answer only while the mode is On and no area load is in flight (on()). The authority's own set is what CMNDAT saves, solo
// play and the next session read, and nothing writes it while the rows answer Cloud, so it is copied Cloud -> own at the two points
// every CMNDAT write passes: the pre-save fixups 0x5aa8b8 (the autosave snapshot 0x5ae428 on the game thread, the serialize 0x5aa0e0 of
// every save type but 5 on the TaskMgr thread) and the save request 0x241324 (the type-5 saves: menu save, save_now, Save and Quit).
// Members never copy back: their own set is their solo save's. A visited area's load (0x5a8748 with mode 0 and GM+0x24 == 1) reads
// its STGDAT over all of m_saveStage, the Cloud set included: the fields are set aside at the job and written back at the
// post-load fix-up 0x5af540 (before the actors spawn). Between the two the rows give the game's answer, and a copy-back takes the
// set-aside fields, not the file's. Every STGDAT carries the Cloud block, so the story STGDATs a session saves hold the group's
// fields; they are read as data only on stage 0x11 (accepted: co-op saves are kept apart and co-op never enters Cloud Island).
// State a hook off the game thread reads (the mode, the load flag, the authority flag, the counters) is atomic; the copies take one
// lock. Every checksum is saveset::hash over the family's fields in the StageTable layout (record i's fields, in order), so a host
// line and a member line of the same table compare; the tally (records with the family's tally byte set) compares after the seed too,
// as the game replicates that byte (talk: +0x10).
#include "core/pkp_coop_internal.h"
#include "core/pkp_coop_saveset.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_site_check.h"
#include "core/pkp_site_rows.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::stage {

namespace {

constexpr const char* kTag = "pkp";
constexpr double kAskEvery = 5.0;       // a member asks again until the authority answers (it answers only while its mode is on)
constexpr unsigned kAskLogEvery = 12;   // a member that still asks logs it once a minute
constexpr double kCountEvery = 60.0;    // the answer counts in the log
constexpr double kLoadLate = 30.0;      // a set-aside load whose fix-up has not run this long after, once settled, is logged
constexpr uint32_t kLoadStageData = 0;  // 0x5a8748's w2: the stage data load
constexpr uint8_t kEntryVisited = 1;    // GM+0x24 at a load job: a visited area (its STGDAT is read)

struct Desc {
    Family id;
    uint8_t wire;
    const char* name;
    uint64_t own, cloud, at, bytes, size;
    uint32_t count;
    uint64_t tally;
};
constexpr Desc kFamilies[] = {
#define PKP_STAGE_FAMILY(id, wire, name, own, cloud, at, bytes, size, count, tally) \
    {Family::id, wire, name, own, cloud, at, bytes, size, count, tally},
#define PKP_STAGE_ROW(ret, word, family, what)
#define PKP_STAGE_RESET(ret, word, family, reg, what)
#include "core/pkp_coop_stage_table.inc"
#undef PKP_STAGE_RESET
#undef PKP_STAGE_ROW
#undef PKP_STAGE_FAMILY
};
constexpr size_t kFamilyCount = static_cast<size_t>(Family::Count);
static_assert(sizeof kFamilies / sizeof kFamilies[0] == kFamilyCount, "pkp_coop_stage_table.inc: one PKP_STAGE_FAMILY per Family");
constexpr bool families_ok() {
    for (size_t i = 0; i < kFamilyCount; ++i) {
        const Desc& d = kFamilies[i];
        if (d.id != static_cast<Family>(i) || d.wire == 0 || d.bytes == 0 || d.count == 0 || d.at + d.bytes > d.size) return false;
        if (d.tally < d.at || d.tally >= d.at + d.bytes || d.bytes > 0xffff || d.count > 0xffff) return false;
        for (size_t j = 0; j < i; ++j)
            if (kFamilies[j].wire == d.wire) return false;
    }
    return true;
}
static_assert(families_ok(), "pkp_coop_stage_table.inc: a family is out of order, shares a wire id, or its fields or tally are not "
                             "inside its record");

struct Row {
    uint64_t site;   // main offset of the hooked instruction (the `bl 0x77ecb0`'s x30)
    uint32_t word;
    Family family;
    bool reset;      // a reset row: the game's answer stands, record wReg's fields are cleared in the Cloud set
    uint8_t reg;
    const char* what;
};
constexpr Row kListed[] = {
#define PKP_STAGE_ROW(ret, word, family, what) {ret, word, Family::family, false, 0, what},
#define PKP_STAGE_RESET(ret, word, family, reg, what) {ret, word, Family::family, true, reg, what},
#include "core/pkp_coop_stage_table.inc"
#undef PKP_STAGE_RESET
#undef PKP_STAGE_ROW
    {~0ull, 0, Family::Talk, false, 0, nullptr},
};
constexpr size_t kRowCount = sizeof kListed / sizeof kListed[0] - 1;
constexpr std::array<Row, kRowCount + 1> kRows = site_rows::sorted(kListed);
static_assert(site_rows::unique(kRows), "pkp_coop_stage_table.inc: a site is listed twice");
constexpr bool resets_ok() {
    for (size_t i = 0; i < kRowCount; ++i)
        if (kRows[i].reset && kRows[i].reg > 28) return false;
    return site_rows::aligned(kRows);
}
static_assert(resets_ok(), "pkp_coop_stage_table.inc: a site is not an instruction address, or a reset's register is not w0..w28");

enum class Mode : uint8_t { Off, Asking, On };
const char* mode_name(Mode m) { return m == Mode::On ? "on" : m == Mode::Asking ? "asking" : "off"; }

struct Fam {
    std::atomic<Mode> mode{Mode::Off};
    std::atomic<bool> loading{false};       // the authority: the fields are set aside for an area load in flight
    std::atomic<double> loading_since{0.0};
    std::atomic<uint32_t> answers{0};       // Cloud answers since the last count line
    std::atomic<uint32_t> passes_off{0};    // rows reached with the mode off (or a load in flight), since the last count line
    uint64_t answered = 0;                  // game thread: Cloud answers since the mode turned on
    double asked_at = 0.0;                  // game thread: a member's last StageAsk
    unsigned asks = 0;
    bool late_logged = false;               // game thread: the missing fix-up of this load was logged
    std::vector<uint8_t> aside;             // under State::copy: the group's fields at the load job
    std::vector<uint8_t> before;            // under State::copy, a member: its Cloud fields before the seed
};
struct State {
    std::array<Fam, kFamilyCount> fam;
    std::atomic<bool> authority{false};     // the seeds were the authority's (only it copies back)
    std::atomic<bool> load_pending{false};  // a visited area's load job ran and its fix-up has not: no seed reads the Cloud set yet
    std::atomic<double> load_pending_since{0.0};
    site_rows::Marks<kRowCount + 1> marks;  // each row's check (once per run), this session's first answer written
    std::mutex copy;                        // the copies of fields (game thread, the save worker)
    double counted_at = 0.0;                // game thread
};
State& st() {
    static State s;
    return s;
}

const Desc& desc(Family f) { return kFamilies[static_cast<size_t>(f)]; }
Fam& fam(Family f) { return st().fam[static_cast<size_t>(f)]; }
const Desc* by_wire(uint8_t wire) {
    for (const Desc& d : kFamilies)
        if (d.wire == wire) return &d;
    return nullptr;
}

uint64_t save_root() { return guest::rd<uint64_t>(base() + layout::kSaveRoot); }
uint64_t set_at(const Desc& d, uint64_t set) {
    const uint64_t root = save_root();
    return root && guest::ok(root + set, d.size * d.count) ? root + set : 0;
}
// The family's fields of one record set, record by record (the StageTable layout). False when the set is not there.
bool pack(const Desc& d, uint64_t set, std::vector<uint8_t>* out) {
    const uint64_t at = set_at(d, set);
    if (!at) return false;
    out->resize(static_cast<size_t>(d.bytes * d.count));
    for (uint32_t i = 0; i < d.count; ++i)
        std::memcpy(out->data() + i * d.bytes, reinterpret_cast<const void*>(at + i * d.size + d.at), d.bytes);
    return true;
}
bool unpack(const Desc& d, uint64_t set, const std::vector<uint8_t>& in) {
    const uint64_t at = set_at(d, set);
    if (!at || in.size() != d.bytes * d.count) return false;
    for (uint32_t i = 0; i < d.count; ++i)
        std::memcpy(reinterpret_cast<void*>(at + i * d.size + d.at), in.data() + i * d.bytes, d.bytes);
    return true;
}

// Packed fields for the log: the hash of all of them, how many records have the tally byte set, and the hash of the tally bytes
// alone (record order). The tally column is the one the game replicates, so it compares between processes after the seed too.
struct Sum {
    unsigned long long hash;
    unsigned tally;
    unsigned long long column;
};
Sum sum_of(const Desc& d, const std::vector<uint8_t>& p) {
    std::vector<uint8_t> column;
    column.reserve(d.count);
    for (size_t i = 0; i + d.bytes <= p.size(); i += d.bytes) column.push_back(p[i + (d.tally - d.at)]);
    Sum s{static_cast<unsigned long long>(saveset::hash(p.data(), p.size())), 0,
          static_cast<unsigned long long>(saveset::hash(column.data(), column.size()))};
    for (uint8_t b : column) s.tally += b != 0;
    return s;
}

bool settled_on(Mode m, const Fam& f) { return m == Mode::On && !f.loading.load(std::memory_order_acquire); }

// The authority, under State::copy: the group's fields into its own set (the set-aside ones while a load is in flight).
void copy_back(const Desc& d, Fam& f, const char* path) {
    std::vector<uint8_t> fields;
    const bool aside = f.loading.load(std::memory_order_acquire);
    if (aside) fields = f.aside;
    else if (!pack(d, d.cloud, &fields)) {
        NXLOG_W(kTag, "Co-op stage: %s copy-back (%s) skipped: no save data", d.name, path);
        return;
    }
    if (!unpack(d, d.own, fields)) {
        NXLOG_W(kTag, "Co-op stage: %s copy-back (%s) skipped: the own set is not there (%zu bytes)", d.name, path, fields.size());
        return;
    }
    const Sum s = sum_of(d, fields);
    NXLOG_I(kTag, "Co-op stage: %s copy-back (%s): Cloud -> own%s, %zu bytes, sum %016llx, %u with +0x%llx set (column %016llx)",
            d.name, path, aside ? " from the fields set aside for the area load" : "", fields.size(), s.hash, s.tally,
            static_cast<unsigned long long>(d.tally), s.column);
}

// The live Cloud fields, for the log (game thread).
void log_live(const Desc& d, const char* when) {
    std::vector<uint8_t> live;
    {
        std::lock_guard<std::mutex> lock(st().copy);
        if (!pack(d, d.cloud, &live)) return;
    }
    const Sum s = sum_of(d, live);
    NXLOG_I(kTag, "Co-op stage: %s: the Cloud set %s: sum %016llx, %u with +0x%llx set (column %016llx)", d.name, when, s.hash,
            s.tally, static_cast<unsigned long long>(d.tally), s.column);
}

// Ends the family's mode (game thread).
void end(Family id, const char* why) {
    State& s = st();
    const Desc& d = desc(id);
    Fam& f = fam(id);
    const Mode was = f.mode.load(std::memory_order_acquire);
    if (was == Mode::Off) return;
    if (was == Mode::On) log_live(d, "at the end of the mode");
    {
        std::lock_guard<std::mutex> lock(s.copy);
        if (was == Mode::On && s.authority.load(std::memory_order_acquire)) {
            copy_back(d, f, why);
        } else if (was == Mode::On) {
            const Sum g = sum_of(d, f.before);
            if (unpack(d, d.cloud, f.before))
                NXLOG_I(kTag, "Co-op stage: %s: this member's own Cloud fields written back (%s): sum %016llx, %u with +0x%llx set",
                        d.name, why, g.hash, g.tally, static_cast<unsigned long long>(d.tally));
            else
                NXLOG_W(kTag, "Co-op stage: %s: this member's own Cloud fields not written back (%s): no save data", d.name, why);
        }
        f.mode.store(Mode::Off, std::memory_order_release);
        f.loading.store(false, std::memory_order_release);
        f.aside.clear();
        f.before.clear();
    }
    NXLOG_I(kTag, "Co-op stage: %s off (%s; was %s, %llu Cloud answers since it turned on)", d.name, why, mode_name(was),
            static_cast<unsigned long long>(f.answered + f.answers.exchange(0, std::memory_order_relaxed)));
    f.answered = 0;
    f.asks = 0;
    f.late_logged = false;
    bool any = false;
    for (const Fam& o : s.fam) any = any || o.mode.load(std::memory_order_acquire) != Mode::Off;
    if (!any) {
        s.authority.store(false, std::memory_order_release);
        s.marks.new_session();
    }
}

// The authority, its area settled: its own fields into the Cloud set (game thread).
void seed_authority(Family id) {
    State& s = st();
    const Desc& d = desc(id);
    Fam& f = fam(id);
    std::vector<uint8_t> own, cloud;
    {
        std::lock_guard<std::mutex> lock(s.copy);
        if (!pack(d, d.own, &own) || !pack(d, d.cloud, &cloud) || !unpack(d, d.cloud, own)) return;   // no save data yet: next frame
        s.authority.store(true, std::memory_order_release);
        f.mode.store(Mode::On, std::memory_order_release);
    }
    const Sum o = sum_of(d, own), c = sum_of(d, cloud);
    NXLOG_I(kTag, "Co-op stage: %s on (the authority): own -> Cloud, %u records x %llu bytes, sum %016llx, %u with +0x%llx set "
            "(column %016llx; the Cloud set held sum %016llx, %u set)", d.name, d.count, static_cast<unsigned long long>(d.bytes),
            o.hash, o.tally, static_cast<unsigned long long>(d.tally), o.column, c.hash, c.tally);
}

void ask(Family id) {
    const Desc& d = desc(id);
    Fam& f = fam(id);
    const double now = qpc::seconds();
    if (f.asks && now - f.asked_at < kAskEvery) return;
    const bool queued = link::to_authority(wire::Type::StageAsk, wire::Writer(link::self()).u8(d.wire).take());
    f.asked_at = now;
    if (f.asks++ % kAskLogEvery == 0)
        NXLOG_I(kTag, "Co-op stage: %s: asked the authority (node %d) for its Cloud fields (ask %u%s)", d.name, link::authority(),
                f.asks, queued ? "" : ", not queued");
}

// A row's call is `bl 0x77ecb0` (checked the first time the row is reached).
bool check(const Row& r) {
    uint32_t before = 0;
    if (guest::read(base() + r.site - 4, before) && is_bl_to(before, r.site - 4, layout::kCloudIsland)) return true;
    NXLOG_W(kTag, "Co-op stage: %s row main+0x%llx (%s) left off: main+0x%llx is %08x, not bl main+0x%llx", desc(r.family).name,
            static_cast<unsigned long long>(r.site), r.what, static_cast<unsigned long long>(r.site - 4), before,
            static_cast<unsigned long long>(layout::kCloudIsland));
    return false;
}

void count_line(double now, bool in) {
    State& s = st();
    if (now - s.counted_at < kCountEvery) return;
    if (!in) {   // no session: rows reached with the mode off are every co-op launch's solo frames, not worth a line
        s.counted_at = now;
        for (Fam& f : s.fam) {
            f.answered += f.answers.exchange(0, std::memory_order_relaxed);
            f.passes_off.store(0, std::memory_order_relaxed);
        }
        return;
    }
    const double span = s.counted_at ? now - s.counted_at : kCountEvery;
    s.counted_at = now;
    for (size_t i = 0; i < kFamilyCount; ++i) {
        Fam& f = s.fam[i];
        const uint32_t yes = f.answers.exchange(0, std::memory_order_relaxed);
        const uint32_t off = f.passes_off.exchange(0, std::memory_order_relaxed);
        f.answered += yes;
        const Mode mode = f.mode.load(std::memory_order_acquire);
        if (yes || off)
            NXLOG_I(kTag, "Co-op stage: %s: %u Cloud answers and %u rows reached with the mode off in the last %.0f s (mode %s%s)",
                    kFamilies[i].name, yes, off, span, mode_name(mode),
                    f.loading.load(std::memory_order_acquire) ? ", an area load in flight" : "");
        if (settled_on(mode, f)) log_live(kFamilies[i], "now");
    }
}

// The set-aside fields back into the Cloud set after an area load: at the fix-up, or kLoadLate after a load whose fix-up never ran
// (an aborted load), so the mode cannot stay stuck with the rows off. Game thread.
void restore_aside(Family id, const char* when) {
    State& s = st();
    const Desc& d = desc(id);
    Fam& f = fam(id);
    if (session::area_stage() == layout::kStageCloudIsland) {
        end(id, "the load went to Cloud Island");
        return;
    }
    std::vector<uint8_t> file;
    std::lock_guard<std::mutex> lock(s.copy);
    if (!pack(d, d.cloud, &file) || !unpack(d, d.cloud, f.aside)) {
        NXLOG_W(kTag, "Co-op stage: %s: the fields set aside for the area load were not written back at the fix-up: no save data (the "
                "next load's fix-up writes them)", d.name);
        return;
    }
    f.loading.store(false, std::memory_order_release);
    f.late_logged = false;
    const Sum got = sum_of(d, file), g = sum_of(d, f.aside);
    NXLOG_I(kTag, "Co-op stage: %s: written back %s (stage %u): the file's sum %016llx (%u set), the group's %016llx "
            "(%u set, column %016llx)", d.name, when, session::area_stage(), got.hash, got.tally, g.hash, g.tally, g.column);
    f.aside.clear();
}

}  // namespace

bool on(Family f) {
    const Fam& m = fam(f);
    return settled_on(m.mode.load(std::memory_order_acquire), m);
}

uint64_t authority_set(Family f) { return on(f) ? desc(f).cloud : desc(f).own; }

int on_answer(::Ctx* c, uint64_t site) {
    const Row* r = site_rows::find(kRows.data(), kRowCount, site);
    if (!r) return 0;
    State& s = st();
    Fam& f = fam(r->family);
    if (!on(r->family)) {
        f.passes_off.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const size_t i = static_cast<size_t>(r - kRows.data());
    if (!s.marks.good(i, [r] { return check(*r); })) return 0;
    const Desc& d = desc(r->family);
    const uint64_t game = c->x[0];
    uint32_t record = 0;
    if (r->reset) {
        const uint32_t w = static_cast<uint32_t>(c->x[r->reg]);
        record = w < d.count ? w : 0;
        if (const uint64_t at = set_at(d, d.cloud)) std::memset(reinterpret_cast<void*>(at + record * d.size + d.at), 0, d.bytes);
    } else {
        c->x[0] = 1;
    }
    f.answers.fetch_add(1, std::memory_order_relaxed);
    if (s.marks.first(i)) {
        if (r->reset)
            NXLOG_I(kTag, "Co-op stage: %s reset main+0x%llx (%s): the game's answer %llu stands, Cloud record %u cleared", d.name,
                    static_cast<unsigned long long>(r->site), r->what, static_cast<unsigned long long>(game & 1), record);
        else
            NXLOG_I(kTag, "Co-op stage: %s row main+0x%llx (%s) answers Cloud (1, the game's %llu)", d.name,
                    static_cast<unsigned long long>(r->site), r->what, static_cast<unsigned long long>(game & 1));
    }
    return 0;
}

int on_save_point(::Ctx*, uint64_t site) {
    State& s = st();
    if (!s.authority.load(std::memory_order_acquire)) return 0;
    const char* path = site == layout::kPreSave ? "pre-save 0x5aa8b8" : "save request 0x241324";
    std::lock_guard<std::mutex> lock(s.copy);
    for (size_t i = 0; i < kFamilyCount; ++i)
        if (s.fam[i].mode.load(std::memory_order_acquire) == Mode::On) copy_back(kFamilies[i], s.fam[i], path);
    return 0;
}

int on_load_job(::Ctx* c) {
    State& s = st();
    const uint64_t at = c->x[1];   // &GM+0x18
    const uint32_t mode = static_cast<uint32_t>(c->x[2]);
    if (!guest::ok(at, layout::kLoadEntryKind + 1)) return 0;
    const uint8_t stage = guest::rd<uint8_t>(at), kind = guest::rd<uint8_t>(at + layout::kLoadEntryKind);
    if (mode == kLoadStageData && kind == kEntryVisited) {   // whatever the modes: an authority seed waits for this load's fix-up
        s.load_pending_since.store(qpc::seconds(), std::memory_order_relaxed);
        s.load_pending.store(true, std::memory_order_release);
    }
    for (size_t i = 0; i < kFamilyCount; ++i) {
        const Desc& d = kFamilies[i];
        Fam& f = s.fam[i];
        if (f.mode.load(std::memory_order_acquire) != Mode::On) continue;
        if (!s.authority.load(std::memory_order_acquire)) continue;   // a member's Cloud set is the seed's: nothing set aside
        if (stage == layout::kStageCloudIsland) {
            end(d.id, "a load into Cloud Island");
            continue;
        }
        if (mode != kLoadStageData || kind != kEntryVisited) {
            NXLOG_I(kTag, "Co-op stage: %s: load job (stage %u, mode %u, entry kind %u): no STGDAT read over the Cloud set, nothing set "
                    "aside", d.name, stage, mode, kind);
            continue;
        }
        std::lock_guard<std::mutex> lock(s.copy);
        if (f.loading.load(std::memory_order_acquire)) {
            NXLOG_W(kTag, "Co-op stage: %s: load job (stage %u) while the last one's fields are still set aside: those are kept",
                    d.name, stage);
            continue;
        }
        if (!pack(d, d.cloud, &f.aside)) continue;
        f.loading_since.store(qpc::seconds(), std::memory_order_relaxed);
        f.loading.store(true, std::memory_order_release);
        const Sum g = sum_of(d, f.aside);
        NXLOG_I(kTag, "Co-op stage: %s: set aside for the load of stage %u: %zu bytes, sum %016llx, %u with +0x%llx set (rows give the "
                "game's answer until the fix-up)", d.name, stage, f.aside.size(), g.hash, g.tally,
                static_cast<unsigned long long>(d.tally));
    }
    return 0;
}

int on_load_fixup(::Ctx*) {
    st().load_pending.store(false, std::memory_order_release);
    for (size_t i = 0; i < kFamilyCount; ++i)
        if (st().fam[i].loading.load(std::memory_order_acquire)) restore_aside(kFamilies[i].id, "after the area load");
    return 0;
}

void on_member(int node, bool joined, bool self) {
    if (self) {
        if (!joined)
            for (const Desc& d : kFamilies) end(d.id, "this process left the session's channel");
        return;
    }
    if (!link::is_authority()) return;
    for (const Desc& d : kFamilies)
        NXLOG_I(kTag, "Co-op stage: %s: node %d %s (mode %s here%s)", d.name, node,
                joined ? "joined, it asks for the Cloud fields itself" : "left, the group's fields stay here",
                mode_name(fam(d.id).mode.load(std::memory_order_acquire)), on(d.id) ? "" : ", not answering");
}

void on_ask_message(::Ctx*, int from, uint8_t, wire::Reader& r) {
    const uint8_t w = r.u8();
    if (!r.ok() || !link::is_authority()) return;
    const Desc* d = by_wire(w);
    if (!d) {
        NXLOG_W(kTag, "Co-op stage: node %d asks for family %u, which this build's table does not have", from, w);
        return;
    }
    if (!on(d->id)) {
        NXLOG_I(kTag, "Co-op stage: %s: node %d asks: not answered, the mode here is %s%s (it asks again)", d->name, from,
                mode_name(fam(d->id).mode.load(std::memory_order_acquire)),
                fam(d->id).loading.load(std::memory_order_acquire) ? " with an area load in flight" : "");
        return;
    }
    std::vector<uint8_t> fields;
    {
        std::lock_guard<std::mutex> lock(st().copy);
        if (!pack(*d, d->cloud, &fields)) return;
    }
    const Sum g = sum_of(*d, fields);
    const bool queued = link::to_node(from, wire::Type::StageTable,
                                      wire::Writer(link::self())
                                          .u8(d->wire)
                                          .u16(static_cast<uint16_t>(d->count))
                                          .u16(static_cast<uint16_t>(d->bytes))
                                          .bytes(fields.data(), fields.size())
                                          .take());
    NXLOG_I(kTag, "Co-op stage: %s: sent the Cloud fields to node %d%s: %u records x %llu bytes, sum %016llx, %u with +0x%llx set "
            "(column %016llx)", d->name, from, queued ? "" : " (not queued)", d->count, static_cast<unsigned long long>(d->bytes),
            g.hash, g.tally, static_cast<unsigned long long>(d->tally), g.column);
}

void on_table_message(::Ctx*, int from, uint8_t, wire::Reader& r) {
    const uint8_t w = r.u8();
    const uint32_t n = r.u16(), m = r.u16();
    if (!r.ok() || link::is_authority()) return;
    const Desc* d = by_wire(w);
    if (!d || n != d->count || m != d->bytes) {
        NXLOG_W(kTag, "Co-op stage: node %d's StageTable (family %u, %u records x %u bytes) does not match this build's table%s", from,
                w, n, m, d ? "" : " (no such family)");
        return;
    }
    std::vector<uint8_t> fields(static_cast<size_t>(n) * m);
    r.bytes(fields.data(), fields.size());
    if (!r.ok()) {
        NXLOG_W(kTag, "Co-op stage: %s: node %d's StageTable is shorter than %u records x %u bytes", d->name, from, n, m);
        return;
    }
    Fam& f = fam(d->id);
    const Mode mode = f.mode.load(std::memory_order_acquire);
    const Sum g = sum_of(*d, fields);
    if (mode != Mode::Asking) {
        NXLOG_I(kTag, "Co-op stage: %s: node %d's Cloud fields (sum %016llx) not taken: the mode here is %s", d->name, from, g.hash,
                mode_name(mode));
        return;
    }
    std::vector<uint8_t> before;
    {
        std::lock_guard<std::mutex> lock(st().copy);
        if (!pack(*d, d->cloud, &before) || !unpack(*d, d->cloud, fields)) {
            NXLOG_W(kTag, "Co-op stage: %s: node %d's Cloud fields not taken: no save data (it asks again)", d->name, from);
            return;
        }
        f.before = before;
        f.mode.store(Mode::On, std::memory_order_release);
    }
    const Sum b = sum_of(*d, before);
    NXLOG_I(kTag, "Co-op stage: %s on (a member): node %d's Cloud fields taken after %u asks: %zu bytes, sum %016llx, %u with +0x%llx "
            "set (column %016llx; this member's Cloud set held sum %016llx, %u set: written back when the mode ends)", d->name, from,
            f.asks, fields.size(), g.hash, g.tally, static_cast<unsigned long long>(d->tally), g.column, b.hash, b.tally);
}

void tick(bool in_game) {
    const bool in = link::in_session();
    const uint8_t stage = session::area_stage();
    const double now = qpc::seconds();
    State& s = st();
    if (s.load_pending.load(std::memory_order_acquire) &&
        now - s.load_pending_since.load(std::memory_order_relaxed) > kLoadLate && session::area_settled(in_game)) {
        s.load_pending.store(false, std::memory_order_release);
        NXLOG_W(kTag, "Co-op stage: no fix-up 0x5af540 %.0f s after a visited area's load job: taken as done", kLoadLate);
    }
    const bool load_pending = s.load_pending.load(std::memory_order_acquire);
    for (const Desc& d : kFamilies) {
        Fam& f = fam(d.id);
        Mode mode = f.mode.load(std::memory_order_acquire);
        if (mode != Mode::Off && (!in || stage == layout::kStageCloudIsland)) {
            end(d.id, !in ? "the session's channel closed" : "the stage is Cloud Island");
            mode = Mode::Off;
        }
        if (!in || stage == layout::kStageCloudIsland) continue;
        if (mode == Mode::Off) {
            if (link::is_authority()) {
                if (!load_pending && session::area_settled(in_game)) seed_authority(d.id);
            } else if (session_bits() & layout::kSessionVisitor) {
                f.mode.store(Mode::Asking, std::memory_order_release);
                mode = Mode::Asking;
            }
        }
        if (mode == Mode::Asking) ask(d.id);
        if (f.loading.load(std::memory_order_acquire) && !f.late_logged &&
            now - f.loading_since.load(std::memory_order_relaxed) > kLoadLate && session::area_settled(in_game)) {
            f.late_logged = true;
            NXLOG_W(kTag, "Co-op stage: %s: no fix-up 0x5af540 %.0f s after the area load (an aborted load?)", d.name, kLoadLate);
            restore_aside(d.id, "with no fix-up");
        }
    }
    count_line(now, in);
}

void reset() {
    State& s = st();
    for (const Desc& d : kFamilies) end(d.id, "co-op reset");
    for (Fam& f : s.fam) {
        f.answers.store(0, std::memory_order_relaxed);
        f.passes_off.store(0, std::memory_order_relaxed);
        f.answered = 0;
        f.asked_at = 0.0;
        f.asks = 0;
        f.late_logged = false;
    }
    s.authority.store(false, std::memory_order_relaxed);
    s.load_pending.store(false, std::memory_order_relaxed);
    s.marks.reset();
    s.counted_at = 0.0;
}

}  // namespace nxrt::games::pkp::coop::stage
