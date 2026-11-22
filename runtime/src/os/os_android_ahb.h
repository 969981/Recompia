// SPDX-License-Identifier: GPL-3.0-or-later
// os_android_ahb.h: guest memory moved onto AHardwareBuffers (Android), so a GPU without VK_EXT_external_memory_host can import it.
//
// back() replaces the private pages of a guest range with a shared mapping of a new AHardwareBuffer BLOB at the same address, keeping
// the bytes; import() wraps the buffer in a VkDeviceMemory and a VkBuffer (VK_ANDROID_external_memory_android_hardware_buffer). The
// CPU and the GPU then use the same pages, as with a host-pointer import elsewhere. unback() moves the range back onto private memory.
// While a range moves (a copy of at most kMaxBytes), CPU writes to it wait in a fault handler; reads go on.
// Measured on an Adreno 750 phone (stock and Turnip drivers): the mapping lands at the address, mprotect works on it, and CPU and GPU
// see each other's writes without flushes. A BLOB of 1 GiB aborts the process inside gralloc (ubsan multiplication overflow), so a
// range is never larger than kMaxBytes. Off Android every call fails (nothing to do there: desktop drivers import host memory).
#pragma once
#include <cstdint>

namespace nxrt::os::ahb {

constexpr uint64_t kMaxBytes = 256ull << 20;   // 256 MiB allocated fine on an Adreno 750 phone; 1 GiB aborted

struct Backing;   // one backed range

#if defined(__ANDROID__)
// [addr, addr + size) (page aligned, at most kMaxBytes, mapped read-write) onto a new AHardwareBuffer. Null on failure (logged once;
// the range is unchanged then).
Backing* back(uint64_t addr, uint64_t size);
// The range back onto private read-write memory with the same bytes, then the buffer is released. The GPU must be done with it.
void unback(Backing* b);
// A VkBuffer (usage `usage`) bound to a VkDeviceMemory that imports the backing; the device's functions come through
// `get_device_proc_addr` (vkGetDeviceProcAddr of the loader in use). VkDevice/VkBuffer/VkDeviceMemory as handles. 0 on success, else the VkResult.
int import(Backing* b, void* device, void* get_device_proc_addr, uint32_t usage, uint64_t* buffer, uint64_t* memory);
#else
inline Backing* back(uint64_t, uint64_t) { return nullptr; }
inline void unback(Backing*) {}
inline int import(Backing*, void*, void*, uint32_t, uint64_t*, uint64_t*) { return -1; }
#endif

}  // namespace nxrt::os::ahb
