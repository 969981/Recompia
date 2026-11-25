// SPDX-License-Identifier: GPL-3.0-or-later
// GPU keep-alive ([gfx] gpu_keepalive, off by default): keeps a desktop GPU's driver from parking the card in a low power state while
// the game renders.
//
// Why: the game's frame is a chain of small dependent passes. Much of its time is spent waiting on memory and draining between passes,
// which NVIDIA's driver does not count as load. The load it sees stays near 40%, the threshold where it keeps the card in P5 (memory
// at 810 MHz, PCIe gen2) or P8, and at that memory clock the frame takes ~22 ms instead of ~2: about 40 fps on a card that holds 60
// with its memory clock locked. A smaller frame does not help, because the driver then lowers the clocks further.
//
// What: one 32-lane workgroup runs an ALU-only loop (no memory traffic) on a compute-only queue, beside the frame's work, for
// [gfx] gpu_keepalive_duty percent of the time while frames are being presented. The driver sees load above its threshold and keeps
// the card in P0. It stops within 250 ms of the last present (menus that stop presenting, a minimized window, shutdown).
// [diag] ab_name = gpu_keepalive pauses it while the A/B file reads 0, for paired measurements in one session.
#pragma once
#include <memory>

#include "nxrt/gfx.h"

namespace nxrt::extras {

class GpuKeepAlive {
public:
    // Null unless [gfx] gpu_keepalive is on and the device made its compute queue (Device::aux_compute_queue).
    static std::unique_ptr<GpuKeepAlive> create(gfx::Device& dev);
    ~GpuKeepAlive();
    GpuKeepAlive(const GpuKeepAlive&) = delete;
    GpuKeepAlive& operator=(const GpuKeepAlive&) = delete;

    void note_present();   // any thread: a frame was presented (keeps the keep-alive running for the next 250 ms)

    struct Impl;

private:
    explicit GpuKeepAlive(Impl* impl) : impl_(impl) {}
    Impl* impl_;
};

}  // namespace nxrt::extras
