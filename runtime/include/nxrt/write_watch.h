// SPDX-License-Identifier: GPL-3.0-or-later
// Page write tracking of guest memory (S38). The owner of a region "arms" pages: they become read-only for the CPU, and the first
// write to one of them faults; a vectored exception handler makes the page writable again, marks it written and lets the write go on.
// The owner (the NVN back end keeps device-local copies of pool pages, nvn_gpu_pool.cpp) then knows which pages changed without reading
// them. Only the CPU's view is protected: the GPU reaches the same physical pages through its own import and never faults.
//
// Limits, all handled here or by the callers named:
//  * Regions lie in the heap's primary view (mem::kHeapBase). A page the guest can also reach through an alias view (svcMapMemory and
//    friends, mem::heap_alias) is banned: a write through the alias would not fault. memory.cpp calls forget() for every change of
//    mapping or protection it makes, so an armed page never ends up with a protection somebody else chose.
//  * Writes by the operating system into guest memory (ReadFile into a buffer) do not raise an exception: the call fails instead. The file
//    readers call before_os_write() on the destination first (services/fs_*.cpp).
//  * Granularity is the 4 KiB page.
#pragma once
#include <cstdint>
#include <vector>

namespace nxrt::mem::ww {

struct Region;

// A region over [addr, addr + size) (page aligned down/up). Null when the range is not inside the heap's primary view or the tracker is
// unavailable. Any thread; the region has one owner thread for the calls below marked "owner".
Region* region_create(uint64_t addr, uint64_t size);
// Makes every armed page writable again and frees the region. Owner.
void region_destroy(Region* r);
uint64_t region_base(const Region* r);
uint64_t region_pages(const Region* r);

// Owner: arms pages [first, first + count): read-only, and valid until written. False when a page could not be protected (not committed
// read-write, banned): nothing in the run is armed then.
bool arm(Region* r, uint64_t first, uint64_t count);
// Owner: true when every page of the run is armed and nobody wrote it since.
bool all_valid(const Region* r, uint64_t first, uint64_t count);
bool page_valid(const Region* r, uint64_t page);
bool page_banned(const Region* r, uint64_t page);
// Owner: the owner changed the pages itself (the GPU wrote them): no longer valid, still armed (the next arm() re-validates them).
void invalidate(Region* r, uint64_t first, uint64_t count);
// Owner: appends the pages written by the CPU since the last call (page indices) and clears that record.
void take_written(Region* r, std::vector<uint64_t>& out);

// Any thread: the operating system is about to write [p, p + n) (a file read into guest memory): armed pages in the range become writable
// and count as written. Cheap when no region exists.
void before_os_write(const void* p, uint64_t n);
// The same for the lifetime of the object, and no page of the range is armed again until it ends: hold it around the ReadFile calls,
// and memory.cpp holds it around every change of mapping or protection it makes.
class OsWriteScope {
public:
    OsWriteScope(const void* p, uint64_t n);
    ~OsWriteScope();
    OsWriteScope(const OsWriteScope&) = delete;
    OsWriteScope& operator=(const OsWriteScope&) = delete;
private:
    int slot_ = -1;
};
// memory.cpp: the mapping or protection of [addr, addr + size) is about to change. Armed pages there are made read-write again and
// forgotten (no longer valid or armed); memory.cpp then sets its own protection. ban: they can never be armed again (another view of
// them exists).
void forget(uint64_t addr, uint64_t size, bool ban);
// Section offsets [offset, offset + size) got another view (memory.cpp, heap_alias): those pages are banned in every region.
void ban_section(uint64_t offset, uint64_t size);

uint64_t faults();     // write faults handled so far (statistics)
uint64_t fault_waits(); // rounds a fault spent waiting for another thread's release of the same page (statistics; stays 0 for foreign faults)

}  // namespace nxrt::mem::ww
