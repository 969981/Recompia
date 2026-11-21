// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's shared habitat hints (pkp_coop_internal.h). The Habitat Dex shows, on a habitat's page, a creature's
// shadowed silhouette once the player knows the hint for that (creature, habitat) pair. The game keeps the hints in each player's own
// save and sets them where the player was: a trace investigated (0x1c1e60 -> 0xa96e28), a creature met (0xa970a0), a habitat found
// (0xa97360). pkp_coop_discovery.cpp shares the habitat's found / scene bits, not the pairs, so a teammate's page stayed empty.
//
// The game, 2.0.1 (modding-docs coop-v2/TUTORIAL-RE.md section 3): R + 0x6d85a4 + creature * 0x81 + (hab >> 3), bit hab & 7 = the hint
// for that pair is known (m_pokemonHistory; creature 1..0x3ff, habitat 1..499); the reader is 0xa96fe8(creature, habitat), called by
// the Habitat Dex's creature cells (0xf889b0, 0xf8ac80, 0xf8dd08, 0xf91f90). The 0x40 bytes after it ("read") are the player's own.
// Every process shares the bits that became set (HabitatHints) and takes the union, set straight into its save at a safe moment:
// the setters would also mark the habitat found.
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <array>
#include <bitset>
#include <cstring>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::hints {

namespace {

constexpr const char* kTag = "pkp";

constexpr uint64_t kHints = 0x6d85a4;      // R + this + creature * kStride: the hint bitmap
constexpr uint64_t kStride = 0x81;
constexpr uint32_t kCreatureMax = 0x3ff;
constexpr size_t kBytes = 0x40;            // 512 habitat bits; habitats 1..499 are real
constexpr size_t kChunk = 12;              // creatures in one message
constexpr double kAllEvery = 30.0;
constexpr double kSettle = 1.0;
constexpr unsigned kApplyPerTick = 32;

using Bits = std::array<uint8_t, kBytes>;

struct State {
    std::vector<Bits> seen, want;
    std::bitset<kCreatureMax + 1> wanted;
    double at[kCreatureMax + 1] = {};
    uint8_t origin[kCreatureMax + 1] = {};
    bool have_seen = false, send_all = true;
    double all_at = 0.0;
    State() : seen(kCreatureMax + 1), want(kCreatureMax + 1) {}
};
State& st() {
    static State s;
    return s;
}

uint64_t save_root() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    return r && guest::ok(r + kHints, kCreatureMax * kStride + kBytes) ? r : 0;
}
uint64_t at(uint64_t root, uint32_t creature) { return root + kHints + creature * kStride; }
void clean(Bits& b) {   // habitat 0 and 500..511 are no habitats
    b[0] &= 0xfe;
    b[62] &= 0x0f;
    b[63] = 0;
}
Bits read(uint64_t root, uint32_t creature) {
    Bits b;
    guest::read(at(root, creature), b.data(), kBytes);
    clean(b);
    return b;
}
bool any(const Bits& b) {
    for (uint8_t v : b)
        if (v) return true;
    return false;
}
bool grew(const Bits& now, const Bits& old) {
    for (size_t i = 0; i < kBytes; ++i)
        if (now[i] & ~old[i]) return true;
    return false;
}

bool shared_here() { return (session_bits() & layout::kSessionOnline) && link::in_session(); }
bool safe_now(bool in_game) {
    const bool in_world = role() == CoopRole::Host || (session_bits() & layout::kSessionVisitor);
    return in_world && session::can_walk(in_game) && !talk::talking();
}

bool share(uint64_t root, bool all) {
    State& s = st();
    const bool first = !s.have_seen;
    s.have_seen = true;
    std::vector<uint32_t> ids;
    for (uint32_t c = 1; c <= kCreatureMax; ++c) {
        const Bits b = read(root, c);
        if (any(b) && (all || first || grew(b, s.seen[c]))) ids.push_back(c);
    }
    for (size_t i = 0; i < ids.size(); i += kChunk) {
        const size_t n = std::min(kChunk, ids.size() - i);
        wire::Writer w(link::self());
        w.u16(static_cast<uint16_t>(n));
        for (size_t k = 0; k < n; ++k) {
            const Bits b = read(root, ids[i + k]);
            w.u16(static_cast<uint16_t>(ids[i + k])).bytes(b.data(), kBytes);
        }
        if (!link::share(wire::Type::HabitatHints, w.take())) return false;
        for (size_t k = 0; k < n; ++k) {
            const Bits b = read(root, ids[i + k]);
            if (!first && grew(b, s.seen[ids[i + k]])) NXLOG_I(kTag, "Co-op hints: creature %u has new habitat hints here, shared", ids[i + k]);
            s.seen[ids[i + k]] = b;
        }
    }
    return true;
}

void apply(uint64_t root) {
    State& s = st();
    if (s.wanted.none()) return;
    const double now = qpc::seconds();
    unsigned done = 0;
    for (uint32_t c = 1; c <= kCreatureMax && done < kApplyPerTick; ++c) {
        if (!s.wanted.test(c) || now - s.at[c] < kSettle) continue;
        s.wanted.reset(c);
        Bits have = read(root, c);
        Bits add = s.want[c];
        unsigned bits = 0;
        for (size_t i = 0; i < kBytes; ++i) {
            add[i] &= static_cast<uint8_t>(~have[i]);
            for (uint8_t v = add[i]; v; v &= static_cast<uint8_t>(v - 1)) ++bits;
        }
        s.want[c].fill(0);
        if (!bits) continue;
        for (size_t i = 0; i < kBytes; ++i)
            if (add[i]) guest::wr<uint8_t>(at(root, c) + i, static_cast<uint8_t>(guest::rd<uint8_t>(at(root, c) + i) | add[i]));
        s.seen[c] = read(root, c);   // ours now: not sent back as a change
        ++done;
        NXLOG_I(kTag, "Co-op hints: creature %u: %u habitat hints from node %u set here", c, bits, s.origin[c]);
    }
}

}  // namespace

void on_message(::Ctx*, int, uint8_t origin, wire::Reader& r) {
    State& s = st();
    const uint16_t n = r.u16();
    const uint64_t root = save_root();
    if (!r.ok() || !root || !shared_here()) return;
    for (uint16_t k = 0; k < n; ++k) {
        const uint16_t c = r.u16();
        Bits b;
        r.bytes(b.data(), kBytes);
        if (!r.ok()) return;
        if (c < 1 || c > kCreatureMax) continue;
        clean(b);
        const Bits have = read(root, c);
        bool add = false;
        for (size_t i = 0; i < kBytes; ++i) {
            if (b[i] & ~have[i]) add = true;
            s.want[c][i] |= b[i];
        }
        if (!add) continue;
        if (!s.wanted.test(c)) s.at[c] = qpc::seconds();
        s.wanted.set(c);
        s.origin[c] = origin;
    }
}

void tick(::Ctx*, bool in_game) {
    State& s = st();
    const uint64_t root = save_root();
    if (!root || !shared_here()) return;
    if (safe_now(in_game)) apply(root);
    const double now = qpc::seconds();
    const bool all = s.send_all || now - s.all_at >= kAllEvery;
    if (!share(root, all) || !all) return;
    s.send_all = false;
    s.all_at = now;
}

void on_session_change() { st() = State{}; }
void on_member_joined() { st().send_all = true; }
void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::hints
