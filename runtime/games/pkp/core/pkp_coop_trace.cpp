// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's trace (pkp_coop_internal.h). What the testers' logs need to put two players' runs side by side:
// it only reads, and only in a session. Every line starts "Co-op trace:".
//   clock    the wall clock (UTC) and this process's node and role, at the session's start and every minute: two logs line up on it
//   quests   the state word of every quest (save root + kQuestWords, u16 per id): at the session's start the ones past 1 (listed) and how
//            many are listed, then each change
//   players  this player's place and the second player slot's (player manager +0x38), every 3 s when one moved 30 units or more
//   chara    a character's place (its CharacterStatus record: position vec4 at +0x10 with w 1.0, data id at +0x24) when it moved 150 units
//            or more since it was last logged, once a second; a scan where more than kStageChange moved (a stage load) is one line instead,
//            at most every kStageLineEvery seconds
#include "core/pkp_coop_internal.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::coop::trace {

namespace {

constexpr const char* kTag = "pkp";
constexpr double kClockEvery = 60.0, kPlayersEvery = 3.0, kCharasEvery = 1.0, kStageLineEvery = 30.0;
constexpr float kPlayerMoved = 30.0f, kCharaMoved = 150.0f;
constexpr int kCharaLinesMax = 12, kStageChange = 30, kQuestsPerLine = 16;
constexpr uint64_t kSecondPlayer = 0x38;   // player manager: the second player's character
constexpr uint64_t kCharaPos = 0x10, kCharaDataId = 0x24;   // CharacterStatus: m_position (vec4, w 1.0 when set), m_id (u16)

struct Vec {
    float x = 0, y = 0, z = 0;
};

struct State {
    bool on = false;   // in a session: the baselines are taken
    double clock_at = 0, players_at = 0, charas_at = 0, stage_line_at = -1e9;
    uint16_t quests[layout::kQuestWordCount] = {};
    Vec me, other;
    std::vector<Vec> charas;   // the place last logged per chara (0,0,0: not placed)
};
State& st() {
    static State s;
    return s;
}

float dist(const Vec& a, const Vec& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}
bool placed(const Vec& v) { return v.x != 0.0f || v.y != 0.0f || v.z != 0.0f; }

Vec vec_at(uint64_t a) {
    Vec v;
    if (guest::ok(a, 12)) std::memcpy(&v, reinterpret_cast<const void*>(a), 12);
    return v;
}

uint64_t quest_words() {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    if (!guest::ok(r, 8)) return 0;
    const uint64_t q = r + layout::kQuestWords;
    return guest::ok(q, layout::kQuestWordCount * 2) ? q : 0;
}

uint64_t chara_records() {
    const uint64_t p = guest::rd<uint64_t>(base() + layout::kCharaStatusRoot);
    if (!guest::ok(p, 8)) return 0;
    const uint64_t d = guest::rd<uint64_t>(p);
    if (!guest::ok(d, 8)) return 0;
    const uint64_t r = d + layout::kCharaStatus;
    return guest::ok(r, uint64_t{layout::kCharaCount} * layout::kCharaStatusStride) ? r : 0;
}

uint64_t player_slot(uint64_t slot) {
    const uint64_t m = guest::rd<uint64_t>(base() + layout::kPlayerManager);
    if (!guest::ok(m, slot + 8)) return 0;
    const uint64_t p = guest::rd<uint64_t>(m + slot);
    return guest::ok(p, layout::kPosition + 16) ? p : 0;
}

void log_clock() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const long long secs = std::chrono::duration_cast<std::chrono::seconds>(now).count() % 86400;
    NXLOG_I(kTag, "Co-op trace: clock %02lld:%02lld:%02lld UTC, node %u (%s)", secs / 3600, secs / 60 % 60, secs % 60,
            static_cast<unsigned>(link::self()), link::is_authority() ? "the authority" : "a member");
}

void start_quests(State& s) {
    const uint64_t q = quest_words();
    if (!q) return;
    std::memcpy(s.quests, reinterpret_cast<const void*>(q), sizeof s.quests);
    std::string line;
    int n = 0, listed = 0;
    for (uint32_t i = 0; i < layout::kQuestWordCount; ++i) {
        if (s.quests[i] == 1) ++listed;
        if (s.quests[i] <= 1) continue;
        char item[24];
        std::snprintf(item, sizeof item, " %u=0x%x", i + 1, static_cast<unsigned>(s.quests[i]));
        line += item;
        if (++n % kQuestsPerLine == 0) {
            NXLOG_I(kTag, "Co-op trace: quests at the start:%s", line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) NXLOG_I(kTag, "Co-op trace: quests at the start:%s", line.c_str());
    NXLOG_I(kTag, "Co-op trace: quests at the start: %d past listed, %d listed (word 1)", n, listed);
}

void watch_quests(State& s) {
    const uint64_t q = quest_words();
    if (!q) return;
    uint16_t now[layout::kQuestWordCount];
    std::memcpy(now, reinterpret_cast<const void*>(q), sizeof now);
    if (std::memcmp(now, s.quests, sizeof now) == 0) return;
    for (uint32_t i = 0; i < layout::kQuestWordCount; ++i)
        if (now[i] != s.quests[i])
            NXLOG_I(kTag, "Co-op trace: quest %u word 0x%x -> 0x%x", i + 1, static_cast<unsigned>(s.quests[i]), static_cast<unsigned>(now[i]));
    std::memcpy(s.quests, now, sizeof now);
}

void watch_players(State& s, double t) {
    if (t - s.players_at < kPlayersEvery) return;
    s.players_at = t;
    const uint64_t me = player_slot(layout::kManagerPlayer), other = player_slot(kSecondPlayer);
    const Vec m = me ? vec_at(me + layout::kPosition) : Vec{};
    const Vec o = other ? vec_at(other + layout::kPosition) : Vec{};
    if (dist(m, s.me) < kPlayerMoved && dist(o, s.other) < kPlayerMoved) return;
    s.me = m;
    s.other = o;
    if (other)
        NXLOG_I(kTag, "Co-op trace: players: here (%.0f, %.0f, %.0f), second slot (%.0f, %.0f, %.0f), %.0f apart", m.x, m.y, m.z, o.x, o.y,
                o.z, dist(m, o));
    else
        NXLOG_I(kTag, "Co-op trace: players: here (%.0f, %.0f, %.0f), second slot empty", m.x, m.y, m.z);
}

void watch_charas(State& s, double t, bool first) {
    if (!first && t - s.charas_at < kCharasEvery) return;
    s.charas_at = t;
    const uint64_t r = chara_records();
    if (!r) return;
    s.charas.resize(layout::kCharaCount);
    std::vector<Vec> now(layout::kCharaCount);
    std::vector<uint32_t> moved;
    for (uint32_t i = 0; i < layout::kCharaCount; ++i) {
        float v[4];
        std::memcpy(v, reinterpret_cast<const void*>(r + i * layout::kCharaStatusStride + kCharaPos), sizeof v);
        if (v[3] == 1.0f) now[i] = {v[0], v[1], v[2]};   // otherwise not a set position: (0,0,0)
        if (placed(now[i]) != placed(s.charas[i]) || (placed(now[i]) && dist(now[i], s.charas[i]) >= kCharaMoved)) moved.push_back(i);
    }
    if (first) {
        NXLOG_I(kTag, "Co-op trace: %zu characters placed at the start", moved.size());
    } else if (static_cast<int>(moved.size()) > kStageChange) {
        if (t - s.stage_line_at >= kStageLineEvery) {
            s.stage_line_at = t;
            NXLOG_I(kTag, "Co-op trace: %zu characters changed place at once (a stage load); not listed", moved.size());
        }
    } else {
        int lines = 0;
        for (uint32_t i : moved) {
            if (lines++ == kCharaLinesMax) {
                NXLOG_I(kTag, "Co-op trace: and %zu more characters moved", moved.size() - kCharaLinesMax);
                break;
            }
            const unsigned id = guest::rd<uint16_t>(r + i * layout::kCharaStatusStride + kCharaDataId);
            if (!placed(now[i]))
                NXLOG_I(kTag, "Co-op trace: chara %u (data id %u) left the stage", i, id);
            else
                NXLOG_I(kTag, "Co-op trace: chara %u (data id %u) at (%.0f, %.0f, %.0f)", i, id, now[i].x, now[i].y, now[i].z);
        }
    }
    for (uint32_t i : moved) s.charas[i] = now[i];
}

}  // namespace

void tick(bool in_game) {
    State& s = st();
    if (!link::in_session()) {
        if (s.on) reset();
        return;
    }
    if (!in_game) return;
    const double t = qpc::seconds();
    const bool first = !s.on;
    if (first) {
        s.on = true;
        s.clock_at = t;
        log_clock();
        start_quests(s);
    } else if (t - s.clock_at >= kClockEvery) {
        s.clock_at = t;
        log_clock();
    }
    if (!first) watch_quests(s);
    watch_players(s, t);
    watch_charas(s, t, first);
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::trace
