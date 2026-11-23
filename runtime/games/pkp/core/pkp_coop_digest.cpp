// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's shared-state digests (pkp_coop_internal.h digest, the State* messages in pkp_coop_wire.h). Logging only:
// nothing here changes the game. What the shared parts promise is that every member ends with the authority's quest states, abilities,
// the dex, habitats, recipes, unlock lists and played scenes (and, logged the same way, the area levels, gates and open areas the game reads). When one member does not (a dex entry that reached 3 players of 4),
// each part's own lines say what it applied, but not that the result differs. So:
//   - the authority sends every member a digest of each domain (a hash of its set entries and their count) every kDigestEvery seconds;
//   - a member compares its own. One differing digest can be a change still on its way, so only a domain that differs in two digests
//     in a row is a MISMATCH (Warn): the member asks for the authority's entries (StateAsk, once per kAskEvery per domain);
//   - with them (StateFull), the member writes every entry that differs (the authority's value and its own) and sends the first
//     kReportMax of them to the authority (StateReport), which writes them in its own log: a host's log alone shows which member lacks what.
// Every look writes a line (j.sync digest) when a domain's own hash changed, on every process, so logs can also be compared by hand.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_journal.h"
#include "core/pkp_layout.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::digest {

namespace {

constexpr double kDigestEvery = 30.0, kAskEvery = 120.0;
constexpr size_t kReportMax = 64, kDiffLines = 400;

struct Domain {
    uint8_t id;
    const char* name;
    bool (*read)(Entries& out);
};
constexpr Domain kDomains[] = {
    {1, "quests", &quest_states}, {2, "abilities", &abilities}, {3, "dex", &dex},         {4, "habitats", &habitats},
    {5, "recipes", &recipes},     {6, "unlocks", &unlocks},     {7, "scenes", &scenes},       {8, "area levels", &area_levels},
    {9, "gates", &gates},         {10, "trainer level", &trainer_level}, {11, "open areas", &open_areas},
};
constexpr size_t kCount = sizeof kDomains / sizeof kDomains[0];

struct Mine {
    bool read = false;
    uint32_t hash = 0;
    uint16_t count = 0;
};
std::array<double, kCount> never() {
    std::array<double, kCount> a;
    a.fill(-1e9);
    return a;
}

struct State {
    double sent_at = -1e9;
    std::array<uint32_t, kCount> logged_hash{};   // the own hash last written per domain
    std::array<bool, kCount> logged{};
    std::array<uint8_t, kCount> differs{};        // digests in a row this domain differed in (member)
    std::array<double, kCount> asked_at = never();
};
State& st() {
    static State s;
    return s;
}

const Domain* domain_of(uint8_t id) {
    for (const Domain& d : kDomains)
        if (d.id == id) return &d;
    return nullptr;
}
size_t index_of(const Domain* d) { return static_cast<size_t>(d - kDomains); }

uint32_t hash_of(const Entries& e) {
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) {
        for (int i = 0; i < 4; ++i) h = (h ^ ((v >> (i * 8)) & 0xff)) * 16777619u;
    };
    for (const Entry& x : e) {
        mix(x.id);
        mix(x.value);
    }
    return h;
}

Mine mine(const Domain& d, Entries* keep = nullptr) {
    Entries e;
    Mine m;
    if (!d.read(e)) return m;
    std::sort(e.begin(), e.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
    m.read = true;
    m.hash = hash_of(e);
    m.count = static_cast<uint16_t>(std::min<size_t>(e.size(), 0xffff));
    if (keep) *keep = std::move(e);
    return m;
}

// Writes a line for each domain whose own entries changed since the last one (every process).
void log_own(State& s, const Mine* m) {
    for (size_t i = 0; i < kCount; ++i) {
        if (!m[i].read || (s.logged[i] && s.logged_hash[i] == m[i].hash)) continue;
        s.logged[i] = true;
        s.logged_hash[i] = m[i].hash;
        journal::Rec(journal::Area::Sync, "digest").note("%s here", kDomains[i].name).out("%u set, hash %08x", m[i].count, m[i].hash);
    }
}

}  // namespace

// Not gated on in_game: that is play mode's "playing", false under every game menu, choice box and talk box, so the authority, whose
// player is in one most of the time, sent only in the gaps (alpha 6: 31..164 s apart for a 30 s period). Every domain is read from
// the save root, not from area data, so a look is safe on the game thread whenever the player is in the world.
void tick(::Ctx*, bool) {
    State& s = st();
    if (!link::in_session() || !local_player()) return;
    const double t = qpc::seconds();
    if (t - s.sent_at < kDigestEvery) return;
    s.sent_at = t;
    Mine m[kCount];
    for (size_t i = 0; i < kCount; ++i) m[i] = mine(kDomains[i]);
    log_own(s, m);
    if (!link::is_authority() || link::member_count() == 0) return;
    wire::Writer w(link::self());
    uint8_t n = 0;
    for (size_t i = 0; i < kCount; ++i) n += m[i].read;
    w.u8(n);
    for (size_t i = 0; i < kCount; ++i)
        if (m[i].read) w.u8(kDomains[i].id).u32(m[i].hash).u16(m[i].count);
    link::to_members(wire::Type::StateDigest, w.take());
}

// A member: the authority's digest against this process's own entries.
void on_digest_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    State& s = st();
    const uint8_t n = r.u8();
    const double t = qpc::seconds();
    for (uint8_t k = 0; k < n && r.ok(); ++k) {
        const uint8_t id = r.u8();
        const uint32_t hash = r.u32();
        const uint16_t count = r.u16();
        const Domain* d = domain_of(id);
        if (!r.ok() || !d) continue;   // a domain a newer authority has and this build does not
        const size_t i = index_of(d);
        const Mine m = mine(*d);
        if (!m.read) continue;
        if (m.hash == hash) {
            if (s.differs[i] >= 2)
                journal::Rec(journal::Area::Sync, "digest").note("%s", d->name).in("host %u set, hash %08x", count, hash)
                    .out("here %u set, hash %08x", m.count, m.hash).expect("the host's").ok(true).note("equal again");
            s.differs[i] = 0;
            continue;
        }
        if (s.differs[i] < 0xff) ++s.differs[i];
        if (s.differs[i] < 2) continue;   // once can be a change on its way
        journal::Rec rec(journal::Area::Sync, "digest");
        rec.note("%s", d->name).in("host %u set, hash %08x", count, hash).out("here %u set, hash %08x", m.count, m.hash)
            .expect("the host's").ok(false).note("%u digests in a row", s.differs[i]);
        if (t - s.asked_at[i] >= kAskEvery) {
            s.asked_at[i] = t;
            link::to_authority(wire::Type::StateAsk, wire::Writer(link::self()).u8(id).take());
            rec.note("asked the host for its entries");
        }
    }
}

// The authority: a member asks for one domain's entries.
void on_ask_message(::Ctx*, int from, uint8_t, wire::Reader& r) {
    const uint8_t id = r.u8();
    const Domain* d = domain_of(id);
    if (!r.ok() || !d) return;
    Entries e;
    if (!mine(*d, &e).read) return;
    const size_t n = std::min<size_t>(e.size(), 0xffff);
    wire::Writer w(link::self());
    w.u8(id).u16(static_cast<uint16_t>(n));
    for (size_t i = 0; i < n; ++i) w.u32(e[i].id).u32(e[i].value);
    journal::Rec(journal::Area::Sync, "digest ask").note("%s from n%d", d->name, from).out("sent %zu entries", n);
    link::to_node(from, wire::Type::StateFull, w.take());
}

// A member: the authority's entries; every difference is written here and the first kReportMax go to the authority's log.
void on_full_message(::Ctx*, int, uint8_t, wire::Reader& r) {
    const uint8_t id = r.u8();
    const uint16_t n = r.u16();
    const Domain* d = domain_of(id);
    if (!r.ok() || !d) return;
    Entries host;
    host.reserve(n);
    for (uint16_t i = 0; i < n && r.ok(); ++i) {
        const uint32_t eid = r.u32();
        host.push_back({eid, r.u32()});
    }
    if (!r.ok()) return;
    std::sort(host.begin(), host.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
    Entries here;
    if (!mine(*d, &here).read) return;
    struct Diff {
        uint32_t id, host, here;
    };
    std::vector<Diff> diffs;
    size_t a = 0, b = 0;
    while (a < host.size() || b < here.size()) {
        if (b == here.size() || (a < host.size() && host[a].id < here[b].id)) {
            diffs.push_back({host[a].id, host[a].value, 0});
            ++a;
        } else if (a == host.size() || here[b].id < host[a].id) {
            diffs.push_back({here[b].id, 0, here[b].value});
            ++b;
        } else {
            if (host[a].value != here[b].value) diffs.push_back({host[a].id, host[a].value, here[b].value});
            ++a;
            ++b;
        }
    }
    journal::Rec(journal::Area::Sync, "digest diff").note("%s", d->name).in("host %zu set", host.size()).out("here %zu set", here.size())
        .expect("the same entries").ok(diffs.empty()).note("%zu entries differ", diffs.size());
    std::vector<std::string> items;
    for (size_t i = 0; i < diffs.size() && i < kDiffLines; ++i)
        items.push_back(journal::fmt("%u host=%u here=%u", diffs[i].id, diffs[i].host, diffs[i].here));
    if (!items.empty()) journal::list(journal::Area::Sync, journal::fmt("digest diff %s (id host here)", d->name).c_str(), items);
    const size_t m = std::min(diffs.size(), kReportMax);
    wire::Writer w(link::self());
    w.u8(id).u16(static_cast<uint16_t>(std::min<size_t>(diffs.size(), 0xffff))).u16(static_cast<uint16_t>(m));
    for (size_t i = 0; i < m; ++i) w.u32(diffs[i].id).u32(diffs[i].host).u32(diffs[i].here);
    link::to_authority(wire::Type::StateReport, w.take());
}

// The authority: a member's differences, in the host's log.
void on_report_message(::Ctx*, int from, uint8_t, wire::Reader& r) {
    const uint8_t id = r.u8();
    const uint16_t n = r.u16(), m = r.u16();
    const Domain* d = domain_of(id);
    if (!r.ok() || !d) return;
    std::vector<std::string> items;
    for (uint16_t i = 0; i < m && r.ok(); ++i) {
        const uint32_t eid = r.u32(), host = r.u32(), here = r.u32();
        items.push_back(journal::fmt("%u host=%u n%d=%u", eid, host, from, here));
    }
    journal::Rec(journal::Area::Sync, "digest report").note("%s of n%d", d->name, from).out("%u entries differ (%u listed)", n, m)
        .expect("none").ok(n == 0);
    if (!items.empty()) journal::list(journal::Area::Sync, journal::fmt("digest report %s n%d (id host member)", d->name, from).c_str(), items);
}

void on_session_change() {
    State& s = st();
    s.sent_at = -1e9;
    s.differs.fill(0);
    s.asked_at.fill(-1e9);
}

void reset() { st() = State{}; }

// Domains no co-op part owns (layout: kSaveArea*, kSaveGate*). Levels: stage -> the level the game reads here (0x58042c: Cloud
// Island's byte for its stage, the mirror on a visitor, else the own copy), so a member that does not show the host's level differs.
// Gates: gate id -> the byte the getter 0x5c0504 reads here (the visitor mirror on a visitor, else m_gateHistory), non-zero only.
// Trainer level: m_trainerLevel, the rank the gates ask for (own save on every process, no visitor copy).
bool area_levels(Entries& out) {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t g = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!r || !g) return false;
    const bool visitor = guest::rd<uint8_t>(g + layout::kStateSession) & layout::kSessionVisitor;
    uint8_t lv[layout::kAreaStages];
    if (!guest::read(r + (visitor ? layout::kSaveAreaLevelMirror : layout::kSaveAreaLevel), lv, sizeof lv)) return false;
    lv[layout::kCloudStage] = guest::rd<uint8_t>(r + layout::kSaveCloudLevel);
    for (uint32_t s = 0; s < layout::kAreaStages; ++s)
        if (lv[s]) out.push_back({s, lv[s]});
    return true;
}

bool gates(Entries& out) {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    const uint64_t g = guest::rd<uint64_t>(base() + layout::kGameState);
    if (!r || !g) return false;
    const bool visitor = guest::rd<uint8_t>(g + layout::kStateSession) & layout::kSessionVisitor;
    uint8_t b[layout::kGateHistoryBytes];
    if (!guest::read(r + (visitor ? layout::kSaveGateMirror : layout::kSaveGateHistory), b, sizeof b)) return false;
    for (uint32_t i = 0; i < sizeof b; ++i)
        if (b[i]) out.push_back({i, b[i]});
    return true;
}

bool trainer_level(Entries& out) {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    if (!r || !guest::ok(r + layout::kSaveTrainerLevel, 1)) return false;
    out.push_back({0, guest::rd<uint8_t>(r + layout::kSaveTrainerLevel)});
    return true;
}

}  // namespace nxrt::games::pkp::coop::digest
