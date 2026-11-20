// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: seamless co-op's trace (pkp_coop_internal.h). What the testers' logs need to put two players' runs side by side:
// it only reads, and only in a session. Every line starts "Co-op trace:".
//   clock    the wall clock (UTC) and this process's node and role, at the session's start and every minute: two logs line up on it
//   quests   the state word of every quest (save root + layout::quest_word(id), u16 per id): at the session's start the ones past 1 (listed) and how
//            many are listed, then each change
//   players  this player's place and the second player slot's (player manager +0x38), every 3 s when one moved 30 units or more
//   chara    a character's place (its CharacterStatus record: position vec4 at +0x10 with w 1.0, data id at +0x24) when it moved 150 units
//            or more since it was last logged, once a second; a scan where more than kStageChange moved (a stage load) is one line instead,
//            at most every kStageLineEvery seconds
//   held     a queued event (the event queue's record slots of player slots 0 and 1) not started after kHeldAfter seconds: its id, kind,
//            mode and flag word (rec +0x704, u24), with what the queue's ready test 0x479c60 reads besides: the visitor bit, Link Play's
//            request counters (block map +0x9218378, u16 x5; bit 22 of the flag word waits on a visitor while the first two differ), the
//            slot's mode word and the queue's locks. Logged once per record and on each change, and when a held record starts or leaves.
//            The counters alone are logged too when they stand unequal and still for kHeldAfter seconds, and the stage-change request
//            when it stays kHeldAfter seconds in a phase below 5 (new events are dropped then, so they never show as held)
//   work     a character's item-making job (its m_CraftRefine record, 0x40 bytes per chara: the host's own at save root + 0x62820, a
//            member's copy of the host's at + 0x1525428, picked on the visitor bit as the game does): at the session's start every record
//            with a job, then each change of recipe, count, made, taken, end or the user's id, and a running timer that stood still for
//            kHeldAfter seconds. The host makes the job (0x34dc64) and ticks it (0x3b38f0, never on a visitor); a member's copy changes
//            only by the host's replication. At the start also this process's own user id and the four of the member table, so two
//            logs show whose job it is
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
// The event queue (HELD-EVENTS-RE): main+0x3483eb0 holds the queue global's address; per player slot (kSlotSize apart) 0x40 records of
// 0x708 bytes, then 0x40 nodes of 0x130 (+8 its record, +0x128 started). A record slot is taken when its id (u16 +0) is 1..0x18ff or its
// kind (u32 +0x664) is below 0xd1.
constexpr uint64_t kEventQueueRef = 0x3483eb0, kRecordSize = 0x708, kRecId = 0, kRecKind = 0x664, kRecMode = 0x6ec, kRecFlags = 0x704;
constexpr uint64_t kSlotSize = 0x20fa0, kNodes = 0x1c200, kNodeSize = 0x130, kNodeRecord = 8, kNodeStarted = 0x128;
constexpr int kRecords = 0x40, kSlots = 2;
constexpr uint64_t kQueueLocks = 0x41f40;   // u8 x3 after both slots, tested by flag bits 14, 16 and 23
constexpr uint64_t kModeWords = 0x35650e8, kModeWord = 0x140;   // u32 per slot: the game mode the queue matches against rec +0x6ec
constexpr uint64_t kEventRunnerRef = 0x3483e60, kRunnerHold = 0x4022;   // u8: set, the ready test passes only a few fixed ids
// The dispatcher 0x479380 holds a whole slot (0 or 1) while the slot's load phases (u32 +8, +0xc of kSlotPhases + slot * 0x2010) are
// not both above 8, and every slot while the scene holder (main+0x3485888 holds its address) has a scene at +0x32050.
constexpr uint64_t kSlotPhases = 0x35650f8, kSlotPhaseSize = 0x2010, kSceneHolderRef = 0x3485888, kSceneHeld = 0x32050;
// The queue refuses (drops) a new event without flag bit 4 while the game state's stage-change request (+0x3bb600: u32 phase, u32 kind)
// is in a phase below 5, unless it is slot 0's kind 0xf (0x92210).
constexpr uint64_t kGameState = 0x3561710, kTripRequest = 0x3bb600;
constexpr double kHeldEvery = 2.0, kHeldAfter = 10.0;
// The work watch (layout: m_CraftRefine, user ids): Cloud Island's third set of records is not traced. All records are read with one
// checked block copy a second (kCharaCount * kCraftWorkSize = 67200 bytes).
constexpr int kWorkLinesMax = 12;
constexpr double kWorkEvery = 1.0;

struct Vec {
    float x = 0, y = 0, z = 0;
};

struct State {
    bool on = false;   // in a session: the baselines are taken
    double clock_at = 0, players_at = 0, charas_at = 0, stage_line_at = -1e9;
    uint16_t quests[layout::kQuestWordCount] = {};
    Vec me, other;
    std::vector<Vec> charas;   // the place last logged per chara (0,0,0: not placed)
    double held_at = 0;
    struct Held {
        uint16_t id = 0;
        uint32_t kind = 0, mode = 0, flags = 0;
        double since = 0;
        bool logged = false;
    } held[kSlots][kRecords];
    std::string held_context;   // the last context line logged
    uint16_t rq_sent = 0, rq_answered = 0;
    double rq_since = 0;
    bool rq_logged = false;
    uint32_t trip_phase = 0xffffffffu, trip_kind = 0;
    double trip_since = 0;
    bool trip_logged = false;
    struct Work {
        float timer = 0;
        uint16_t recipe = 0;
        uint8_t count = 0, made = 0, taken = 0, dummy = 0, end = 0;
        uint8_t user[layout::kUserIdSize] = {};
        double timer_since = 0;
        bool timer_logged = false;
    };
    std::vector<Work> work;   // per chara, the record as last seen
    std::vector<uint8_t> work_block;   // this second's copy of all records
    uint64_t work_base = 0;   // the records it was read from (own or the host's copy); a change starts the listing again
    double work_at = 0;
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

// Every quest's word (ids 801..1000 from their own run, as the game keeps them) into out; false when the save is not readable.
bool read_quests(uint16_t (&out)[layout::kQuestWordCount]) {
    const uint64_t r = guest::rd<uint64_t>(base() + layout::kSaveRoot);
    if (!guest::ok(r, 8) || !guest::ok(r + layout::kQuestWords, layout::kQuestWordsEnd - layout::kQuestWords)) return false;
    for (uint32_t i = 0; i < layout::kQuestWordCount; ++i) out[i] = guest::rd<uint16_t>(r + layout::quest_word(i + 1));
    return true;
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
    if (!read_quests(s.quests)) return;
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
    uint16_t now[layout::kQuestWordCount];
    if (!read_quests(now)) return;
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

std::string hex16(const uint8_t* b) {
    char out[layout::kUserIdSize * 2 + 1];
    for (size_t i = 0; i < layout::kUserIdSize; ++i) std::snprintf(out + i * 2, 3, "%02x", b[i]);
    return out;
}

bool has_job(const State::Work& w) { return static_cast<uint16_t>(w.recipe - 1) < layout::kRecipeMax; }

std::string work_line(uint32_t chara, const State::Work& w) {
    char line[200];
    std::snprintf(line, sizeof line, "chara %u: recipe %u, count %u, made %u, taken %u, end %u%s, timer %.1f, user %s", chara, w.recipe,
                  w.count, w.made, w.taken, w.end, w.dummy ? " (dummy end approach)" : "", w.timer, hex16(w.user).c_str());
    return line;
}

std::string user_id(uint64_t a) {
    uint8_t id[layout::kUserIdSize];
    return guest::read(a, id, sizeof id) ? hex16(id) : std::string("unreadable");
}

void log_users(uint64_t r) {
    const uint64_t obj = guest::deref(guest::deref(base() + layout::kOwnMemberIndexRef));
    uint8_t own = 0;
    const bool have_index = obj && guest::read(obj + layout::kOwnMemberIndex, own);
    char idx[40];
    std::snprintf(idx, sizeof idx, ", own index %d; member table", have_index ? static_cast<int>(own) : -1);
    std::string line = "own " + user_id(r + layout::kOwnUserId) + idx;
    for (uint32_t m = 0; m < layout::kMemberCount; ++m)
        line += " " + std::to_string(m) + "=" + user_id(r + layout::kMemberTable + m * layout::kMemberSize + layout::kMemberUserId);
    NXLOG_I(kTag, "Co-op trace: user ids: %s", line.c_str());
}

// One record out of the block copy (rec points at its first byte, kCraftWorkSize bytes).
State::Work parse_work(const uint8_t* rec) {
    State::Work w;
    std::memcpy(&w.timer, rec + layout::kCraftWorkTimer, sizeof w.timer);
    std::memcpy(&w.recipe, rec + layout::kCraftWorkRecipe, sizeof w.recipe);
    w.count = rec[layout::kCraftWorkCount];
    w.made = rec[layout::kCraftWorkMade];
    w.taken = rec[layout::kCraftWorkTaken];
    w.dummy = rec[layout::kCraftWorkDummyEnd];
    w.end = rec[layout::kCraftWorkEnd];
    std::memcpy(w.user, rec + layout::kCraftWorkUser, sizeof w.user);
    return w;
}

bool same_job(const State::Work& a, const State::Work& b) {
    return a.recipe == b.recipe && a.count == b.count && a.made == b.made && a.taken == b.taken && a.dummy == b.dummy && a.end == b.end &&
           std::memcmp(a.user, b.user, sizeof a.user) == 0;
}

void watch_work(State& s, double t, bool session_start) {
    if (!session_start && t - s.work_at < kWorkEvery) return;
    s.work_at = t;
    const uint64_t r = guest::deref(base() + layout::kSaveRoot);
    if (!r) return;
    const bool visitor = (session_bits() & layout::kSessionVisitor) != 0;
    const uint64_t recs = r + (visitor ? layout::kCraftWorkHostCopy : layout::kCraftWorkOwn);
    s.work_block.resize(size_t{layout::kCharaCount} * layout::kCraftWorkSize);
    if (!guest::read(recs, s.work_block.data(), s.work_block.size())) return;
    const char* whose = visitor ? "the host's copy" : "own records";
    const bool first = recs != s.work_base;   // the session's start, or the visitor bit changed: list again
    s.work_base = recs;
    if (first) log_users(r);
    s.work.resize(layout::kCharaCount);
    int lines = 0, jobs = 0;
    for (uint32_t i = 0; i < layout::kCharaCount; ++i) {
        State::Work now = parse_work(s.work_block.data() + size_t{i} * layout::kCraftWorkSize);
        State::Work& was = s.work[i];
        if (first) {
            if (has_job(now) && ++jobs <= kWorkLinesMax)
                NXLOG_I(kTag, "Co-op trace: work at the start (%s): %s", whose, work_line(i, now).c_str());
            now.timer_since = t;
            was = now;
            continue;
        }
        const bool changed = !same_job(now, was);
        if (changed && (has_job(now) || has_job(was)) && lines++ < kWorkLinesMax)
            NXLOG_I(kTag, "Co-op trace: work %s", work_line(i, now).c_str());
        if (changed || now.timer != was.timer) {
            now.timer_since = t;
        } else {
            now.timer_since = was.timer_since;
            now.timer_logged = was.timer_logged;
            if (has_job(now) && now.timer > 0.0f && !now.timer_logged && t - now.timer_since >= kHeldAfter) {
                now.timer_logged = true;
                NXLOG_I(kTag, "Co-op trace: work timer of chara %u stands at %.1f for %.0f s (recipe %u, end %u, %s)", i, now.timer,
                        t - now.timer_since, now.recipe, now.end, whose);
            }
        }
        was = now;
    }
    if (first) NXLOG_I(kTag, "Co-op trace: work at the start: %d characters with a job (%s)", jobs, whose);
    if (lines > kWorkLinesMax) NXLOG_I(kTag, "Co-op trace: work: and %d more records changed", lines - kWorkLinesMax);
}

uint64_t event_queue() {
    const uint64_t ref = guest::rd<uint64_t>(base() + kEventQueueRef);
    if (!guest::ok(ref, 8)) return 0;
    const uint64_t q = guest::rd<uint64_t>(ref);
    return guest::ok(q, kQueueLocks + 3) ? q : 0;
}

bool record_started(uint64_t q, int slot, uint64_t rec) {
    const uint64_t n = q + slot * kSlotSize + kNodes;
    for (int i = 0; i < kRecords; ++i)
        if (guest::rd<uint64_t>(n + i * kNodeSize + kNodeRecord) == rec) return guest::rd<uint8_t>(n + i * kNodeSize + kNodeStarted) != 0;
    return false;
}

// Link Play's request counters stand unequal and still: logged once per stall, even with no held record.
void watch_requests(State& s, double t) {
    const uint64_t bm = guest::rd<uint64_t>(base() + layout::kWorld);
    if (!guest::ok(bm, 8) || !guest::ok(bm + layout::kWorldRequests, 4)) return;
    const uint16_t sent = guest::rd<uint16_t>(bm + layout::kWorldRequests), answered = guest::rd<uint16_t>(bm + layout::kWorldRequests + 2);
    if (sent == answered) {
        if (s.rq_logged) NXLOG_I(kTag, "Co-op trace: Link Play's request counters met again (%u)", sent);
        s.rq_since = 0;
        s.rq_logged = false;
        return;
    }
    if (s.rq_since == 0 || sent != s.rq_sent || answered != s.rq_answered) {
        s.rq_sent = sent;
        s.rq_answered = answered;
        s.rq_since = t;
        return;
    }
    if (s.rq_logged || t - s.rq_since < kHeldAfter) return;
    s.rq_logged = true;
    NXLOG_I(kTag, "Co-op trace: Link Play's request counters stand at %u sent / %u answered for %.0f s", sent, answered, t - s.rq_since);
}

// The stage-change request stays in a phase below 5 (0x45d5a0 drops new events then): logged once per phase and kind.
void watch_trip(State& s, double t) {
    const uint64_t gm = guest::rd<uint64_t>(base() + kGameState);
    if (!guest::ok(gm, kTripRequest + 8)) return;
    const uint32_t phase = guest::rd<uint32_t>(gm + kTripRequest), kind = guest::rd<uint32_t>(gm + kTripRequest + 4);
    if (phase != s.trip_phase || kind != s.trip_kind) {
        if (s.trip_logged) NXLOG_I(kTag, "Co-op trace: stage-change request now phase %u kind %u", phase, kind);
        s.trip_phase = phase;
        s.trip_kind = kind;
        s.trip_since = t;
        s.trip_logged = false;
        return;
    }
    if (phase >= 5 || s.trip_logged || t - s.trip_since < kHeldAfter) return;
    s.trip_logged = true;
    NXLOG_I(kTag, "Co-op trace: stage-change request in phase %u kind %u for %.0f s: new story events without flag bit 4 are dropped", phase,
            kind, t - s.trip_since);
}

std::string held_context(uint64_t q) {
    char line[256];
    const uint64_t bm = guest::rd<uint64_t>(base() + layout::kWorld);
    uint16_t rq[5] = {};
    const bool have_rq = guest::ok(bm, 8) && guest::ok(bm + layout::kWorldRequests, sizeof rq);
    if (have_rq) std::memcpy(rq, reinterpret_cast<const void*>(bm + layout::kWorldRequests), sizeof rq);
    const uint64_t mw = guest::rd<uint64_t>(base() + kModeWords);
    const unsigned mode = guest::ok(mw, kModeWord + 4) ? guest::rd<uint32_t>(mw + kModeWord) : 0xffffffffu;
    const uint64_t er_ref = guest::rd<uint64_t>(base() + kEventRunnerRef);
    const uint64_t er = guest::ok(er_ref, 8) ? guest::rd<uint64_t>(er_ref) : 0;
    const int hold = guest::ok(er, kRunnerHold + 1) ? guest::rd<uint8_t>(er + kRunnerHold) : -1;
    const uint64_t ph = guest::rd<uint64_t>(base() + kSlotPhases);
    unsigned phase[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    if (guest::ok(ph, kSlotPhaseSize + 0x10))
        for (int i = 0; i < 2; ++i) {
            phase[i * 2] = guest::rd<uint32_t>(ph + i * kSlotPhaseSize + 8);
            phase[i * 2 + 1] = guest::rd<uint32_t>(ph + i * kSlotPhaseSize + 0xc);
        }
    const uint64_t sh_ref = guest::rd<uint64_t>(base() + kSceneHolderRef);
    const uint64_t sh = guest::ok(sh_ref, 8) ? guest::rd<uint64_t>(sh_ref) : 0;
    const int scene = guest::ok(sh, kSceneHeld + 8) ? (guest::rd<uint64_t>(sh + kSceneHeld) != 0) : -1;
    const uint64_t gm = guest::rd<uint64_t>(base() + kGameState);
    const bool trip = guest::ok(gm, kTripRequest + 8);
    char more[128];
    std::snprintf(more, sizeof more, ", slot phases %u/%u %u/%u, scene held %d, trip request phase %d kind %d", phase[0], phase[1],
                  phase[2], phase[3], scene, trip ? static_cast<int>(guest::rd<uint32_t>(gm + kTripRequest)) : -1,
                  trip ? static_cast<int>(guest::rd<uint32_t>(gm + kTripRequest + 4)) : -1);
    if (have_rq)
        std::snprintf(line, sizeof line, "session bits 0x%x, requests %u/%u (%u %u %u)%s, mode word %u, locks %u %u %u, runner hold %d",
                      static_cast<unsigned>(session_bits()), rq[0], rq[1], rq[2], rq[3], rq[4], rq[0] != rq[1] ? " UNEQUAL" : "", mode,
                      guest::rd<uint8_t>(q + kQueueLocks), guest::rd<uint8_t>(q + kQueueLocks + 1), guest::rd<uint8_t>(q + kQueueLocks + 2), hold);
    else
        std::snprintf(line, sizeof line, "session bits 0x%x, no block map, mode word %u, locks %u %u %u, runner hold %d",
                      static_cast<unsigned>(session_bits()), mode, guest::rd<uint8_t>(q + kQueueLocks),
                      guest::rd<uint8_t>(q + kQueueLocks + 1), guest::rd<uint8_t>(q + kQueueLocks + 2), hold);
    return std::string(line) + more;
}

void watch_held(State& s, double t) {
    if (t - s.held_at < kHeldEvery) return;
    s.held_at = t;
    watch_requests(s, t);
    watch_trip(s, t);
    const uint64_t q = event_queue();
    if (!q || !guest::ok(q, kQueueLocks + 3)) return;
    bool any_logged = false;
    for (int slot = 0; slot < kSlots; ++slot) {
        for (int i = 0; i < kRecords; ++i) {
            const uint64_t r = q + slot * kSlotSize + i * kRecordSize;
            State::Held now;
            now.id = guest::rd<uint16_t>(r + kRecId);
            now.kind = guest::rd<uint32_t>(r + kRecKind);
            const bool taken = static_cast<uint16_t>(now.id - 1) < 0x18ff || now.kind < 0xd1;
            State::Held& h = s.held[slot][i];
            if (!taken || record_started(q, slot, r)) {
                if (h.logged)
                    NXLOG_I(kTag, "Co-op trace: held event %u (slot %d record %d) %s after %.0f s", h.id, slot, i,
                            taken ? "started" : "left the queue", t - h.since);
                h = State::Held{};
                continue;
            }
            now.mode = guest::rd<uint32_t>(r + kRecMode);
            now.flags = guest::rd<uint32_t>(r + kRecFlags) & 0xffffffu;
            if (h.since == 0 || h.id != now.id || h.kind != now.kind) {
                now.since = t;
                h = now;
                continue;
            }
            const bool changed = h.mode != now.mode || h.flags != now.flags;
            h.mode = now.mode;
            h.flags = now.flags;
            if (t - h.since < kHeldAfter || (h.logged && !changed)) continue;
            h.logged = true;
            any_logged = true;
            NXLOG_I(kTag, "Co-op trace: held event %u (slot %d record %d, kind %u) waiting %.0f s: mode 0x%x, flags 0x%06x%s", h.id, slot,
                    i, h.kind, t - h.since, h.mode, h.flags,
                    (h.flags >> 22 & 1) ? " (bit 22: waits for the request counters on a visitor)" : "");
        }
    }
    bool any_held = false;
    for (const auto& row : s.held)
        for (const auto& h : row) any_held = any_held || h.logged;
    if (!any_held) return;
    std::string ctx = held_context(q);
    if (any_logged || ctx != s.held_context) {
        NXLOG_I(kTag, "Co-op trace: held events context: %s", ctx.c_str());
        s.held_context = std::move(ctx);
    }
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
    watch_held(s, t);
    watch_work(s, t, first);
}

void reset() { st() = State{}; }

}  // namespace nxrt::games::pkp::coop::trace
