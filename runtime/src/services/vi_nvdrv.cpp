// SPDX-License-Identifier: GPL-3.0-or-later
// nvdrv, nvdrv:a, nvdrv:s, nvdrv:t: the nvhost devices and their ioctls (see vi_nv.cpp for the provenance).
#include "vi_nv_internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "services_internal.h"

namespace nxrt::services::vinv {

// ============================================================ nvdrv: devices, handles, address spaces

NvState& nv() {
    static NvState s;
    return s;
}

namespace {

// ---- gauges (include/nxrt/gauge.h): the nvdrv tables, counted under the state lock ----
GaugeHook gauge_nv_files("nv.files", [] {   // open nvdrv device files (nvmap, gpu channels, as-gpu, ...)
    NvState& s = nv();
    std::lock_guard<std::mutex> lk(s.mu);
    return static_cast<int64_t>(s.files.size());
});
GaugeHook gauge_nv_maps("nv.nvmap_objects", [] {   // nvmap allocations alive (removed when the last reference is freed)
    NvState& s = nv();
    std::lock_guard<std::mutex> lk(s.mu);
    return static_cast<int64_t>(s.maps.size());
});

struct DevEntry {
    const char* path;
    Dev dev;
    bool debug_only;   // the debug and profiler devices answer NotSupported (debug mode is off on a retail console)
};
constexpr DevEntry kDevices[] = {
    {"/dev/nvmap", Dev::NvMap, false},
    {"/dev/nvhost-ctrl", Dev::Ctrl, false},
    {"/dev/nvhost-ctrl-gpu", Dev::CtrlGpu, false},
    {"/dev/nvhost-as-gpu", Dev::AsGpu, false},
    {"/dev/nvhost-gpu", Dev::Gpu, false},
    {"/dev/nvhost-nvdec", Dev::Other, false},
    {"/dev/nvhost-nvjpg", Dev::Other, false},
    {"/dev/nvhost-vic", Dev::Other, false},
    {"/dev/nvdisp-ctrl", Dev::Other, false},
    {"/dev/nvsched-ctrl-gpu", Dev::Other, false},
    {"/dev/nvhost-dbg-gpu", Dev::Other, true},
    {"/dev/nvhost-prof-gpu", Dev::Other, true},
};

const char* dev_name(Dev d) {
    switch (d) {
    case Dev::NvMap: return "nvmap";
    case Dev::Ctrl: return "nvhost-ctrl";
    case Dev::CtrlGpu: return "nvhost-ctrl-gpu";
    case Dev::AsGpu: return "nvhost-as-gpu";
    case Dev::Gpu: return "nvhost-gpu";
    default: return "other";
    }
}

// The as-gpu regions (Ryujinx AddressSpaceContext): small pages below 16 GiB, big pages above it.
struct Region {
    uint64_t start, limit, page;
};
constexpr Region kRegions[2] = {{0x100000000ull, 0x400000000ull, 0x1000}, {0x400000000ull, 0x2000000000ull, 0x10000}};

const Region& region_for(uint64_t page) { return page == 0x10000 ? kRegions[1] : kRegions[0]; }

// The first run of `size` bytes in [lo, hi) that no range of `ranges` covers, aligned to `align`; 0 when there is none.
uint64_t find_gap(const std::map<uint64_t, uint64_t>& ranges, uint64_t lo, uint64_t hi, uint64_t size, uint64_t align) {
    if (align == 0) align = 1;
    uint64_t cand = (lo + align - 1) / align * align;
    for (const auto& [start, len] : ranges) {
        if (start + len <= cand) continue;
        if (cand + size <= start) break;
        cand = (start + len + align - 1) / align * align;
    }
    return cand + size <= hi ? cand : 0;
}

bool range_free(const std::map<uint64_t, uint64_t>& ranges, uint64_t start, uint64_t size) {
    for (const auto& [s, len] : ranges)
        if (s < start + size && start < s + len) return false;
    return true;
}

// The ioctl-number fields (Linux _IOC layout, Switchbrew NV_services): direction (bit 30 copies in, bit 31 copies out), size, type, nr.
void log_ioctl(const NvFile& f, uint32_t cmd, uint32_t size, uint32_t dir, bool first) {
    if (first)
        NXLOG_I(kTagNv, "ioctl %s cmd %08x (type %02x nr %02x) size %u dir %u", dev_name(f.dev), cmd, (cmd >> 8) & 0xFF, cmd & 0xFF,
                size, dir);
    NXLOG_T(kTagNv, "ioctl %s cmd %08x size %u", dev_name(f.dev), cmd, size);
}

// ---- nvmap (type 0x01)

uint32_t nvmap_ioctl(uint32_t nr, Arg a, bool& handled) {
    NvState& s = nv();
    switch (nr) {
    case 0x01: {   // CREATE {u32 size; out u32 handle}: size rounded up to a page
        const uint32_t size = a.u32(0);
        if (!size) return kNvInvalidValue;
        NvMapObj m;
        m.size = static_cast<uint32_t>((static_cast<uint64_t>(size) + 0xFFF) & ~static_cast<uint64_t>(0xFFF));
        const uint32_t h = s.next_handle++;
        if (s.next_handle == 0) s.next_handle = 1;
        s.maps[h] = m;
        a.put32(4, h);
        return kNvOk;
    }
    case 0x03: {   // FROM_ID {u32 id; out u32 handle}: the id is the handle
        auto it = s.maps.find(a.u32(0));
        if (it == s.maps.end()) return kNvInvalidValue;
        it->second.refs++;
        a.put32(4, it->first);
        return kNvOk;
    }
    case 0x04: {   // ALLOC {u32 handle, heap, flags; u32 align (inout); u8 kind at 16; u64 addr at 24}
        auto it = s.maps.find(a.u32(0));
        if (it == s.maps.end()) return kNvInvalidValue;
        NvMapObj& m = it->second;
        uint32_t align = a.u32(12);
        if (align & (align - 1)) return kNvInvalidValue;
        if (align < 0x1000) align = 0x1000;
        if (!m.allocated) {
            m.allocated = true;
            m.heap = a.u32(4);
            m.flags = a.u32(8);
            m.kind = a.u32(16) & 0xFF;
            m.addr = a.u64(24);
        }
        m.align = align;
        a.put32(12, align);
        return kNvOk;
    }
    case 0x05: {   // FREE {u32 handle; pad; out u64 address at 8, u32 size at 16, u32 flags at 20}: flags 1 = not freed yet
        auto it = s.maps.find(a.u32(0));
        if (it == s.maps.end()) return kNvInvalidValue;
        NvMapObj& m = it->second;
        if (m.refs > 0) m.refs--;
        const bool freed = m.refs == 0;
        a.put64(8, freed ? m.addr : 0);
        a.put32(16, m.size);
        a.put32(20, freed ? 0 : 1);
        if (freed) s.maps.erase(it);
        return kNvOk;
    }
    case 0x09: {   // PARAM {u32 handle, param; out u32 result}: 1 size, 2 align, 4 heap, 5 kind, 6 compr
        auto it = s.maps.find(a.u32(0));
        if (it == s.maps.end()) return kNvInvalidValue;
        const NvMapObj& m = it->second;
        uint32_t v = 0;
        switch (a.u32(4)) {
        case 1: v = m.size; break;
        case 2: v = m.align; break;
        case 4: v = 0x40000000; break;
        case 5: v = m.kind; break;
        case 6: v = 0; break;
        default: return kNvInvalidValue;   // Base (3) and anything else
        }
        a.put32(8, v);
        return kNvOk;
    }
    case 0x0e: {   // GET_ID {out u32 id; u32 handle}
        const uint32_t h = a.u32(4);
        if (s.maps.find(h) == s.maps.end()) return kNvInvalidValue;
        a.put32(0, h);
        return kNvOk;
    }
    case 0x02:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x0a:
    case 0x0b:
    case 0x0c:
    case 0x0d:
    case 0x0f:
    case 0x10:
    case 0x11: return kNvNotSupported;   // CLAIM, MMAP, WRITE, READ, PIN, UNPIN, CACHE, IVC, RESERVE: NotSupported on the console
    default:
        handled = false;
        return kNvOk;
    }
}

// ---- nvhost-ctrl (type 0x00)

uint32_t ctrl_ioctl(uint32_t nr, Arg a, bool& handled) {
    switch (nr) {
    case 0x14:   // SYNCPT_READ {u32 id; out u32 value}: zeros
        if (a.u32(0) >= kSyncptCount) return kNvInvalidValue;
        a.put32(4, 0);
        return kNvOk;
    case 0x15:   // SYNCPT_INCR {u32 id}
        if (a.u32(0) >= kSyncptCount) return kNvInvalidValue;
        return kNvOk;
    case 0x16:   // SYNCPT_WAIT {u32 id, thresh; i32 timeout}: the threshold counts as reached
        if (a.u32(0) >= kSyncptCount) return kNvInvalidValue;
        return kNvOk;
    case 0x19:   // SYNCPT_WAITEX {u32 id, thresh; i32 timeout; out u32 value at 12}
        if (a.u32(0) >= kSyncptCount) return kNvInvalidValue;
        a.put32(12, 0);
        return kNvOk;
    case 0x1a:   // SYNCPT_READ_MAX {u32 id; out u32 max}
        if (a.u32(0) >= kSyncptCount) return kNvInvalidValue;
        a.put32(4, 0);
        return kNvOk;
    case 0x1b:   // GET_CONFIG: a retail console answers NotAvailableInProduction (Switchbrew; Ryujinx's note on the same ioctl)
        return kNvNotAvailableInProduction;
    case 0x1c:   // SYNCPT_CLEAR_EVENT_WAIT {u32 slot}
    case 0x1f:   // SYNCPT_ALLOC_EVENT {u32 slot}
    case 0x20:   // SYNCPT_FREE_EVENT {u32 slot}
    case 0x21:   // SYNCPT_FREE_EVENT_BATCH {u64 mask}
        return kNvOk;
    case 0x1d:   // SYNCPT_WAIT_EVENT {u32 id, thresh; i32 timeout; out u32 value at 12}
    case 0x1e:   // SYNCPT_WAIT_EVENT_EX, the same layout
        a.put32(12, 0);
        return kNvOk;
    default:
        handled = false;
        return kNvOk;
    }
}

// ---- nvhost-ctrl-gpu (type 0x47): the values of a Tegra X1 (GM20B), the same words Ryujinx and ScarletNX report

void write_characteristics(uint8_t* c) {   // 0xA0 bytes: GpuCharacteristics, 64-bit fields as low word then high word
    static const uint32_t words[40] = {
        0x120, 0xB, 0xA1, 1,                      // arch, impl, rev, GPC count
        0x40000, 0, 0, 0,                         // L2 cache size, on-board video memory (u64s)
        2, 0x20, 0x20000, 0x20000,                // TPCs per GPC, bus type, big page size, compression page size
        0x1B, 0x30000, 1, 0x503,                  // PDE coverage bits, available big page sizes, GPC mask, SM version
        0x503, 0x80, 0x28, 0,                     // SPA version, warp count, GPU VA bits, reserved
        0x55, 0,                                  // flags (u64)
        0x902D, 0xB197, 0xB1C0, 0xB06F,           // 2D, 3D, compute, GPFIFO classes
        0xA140, 0xB0B5,                           // inline-to-memory, DMA copy classes
        1, 0, 2, 1,                               // FBPs, FBP enable mask, LTCs per FBP, LTSs per LTC
        0, 1, 0x21D70, 0,                         // textures per TPC, GPCs, ROP L2 enable masks
        0x30326D67, 0x62,                         // chip name "gm20b" (u64)
        0, 0,                                     // compression bit store base (u64)
    };
    std::memcpy(c, words, sizeof words);
}

uint32_t ctrlgpu_ioctl(uint32_t nr, Arg a, Arg xout, bool& handled) {
    switch (nr) {
    case 0x01:   // ZCULL_GET_CTX_SIZE {out u32 size}
        a.put32(0, 1);
        return kNvOk;
    case 0x02: {   // ZCULL_GET_INFO: ten u32s
        static const uint32_t info[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
        for (uint32_t i = 0; i < 10; ++i) a.put32(i * 4, info[i]);
        return kNvOk;
    }
    case 0x03:   // ZBC_SET_TABLE: accepted
        return kNvOk;
    case 0x05: {   // GET_CHARACTERISTICS {u64 buf_size at 0, u64 buf_addr at 8, GpuCharacteristics at 16}; also copied to an inline out
        uint8_t c[0xA0];
        write_characteristics(c);
        a.put64(0, sizeof c);
        if (a.p && a.n >= 16 + sizeof c) std::memcpy(a.p + 16, c, sizeof c);
        if (xout.p && xout.n >= sizeof c) std::memcpy(xout.p, c, sizeof c);
        return kNvOk;
    }
    case 0x06: {   // GET_TPC_MASKS {u32 mask_buf_size; pad; u64 mask_buf_addr; out u32 mask at 16}
        if (a.u32(0) != 0) {
            const uint32_t mask = 3;   // TPC 0 and 1 present
            a.put32(16, mask);
            if (xout.p && xout.n >= 4) std::memcpy(xout.p, &mask, 4);
        }
        return kNvOk;
    }
    case 0x1c: {   // GET_GPU_TIME {out u64 timestamp; reserved}: nanoseconds since the first call, 56 bits as the device has them
        static const auto t0 = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        a.put64(0, static_cast<uint64_t>(ns) & 0xFFFFFFFFFFFFFFull);
        return kNvOk;
    }
    default:
        handled = false;
        return kNvOk;
    }
}

// ---- nvhost-as-gpu (type 0x41): a simple address space, first fit in the regions, reservations and mappings in one map

uint32_t as_ioctl(NvFile& f, uint32_t nr, Arg a, bool& handled) {
    NvState& s = nv();
    switch (nr) {
    case 0x01:   // BIND_CHANNEL {i32 fd}
        return kNvOk;
    case 0x02: {   // ALLOC_SPACE {u32 pages, page_size, flags; u32 pad; u64 offset at 16 (the alignment when not fixed)}
        const uint64_t pages = a.u32(0), page = a.u32(4), size = pages * page;
        const uint32_t flags = a.u32(8);
        const uint64_t want = a.u64(16);
        if (!size) return kNvInvalidValue;
        const Region& rg = region_for(page);
        uint64_t got = 0;
        if (flags & 1) {   // FIXED_OFFSET
            if (want >= rg.start && want + size <= rg.limit && range_free(f.ranges, want, size)) got = want;
        } else {
            got = find_gap(f.ranges, rg.start, rg.limit, size, want ? want : page);
        }
        if (!got) {
            a.put64(16, 0);
            NXLOG_W(kTagNv, "ALLOC_SPACE of 0x%llx bytes has no room", static_cast<unsigned long long>(size));
            return kNvInsufficientMemory;
        }
        f.ranges[got] = size;
        a.put64(16, got);
        return kNvOk;
    }
    case 0x03: {   // FREE_SPACE {u64 offset; u32 pages, page_size}
        const uint64_t off = a.u64(0);
        auto it = f.ranges.find(off);
        if (it == f.ranges.end()) return kNvInvalidValue;
        f.ranges.erase(it);
        return kNvOk;
    }
    case 0x05:   // UNMAP_BUFFER {u64 offset}: unknown offsets are accepted, as Ryujinx does
        f.ranges.erase(a.u64(0));
        return kNvOk;
    case 0x06: {   // MAP_BUFFER_EX {u32 flags; i32 kind; u32 nvmap; i32 page_size; u64 buffer_off; u64 map_size; u64 offset at 32}
        const uint32_t flags = a.u32(0), handle = a.u32(8), page_arg = a.u32(12);
        const uint64_t map_size_arg = a.u64(24), want = a.u64(32);
        auto it = s.maps.find(handle);
        if (it == s.maps.end()) return kNvInvalidValue;
        const NvMapObj& m = it->second;
        const uint64_t size = map_size_arg ? map_size_arg : m.size;
        const uint64_t page = page_arg ? page_arg : (m.align ? m.align : 0x1000);
        const Region& rg = region_for(page);
        uint64_t va = 0;
        if (flags & 1) {
            if (want >= rg.start && want + size <= rg.limit && range_free(f.ranges, want, size)) va = want;
        } else {
            va = find_gap(f.ranges, rg.start, rg.limit, size, page);
        }
        if (!va) {
            a.put64(32, 0);
            return kNvInvalidValue;
        }
        f.ranges[va] = size;
        a.put64(32, va);
        return kNvOk;
    }
    case 0x08: {   // GET_VA_REGIONS {u64; u32 buffer_size at 8; pad; {u64 offset, u32 page, u32 pad, u64 pages} x2 at 16}
        const uint32_t entries = std::min<uint32_t>(a.u32(8) / 24, 2);
        for (uint32_t i = 0; i < entries; ++i) {
            const uint64_t base = 16 + 24 * static_cast<uint64_t>(i);
            const Region& rg = kRegions[i];
            a.put64(base, rg.start);
            a.put32(base + 8, static_cast<uint32_t>(rg.page));
            a.put32(base + 12, 0);
            a.put64(base + 16, (rg.limit - rg.start) / rg.page);
        }
        a.put32(8, 48);
        return kNvOk;
    }
    case 0x09: {   // INITIALIZE_EX {u32 flags, i32 as_fd, u32 big_page_size at 8, ...}
        const uint32_t big = a.u32(8);
        f.big_page = big ? big : 0x20000;
        return kNvOk;
    }
    default:
        handled = false;
        return kNvOk;
    }
}

// ---- nvhost-gpu (type 0x48 the channel, 0x47 SET_USER_DATA): the channel's bookkeeping; submission is the GPU back end's

uint32_t gpu_ioctl(NvFile& f, uint32_t nr, Arg a, bool& handled) {
    NvState& s = nv();
    switch (nr) {
    case 0x01:   // SET_NVMAP_FD {i32}
        return kNvOk;
    case 0x08:   // SUBMIT_GPFIFO
    case 0x1b:   // SUBMIT_GPFIFO through Ioctl2: answered, never run here
        if (!s.submit_logged) {
            s.submit_logged = true;
            NXLOG_W(kTagNv, "SUBMIT_GPFIFO accepted and not run: the GPU back end owns command submission");
        }
        return kNvOk;
    case 0x09:   // ALLOC_OBJ_CTX {u32 class, flags; out u64 object id at 8}: the id is the class number, as ScarletNX answers
        a.put64(8, a.u32(0));
        return kNvOk;
    case 0x16:   // GET_ERROR_NOTIFIER: nothing has gone wrong, so the notifier is all zeros
        if (a.p) std::memset(a.p, 0, static_cast<size_t>(a.n));
        return kNvOk;
    case 0x1a:   // ALLOC_GPFIFO_EX2 {u32 entries, jobs, flags; NvFence {u32 id at 12, u32 value at 16}; reserved}
        if (!f.syncpt) f.syncpt = s.next_syncpt++;
        a.put32(12, f.syncpt);
        a.put32(16, 0);
        return kNvOk;
    default:
        handled = false;
        return kNvOk;
    }
}

// Dispatch on the device, then on the command word's type and number. xout is the inline output of Ioctl3 (empty otherwise).
uint32_t dispatch(NvFile& f, uint32_t cmd, Arg a, Arg xout, bool& handled) {
    const uint32_t type = (cmd >> 8) & 0xFF, nr = cmd & 0xFF;
    handled = true;
    switch (f.dev) {
    case Dev::NvMap:
        if (type == 0x01) return nvmap_ioctl(nr, a, handled);
        break;
    case Dev::Ctrl:
        if (type == 0x00) return ctrl_ioctl(nr, a, handled);
        break;
    case Dev::CtrlGpu:
        if (type == 0x47) return ctrlgpu_ioctl(nr, a, xout, handled);
        break;
    case Dev::AsGpu:
        if (type == 0x41) return as_ioctl(f, nr, a, handled);
        break;
    case Dev::Gpu:
        if (type == 0x48) return gpu_ioctl(f, nr, a, handled);
        if (type == 0x47 && nr == 0x14) {   // SET_USER_DATA {u64}
            f.user_data = a.u64(0);
            return kNvOk;
        }
        break;
    default: break;
    }
    handled = false;
    return kNvOk;
}

// Ioctl, Ioctl2 and Ioctl3 share this: the command's direction and size decide what is copied in and out (as Ryujinx's
// GetIoctlArgument checks it). xout is Ioctl3's inline output buffer, read and written in place; Ioctl2's inline input is unused.
uint32_t ioctl_call(uint32_t fd, uint32_t cmd, const ipc::Buffer& in, const ipc::Buffer& out, const ipc::Buffer& xout) {
    NvState& s = nv();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.initialized) return kNvNotInitialized;
    if (static_cast<int32_t>(fd) < 0) return kNvInvalidParameter;
    auto it = s.files.find(fd);
    if (it == s.files.end()) return kNvNotImplemented;
    NvFile& f = *it->second;

    const uint32_t dir = cmd >> 30, size = (cmd >> 16) & 0x3FFF;
    const bool rd = dir & 1, wr = dir & 2;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(f.dev)) << 32) | cmd;
    log_ioctl(f, cmd, size, dir, s.seen.insert(key).second);
    if ((wr && size > out.size) || (rd && size > in.size)) {
        NXLOG_W(kTagNv, "ioctl size inconsistency on cmd %08x", cmd);
        return kNvInvalidSize;
    }
    std::vector<uint8_t> v(size, 0);
    if (rd) buf_read(in, v.data(), size);
    bool handled = false;
    uint32_t err = dispatch(f, cmd, Arg{v.data(), size}, arg_of(xout), handled);
    if (!handled) {
        if (s.warned.insert(key).second)
            NXLOG_W(kTagNv, "UNHANDLED IOCTL on %s: cmd %08x (type %02x nr %02x size %u), answered with zeros", dev_name(f.dev), cmd,
                    (cmd >> 8) & 0xFF, cmd & 0xFF, size);
        std::fill(v.begin(), v.end(), 0);
        err = kNvOk;
    }
    if (wr) buf_write(out, v.data(), size);
    return err;
}

// Open(buffer path) -> (i32 fd, u32 error)
uint32_t nv_open(ipc::Request& r, ipc::Response& rsp) {
    NvState& s = nv();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.initialized) {
        rsp.push<int32_t>(-1);
        rsp.push<uint32_t>(kNvNotInitialized);
        return 0;
    }
    const std::string path = c_string(r.in_buf(0));
    for (const DevEntry& e : kDevices) {
        if (path != e.path) continue;
        if (e.debug_only) {
            rsp.push<int32_t>(-1);
            rsp.push<uint32_t>(kNvNotSupported);
            return 0;
        }
        const uint32_t fd = s.next_fd++;
        auto file = std::make_shared<NvFile>();
        file->dev = e.dev;
        s.files[fd] = file;
        rsp.push<uint32_t>(fd);
        rsp.push<uint32_t>(kNvOk);
        return 0;
    }
    NXLOG_W(kTagNv, "cannot find device \"%s\"", path.c_str());
    rsp.push<int32_t>(-1);
    rsp.push<uint32_t>(kNvFileOperationFailed);
    return 0;
}

uint32_t nv_close(ipc::Request& r, ipc::Response& rsp) {
    NvState& s = nv();
    std::lock_guard<std::mutex> g(s.mu);
    uint32_t err = kNvOk;
    const int32_t fd = static_cast<int32_t>(r.arg<uint32_t>(0));
    if (!s.initialized) err = kNvNotInitialized;
    else if (fd < 0) err = kNvInvalidParameter;
    else {
        auto it = s.files.find(static_cast<uint32_t>(fd));
        if (it == s.files.end()) err = kNvNotImplemented;
        else s.files.erase(it);
    }
    rsp.push<uint32_t>(err);
    return 0;
}

// QueryEvent(fd, id) -> (u32 error, copy handle). The ctrl device keys its events by slot, the gpu ones by id.
uint32_t nv_query_event(uint32_t fd, uint32_t id, ipc::Response& rsp) {
    NvState& s = nv();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.initialized) return kNvNotInitialized;
    auto it = s.files.find(fd);
    if (it == s.files.end()) return kNvNotImplemented;
    NvFile& f = *it->second;
    uint32_t key = 0;
    switch (f.dev) {
    case Dev::Ctrl: {
        const uint32_t slot = (id >> 28) == 1 ? (id & 0xFFFF) : (id & 0xFF);
        if (slot >= 64) return kNvInvalidValue;
        key = slot;
        break;
    }
    case Dev::CtrlGpu:
        if (id != 1 && id != 2) return kNvInvalidValue;   // ErrorEvent, SemaphoreEvent
        key = id;
        break;
    case Dev::Gpu:
        if (id < 1 || id > 3) return kNvInvalidValue;     // BptIntEvent, BptPauseEvent, ErrorNotifierEvent
        key = id;
        break;
    default: return kNvNotSupported;
    }
    auto& ev = f.events[key];
    if (!ev) ev = std::make_shared<ServiceEvent>();
    const uint32_t h = ev->new_handle();
    if (!h) return kNvInsufficientMemory;
    rsp.out_copy.push_back(h);
    return kNvOk;
}

}  // namespace

void setup_nvdrv(ipc::Service& s) {
    s.add_cmd(0, "Open", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t { return nv_open(r, rsp); });
    s.add_cmd(1, "Ioctl", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(ioctl_call(r.arg<uint32_t>(0), r.arg<uint32_t>(4), r.in_buf(0), r.out_buf(0), ipc::Buffer{}));
        return 0;
    });
    s.add_cmd(2, "Close", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t { return nv_close(r, rsp); });
    s.add_cmd(3, "Initialize", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        // (u32 size, process and transfer memory handles): the handles are not used, as Ryujinx does
        (void)r;
        std::lock_guard<std::mutex> g(nv().mu);
        nv().initialized = true;
        rsp.push<uint32_t>(kNvOk);
        return 0;
    });
    s.add_cmd(4, "QueryEvent", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(nv_query_event(r.arg<uint32_t>(0), r.arg<uint32_t>(4), rsp));
        return 0;
    });
    s.add_cmd(6, "GetStatus", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> g(nv().mu);
        if (!nv().initialized) {
            rsp.push<uint32_t>(kNvNotInitialized);
            return 0;
        }
        for (int i = 0; i < 8; ++i) rsp.push<uint32_t>(0);   // NvDrvStatus: four u32s and 0x10 reserved bytes, all zero, then the error
        rsp.push<uint32_t>(kNvOk);
        return 0;
    });
    s.add_cmd(7, "SetAruidWithoutCheck", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(kNvOk);
        return 0;
    });
    s.add_cmd(8, "SetAruid", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(kNvOk);
        return 0;
    });
    s.add_cmd(9, "DumpStatus", [](ipc::Request&, ipc::Response&) -> uint32_t {
        NXLOG_D(kTagNv, "DumpStatus");
        return 0;
    });
    s.add_cmd(11, "Ioctl2", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(ioctl_call(r.arg<uint32_t>(0), r.arg<uint32_t>(4), r.in_buf(0), r.out_buf(0), ipc::Buffer{}));
        return 0;
    });
    s.add_cmd(12, "Ioctl3", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(ioctl_call(r.arg<uint32_t>(0), r.arg<uint32_t>(4), r.in_buf(0), r.out_buf(0), r.out_buf(1)));
        return 0;
    });
    s.add_cmd(13, "SetGraphicsFirmwareMemoryMarginEnabled", [](ipc::Request&, ipc::Response&) -> uint32_t {
        NXLOG_D(kTagNv, "SetGraphicsFirmwareMemoryMarginEnabled accepted");
        return 0;
    });
}

}  // namespace nxrt::services::vinv
