// SPDX-License-Identifier: GPL-3.0-or-later
// The device-recovery pieces of the XAudio2 sink (include/nxrt/audio_standin.h): the silent stand-in queue and the retry delays.
#include <cstdint>

#include "nxrt/audio_standin.h"
#include "test_framework.h"

NXTEST_GROUP(audio_standin)

using namespace nxrt;

NXTEST(standin_retry_delays_grow_then_cap) {
    CHECK_EQ(audio::reopen_delay_s(0), 1.0);
    CHECK_EQ(audio::reopen_delay_s(1), 2.0);
    CHECK_EQ(audio::reopen_delay_s(2), 4.0);
    CHECK_EQ(audio::reopen_delay_s(3), 5.0);
    CHECK_EQ(audio::reopen_delay_s(1000), 5.0);
}

NXTEST(standin_drains_at_sample_rate) {
    audio::StandInClock c;
    c.begin(48000, 10.0);
    CHECK_EQ(c.queued(10.0), 0u);
    CHECK_EQ(c.write(1920, 10.0), 1920u);   // 40 ms
    CHECK_EQ(c.queued(10.0), 1920u);
    CHECK_EQ(c.queued(10.020), 960u);       // 20 ms later
    CHECK_EQ(c.queued(10.040), 0u);
    CHECK_EQ(c.queued(11.0), 0u);           // an empty queue stays empty
}

NXTEST(standin_does_not_play_before_first_write) {
    audio::StandInClock c;
    c.begin(48000, 0.0);
    CHECK_EQ(c.queued(5.0), 0u);
    CHECK_EQ(c.write(480, 5.0), 480u);
    CHECK_EQ(c.queued(5.0), 480u);   // the 5 s idle time did not count as played
}

NXTEST(standin_caps_the_queue_at_120ms) {
    audio::StandInClock c;
    c.begin(48000, 0.0);
    const uint32_t cap = 5760;   // 120 ms
    CHECK_EQ(c.write(10000, 0.0), cap);
    CHECK_EQ(c.write(100, 0.0), 0u);
    const uint32_t room = c.write(100, 0.001);   // 1 ms played makes room for about 48 frames
    CHECK(room >= 47 && room <= 48);
}

NXTEST(standin_backwards_clock_plays_nothing) {
    audio::StandInClock c;
    c.begin(48000, 10.0);
    c.write(4800, 10.0);
    CHECK_EQ(c.queued(9.0), 4800u);
}

NXTEST(standin_begin_resets_for_a_new_loss) {
    audio::StandInClock c;
    c.begin(48000, 0.0);
    c.write(4800, 0.0);
    c.begin(44100, 20.0);
    CHECK_EQ(c.queued(20.0), 0u);
    CHECK_EQ(c.write(4410, 20.0), 4410u);
    const uint32_t left = c.queued(20.05);   // 50 ms of 4410 frames played: about 2205 left
    CHECK(left >= 2204 && left <= 2205);
}
