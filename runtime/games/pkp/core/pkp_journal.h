// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: the journal, pkp.log's record of what the game was asked to do, what it did and what it should have done. A bug report
// (co-op v2 above all: one player gets a different answer, a step crashes, a shared unlock reaches 3 of 4 players) needs the cause in the
// log, not in a new reverse-engineering run. Every module that sees a game decision writes it here, in one shape:
//
//   [  12.345678 I t1234 main] j.quest: state-write n2 member | in: id=122 from=3 to=4 by=script 0x5d865c | out: 4 | expect: 4 | ok
//
//   j.<area>    one tag per area (Area below): grep "j.talk" for every talk, "j\." for the whole journal
//   n<node> <role>   who wrote the line: the co-op node and role (host / member), "solo" outside a session
//   in / out / expect   what the decision read, what it produced, what the shared rules say it should produce (each optional)
//   verdict     ok, MISMATCH (out differs from expect: written at Warn), REFUSED (the game or co-op said no: the reason follows), or none
//
// Steps (step()) are the phases of a longer process (an area trip, a scene, a join): each is flushed to the file at once, so the last
// step written before a crash is the one that was running. The last kRing records and the last step of every area also go into the
// crash report and the freeze dump (nxrt/diag.h diag_add_report).
// Long lists (a box's contents, the quest table) go through list(): numbered lines under the 2000-byte line limit of nxrt/log.h.
// Every record is Info (a pre-alpha build wants them from every player, [log] level warn silences all but the MISMATCH lines), so
// nothing here may be called once a frame without a change test: journal lines are for events, polled state logs only what changed.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define PKP_JOURNAL_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define PKP_JOURNAL_PRINTF(f, a)
#endif

namespace nxrt::games::pkp::journal {

enum class Area : uint8_t {
    Session,     // co-op session: open, join, leave, roles
    Sync,        // co-op messages: sent, received, forwarded, applied, the shared-state digests
    Quest,       // quest states, flags, steps, conditions
    Talk,        // talks to characters: the press, the talk chosen, the lock
    Event,       // the event queue and runner: scenes offered, queued, refused, started, ended
    Role,        // host / visitor branches the game took
    Item,        // the bag: items in and out, the bag shown
    Box,         // chests and storage boxes: opened (where), their contents
    Travel,      // area trips and stage loads, phase by phase
    Discovery,   // the dex, habitats, recipes, unlock lists
    Service,     // creatures' work, NPC services, requests, shop
    Interact,    // the player's presses on world objects (doors, gates, boxes, signs)
    Count
};
const char* area_name(Area a);

// Who this process is, for the line's "n<node> <role>". The co-op module sets it at install; null (and before) is "solo".
struct Who {
    int node = -1;            // -1: no session
    const char* role = "solo";
};
void set_who(Who (*fn)());

// One record, written once when it goes out of scope (or at write()). Each part is printf-style; a part given twice is joined with "; ".
class Rec {
public:
    Rec(Area a, const char* what);
    ~Rec();
    Rec(const Rec&) = delete;
    Rec& operator=(const Rec&) = delete;

    Rec& in(const char* fmt, ...) PKP_JOURNAL_PRINTF(2, 3);
    Rec& out(const char* fmt, ...) PKP_JOURNAL_PRINTF(2, 3);
    Rec& expect(const char* fmt, ...) PKP_JOURNAL_PRINTF(2, 3);
    // The verdict: matched false is a MISMATCH (Warn). A record with no verdict has none.
    Rec& ok(bool matched);
    // The game or co-op refused: verdict REFUSED with the reason.
    Rec& refused(const char* fmt, ...) PKP_JOURNAL_PRINTF(2, 3);
    Rec& note(const char* fmt, ...) PKP_JOURNAL_PRINTF(2, 3);
    // Writes the line at Warn whatever the verdict (something that should never happen).
    Rec& warn();
    // A step of a longer process: flushed at once and kept as the area's last step for the crash report.
    Rec& step();
    void write();

private:
    void add(std::string& part, const char* fmt, va_list ap);
    Area area_;
    const char* what_;
    std::string in_, out_, expect_, verdict_, note_;
    bool warn_ = false, step_ = false, done_ = false;
};

// A step of a longer process with no other part (see Rec::step).
void step(Area a, const char* what, const char* fmt, ...) PKP_JOURNAL_PRINTF(3, 4);

// A list too long for one line: "<what> (n items)" then the items, as many per line as fit, each line "<what> [i/n]: a, b, c".
void list(Area a, const char* what, const std::vector<std::string>& items);

// Once at start: the crash report / freeze dump section. Safe to call again.
void install();

// Small formatting helpers for the parts.
std::string hex_bytes(const void* p, size_t n, size_t max = 64);   // "0a 1b ..", cut with "+N" past max
std::string fmt(const char* f, ...) PKP_JOURNAL_PRINTF(1, 2);

}  // namespace nxrt::games::pkp::journal
