// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only game tables for the play-mode tests (gfx and launcher test executables).
//
// The test executables register the test game (CMakeLists.txt NXRT_TEST_GAME, Pokopia), and Pokopia names none of the generic action and
// context slots (no GameVocabulary) and has no PromptGame. The mechanisms that read those tables are shared runtime code, so the tests that
// cover them install a small table of their own for the duration of one test: ScopedGame copies the registered game's Info, replaces the
// tables it is given and restores the registered game when it goes out of scope.
#pragma once
#include <array>
#include <cstring>

#include "nxrt/game.h"
#include "play/play_types.h"

namespace nxtest {

// Installs a copy of the registered game's Info with play_defaults and/or prompts replaced (nullptr keeps the registered game's table).
class ScopedGame {
public:
    explicit ScopedGame(const nxrt::play::GameDefaults* defaults, const nxrt::play::PromptGame* prompts = nullptr)
        : prev_(nxrt::game::current()) {
        if (prev_) info_ = *prev_;
        if (defaults) info_.play_defaults = defaults;
        if (prompts) info_.prompts = prompts;
        nxrt::game::set_current(&info_);
    }
    ~ScopedGame() { nxrt::game::set_current(prev_); }
    ScopedGame(const ScopedGame&) = delete;
    ScopedGame& operator=(const ScopedGame&) = delete;

private:
    const nxrt::game::Info* prev_;
    nxrt::game::Info info_;
};

// A made-up vocabulary in the style of a game that names its slots (the first game of the upstream runtime, Metroid Prime Remastered, did:
// fire, beam_1, gameplay, morph, ball_camera ...). Only the names matter to the tests; the factory text is the registered game's.
struct TestVocabulary {
    std::array<nxrt::play::ActInfo, nxrt::play::kActCount> acts{};
    const char* ctx_names[nxrt::play::kCtxCount] = {"all", "gameplay", "morph", "map", "menu", "scan"};
    nxrt::play::GameActLabel labels[3] = {
        {nxrt::play::Act::Primary, "Fire", "Combat"},
        {nxrt::play::Act::Jump, "Jump", "Movement"},
        {nxrt::play::Act::AltForm, "Morph ball", "Movement"},
    };
    nxrt::play::GameVocabulary vocab;
    nxrt::play::GameDefaults defaults;

    TestVocabulary() {
        using nxrt::play::Act;
        // The generic rows (read before anything is installed), with a few slots renamed and given a factory output.
        for (int i = 0; i < nxrt::play::kActCount; ++i) acts[i] = nxrt::play::act_info(static_cast<Act>(i));
        acts[static_cast<int>(Act::Primary)] = {Act::Primary, "fire", "ZR"};
        acts[static_cast<int>(Act::Jump)] = {Act::Jump, "jump", "B"};
        acts[static_cast<int>(Act::AltForm)] = {Act::AltForm, "morph", "Y"};
        acts[static_cast<int>(Act::SetA1)] = {Act::SetA1, "beam_1", "X+UP"};
        vocab.acts = acts.data();
        vocab.ctx_names = ctx_names;
        vocab.labels = labels;
        vocab.label_count = 3;
        vocab.key_alt_yaw = "morph_yaw";
        vocab.key_alt_pitch = "morph_pitch";
        vocab.key_wheel_cycles = "wheel_beams";
        vocab.key_alt_orbit = "ball_orbit";
        vocab.key_alt_camera = "ball_camera";
        vocab.key_alt_sens = "ball_sensitivity";
        if (const nxrt::play::GameDefaults* g = nxrt::play::game_defaults()) defaults = *g;
        defaults.vocabulary = &vocab;
    }
    TestVocabulary(const TestVocabulary&) = delete;
    TestVocabulary& operator=(const TestVocabulary&) = delete;
};

}  // namespace nxtest
