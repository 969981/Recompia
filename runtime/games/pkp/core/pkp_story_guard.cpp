// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: Story co-op's safety net, the save copy at start and the story quest journal (pkp_story_guard.h).
//
// The journal's two sites are the setters of the story quest history in the 2.0.1 main (m_story = save root + 0x4a26a0, in the player's
// own save): 0x5d865c (m_story, id, state 1..4) writes a quest's state byte and 0x5d8774 (m_story, id, bool) bit 0 of its flag byte; the
// quest pump 0x11b600 and the event scripts go through them. Both begin with `sub w8, w1, #1`. The entry hooks only read: x0 = m_story,
// w1 = the quest id, w2 = the new value, and the byte in the save is still the old one. The role and the stage come from the game state
// (pkp_layout.h).
//
// The save after a step is the game's own request for the player's file alone, as its menus make it (0xf707a0 case 8, 0xffa610):
// 0x241324(save manager, request, 0, 0) with the 12-byte request {u32 5, u8 0, u8 the byte at save root + 0xa4, u8 0, u8, u32 2}: kind 2
// writes CMNDAT and nothing of the world, whoever's world the player is in. It answers 0 while a save is running. The autosave's own
// job (state at game state + 0x3bb5e0, 1..4 while it works) and the game's "busy" flag (0x1176a4: the byte at +0x6ea of the object at
// game state + 0xd0, which its autosave waits for too) put the request off.
#include "core/pkp_story_guard.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_save_files.h"
#include "core/pkp_settings.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::games::pkp {

namespace {

namespace stdfs = std::filesystem;
constexpr const char* kTag = "pkp";
constexpr const char* kCopyFolder = "story-coop-backup";
constexpr const char* kJournalFolder = "story-journal";
constexpr uintmax_t kJournalMax = 2u << 20;   // past this the journal becomes <key>.1.tsv (one older file is kept)

constexpr uint32_t kEntryWord = 0x51000428u;  // sub w8, w1, #1: the first instruction of both setters

using layout::kEventBusy;
using layout::kSaveManager;
using layout::kSaveRequest;
using layout::kSaveRootHome;
using layout::kStateAutosave;
using layout::kStateEvent;
constexpr int kSaveSettle = 120;              // game frames from the last change of a step to the request
constexpr int kSaveTries = 3600;              // then the request is given up (the next step asks again)

enum class Change : uintptr_t { State, Flag };
struct Site {
    uint64_t site;
    Change change;
};
constexpr Site kSites[] = {{0x5d865c, Change::State}, {0x5d8774, Change::Flag}};
constexpr size_t kSiteCount = sizeof kSites / sizeof kSites[0];

struct State {
    uint64_t base = 0;
    HookSet* hooks = nullptr;
    uint32_t ids[kSiteCount] = {};         // all 0: off or not installed
    std::atomic<bool> armed{false};
    std::atomic<bool> checked{false};
    int save_in = 0;                       // the game thread's: frames to the save request, 0 none; counts down to 1 and stays while refused
    int save_tries = 0;
    bool logged_save = false;
    alignas(4) uint8_t request[12] = {};
    std::mutex file_mutex;
    stdfs::path journal;
    bool write_failed = false;             // under file_mutex: logged once
};
State& st() {
    static State s;
    return s;
}

const char* role_str(uint8_t session) {
    if (!(session & layout::kSessionOnline)) return "offline";
    return (session & layout::kSessionVisitor) ? "visitor" : "host";
}

void append(State& s, const char* role, unsigned stage, uint32_t id, const char* change, unsigned before, unsigned after) {
    char when[32];
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    std::strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    char line[160];
    const int n = std::snprintf(line, sizeof line, "%s\t%s\t%u\t%u\t%s\t%u\t%u\n", when, role, stage, id, change, before, after);
    if (n <= 0) return;

    const std::lock_guard<std::mutex> lock(s.file_mutex);
    std::error_code ec;
    const uintmax_t size = stdfs::file_size(s.journal, ec);
    const bool fresh = static_cast<bool>(ec);
    if (!fresh && size > kJournalMax) {
        stdfs::path older = s.journal;
        older.replace_extension(".1.tsv");
        stdfs::rename(s.journal, older, ec);
    }
    std::ofstream f(s.journal, std::ios::binary | std::ios::app);
    if (f && (fresh || size > kJournalMax)) f << "time\trole\tstage\tquest\tchange\tbefore\tafter\n";
    if (f) f.write(line, n);
    if (!f && !s.write_failed) {
        s.write_failed = true;
        NXLOG_W(kTag, "Story co-op journal: writing %s failed; the story itself is not affected", save_files::u8(s.journal).c_str());
    }
}

// One line with the player's role and stage as they are now. Returns the session byte.
uint8_t note(State& s, uint32_t id, const char* change, unsigned before, unsigned after) {
    uint8_t session = 0, stage = 0;
    const uint64_t game = guest::rd<uint64_t>(s.base + layout::kGameState);
    if (guest::ok(game, layout::kStateSession + 1)) {
        session = guest::rd<uint8_t>(game + layout::kStateSession);
        stage = guest::rd<uint8_t>(game + layout::kStateStage);
    }
    append(s, role_str(session), stage, id, change, before, after);
    return session;
}

int hook_set(::Ctx* c, uint64_t, void* user) {
    State& s = st();
    if (!s.armed.load(std::memory_order_acquire)) return 0;
    const Change change = static_cast<Change>(reinterpret_cast<uintptr_t>(user));
    const uint32_t id = static_cast<uint32_t>(c->x[1]) & 0xffffu;
    const uint64_t off = story_quest_offset(id);
    if (!off) return 0;
    unsigned after = static_cast<unsigned>(c->x[2]) & 0xffu;
    uint64_t at = c->x[0] + off;
    if (change == Change::State) {
        if (after - 1 >= 4) return 0;   // the setter ignores other states
    } else {
        after &= 1;
        ++at;
    }
    if (!guest::ok(at, 1)) return 0;
    unsigned before = guest::rd<uint8_t>(at);
    if (change == Change::Flag) before &= 1;
    if (before == after) return 0;

    // In a session the step is written to the player's own save once it has settled (game_frame).
    if (note(s, id, change == Change::State ? "state" : "flag", before, after) & layout::kSessionOnline) {
        s.save_in = kSaveSettle;
        s.save_tries = kSaveTries;
    }
    return 0;
}

// The game thread, once a frame: asks the game to write the player's own file when a step has settled and nothing else saves.
void game_frame(::Ctx* c) {
    State& s = st();
    if (!s.save_in || !s.armed.load(std::memory_order_acquire)) return;
    if (s.save_in > 1) {
        --s.save_in;
        return;
    }
    if (--s.save_tries <= 0) {
        s.save_in = 0;
        NXLOG_W(kTag, "Story co-op: the save after a story step was put off too long and is left to the game's own saves");
        return;
    }
    const uint64_t game = guest::rd<uint64_t>(s.base + layout::kGameState);
    const uint64_t manager = guest::rd<uint64_t>(s.base + kSaveManager);
    const uint64_t root = guest::rd<uint64_t>(s.base + layout::kSaveRoot);
    if (!guest::ok(game, kStateAutosave + 4) || !guest::ok(manager, 0x40) || !guest::ok(root, kSaveRootHome + 1)) return;
    if (guest::rd<uint32_t>(game + kStateAutosave) - 1 < 4) return;
    const uint64_t event = guest::rd<uint64_t>(game + kStateEvent);
    if (event && guest::ok(event, kEventBusy + 1) && guest::rd<uint8_t>(event + kEventBusy)) return;
    const uint32_t type = 5, kind = 2;
    std::memset(s.request, 0, sizeof s.request);
    std::memcpy(s.request, &type, 4);
    s.request[5] = guest::rd<uint8_t>(root + kSaveRootHome);
    std::memcpy(s.request + 8, &kind, 4);
    if (!(guest::call(c, s.base + kSaveRequest, manager, reinterpret_cast<uint64_t>(s.request), 0, 0) & 1)) return;
    s.save_in = 0;
    note(s, 0, "save", 0, 0);
    if (!s.logged_save) {
        s.logged_save = true;
        NXLOG_I(kTag, "Story co-op: after a story step in a session the player's own save file is written (the game's own request)");
    }
}

void remove_hooks(State& s) {
    for (uint32_t& h : s.ids) {
        if (h && s.hooks) s.hooks->remove(h);
        h = 0;
    }
}

void install(const FeatureInstall& in) {
    State& s = st();
    if (s.ids[0] || !settings::get_bool(settings::Key::LinkStoryCoop)) return;
    s.base = in.base;
    s.hooks = &in.hooks;
    s.armed.store(false);
    s.checked.store(false);
    s.save_in = 0;

    static std::atomic<bool> copied{false};   // once per process: the save as it was before the game read it
    if (!copied.exchange(true)) {
        stdfs::path where;
        std::string error;
        if (save_files::copy_account(kCopyFolder, kStoryCopiesKept, &where, &error))
            NXLOG_I(kTag, "Story co-op: the save as it is now was copied to %s (the newest %zu copies are kept)", save_files::u8(where).c_str(),
                    kStoryCopiesKept);
        else if (!error.empty())
            NXLOG_W(kTag, "Story co-op: no copy of the save was made (%s)", error.c_str());
    }

    std::error_code ec;
    const stdfs::path dir = save_files::save_dir() / kJournalFolder;
    stdfs::create_directories(dir, ec);
    s.journal = dir / (save_files::account_key() + ".tsv");
    for (size_t i = 0; i < kSiteCount; ++i)
        s.ids[i] = in.hooks.add_entry(in.base + kSites[i].site, &hook_set, reinterpret_cast<void*>(static_cast<uintptr_t>(kSites[i].change)));
}

// Play mode's frame (main is loaded): the first call checks the two setters' first instruction and arms the hooks or removes them.
void frame(bool, double, play::GameFrame*) {
    State& s = st();
    if (!s.ids[0] || s.checked.exchange(true)) return;
    for (const Site& site : kSites) {
        const uint64_t a = s.base + site.site;
        uint32_t word = 0;
        if (mem::is_mapped(a, 4)) std::memcpy(&word, reinterpret_cast<const void*>(a), 4);
        if (word == kEntryWord) continue;
        remove_hooks(s);
        NXLOG_W(kTag, "Story co-op journal: off: the word at main+0x%llx is %08x, expected %08x (another game version)",
                static_cast<unsigned long long>(site.site), word, kEntryWord);
        return;
    }
    s.armed.store(true, std::memory_order_release);
    NXLOG_I(kTag, "Story co-op journal: every story quest change is written to %s", save_files::u8(s.journal).c_str());
}

void remove() {   // the registry has removed the hooks
    State& s = st();
    s.armed.store(false);
    s.checked.store(false);
    for (uint32_t& h : s.ids) h = 0;
    s.hooks = nullptr;
}

}  // namespace

void story_journal_note(uint32_t quest, const char* change, unsigned a, unsigned b) {
    State& s = st();
    if (s.armed.load(std::memory_order_acquire)) note(s, quest, change, a, b);
}

uint64_t story_quest_offset(uint32_t id) {
    if (id - 1 >= 1000) return 0;
    return id > 800 ? 0x1860 + 2ull * (id - 800) : 0x1060 + 2ull * id;
}

const Feature kStoryGuardFeature = {.name = "story_guard", .install = &install, .remove = &remove, .frame = &frame, .game_frame = &game_frame};

}  // namespace nxrt::games::pkp
