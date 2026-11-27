// SPDX-License-Identifier: GPL-3.0-or-later
// What one NVN submission asked the GPU to run: the shader keys of its dispatches (with their group counts) and of its draws, for the
// report of the submissions the GPU had not finished when the device was lost or the game froze (nvn_gpu.cpp in_flight_report, the log
// and the freeze dump / crash report). A shader that hangs the GPU (a compute loop whose count was never reached, NVIDIA's TDR) is then
// named in a player's log on every vendor, with or without [gfx] gpu_checkpoints. Idea from pokopia-native 1.6.7 (MIT) vk_dev.c /
// vk_replay.c (vk_checkpoint, vk_lost); this is a host-side list per submission slot, no GPU markers.
//
// One WorkLog per submission slot. The render thread records (dispatch, draw) between reset (the slot is reused: its last serial
// completed) and seal (the recording ended: its serial is fixed); a record is a 16 or 32-byte copy and an increment, no allocation, no
// lock. Each kind is a ring of the latest entries: draws never push dispatches out. Readers on other threads (the thread that saw
// VK_ERROR_DEVICE_LOST, the freeze watchdog) read a sealed log only, without a lock: best effort, a slot reused meanwhile reads as
// changed and is left out.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace nxrt::nvn::be::gpu {

struct WorkLog {
    struct Dispatch {
        char cs[16];
        uint32_t x, y, z;
        uint32_t indirect;   // 1: x, y are the guest address of the arguments (low, high), z 0
    };
    struct Draw {
        char vs[16], fs[16];   // all zero: no such stage
    };
    static constexpr uint32_t kDispatches = 256, kDraws = 256;

    Dispatch disp[kDispatches];
    Draw draw[kDraws];
    uint32_t ndisp = 0, ndraw = 0;    // recorded since reset; entry i is at [i % k]
    std::atomic<uint64_t> sealed{0};  // the serial of the finished recording (release: the entries before it), 0 while recording

    void reset() {
        ndisp = ndraw = 0;
        sealed.store(0, std::memory_order_relaxed);
    }
    void seal(uint64_t serial) { sealed.store(serial, std::memory_order_release); }

    void dispatch(const std::string& cs, uint32_t x, uint32_t y, uint32_t z, bool indirect) {
        Dispatch& e = disp[ndisp++ % kDispatches];
        key(e.cs, &cs);
        e.x = x;
        e.y = y;
        e.z = z;
        e.indirect = indirect;
    }
    void add_draw(const std::string* vs, const std::string* fs) {
        Draw& e = draw[ndraw++ % kDraws];
        key(e.vs, vs);
        key(e.fs, fs);
    }

private:
    static void key(char* to, const std::string* s) {   // the 16 hex digits of a ShaderInfo::hash, zero padded
        const size_t n = s ? std::min<size_t>(s->size(), 16) : 0;
        if (n) std::memcpy(to, s->data(), n);
        if (n < 16) std::memset(to + n, 0, 16 - n);
    }
};

// The log of submission `serial` as report lines (each indented by `indent` and ending in '\n'): its counts, then each distinct dispatch
// (shader and group counts) and each distinct draw (vertex and fragment shader) in the order first recorded, with how many times, at most
// `max_draw_lines` draws (dispatches are never cut: they are what a loop cap is for). Any thread; the caller checks `sealed` before and
// after (the slot may be reused while this formats).
std::string work_log_text(const WorkLog& log, uint64_t serial, const char* indent, uint32_t max_draw_lines);

}  // namespace nxrt::nvn::be::gpu
