// SPDX-License-Identifier: GPL-3.0-or-later
// Game layer: Story co-op keeps a visitor's town-changing quest commands for the visitor's own world (pkp_story_hold.h).
//
// A story quest's commands run in 0x11c0a4(this, id, phase) of the 2.0.1 main: phase 0 is the completion (0x11bd3c, from the quest
// pump 0x11b600), phase 1 the second phase (the pump 0x11b700). For slot 0..3 it reads the command with 0x11e520(id, slot, set, out),
// set 1 for phase 0 and set 0 for phase 1, out the 0x14-byte command on its stack (sp + 0xc), its first byte the type (1..0x3c, 0 for
// none), and runs a type 1..0x3c with 0x11b214(command). The runner reads nothing else of the quest: x0 is its only argument, a switch
// on the type, and the callers in 0x11c0a4 read no result. 0x11c0a4 keeps the id in w19 and the phase in w20 across the four calls,
// whose return addresses (kSlotReturn) give the slot. The other callers of the runner (0xb4ca4, 0xb4d60, 0xb4e3c: a fix-up that runs
// single commands of quests 0x20b and 0xea while they are active) are left alone.
//
// The entry hook on 0x11b214 returns "handled" for a held command of a visitor, so the runner returns at once and the quest goes on
// with its other commands, and notes (quest, set, slot) in <save dir>/story-held/<key>.tsv. Back in the player's own world, the game
// thread reads each noted command again with 0x11e520 and runs it with 0x11b214, as 0x11c0a4 would have. The line is taken out of the
// file before the call, so a command is replayed at most once even if the game ends during it.
#include "core/pkp_story_hold.h"
#include "core/pkp_guest.h"
#include "core/pkp_layout.h"
#include "core/pkp_save_files.h"
#include "core/pkp_settings.h"
#include "core/pkp_story_guard.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "nxrt/file_write.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/notices.h"

namespace nxrt::games::pkp {

namespace {

namespace stdfs = std::filesystem;
constexpr const char* kTag = "pkp";
constexpr const char* kFolder = "story-held";
constexpr uint64_t kRunner = 0x11b214;            // (const command*): runs one quest command
constexpr uint32_t kRunnerWord = 0x39400008u;     // ldrb w8, [x0]
constexpr uint64_t kQuestCommand = 0x11e520;      // (id, slot, set, out)
// 0x11c0a4's four `bl 0x11b214`, slot 0..3: the return address and the call's word.
constexpr uint64_t kSlotReturn[4] = {0x11c124, 0x11c16c, 0x11c1b4, 0x11c1fc};
constexpr uint32_t kSlotCallWord[4] = {0x97fffc3du, 0x97fffc2bu, 0x97fffc19u, 0x97fffc07u};
constexpr uint64_t kQuestIdReg = 19, kPhaseReg = 20;
constexpr uint8_t kVisitor = layout::kSessionOnline | layout::kSessionVisitor;
constexpr uint32_t kQuestMax = 1000;
constexpr int kHomeSettle = 180;                  // game frames in the player's own world, in game and idle, before a replay
constexpr double kNoticeSeconds = 8.0;

using layout::kEventBusy;
using layout::kStateAutosave;
using layout::kStateEvent;

struct Held {
    uint16_t quest;
    uint8_t set;
    uint8_t slot;
    uint8_t type;   // as it was skipped (the journal's); the replay reads the command again
};

struct State {
    uint64_t base = 0;
    HookSet* hooks = nullptr;
    uint32_t hook = 0;                            // 0: off or not installed
    std::atomic<bool> armed{false};
    std::atomic<bool> checked{false};
    std::atomic<bool> in_game{false};             // play mode's frame: the game is past its menus
    stdfs::path file;
    // The game thread's (the runner and game_frame run there); install fills `held` before the hook is armed.
    std::vector<Held> held;
    std::array<bool, kQuestMax + 1> told{};       // this quest's skip was shown
    int home_frames = 0;
    int replayed = 0;                             // in this stay in the player's own world, for the notice
    bool write_failed = false;                    // logged once
    alignas(8) uint8_t command[0x20] = {};        // 0x11e520's out
};
State& st() {
    static State s;
    return s;
}

uint8_t session(const State& s) {
    const uint64_t game = guest::rd<uint64_t>(s.base + layout::kGameState);
    return guest::ok(game, layout::kStateSession + 1) ? guest::rd<uint8_t>(game + layout::kStateSession) : 0;
}

std::string format(const std::vector<Held>& held) {
    std::string out;
    char line[48];
    for (const Held& h : held) {
        const int n = std::snprintf(line, sizeof line, "%u\t%u\t%u\t%u\n", h.quest, h.set, h.slot, h.type);
        if (n > 0) out.append(line, static_cast<size_t>(n));
    }
    return out;
}

bool save(State& s) {
    std::string err;
    if (write_file_atomic(save_files::u8(s.file), format(s.held), &err)) return true;
    if (!s.write_failed) {
        s.write_failed = true;
        NXLOG_W(kTag, "Story co-op: writing %s failed (%s): a held town change is replayed only once the file is written",
                save_files::u8(s.file).c_str(), err.c_str());
    }
    return false;
}

void load(State& s) {
    s.held.clear();
    std::ifstream f(s.file, std::ios::binary);
    for (std::string line; std::getline(f, line);) {
        unsigned quest = 0, set = 0, slot = 0, type = 0;
        if (std::sscanf(line.c_str(), "%u\t%u\t%u\t%u", &quest, &set, &slot, &type) != 4) continue;
        if (quest - 1 >= kQuestMax || set > 1 || slot > 3) continue;
        s.held.push_back({static_cast<uint16_t>(quest), static_cast<uint8_t>(set), static_cast<uint8_t>(slot), static_cast<uint8_t>(type)});
    }
}

int slot_of(const State& s, uint64_t ret) {
    for (int i = 0; i < 4; ++i)
        if (ret == s.base + kSlotReturn[i]) return i;
    return -1;
}

// 0x11b214(command), entered from 0x11c0a4 while the player is a visitor: a held type is noted and not run.
int hook_runner(::Ctx* c, uint64_t, void*) {
    State& s = st();
    if (!s.armed.load(std::memory_order_acquire)) return 0;
    const int slot = slot_of(s, c->x[30]);
    if (slot < 0 || !guest::ok(c->x[0], 1)) return 0;
    const uint8_t type = guest::rd<uint8_t>(c->x[0]);
    if (!story_effect_held(type) || (session(s) & kVisitor) != kVisitor) return 0;
    const uint32_t id = static_cast<uint32_t>(c->x[kQuestIdReg]) & 0xffffu;
    const uint32_t phase = static_cast<uint32_t>(c->x[kPhaseReg]);
    if (id - 1 >= kQuestMax || phase > 1) return 0;
    const Held h{static_cast<uint16_t>(id), static_cast<uint8_t>(phase == 0 ? 1 : 0), static_cast<uint8_t>(slot), type};

    bool known = false;
    for (const Held& o : s.held) known |= o.quest == h.quest && o.set == h.set && o.slot == h.slot;
    if (!known) {
        s.held.push_back(h);
        save(s);
    }
    NXLOG_I(kTag, "Story co-op: quest %u set %u slot %u changes the town (effect 0x%02x): skipped here, made in this player's own world", id,
            h.set, slot, type);
    story_journal_note(id, "skip", type, h.set * 4u + h.slot);
    if (!s.told[id]) {
        s.told[id] = true;
        notices::post("pkp.story_hold", "Story co-op: this step's changes to your town happen when you are back in your own world", kNoticeSeconds);
    }
    return 1;   // handled: the runner returns at once (0x11c0a4 reads no result) and the quest goes on
}

// The game thread, once a frame: back in the player's own world, in game and idle for kHomeSettle frames, one held command a frame.
void game_frame(::Ctx* c) {
    State& s = st();
    if (!s.armed.load(std::memory_order_acquire)) return;
    const uint64_t game = guest::rd<uint64_t>(s.base + layout::kGameState);
    bool home = s.in_game.load(std::memory_order_relaxed) && guest::ok(game, kStateAutosave + 4) &&
                !(guest::rd<uint8_t>(game + layout::kStateSession) & layout::kSessionVisitor);
    if (home) {
        const uint64_t event = guest::rd<uint64_t>(game + kStateEvent);
        home = guest::rd<uint32_t>(game + kStateAutosave) - 1 >= 4 && !(event && guest::ok(event, kEventBusy + 1) && guest::rd<uint8_t>(event + kEventBusy));
    }
    if (!home) {
        s.home_frames = 0;
        s.replayed = 0;
        return;
    }
    if (s.held.empty()) return;
    if (s.home_frames < kHomeSettle) {
        ++s.home_frames;
        return;
    }

    const Held h = s.held.front();
    s.held.erase(s.held.begin());
    if (!save(s)) {   // not taken out of the file: not run, so it can never run twice; tried again after another settle
        s.held.insert(s.held.begin(), h);
        s.home_frames = 0;
        return;
    }
    // A held command belongs to the save its quest was completed in. A save put back from before that (the launcher's save import, a
    // story-coop-backup copy) has the quest not completed, and the command would change this world for a step it never took.
    const uint64_t save = guest::rd<uint64_t>(s.base + layout::kSaveRoot);
    const uint64_t at = save + layout::kSaveStory + story_quest_offset(h.quest);
    if (!save || !guest::ok(at, 1) || guest::rd<uint8_t>(at) != layout::kQuestDone) {
        NXLOG_W(kTag, "Story co-op: quest %u is not completed in this save (state %u; a save from before it was put back): its held town "
                "change (set %u slot %u, effect 0x%02x) is dropped", h.quest, save && guest::ok(at, 1) ? guest::rd<uint8_t>(at) : 0u, h.set,
                h.slot, h.type);
        story_journal_note(h.quest, "replay-dropped", h.type, h.set * 4u + h.slot);
        return;
    }
    std::memset(s.command, 0, sizeof s.command);
    guest::call(c, s.base + kQuestCommand, h.quest, h.slot, h.set, reinterpret_cast<uint64_t>(s.command));
    const uint8_t type = s.command[0];
    if (!story_effect_held(type)) {
        NXLOG_W(kTag, "Story co-op: quest %u set %u slot %u is no longer a town change (type 0x%02x, was 0x%02x): not replayed", h.quest, h.set,
                h.slot, type, h.type);
        story_journal_note(h.quest, "replay-dropped", type, h.set * 4u + h.slot);
        return;
    }
    guest::call(c, s.base + kRunner, reinterpret_cast<uint64_t>(s.command));
    NXLOG_I(kTag, "Story co-op: quest %u set %u slot %u's town change (effect 0x%02x) was made in this player's own world (%zu left)",
            h.quest, h.set, h.slot, type, s.held.size());
    story_journal_note(h.quest, "replay", type, h.set * 4u + h.slot);
    if (++s.replayed == 1)
        notices::post("pkp.story_hold", "Story co-op: changes to your town from steps done in another world are being made", kNoticeSeconds);
}

void install(const FeatureInstall& in) {
    State& s = st();
    if (s.hook || !settings::get_bool(settings::Key::LinkStoryCoop)) return;
    s.base = in.base;
    s.hooks = &in.hooks;
    s.armed.store(false);
    s.checked.store(false);
    s.in_game.store(false);
    s.told.fill(false);
    s.home_frames = 0;
    s.replayed = 0;
    s.write_failed = false;
    std::error_code ec;
    const stdfs::path dir = save_files::save_dir() / kFolder;
    stdfs::create_directories(dir, ec);
    s.file = dir / (save_files::account_key() + ".tsv");
    load(s);
    s.hook = in.hooks.add_entry(in.base + kRunner, &hook_runner);
}

// Play mode's frame (main is loaded): the first call checks the runner's first word and the four calls of 0x11c0a4 and arms the hook
// or removes it; every call notes whether the game is in game.
void frame(bool in_game, double, play::GameFrame*) {
    State& s = st();
    s.in_game.store(in_game, std::memory_order_relaxed);
    if (!s.hook || s.checked.exchange(true)) return;
    auto word_at = [&](uint64_t off) {
        uint32_t w = 0;
        if (mem::is_mapped(s.base + off, 4)) std::memcpy(&w, reinterpret_cast<const void*>(s.base + off), 4);
        return w;
    };
    uint64_t bad = word_at(kRunner) == kRunnerWord ? 0 : kRunner;
    for (int i = 0; i < 4 && !bad; ++i)
        if (word_at(kSlotReturn[i] - 4) != kSlotCallWord[i]) bad = kSlotReturn[i] - 4;
    if (!bad) {
        s.armed.store(true, std::memory_order_release);
        NXLOG_I(kTag, "Story co-op: a visitor's town changes wait for the visitor's own world, the rest of the quest completes (hook at "
                "main+0x%llx, %zu waiting)", static_cast<unsigned long long>(kRunner), s.held.size());
        return;
    }
    s.hooks->remove(s.hook);
    s.hook = 0;
    NXLOG_W(kTag, "Story co-op: town changes are not held: main+0x%llx is %08x (another game version)", static_cast<unsigned long long>(bad),
            word_at(bad));
}

void remove() {   // the registry has removed the hook
    State& s = st();
    s.armed.store(false);
    s.checked.store(false);
    s.in_game.store(false);
    s.hook = 0;
    s.hooks = nullptr;
}

}  // namespace

bool story_effect_held(uint8_t type) {
    switch (type) {
        case 0x05: case 0x06: case 0x0d: case 0x0f: case 0x26: case 0x2e: case 0x2f: case 0x37: case 0x38: case 0x39: return true;
        default: return false;
    }
}

const Feature kStoryHoldFeature = {.name = "story_hold", .install = &install, .remove = &remove, .frame = &frame, .game_frame = &game_frame};

}  // namespace nxrt::games::pkp
