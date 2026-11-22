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
// The state starts "everything" (unknown) and is only ever made smaller by a full barrier on the render thread; any thread's recording
// can only add kinds. Templates: a build without the fact compiles none of it.
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
template <bool kOn = facts::kPhone>
inline void work_mark(uint32_t kind) {
    std::atomic<uint32_t>& k = work_kinds();
    if ((k.load(std::memory_order_relaxed) & kind) != kind) k.fetch_or(kind, std::memory_order_relaxed);
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
