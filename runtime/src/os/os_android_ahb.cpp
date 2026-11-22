// SPDX-License-Identifier: GPL-3.0-or-later
// os_android_ahb.cpp: guest memory moved onto AHardwareBuffers (os_android_ahb.h). Android only (the CMake filter in
// runtime/CMakeLists.txt, and the guard below).
//
// Moving a range: the buffer is allocated and mapped once at a temporary address; the range is made read-only so that a CPU write
// faults (the fault handler below makes that thread wait until the move is over, then the write goes to the new mapping); the bytes
// are copied; the buffer's dmabuf is mapped MAP_SHARED | MAP_FIXED over the range, which frees the private pages in the same call.
// Moving back is the same with private memory moved in by mremap. The dmabuf fd comes through public NDK API
// (AHardwareBuffer_sendHandleToUnixSocket: the first fd of the native handle is the buffer).
#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR
#include "os_android_ahb.h"

#include <android/hardware_buffer.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <windows.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>

#include "nxrt/fault_guard.h"
#include "nxrt/log.h"

namespace nxrt::os::ahb {

struct Backing {
    AHardwareBuffer* buf = nullptr;
    int fd = -1;
    uint64_t addr = 0, size = 0;
};

namespace {

constexpr const char* kTag = "nvn.pool";

// The range being moved (one at a time: moves run on the render thread). A fault inside it waits for the move to end.
std::atomic<uint64_t> g_lo{0}, g_hi{0};
std::mutex g_move_mu;
thread_local bool t_mover = false;   // the thread moving a range: its own faults (a page it cannot read) are not waited for

LONG CALLBACK on_fault(EXCEPTION_POINTERS* ep) {
    if (t_mover || !ep || !ep->ExceptionRecord || ep->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t a = static_cast<uint64_t>(ep->ExceptionRecord->ExceptionInformation[1]);
    if (a < g_lo.load(std::memory_order_acquire) || a >= g_hi.load(std::memory_order_acquire)) return EXCEPTION_CONTINUE_SEARCH;
    while (a >= g_lo.load(std::memory_order_acquire) && a < g_hi.load(std::memory_order_acquire)) std::this_thread::yield();
    return EXCEPTION_CONTINUE_EXECUTION;   // the access runs again, on the new mapping
}

void install_handler() {
    static std::once_flag once;
    std::call_once(once, [] { AddVectoredExceptionHandler(1, on_fault); });
}

int buffer_fd(AHardwareBuffer* b) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return -1;
    int fd = -1;
    if (AHardwareBuffer_sendHandleToUnixSocket(b, sv[0]) == 0) {
        char data[1024];
        char cbuf[CMSG_SPACE(sizeof(int) * 8)];
        iovec iov = {data, sizeof data};
        msghdr msg = {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof cbuf;
        if (recvmsg(sv[1], &msg, MSG_CMSG_CLOEXEC) > 0)
            for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                    const int n = static_cast<int>((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                    const int* fds = reinterpret_cast<const int*>(CMSG_DATA(c));
                    if (n > 0) fd = fds[0];
                    for (int i = 1; i < n; ++i) close(fds[i]);   // the handle's other fds (metadata) are not needed
                }
    }
    close(sv[0]);
    close(sv[1]);
    return fd;
}

void fail_once(const char* what, uint64_t size) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) NXLOG_W(kTag, "pool_ahb: %s (%llu KiB, errno %d): the range stays as it is (logged once)", what,
                                        static_cast<unsigned long long>(size >> 10), errno);
}

// Moves [lo, lo + size) from its current mapping to `map_new()` with the bytes kept: writers wait meanwhile.
template <class Copy, class Map>
bool move(uint64_t lo, uint64_t size, Copy copy, Map map_new) {
    install_handler();
    std::lock_guard<std::mutex> lk(g_move_mu);
    void* p = reinterpret_cast<void*>(lo);
    t_mover = true;
    g_lo.store(lo, std::memory_order_release);
    g_hi.store(lo + size, std::memory_order_release);
    bool ok = mprotect(p, size, PROT_READ) == 0;
    if (ok) {
        ok = nxrt::fault_guarded(copy) && map_new();   // a page that cannot be read (not committed) ends the move: nothing changes
        if (!ok) mprotect(p, size, PROT_READ | PROT_WRITE);   // the old mapping stays, writable again
    }
    g_hi.store(0, std::memory_order_release);
    g_lo.store(0, std::memory_order_release);
    t_mover = false;
    return ok;
}

}  // namespace

Backing* back(uint64_t addr, uint64_t size) {
    if (!size || size > kMaxBytes || (addr | size) & 4095) return nullptr;
    AHardwareBuffer_Desc d = {};
    d.width = static_cast<uint32_t>(size);
    d.height = 1;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_BLOB;
    d.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    auto* b = new Backing();
    b->addr = addr;
    b->size = size;
    if (AHardwareBuffer_allocate(&d, &b->buf) != 0 || !b->buf) {
        fail_once("AHardwareBuffer_allocate failed", size);
        delete b;
        return nullptr;
    }
    b->fd = buffer_fd(b->buf);
    void* tmp = b->fd >= 0 ? mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0) : MAP_FAILED;
    if (tmp == MAP_FAILED) {
        fail_once("the buffer's dmabuf cannot be mapped", size);
        if (b->fd >= 0) close(b->fd);
        AHardwareBuffer_release(b->buf);
        delete b;
        return nullptr;
    }
    const bool ok = move(addr, size, [&] { std::memcpy(tmp, reinterpret_cast<void*>(addr), size); }, [&] {
        return mmap(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, b->fd, 0) == reinterpret_cast<void*>(addr);
    });
    munmap(tmp, size);
    if (!ok) {
        fail_once("the buffer cannot be mapped over the range", size);
        close(b->fd);
        AHardwareBuffer_release(b->buf);
        delete b;
        return nullptr;
    }
    return b;
}

void unback(Backing* b) {
    if (!b) return;
    void* tmp = mmap(nullptr, b->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    bool ok = tmp != MAP_FAILED && move(b->addr, b->size, [&] { std::memcpy(tmp, reinterpret_cast<void*>(b->addr), b->size); }, [&] {
        return mremap(tmp, b->size, b->size, MREMAP_MAYMOVE | MREMAP_FIXED, reinterpret_cast<void*>(b->addr)) == reinterpret_cast<void*>(b->addr);
    });
    if (!ok) {
        // The bytes stay where they are: the range keeps its shared mapping (the buffer is then never released, a leak, not a loss).
        NXLOG_E(kTag, "pool_ahb: the range at %#llx (%llu KiB) could not move back to private memory (errno %d): it keeps its buffer",
                static_cast<unsigned long long>(b->addr), static_cast<unsigned long long>(b->size >> 10), errno);
        if (tmp != MAP_FAILED) munmap(tmp, b->size);
        return;
    }
    close(b->fd);
    AHardwareBuffer_release(b->buf);
    delete b;
}

int import(Backing* b, void* device, void* get_device_proc_addr, uint32_t usage, uint64_t* buffer, uint64_t* memory) {
    VkDevice dev = static_cast<VkDevice>(device);
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_device_proc_addr);
#define NXRT_AHB_FN(name) const auto name = reinterpret_cast<PFN_##name>(gdpa(dev, #name))
    NXRT_AHB_FN(vkGetAndroidHardwareBufferPropertiesANDROID);
    NXRT_AHB_FN(vkCreateBuffer);
    NXRT_AHB_FN(vkDestroyBuffer);
    NXRT_AHB_FN(vkAllocateMemory);
    NXRT_AHB_FN(vkFreeMemory);
    NXRT_AHB_FN(vkBindBufferMemory);
#undef NXRT_AHB_FN
    if (!vkGetAndroidHardwareBufferPropertiesANDROID || !vkCreateBuffer || !vkDestroyBuffer || !vkAllocateMemory || !vkFreeMemory || !vkBindBufferMemory)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkAndroidHardwareBufferPropertiesANDROID hp = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkResult r = vkGetAndroidHardwareBufferPropertiesANDROID(dev, b->buf, &hp);
    if (r != VK_SUCCESS || !hp.memoryTypeBits) return r != VK_SUCCESS ? r : VK_ERROR_FEATURE_NOT_PRESENT;
    uint32_t ty = 0;
    while (!(hp.memoryTypeBits & (1u << ty))) ++ty;   // the lowest type: the one the coherence test passed with on both drivers
    VkExternalMemoryBufferCreateInfo ebi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ebi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi};
    bci.size = b->size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer vb = VK_NULL_HANDLE;
    r = vkCreateBuffer(dev, &bci, nullptr, &vb);
    if (r != VK_SUCCESS) return r;
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.buffer = vb;
    VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &ded};
    imp.buffer = b->buf;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
    mai.allocationSize = hp.allocationSize;
    mai.memoryTypeIndex = ty;
    VkDeviceMemory vm = VK_NULL_HANDLE;
    r = vkAllocateMemory(dev, &mai, nullptr, &vm);
    if (r == VK_SUCCESS) r = vkBindBufferMemory(dev, vb, vm, 0);
    if (r != VK_SUCCESS) {
        if (vm) vkFreeMemory(dev, vm, nullptr);
        vkDestroyBuffer(dev, vb, nullptr);
        return r;
    }
    *buffer = reinterpret_cast<uint64_t>(vb);
    *memory = reinterpret_cast<uint64_t>(vm);
    return 0;
}

}  // namespace nxrt::os::ahb
#endif
