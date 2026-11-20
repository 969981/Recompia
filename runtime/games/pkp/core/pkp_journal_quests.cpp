// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal's quests watcher (pkp_journal_watch.h): every quest state and flag write the game attempts (with its caller),
// what the word became the next frame, the quest lists the pump steps, the story lines, the step effects and the step actions.
//
// m_story S = save root R + 0x4a26a0. Per quest id a u16 word: low byte the state (1 listed, 2 started, 3 complete, 4 closed), +1 bit 0
// the flag ("the next step's condition is met"). Ids 1..800 at S+0x1060+id*2, 801..1000 at S+0x1860+(id-800)*2 (0x5d865c / 0x5d8774
// through 0x11e884); co-op reads the same words at R + layout::quest_word(id), and the state line says MISMATCH when the two differ.
// The pump 0x11b600 steps only quests in its lists QL = GM+0x3b9328 (C state 1, A state 2, B state 3) and only on their flag.
//
// Sites (all event rate except 0x11b6a4, which is deduped per quest):
//   0x5d865c (S, id, state)  state setter. The setter writes the byte only for id 1..1000 and state 1..4; the next frame's look says
//                            what the word became (expect = the state asked). Its three callers are the pump's steps.
//   0x5d8774 (S, id, value)  flag setter, the only way a quest moves on. On a co-op member its hook returns 1 (the chain stops there),
//                            so this line is the attempt; co-op's decision (asked / skipped / set) is its own line in
//                            pkp_coop_quests.cpp. The C / B passes call it every frame while a condition holds: deduped per quest
//                            on (caller, value, word), repeats counted and written every kRepeatEvery seconds at most.
//   0x11b214 (cmd)           step effect runner: x0 is only the 0x14-byte command (u8 kind +0, u16 args +2..). The quest is the
//                            caller's: from 0x11c0a4's four calls (returns 0x11c124 / 0x11c16c / 0x11c1b4 / 0x11c1fc = slot 0..3) w19
//                            is the quest id and w20 the set (0 completion, 1 close), callee-saved at the call (0x11c0d0 / 0x11c0d8,
//                            not written again before the bl). 0xb4b60's three re-runs at the stage play start hold no quest there.
//   0x11b6a4 (mid)           the pump's list A loop, `tbz w0, #0` after the flag read 0x5d86e4: w22 the quest id, w0 bit 0 its flag
//                            (set: complete 0x11bd3c). The list B loop's own test (0x11b790) is not a hook site.
//   0x10afc0 (entry)         step actions of a story line: x0 the 3-byte bus entry, +0 the line b; its step is the record's +0xa.
//                            The action kinds and nodes are per node inside the loop (node +0x16 at 0x10b0ec, the call at 0x10b098):
//                            not readable at entry, left out.
//   0x10d260 (bus, scene)    the story bus told "scene ended" (runner state 4, 0x47e140): which lines sit on the bus, at which step.
//   0x11c220 (QL, id)        a closed quest's follow-ups put into list C (the pump's list B close, or co-op's list_quest).
// Polls (game_frame, compare and write only what changed): the quest words, the three lists (ids only: the A timers move every
// frame), the story lines R+0x4a4410+b*0x10 (+8 id, +0xa step, +0xc bits). The whole table (every quest past state 0 with its state,
// flag and list) is written at the first look of a save and again when the save root moves or many words change at once (a load);
// not again while it is the snapshot last written.
#include "core/pkp_journal.h"
#include "core/pkp_journal_watch.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "nxrt/qpc.h"

namespace nxrt::games::pkp::journal {

namespace {

constexpr Area kQ = Area::Quest;

constexpr uint32_t kQuestMax = layout::kQuestWordCount, kBaseCount = layout::kQuestBaseCount;
constexpr uint64_t kStoryBaseRun = 0x1060, kStoryDlcRun = 0x1860;   // m_story + : 0x5d865c's two runs (id * 2, (id - 800) * 2)
constexpr uint16_t kFlagBit = 0x100;                                  // the word's flag (byte +1 bit 0, 0x5d8774)
constexpr uint8_t kStateMax = 4;                                      // the setter writes states 1..4 only (0x5d8680..0x5d8688)

// Quest lists QL = GM + 0x3b9328 (copied from pkp_coop_story.cpp's anonymous namespace: kGmQuestLists, kList*, kListAEntry).
constexpr uint64_t kGmQuestLists = 0x3b9328;
constexpr uint64_t kListC = 0x0, kListCIds = 0x4, kListA = 0x60, kListAEntries = 0x64, kListAEntry = 8, kListB = 0x1b8, kListBIds = 0x1bc;
constexpr uint64_t kListsEnd = 0x258;
constexpr uint32_t kListCap = 0x28;

// Story lines (EVENT-END-RE.md "The story lines"): records b = 1..0xfe of 0x10 bytes at R + 0x4a4410 + b * 0x10.
constexpr uint64_t kLines = 0x4a4410, kLineSize = 0x10, kLineId = 8, kLineStep = 0xa, kLineBits = 0xc;
constexpr uint32_t kLineCount = 0xff;   // index 0 unused
// The story bus (GM + 0xd0, 0x10d260's x0): 3-byte entries at +1 + k * 3 up to +0x2fe, +0 the line, +2 busy.
constexpr uint64_t kBusFirst = 1, kBusEnd = 0x2fe, kBusEntry = 3;

constexpr uint32_t kLoadBurst = 16;     // more quest words changed in one frame than this: a load, written as the whole table
constexpr double kRepeatEvery = 30.0;   // a repeated flag attempt: its count written at most this often
constexpr size_t kPendingMax = 64;
constexpr uint32_t kPumpRepeatFrames = 1800;   // about kRepeatEvery at 60 fps

struct Caller {
    uint64_t ret;   // return address, main +
    const char* what;
};
// 0x5d865c's callers (pkp_coop_story.cpp kPumpStateWrites).
constexpr Caller kStateCallers[] = {
    {0x11bcc0, "start (0x11bc60, state 2)"},
    {0x11bda8, "complete (0x11bd3c, state 3)"},
    {0x11c00c, "close (0x11bf9c, state 4: the pump's list B step or effect 0x15)"},
};
// 0x5d8774's callers (PR #805's table, pkp_coop_quests.cpp's header).
constexpr Caller kFlagCallers[] = {
    {0x119c0c, "C pass: start condition holds (0x119920, state 1)"},
    {0x119d94, "A pass: the list A timer passed (state 2)"},
    {0x119f38, "B pass: close condition holds (state 3)"},
    {0x11d644, "event-end match (0x11d4a0: talk / scene end)"},
    {0x11d744, "event-end match (0x11d4a0: talk / scene end)"},
    {0x115ae0, "list A quest tied to a request (0x11598c)"},
    {0x1fbad8, "new day (0x1fb880)"},
    {0x11bcd4, "clear: the pump's start step (0x11bc60)"},
    {0x11bdbc, "clear: the pump's complete step (0x11bd3c)"},
    {0x11c024, "clear: the close step (0x11bf9c)"},
};
// 0x11b214's callers: 0x11c0a4's slots, 0xb4b60's re-runs (P0-RE.md: quest 523 slots 2 / 1, quest 234 slot 0, set 1).
constexpr uint64_t kEffectSlots[] = {0x11c124, 0x11c16c, 0x11c1b4, 0x11c1fc};
constexpr Caller kEffectReruns[] = {
    {0xb4ca8, "stage-start re-run (0xb4b60: quest 523 slot 2)"},
    {0xb4d64, "stage-start re-run (0xb4b60: quest 523 slot 1)"},
    {0xb4e40, "stage-start re-run (0xb4b60: quest 234 slot 0)"},
};
constexpr Caller kActionCallers[] = {
    {0x10ab50, "line start (0x10aaa8, the stage start)"},
    {0x10b77c, "step advance (0x10b580)"},
};
constexpr Caller kBusCallers[] = {{0x47e1f8, "event end (runner state 4, 0x47e140)"}};
constexpr Caller kFollowUpCallers[] = {{0x11b7b8, "the pump's list B close (0x11b600)"}};
constexpr const char* kNotGame = "not a game caller (co-op's own call, or unknown)";

template <size_t N>
const char* caller_name(const Caller (&t)[N], uint64_t from) {
    for (const Caller& k : t)
        if (k.ret == from) return k.what;
    return kNotGame;
}

struct Lists {
    bool ok = false;
    uint32_t n[3] = {};   // C, A, B
    uint16_t ids[3][kListCap] = {};
};

struct Pending {   // a state write to confirm on the next frame
    uint16_t id;
    uint8_t want;
    uint16_t before;
    uint64_t from;
    uint64_t addr;   // the word's address, as the setter computed it
};

struct FlagSeen {   // the dedupe of a quest's flag attempts
    uint64_t from = ~0ull;
    uint8_t value = 0xff;
    uint16_t word = 0;
    uint32_t repeats = 0;
    double written_at = 0.0;
};

struct Line {
    uint8_t id;
    int16_t step;
    uint16_t bits;
    bool operator==(const Line& o) const { return id == o.id && step == o.step && bits == o.bits; }
};

struct State {
    std::mutex m;
    // polls
    uint64_t root = 0;
    bool have_words = false;
    uint16_t words[kQuestMax] = {};
    bool have_dump = false;
    uint16_t dumped[kQuestMax] = {};
    Lists lists;
    bool have_lines = false;
    Line lines[kLineCount] = {};
    // hooks
    std::vector<Pending> pending;
    FlagSeen flag[kQuestMax];
    uint64_t set_from[kQuestMax] = {};    // since the last poll: the last set / clear attempt's caller + 1 (0: none), for the applied line
    uint64_t clear_from[kQuestMax] = {};
    uint8_t pump_flag[kQuestMax] = {};    // 0x11b6a4: the flag last seen (0xff: not yet), frames seen set
    uint32_t pump_frames[kQuestMax] = {};
};
State& st() {
    static State* s = new State;   // the hooks may run until the registry removes them
    return *s;
}

bool quest_id(uint32_t id) { return id - 1u < kQuestMax; }

uint64_t save_root() {
    const uint64_t b = base();
    if (!b) return 0;
    const uint64_t r = guest::rd<uint64_t>(b + layout::kSaveRoot);
    return guest::ok(r, layout::kQuestWordsEnd) ? r : 0;
}

uint64_t quest_lists() {
    const uint64_t b = base();
    if (!b) return 0;
    const uint64_t gm = guest::rd<uint64_t>(b + layout::kGameState);
    return guest::ok(gm, kGmQuestLists + kListsEnd) ? gm + kGmQuestLists : 0;
}

bool read_words(uint64_t r, uint16_t (&out)[kQuestMax]) {
    if (!r) return false;
    for (uint32_t i = 0; i < kQuestMax; ++i) out[i] = guest::rd<uint16_t>(r + layout::quest_word(i + 1));
    return true;
}

Lists read_lists() {
    Lists l;
    const uint64_t ql = quest_lists();
    if (!ql) return l;
    const uint64_t count[3] = {kListC, kListA, kListB}, first[3] = {kListCIds, kListAEntries, kListBIds}, size[3] = {2, kListAEntry, 2};
    for (int k = 0; k < 3; ++k) {
        uint32_t n = guest::rd<uint32_t>(ql + count[k]);
        if (n > kListCap) n = kListCap;
        l.n[k] = n;
        for (uint32_t j = 0; j < n; ++j) l.ids[k][j] = guest::rd<uint16_t>(ql + first[k] + j * size[k]);
    }
    l.ok = true;
    return l;
}

bool in_list(const Lists& l, int k, uint16_t id) {
    for (uint32_t j = 0; j < l.n[k]; ++j)
        if (l.ids[k][j] == id) return true;
    return false;
}

// "C", "A", "B" (or several), "-" in none, "?" when the lists are not readable.
std::string membership(const Lists& l, uint16_t id) {
    if (!l.ok) return "?";
    std::string s;
    const char names[3] = {'C', 'A', 'B'};
    for (int k = 0; k < 3; ++k)
        if (in_list(l, k, id)) s += names[k];
    return s.empty() ? "-" : s;
}

// Quest id's word now, where the game keeps it (0 when the save is not readable).
uint16_t live_word(uint16_t id) {
    const uint64_t r = save_root();
    return r && quest_id(id) ? guest::rd<uint16_t>(r + layout::quest_word(id)) : 0;
}

std::string word_text(uint16_t w) { return fmt("0x%04x (state %u%s)", w, w & 0xff, (w & kFlagBit) ? ", flag" : ""); }

uint64_t from_of(::Ctx* c) { return c->x[30] - base(); }

Line read_line(uint64_t r, uint32_t b) {
    const uint64_t a = r + kLines + b * kLineSize;
    return {guest::rd<uint8_t>(a + kLineId), guest::rd<int16_t>(a + kLineStep), guest::rd<uint16_t>(a + kLineBits)};
}
bool line_valid(const Line& l) { return static_cast<uint8_t>(l.id - 1) < 0xfe && l.step >= 0; }
bool lines_readable(uint64_t r) { return r && guest::ok(r + kLines, kLineCount * kLineSize); }

// ---- the whole table ----

void dump(State& s, uint64_t r, const char* why) {
    std::vector<std::string> items;
    unsigned per[kStateMax + 2] = {};
    for (uint32_t i = 0; i < kQuestMax; ++i) {
        const uint16_t w = s.words[i];
        const uint8_t state = w & 0xff;
        if (!state && !(w & kFlagBit)) continue;
        ++per[state <= kStateMax ? state : kStateMax + 1];
        const std::string in = s.lists.ok ? membership(s.lists, static_cast<uint16_t>(i + 1)) : "-";
        items.push_back(fmt("%u:%u%s%s%s", i + 1, state, (w & kFlagBit) ? "f" : "", in == "-" ? "" : "/", in == "-" ? "" : in.c_str()));
    }
    Rec(kQ, "quest-table")
        .in("%s; save root moved=%s", why, r == s.root ? "no" : "yes")
        .out("%zu quests past state 0: state1=%u state2=%u state3=%u state4=%u other=%u (+f flag set, /C /A /B its list)", items.size(),
             per[1], per[2], per[3], per[4], per[kStateMax + 1])
        .step();
    list(kQ, "quest-table", items);
    if (s.lists.ok) {
        std::string ls[3];
        for (int k = 0; k < 3; ++k)
            for (uint32_t j = 0; j < s.lists.n[k]; ++j) ls[k] += fmt(j ? ",%u" : "%u", s.lists.ids[k][j]);
        Rec(kQ, "quest-lists").out("C(state 1)=[%s] A(state 2)=[%s] B(state 3)=[%s]", ls[0].c_str(), ls[1].c_str(), ls[2].c_str());
    }
    if (lines_readable(r)) {
        std::vector<std::string> ln;
        for (uint32_t b = 1; b < kLineCount; ++b) {
            const Line l = read_line(r, b);
            if (line_valid(l)) ln.push_back(fmt("%u:step%d:0x%04x", b, l.step, l.bits));
        }
        list(kQ, "story-lines (record:step:bits)", ln);
        for (uint32_t b = 1; b < kLineCount; ++b) s.lines[b] = read_line(r, b);   // the poll's baseline: no line-step for these
        s.have_lines = true;
    }
    std::memcpy(s.dumped, s.words, sizeof s.dumped);
    s.have_dump = true;
}

// ---- polls ----

// The state writes the setter made last frame: what the word is now.
void confirm(State& s, uint64_t r, std::vector<uint16_t>& confirmed) {
    for (const Pending& p : s.pending) {
        const uint16_t now = guest::ok(p.addr, 2) ? guest::rd<uint16_t>(p.addr) : 0;
        const bool same_addr = r && p.addr == r + layout::quest_word(p.id);
        Rec(kQ, "state-applied")
            .in("id=%u asked=%u before=%s by=%s main+0x%llx", p.id, p.want, word_text(p.before).c_str(),
                caller_name(kStateCallers, p.from), static_cast<unsigned long long>(p.from))
            .out("%s lists=%s%s", word_text(now).c_str(), membership(s.lists, p.id).c_str(), same_addr ? "" : " (save root moved since)")
            .expect("state %u", p.want)
            .ok((now & 0xff) == p.want);
        confirmed.push_back(p.id);
    }
    s.pending.clear();
}

void poll_words(State& s, uint64_t r, const std::vector<uint16_t>& confirmed) {
    uint16_t now[kQuestMax];
    if (!read_words(r, now)) return;
    bool any = false;
    for (uint32_t i = 0; i < kQuestMax && !any; ++i) any = now[i] != 0;
    if (!s.have_words || r != s.root) {
        std::memcpy(s.words, now, sizeof now);
        s.have_words = true;
        if (any && (!s.have_dump || std::memcmp(s.dumped, now, sizeof now) != 0)) dump(s, r, "first look of this save");
        s.root = r;
        return;
    }
    uint32_t changed = 0;
    bool was_any = false;
    for (uint32_t i = 0; i < kQuestMax; ++i) {
        changed += now[i] != s.words[i];
        was_any |= s.words[i] != 0;
    }
    if (!changed) return;
    if (!was_any) {   // an empty table filled: a save loaded (or a new game's first quests)
        std::memcpy(s.words, now, sizeof now);
        if (std::memcmp(s.dumped, now, sizeof now) != 0) dump(s, r, "the quest table filled (a save loaded)");
        return;
    }
    if (changed > kLoadBurst) {
        std::memcpy(s.words, now, sizeof now);
        if (std::memcmp(s.dumped, now, sizeof now) != 0) dump(s, r, fmt("%u quest words changed in one frame (a load)", changed).c_str());
        return;
    }
    for (uint32_t i = 0; i < kQuestMax; ++i) {
        const uint16_t was = s.words[i], is = now[i];
        if (was == is) continue;
        const uint16_t id = static_cast<uint16_t>(i + 1);
        bool by_setter = false;
        for (uint16_t c : confirmed) by_setter |= c == id;
        if ((was & 0xff) != (is & 0xff) && !by_setter) {
            Rec(kQ, "state-changed")
                .in("id=%u", id)
                .out("%s -> %s lists=%s", word_text(was).c_str(), word_text(is).c_str(), membership(s.lists, id).c_str())
                .note("no 0x5d865c call seen for it: another writer, or a load (state 1 is only ever set this way)");
        }
        if ((was ^ is) & kFlagBit) {
            const bool on = is & kFlagBit;
            // An attempt that did not apply (co-op skipped it) shows as a flag-write with no flag-applied, and co-op's flag-decision.
            const uint64_t by = on ? s.set_from[i] : s.clear_from[i];
            Rec r2(kQ, "flag-applied");
            r2.in("id=%u", id).out("flag %s, word %s -> %s lists=%s", on ? "set" : "cleared", word_text(was).c_str(), word_text(is).c_str(),
                                   membership(s.lists, id).c_str());
            if (by) r2.note("by %s main+0x%llx", caller_name(kFlagCallers, by - 1), static_cast<unsigned long long>(by - 1));
            else r2.note("no matching flag setter call seen since the last look");
        }
    }
    std::memcpy(s.words, now, sizeof now);
}

void forget_attempts(State& s) {
    std::memset(s.set_from, 0, sizeof s.set_from);
    std::memset(s.clear_from, 0, sizeof s.clear_from);
}

void poll_lists(State& s) {
    const Lists now = read_lists();
    if (!now.ok) return;
    if (!s.lists.ok) {
        s.lists = now;
        return;
    }
    const char* names[3] = {"list-C (state 1)", "list-A (state 2)", "list-B (state 3)"};
    for (int k = 0; k < 3; ++k) {
        std::string gained, lost;
        for (uint32_t j = 0; j < now.n[k]; ++j) {
            const uint16_t id = now.ids[k][j];
            if (in_list(s.lists, k, id)) continue;
            const uint8_t state = live_word(id) & 0xff;
            gained += fmt("%s%u(state %u)", gained.empty() ? "" : " ", id, state);
        }
        for (uint32_t j = 0; j < s.lists.n[k]; ++j) {
            const uint16_t id = s.lists.ids[k][j];
            if (in_list(now, k, id)) continue;
            const uint8_t state = live_word(id) & 0xff;
            lost += fmt("%s%u(state %u, now in %s)", lost.empty() ? "" : " ", id, state, membership(now, id).c_str());
        }
        if (gained.empty() && lost.empty()) continue;
        Rec(kQ, names[k]).out("gained [%s] lost [%s] count %u -> %u", gained.c_str(), lost.c_str(), s.lists.n[k], now.n[k]);
    }
    s.lists = now;
}

void poll_lines(State& s, uint64_t r) {
    if (!lines_readable(r)) return;
    Line now[kLineCount];
    for (uint32_t b = 1; b < kLineCount; ++b) now[b] = read_line(r, b);
    now[0] = {};
    if (!s.have_lines) {
        std::memcpy(s.lines, now, sizeof now);
        s.have_lines = true;
        return;
    }
    for (uint32_t b = 1; b < kLineCount; ++b) {
        const Line& was = s.lines[b];
        const Line& is = now[b];
        if (was == is) continue;
        const bool stepped = was.step != is.step || was.id != is.id;
        Rec(kQ, stepped ? "line-step" : "line-bits")
            .in("record=%u", b)
            .out("id %u -> %u, step %d -> %d, condition bits 0x%04x -> 0x%04x", was.id, is.id, was.step, is.step, was.bits, is.bits);
    }
    std::memcpy(s.lines, now, sizeof now);
}

// Once a frame on the game thread: last frame's state writes confirmed, then the lists, the words and the story lines compared.
void game_frame(::Ctx*, bool) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    const uint64_t r = save_root();
    std::vector<uint16_t> confirmed;
    poll_lists(s);   // first: the confirm and word lines name the lists as they are now
    confirm(s, r, confirmed);
    if (!r) return;
    poll_words(s, r, confirmed);
    forget_attempts(s);
    poll_lines(s, r);
}

// ---- hooks ----

void on_state(::Ctx* c) {
    const uint64_t story = c->x[0], from = from_of(c);
    const uint32_t id = static_cast<uint32_t>(c->x[1]) & 0xffff;
    const uint8_t want = static_cast<uint8_t>(c->x[2]);
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    Rec rec(kQ, "state-write");
    if (!quest_id(id)) {
        rec.in("id=%u state=%u by=%s main+0x%llx", id, want, caller_name(kStateCallers, from), static_cast<unsigned long long>(from))
            .refused("id outside 1..%u: the setter writes nothing", kQuestMax);
        return;
    }
    const uint64_t game = story + (id <= kBaseCount ? kStoryBaseRun + id * 2ull : kStoryDlcRun + (id - kBaseCount) * 2ull);
    const uint64_t root = save_root();
    const uint64_t ours = root ? root + layout::quest_word(id) : 0;
    const uint16_t word = guest::ok(game, 2) ? guest::rd<uint16_t>(game) : 0;
    const Lists l = read_lists();
    rec.in("id=%u state=%u word=%s at S+0x%llx by=%s main+0x%llx lists=%s", id, want, word_text(word).c_str(),
           static_cast<unsigned long long>(game - story), caller_name(kStateCallers, from), static_cast<unsigned long long>(from),
           membership(l, static_cast<uint16_t>(id)).c_str());
    if (!ours) {
        rec.note("save root not readable: co-op's address not compared");
    } else if (ours != game) {
        const uint16_t our_word = guest::ok(ours, 2) ? guest::rd<uint16_t>(ours) : 0;
        rec.out("co-op's layout::quest_word reads R+0x%llx = 0x%04x", static_cast<unsigned long long>(ours - root), our_word)
            .expect("the game's word 0x%04x at R+0x%llx", word, static_cast<unsigned long long>(game - root))
            .ok(false);
    }
    if (want - 1u >= kStateMax) {
        rec.refused("state outside 1..%u: the setter writes nothing", kStateMax);
        return;
    }
    if (!guest::ok(game, 2)) return;
    for (Pending& p : s.pending)
        if (p.id == id) {   // a second write in the frame: the last one is the one to confirm
            p.want = want;
            p.from = from;
            return;
        }
    if (s.pending.size() < kPendingMax) s.pending.push_back({static_cast<uint16_t>(id), want, word, from, game});
}

void on_flag(::Ctx* c) {
    const uint64_t story = c->x[0], from = from_of(c);
    const uint32_t id = static_cast<uint32_t>(c->x[1]) & 0xffff;
    const uint8_t value = c->x[2] & 1;   // the setter tests bit 0 (0x5d87d4)
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    if (!quest_id(id)) {
        Rec(kQ, "flag-write")
            .in("id=%u value=%u by=%s main+0x%llx", id, value, caller_name(kFlagCallers, from), static_cast<unsigned long long>(from))
            .refused("id outside 1..%u: the setter writes nothing", kQuestMax);
        return;
    }
    const uint32_t i = id - 1;
    const uint64_t game = story + (id <= kBaseCount ? kStoryBaseRun + id * 2ull : kStoryDlcRun + (id - kBaseCount) * 2ull);
    const uint16_t word = guest::ok(game, 2) ? guest::rd<uint16_t>(game) : 0;
    (value ? s.set_from[i] : s.clear_from[i]) = from + 1;
    FlagSeen& f = s.flag[i];
    const double now = qpc::seconds();
    if (f.from == from && f.value == value && f.word == word) {
        ++f.repeats;
        if (now - f.written_at < kRepeatEvery) return;
        Rec(kQ, "flag-write")
            .in("id=%u value=%u word=%s by=%s main+0x%llx", id, value, word_text(word).c_str(), caller_name(kFlagCallers, from),
                static_cast<unsigned long long>(from))
            .note("asked again %u times in %.0f s, the word unchanged", f.repeats, now - f.written_at);
        f.repeats = 0;
        f.written_at = now;
        return;
    }
    const uint32_t before = f.repeats;
    f = {from, value, word, 0, now};
    const Lists l = read_lists();
    Rec rec(kQ, "flag-write");
    rec.in("id=%u value=%u word=%s by=%s main+0x%llx lists=%s", id, value, word_text(word).c_str(), caller_name(kFlagCallers, from),
           static_cast<unsigned long long>(from), membership(l, static_cast<uint16_t>(id)).c_str());
    if (before) rec.note("the previous attempt was repeated %u times", before);
}

void on_effect(::Ctx* c) {
    const uint64_t cmd = c->x[0], from = from_of(c);
    if (!guest::ok(cmd, 0x14)) return;
    uint8_t raw[0x14];
    std::memcpy(raw, reinterpret_cast<const void*>(cmd), sizeof raw);
    Rec rec(kQ, "effect");
    int slot = -1;
    for (int k = 0; k < 4; ++k)
        if (kEffectSlots[k] == from) slot = k;
    if (slot >= 0) {
        const uint32_t which = static_cast<uint32_t>(c->x[20]) & 0xff;
        rec.in("kind=0x%02x quest=%u set=%u (%s) slot=%d cmd=[%s] by=0x11c0a4 main+0x%llx", raw[0], static_cast<uint32_t>(c->x[19]) & 0xffff,
               which, which == 0 ? "completion" : which == 1 ? "close" : "?", slot, hex_bytes(raw, sizeof raw).c_str(),
               static_cast<unsigned long long>(from));
    } else {
        rec.in("kind=0x%02x cmd=[%s] by=%s main+0x%llx", raw[0], hex_bytes(raw, sizeof raw).c_str(), caller_name(kEffectReruns, from),
               static_cast<unsigned long long>(from));
    }
    if (raw[0] - 1u >= 0x39) rec.out("no handler for this kind: nothing done");
    else rec.note("before co-op's decision (its own line follows in a session)");
}

void on_pump_flag(::Ctx* c) {
    const uint32_t id = static_cast<uint32_t>(c->x[22]) & 0xffff;
    if (!quest_id(id)) return;
    const uint8_t flag = c->x[0] & 1;
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    const uint32_t i = id - 1;
    uint8_t& last = s.pump_flag[i];
    if (flag && last == 1) {
        if (++s.pump_frames[i] % kPumpRepeatFrames == 0)   // a flag the pump reads set frame after frame (it does not step it)
            Rec(kQ, "pump-flag").in("id=%u flag=1", id).note("read set by the pump for %u frames", s.pump_frames[i]);
        return;
    }
    if (flag) {
        const Lists l = read_lists();
        Rec(kQ, "pump-flag")
            .in("id=%u flag=1 word=%s lists=%s", id, word_text(live_word(static_cast<uint16_t>(id))).c_str(),
                membership(l, static_cast<uint16_t>(id)).c_str())
            .out("the pump's list A loop completes it (0x11bd3c)");
        s.pump_frames[i] = 1;
    } else if (last == 1 && s.pump_frames[i] > 1) {
        Rec(kQ, "pump-flag").in("id=%u flag=0", id).note("was read set for %u frames", s.pump_frames[i]);
    }
    last = flag;
}

void on_line_actions(::Ctx* c) {
    const uint64_t entry = c->x[0], from = from_of(c);
    const uint64_t r = save_root();
    if (!guest::ok(entry, 3)) return;
    const uint8_t b = guest::rd<uint8_t>(entry);
    Rec rec(kQ, "line-actions");
    rec.in("line=%u entry=[%s] by=%s main+0x%llx", b, hex_bytes(reinterpret_cast<const void*>(entry), 3).c_str(), caller_name(kActionCallers, from),
           static_cast<unsigned long long>(from));
    if (lines_readable(r) && b - 1u < 0xfeu) {
        const Line l = read_line(r, b);
        rec.out("record id %u step %d condition bits 0x%04x: the step's actions run", l.id, l.step, l.bits);
    } else {
        rec.out("no valid record: the game runs the nodes of step -1");
    }
    rec.step();
}

void on_bus_scene(::Ctx* c) {
    const uint64_t bus = c->x[0], from = from_of(c);
    const uint32_t scene = static_cast<uint32_t>(c->x[1]) & 0xffff;
    const uint64_t r = save_root();
    Rec rec(kQ, "bus-scene");
    rec.in("scene=%u by=%s main+0x%llx", scene, caller_name(kBusCallers, from), static_cast<unsigned long long>(from));
    if (scene - 1u > 0xbb6u) {
        rec.refused("scene outside 1..0xbb7: the bus does nothing");
        return;
    }
    if (!guest::ok(bus, kBusEnd) || !lines_readable(r)) return;
    std::string on;
    unsigned n = 0;
    for (uint64_t o = kBusFirst; o < kBusEnd; o += kBusEntry) {
        const uint8_t b = guest::rd<uint8_t>(bus + o);
        if (b - 1u > 0xfdu) continue;   // 0x10d260's own test (sub 1, cmp #0xfd, b.hi)
        const Line l = read_line(r, b);
        if (!line_valid(l)) continue;
        if (n++ < 24) on += fmt("%s%u:step%d:0x%04x%s", on.empty() ? "" : " ", b, l.step, l.bits, guest::rd<uint8_t>(bus + o + 2) ? "(busy)" : "");
    }
    rec.out("%u lines on the bus (line:step:bits) [%s%s]; their steps follow as line-step lines", n, on.c_str(), n > 24 ? " .." : "");
}

void on_follow_ups(::Ctx* c) {
    const uint64_t from = from_of(c);
    const uint32_t id = static_cast<uint32_t>(c->x[1]) & 0xffff;
    const Lists l = read_lists();
    Rec(kQ, "follow-ups")
        .in("closed=%u by=%s main+0x%llx list C count=%u", id, caller_name(kFollowUpCallers, from), static_cast<unsigned long long>(from),
            l.n[0])
        .note("the ready follow-ups go into list C (a list-C line follows when one does)")
        .step();
}

void reset() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    s.root = 0;
    s.have_words = s.have_dump = s.have_lines = false;
    s.lists = Lists{};
    s.pending.clear();
    for (FlagSeen& f : s.flag) f = FlagSeen{};
    forget_attempts(s);
    std::memset(s.pump_flag, 0xff, sizeof s.pump_flag);
    std::memset(s.pump_frames, 0, sizeof s.pump_frames);
}

constexpr WatchSite kSites[] = {
    {{0x5d865c, 0x51000428u, true}, &on_state, "quest state setter 0x5d865c"},        // sub w8, w1, #1
    {{0x5d8774, 0x51000428u, true}, &on_flag, "quest flag setter 0x5d8774"},          // sub w8, w1, #1
    {{0x11b214, 0x39400008u, true}, &on_effect, "step effect runner 0x11b214"},       // ldrb w8, [x0]
    {{0x11b6a4, 0x3607fde0u, false}, &on_pump_flag, "pump flag test 0x11b6a4"},       // tbz w0, #0, 0x11b660
    {{0x10afc0, kPacibsp, true}, &on_line_actions, "step action dispatcher 0x10afc0"},
    {{0x10d260, kPacibsp, true}, &on_bus_scene, "story bus scene end 0x10d260"},
    {{0x11c220, kPacibsp, true}, &on_follow_ups, "follow-up listing 0x11c220"},
};

}  // namespace

const Watcher kQuestsWatcher = {"quests", kSites, sizeof kSites / sizeof kSites[0], &game_frame, &reset};

}  // namespace nxrt::games::pkp::journal
