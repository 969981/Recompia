// SPDX-License-Identifier: GPL-3.0-or-later
// Performance report ([diag] perf_report = on (default) | off; src/diag/perf_report.cpp): what a low frame rate is bound by, written into
// the normal log (tag "perf") so the log alone answers it, with nothing asked of the player.
//   - once, when the GPU is chosen: the machine (CPU, memory, OS, power, GPU, driver, PCIe link, resizable BAR, video memory);
//   - with every frame summary of the hitch monitor (gauge.h, every [diag] hitch_summary_s, default 10 s): the settings that matter when
//     they changed, then one line on the "free" frames of the period, the frames no pacer held back (frame limiter, the game's ceiling,
//     Reflex) while the window had focus: their rate, median, GPU busy time, render-thread and game-thread time, and a verdict;
//   - on a discrete GPU, after ~30 s of GPU-bound free frames under 55 fps: the share of GPU time per render pass of one frame (at most
//     once every 2 minutes, 5 times a session);
//   - at exit: one "perf: session summary" line.
// What the report costs: per presented frame a few relaxed loads and adds on the render thread. A discrete GPU writes two timestamps per
// submission for the GPU busy time; an integrated GPU writes none (nothing on the GPU changes there).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::perf {

bool enabled();                          // [diag] perf_report, read once

// ---- what the runtime tells the report ----------------------------------------------------------------------------------------
// Play mode: the window gained or lost focus (the report starts as focused, so a run without a window counts its frames). Any thread.
void note_focus(bool focused);
// A frame pacer held a frame back: FrameLimiter::begin_frame* had to wait (the player's cap or the game's ceiling), or Reflex slept while
// it paces the cap. The frame being prepared counts as capped. Any thread, one relaxed add.
void note_pacer_wait();

// The GPU the NVN back end runs on. The system block is logged once, at the first call (no-op when the report is off).
struct GpuFacts {
    std::string name;
    uint32_t vendor_id = 0, device_id = 0;
    bool discrete = false;                // VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
    std::string type;                     // "discrete", "integrated", ...
    std::string driver;                   // the driver version as the vendor writes it
    std::string api;                      // Vulkan version, "1.3.289"
    uint64_t vram_bytes = 0;              // the largest device-local heap
    uint64_t bar_bytes = 0;               // the largest device-local heap with a host-visible memory type (0: none)
    bool pci_valid = false;               // VK_EXT_pci_bus_info answered
    uint32_t pci_domain = 0, pci_bus = 0, pci_device = 0, pci_function = 0;
};
void set_gpu(const GpuFacts& gpu);
// Resizable BAR is off when the host-visible part of the video memory (bar_bytes) is at most this: the fixed window without it is 214-256
// MiB depending on the driver, whatever the size of the video memory. The report (perf_system.cpp) and the uniform snapshots'
// auto rule (nvn_replay_bind.cpp) both use it.
constexpr uint64_t kBarWindowMaxBytes = 272ull << 20;

// Play mode: a function that describes the settings that matter (window and render size, scene scale, frame cap, vsync, present mode,
// upscaler), called on the hitch monitor's thread at each summary; the report logs its text when it changed.
void set_settings_source(std::string (*fn)());

// ---- GPU time (NVN back end, render thread) -------------------------------------------------------------------------------------
// The back end will hand in GPU busy times (a discrete GPU with the report on). Without it the verdict comes from the CPU side.
void set_gpu_timing(bool on);
// Frames counted so far: the tag of GPU work recorded now (it belongs to the next frame presented).
uint64_t frame_index();
// GPU busy time of work recorded at frame_index() == tag, overlaps with earlier work already removed.
void note_gpu_busy(uint64_t tag, uint64_t ns);

// Per-pass GPU sample: true once after the report asked for one (render thread; the back end then times the passes of the submissions of
// the next whole frame).
bool pass_sample_take();
struct PassTime {
    std::string label;                    // the pass profiler's label ("pass 1920x1080 c1:37 d0", "gap", "cs 1a2b3c4d", ...)
    uint64_t ns = 0;
    uint64_t draws = 0;
};
// The sampled frame's passes (any order), its submissions and the GPU time they add up to. Logged at once.
void pass_sample_done(std::vector<PassTime> passes, uint32_t submissions);

// The closing line (once; later calls do nothing). Called on the way out: hitch_stop and play mode's exit path.
void session_end();

}  // namespace nxrt::perf
