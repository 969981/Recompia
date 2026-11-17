// SPDX-License-Identifier: GPL-3.0-or-later
// GPU timestamps of the NVN back end's submissions (nvn_gpu_timing.cpp). Render thread only, unless it says otherwise. Two kinds, one
// readback (collect, when the submission slot is reused after its serial completed):
//   - the exec pair: top and bottom of each submission. Written when the frame trace is on ([prof] trace: gpu.exec, gpu.exec.queued events)
//     or on a discrete GPU with the performance report on (nxrt/perf_report.h: GPU busy time per frame). An integrated GPU without the
//     trace writes none.
//   - pass stamps (gpass_stamp in nvn_gpu_internal.h): a timestamp after each render pass and between passes. Every submission with the frame
//     trace and [prof] gpu_passes (gp:<label> trace events); otherwise only the submissions of one whole frame when the performance report
//     asks for a pass sample (discrete GPUs). Each stamp can drain the GPU pipeline, so they never run continuously for the report.
// `slot` is the submission slot's index (0 .. slots-1); each slot owns its query range.
#pragma once
#include <volk.h>

#include <cstdint>

#include "nxrt/gfx.h"

namespace nxrt::nvn::be::gpu::timing {

void init(gfx::Device& dev, uint32_t slots);     // decides what is timed and makes the query pools (before the render thread starts)
void shutdown(gfx::Device& dev);                 // after the render thread and the GPU are idle
bool queued_wanted();                            // the frame trace wants gpu.exec.queued: stamp when a slot reaches vkQueueSubmit2 (any thread)
void begin(VkCommandBuffer cb, uint32_t slot);   // right after vkBeginCommandBuffer of the submission's first command buffer
void end(VkCommandBuffer cb, uint32_t slot);     // right before vkEndCommandBuffer of its last one
// The slot's results, after its serial completed: the frame trace's events, the report's GPU busy time and pass sample. queued_ns: when the
// slot was handed to vkQueueSubmit2 (frame trace; 0 = unknown).
void collect(uint32_t slot, uint64_t serial, uint64_t queued_ns);
bool pass_on();                                  // the submission being recorded takes pass stamps (any thread; a relaxed load)
void pass_stamp(VkCommandBuffer cb, uint32_t slot, const char* label, uint32_t arg);

}  // namespace nxrt::nvn::be::gpu::timing
