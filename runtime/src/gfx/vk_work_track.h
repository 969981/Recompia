// SPDX-License-Identifier: GPL-3.0-or-later
// Phones: what kinds of GPU work were recorded since the replay's last full memory barrier (nvn_gpu.cpp memory_barrier resets it on the
// render thread). install_work_track() wraps every Vulkan command that can write memory (volk's device function pointers, so every
// recording site of the runtime, ImGui and the upscalers included, goes through it) and marks its kind:
//   draw      vkCmdDraw*, vkCmdBeginRendering / vkCmdEndRendering, vkCmdClearAttachments (attachment writes; the replay adds kStorage
//             itself for a draw whose program may store, the same test as g_bar.pass_unsafe)
//   transfer  copies, blits, resolves, fills, buffer updates, image clears, query result copies
//   compute   dispatches
//   storage   transform feedback (and the replay's storing draws)
// Commands left unwrapped do not write buffer or image memory (state, binds, push constants, barriers, queries, timestamps, labels).
// The state starts "everything" (unknown) and is only ever made smaller by a full barrier the render thread records in the replay's own
// command buffer (work_cb) while no one-shot submission is in flight; any thread's recording can only add kinds. Other threads record
// only through Device::submit_once (scene and SMAA uploads, texture view copies, pool copies), which waits for its fence. Templates: a build without the fact compiles none of it.
#pragma once
#include <atomic>
#include <cstdint>

#include "nxrt/platform_facts.h"

namespace nxrt::gfx {

enum : uint32_t { kWorkDraw = 1, kWorkTransfer = 2, kWorkCompute = 4, kWorkStorage = 8, kWorkAll = 15 };

template <bool kOn = facts::kPhone>
std::atomic<uint32_t>& work_kinds() {
    static std::atomic<uint32_t> k{kWorkAll};
    return k;
}
// Always an atomic or: a test first could skip the mark while the render thread's reset clears the bit, and the work would be lost.
template <bool kOn = facts::kPhone>
inline void work_mark(uint32_t kind) {
    work_kinds().fetch_or(kind, std::memory_order_relaxed);
}
// The replay's command buffer being recorded (render thread only): only a full barrier in it resets the record. A barrier in any other
// command buffer (a one-shot submission, the presenter's) does not order the replay's work.
template <bool kOn = facts::kPhone>
void*& work_cb() {
    static void* cb = nullptr;
    return cb;
}
// One-shot submissions in flight (Device::submit_once, any thread). Their command buffers are submitted on their own and may follow the
// replay's barrier in submission order, so a reset while one is in flight keeps every kind; once its fence was waited for, its work is
// complete before anything the replay submits later.
template <bool kOn = facts::kPhone>
std::atomic<int>& work_inflight() {
    static std::atomic<int> n{0};
    return n;
}
template <bool kOn = facts::kPhone>
struct WorkInflight {   // for the life of one submit_once
    WorkInflight() { if constexpr (kOn) work_inflight().fetch_add(1, std::memory_order_acq_rel); }
    ~WorkInflight() { if constexpr (kOn) work_inflight().fetch_sub(1, std::memory_order_acq_rel); }
};
// Render thread, after a full memory barrier recorded in work_cb().
template <bool kOn = facts::kPhone>
inline void work_reset() {
    work_kinds().store(work_inflight().load(std::memory_order_acquire) > 0 ? kWorkAll : 0u, std::memory_order_relaxed);
}
// Render thread, after a full memory barrier recorded in cb: resets only when cb is the replay's (work_cb()).
template <bool kOn = facts::kPhone>
inline void work_reset_for(const void* cb) {
    if (cb == work_cb()) work_reset();
}

#define NX_WORK_WRAP(fn, kind)                                                                          \
    do {                                                                                                \
        static PFN_##fn orig = nullptr;                                                                 \
        if (fn && orig == nullptr) {                                                                    \
            orig = fn;                                                                                  \
            fn = [](auto... a) { ::nxrt::gfx::work_mark(kind); return orig(a...); };                   \
        }                                                                                               \
    } while (0)

// Once, after volkLoadDevice (the device's function pointers).
template <bool kOn = facts::kPhone>
void install_work_track() {
    NX_WORK_WRAP(vkCmdDraw, kWorkDraw);
    NX_WORK_WRAP(vkCmdDrawIndexed, kWorkDraw);
    NX_WORK_WRAP(vkCmdDrawIndirect, kWorkDraw);
    NX_WORK_WRAP(vkCmdDrawIndexedIndirect, kWorkDraw);
    NX_WORK_WRAP(vkCmdDrawIndirectCount, kWorkDraw);
    NX_WORK_WRAP(vkCmdDrawIndexedIndirectCount, kWorkDraw);
    NX_WORK_WRAP(vkCmdDrawIndirectByteCountEXT, kWorkDraw);
    NX_WORK_WRAP(vkCmdBeginRendering, kWorkDraw);
    NX_WORK_WRAP(vkCmdEndRendering, kWorkDraw);
    NX_WORK_WRAP(vkCmdClearAttachments, kWorkDraw);
    NX_WORK_WRAP(vkCmdDispatch, kWorkCompute);
    NX_WORK_WRAP(vkCmdDispatchIndirect, kWorkCompute);
    NX_WORK_WRAP(vkCmdDispatchBase, kWorkCompute);
    NX_WORK_WRAP(vkCmdCopyBuffer, kWorkTransfer);
    NX_WORK_WRAP(vkCmdCopyImage, kWorkTransfer);
    NX_WORK_WRAP(vkCmdCopyBufferToImage, kWorkTransfer);
    NX_WORK_WRAP(vkCmdCopyImageToBuffer, kWorkTransfer);
    NX_WORK_WRAP(vkCmdCopyBuffer2, kWorkTransfer);
    NX_WORK_WRAP(vkCmdCopyImage2, kWorkTransfer);
    NX_WORK_WRAP(vkCmdCopyBufferToImage2, kWorkTransfer);
    NX_WORK_WRAP(vkCmdCopyImageToBuffer2, kWorkTransfer);
    NX_WORK_WRAP(vkCmdBlitImage, kWorkTransfer);
    NX_WORK_WRAP(vkCmdBlitImage2, kWorkTransfer);
    NX_WORK_WRAP(vkCmdResolveImage, kWorkTransfer);
    NX_WORK_WRAP(vkCmdResolveImage2, kWorkTransfer);
    NX_WORK_WRAP(vkCmdFillBuffer, kWorkTransfer);
    NX_WORK_WRAP(vkCmdUpdateBuffer, kWorkTransfer);
    NX_WORK_WRAP(vkCmdClearColorImage, kWorkTransfer);
    NX_WORK_WRAP(vkCmdClearDepthStencilImage, kWorkTransfer);
    NX_WORK_WRAP(vkCmdCopyQueryPoolResults, kWorkTransfer);
    NX_WORK_WRAP(vkCmdBeginTransformFeedbackEXT, kWorkStorage);
    NX_WORK_WRAP(vkCmdBeginRenderingKHR, kWorkDraw);
    NX_WORK_WRAP(vkCmdEndRenderingKHR, kWorkDraw);
    NX_WORK_WRAP(vkCmdDrawIndirectCountKHR, kWorkDraw);
    NX_WORK_WRAP(vkCmdDrawIndexedIndirectCountKHR, kWorkDraw);
    NX_WORK_WRAP(vkCmdExecuteCommands, kWorkAll);
}

}  // namespace nxrt::gfx
