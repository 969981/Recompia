// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_diag.cpp: what the diag layer and the scheduler ask of src/os/nx_os.h -- the debugger check, thread and process
// times, the memory counters, thread snapshots, MMCSS (none), the processor number and topology, and the IsBad*Ptr walks.
#include "nx_os_internal.h"

#if !defined(_WIN32)

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task_info.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace nxos;

// ==================================================================================================================
// diagnostics
// ==================================================================================================================
BOOL IsDebuggerPresent(void) {
#if defined(__APPLE__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(getpid())};
    struct kinfo_proc info{};
    size_t size = sizeof info;
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) return FALSE;
    return (info.kp_proc.p_flag & P_TRACED) ? TRUE : FALSE;
#elif defined(__linux__)
    // /proc/self/status "TracerPid": non-zero while a debugger is attached.
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    char line[256];
    BOOL traced = FALSE;
    while (std::fgets(line, sizeof line, f)) {
        long pid = 0;
        if (std::sscanf(line, "TracerPid: %ld", &pid) == 1) {
            traced = pid != 0 ? TRUE : FALSE;
            break;
        }
    }
    std::fclose(f);
    return traced;
#else
    return FALSE;
#endif
}

void DebugBreak(void) { __builtin_trap(); }

BOOL QueryThreadCycleTime(HANDLE h, PULONGLONG cycles) {
    if (!cycles) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ThreadObj* t = (h == reinterpret_cast<HANDLE>(static_cast<intptr_t>(kPseudoThread))) ? self_thread()
                                                                                        : as<ThreadObj>(h, Kind::Thread);
    if (!t) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (t->th == pthread_self()) {
        struct timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        *cycles = static_cast<ULONGLONG>(ts.tv_sec) * 1000000000ull + static_cast<ULONGLONG>(ts.tv_nsec);
        return TRUE;
    }
    *cycles = t->cpu_ns;   // written when the thread exits; a running thread reports its last known value
    return TRUE;
}

BOOL GetThreadTimes(HANDLE, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel_t, LPFILETIME user_t) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    const uint64_t cpu = static_cast<uint64_t>(ts.tv_sec) * 10000000ull + static_cast<uint64_t>(ts.tv_nsec) / 100ull;
    if (created) set_filetime(created, 0);
    if (exited) set_filetime(exited, 0);
    if (kernel_t) set_filetime(kernel_t, 0);
    if (user_t) set_filetime(user_t, cpu);
    return TRUE;
}

BOOL GetProcessTimes(HANDLE, LPFILETIME created, LPFILETIME exited, LPFILETIME kernel_t, LPFILETIME user_t) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (created) set_filetime(created, filetime_of(ts));
    if (exited) set_filetime(exited, 0);
    struct rusage ru{};
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        if (kernel_t)
            set_filetime(kernel_t, static_cast<uint64_t>(ru.ru_stime.tv_sec) * 10000000ull +
                                       static_cast<uint64_t>(ru.ru_stime.tv_usec) * 10ull);
        if (user_t)
            set_filetime(user_t, static_cast<uint64_t>(ru.ru_utime.tv_sec) * 10000000ull +
                                     static_cast<uint64_t>(ru.ru_utime.tv_usec) * 10ull);
    }
    return TRUE;
}

BOOL GetProcessMemoryInfo(HANDLE, PROCESS_MEMORY_COUNTERS* pmc, DWORD) {
    if (!pmc) return FALSE;
#if defined(__APPLE__)
    // phys_footprint is what the OS charges the process (Activity Monitor's "Memory"): resident pages plus compressed and the
    // IOKit/Metal memory the GPU driver holds for it. getrusage's ru_maxrss is a *peak* of resident pages only, so a guard
    // built on it sees neither the current size nor the video memory -- on a 16 GiB host the pools alone are ~5 GiB.
    task_vm_info_data_t vi{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vi), &count) == KERN_SUCCESS) {
        const SIZE_T fp = static_cast<SIZE_T>(vi.phys_footprint);
        pmc->WorkingSetSize = static_cast<SIZE_T>(vi.resident_size);
        pmc->PeakWorkingSetSize = static_cast<SIZE_T>(vi.resident_size_peak);
        pmc->PagefileUsage = fp;
        pmc->PeakPagefileUsage = fp;   // task_vm_info has no peak footprint; memguard keeps its own peak
        if (pmc->cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX))
            static_cast<PROCESS_MEMORY_COUNTERS_EX*>(static_cast<void*>(pmc))->PrivateUsage = fp;
        return TRUE;
    }
#elif defined(__linux__)
    // /proc/self/status has both numbers the guard needs: VmRSS (now) and VmHWM (the peak). getrusage's ru_maxrss is the
    // peak only, so a guard built on it would report a high-water mark that never comes back down.
    if (std::FILE* f = std::fopen("/proc/self/status", "r")) {
        SIZE_T rss = 0, peak = 0;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            unsigned long long kb = 0;
            if (std::sscanf(line, "VmRSS: %llu kB", &kb) == 1) rss = static_cast<SIZE_T>(kb) * 1024;
            else if (std::sscanf(line, "VmHWM: %llu kB", &kb) == 1) peak = static_cast<SIZE_T>(kb) * 1024;
        }
        std::fclose(f);
        if (rss || peak) {
            if (!peak) peak = rss;
            pmc->WorkingSetSize = rss;
            pmc->PeakWorkingSetSize = peak;
            pmc->PagefileUsage = rss;
            pmc->PeakPagefileUsage = peak;
            if (pmc->cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX))
                static_cast<PROCESS_MEMORY_COUNTERS_EX*>(static_cast<void*>(pmc))->PrivateUsage = rss;
            return TRUE;
        }
    }
#endif
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    const SIZE_T rss = static_cast<SIZE_T>(ru.ru_maxrss);   // bytes on Darwin, KiB on Linux
#if defined(__APPLE__)
    const SIZE_T rss_bytes = rss;
#else
    const SIZE_T rss_bytes = rss * 1024;
#endif
    pmc->WorkingSetSize = rss_bytes;
    pmc->PeakWorkingSetSize = rss_bytes;
    pmc->PagefileUsage = rss_bytes;
    pmc->PeakPagefileUsage = rss_bytes;
    if (pmc->cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX)) {
        static_cast<PROCESS_MEMORY_COUNTERS_EX*>(static_cast<void*>(pmc))->PrivateUsage = rss_bytes;
    }
    return TRUE;
}

DWORD GetGuiResources(HANDLE, DWORD) { return 0; }

// ==================================================================================================================
// thread snapshots (diag/gauge.cpp counts threads)
// ==================================================================================================================
HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD) {
    if (!(flags & TH32CS_SNAPTHREAD)) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return nullptr;
    }
    auto* s = new SnapshotObj();
    s->tids.push_back(self_tid());
    return s;
}

BOOL Thread32First(HANDLE h, PTHREADENTRY32 te) {
    auto* s = as<SnapshotObj>(h, Kind::Snapshot);
    if (!s || !te) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    s->at = 0;
    te->dwSize = sizeof(THREADENTRY32);
    te->th32OwnerProcessID = GetCurrentProcessId();
    te->th32ThreadID = s->tids[0];
    return TRUE;
}

BOOL Thread32Next(HANDLE h, PTHREADENTRY32 te) {
    auto* s = as<SnapshotObj>(h, Kind::Snapshot);
    if (!s || !te) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (++s->at >= s->tids.size()) {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    te->th32ThreadID = s->tids[s->at];
    return TRUE;
}

// <avrt.h> declares these inside extern "C": the definitions must match, or the call sites link to C++-mangled names.
extern "C" {
HANDLE AvSetMmThreadCharacteristicsW(LPCWSTR, LPDWORD index) {
    // No MMCSS on macOS: the caller (services/audren/Integration/ThreadBoost.h) falls back to SetThreadPriority, which
    // maps to a QoS class and is what actually raises an audio thread's priority here.
    if (index) *index = 0;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return nullptr;
}

HANDLE AvSetMmThreadCharacteristicsA(LPCSTR, LPDWORD index) { return AvSetMmThreadCharacteristicsW(nullptr, index); }
BOOL AvRevertMmThreadCharacteristics(HANDLE) { return TRUE; }
BOOL AvSetMmThreadPriority(HANDLE, int) { return TRUE; }
}

// ==================================================================================================================
// processor topology and the odds and ends the diag layer reaches for
// ==================================================================================================================
BOOL GetThreadIdealProcessorEx(HANDLE, PPROCESSOR_NUMBER out) {
    // There is no "ideal processor" to read back on macOS: the scheduler decides. A caller that only wants a label
    // gets failure and prints nothing, which is better than a fabricated number in a diagnostics line.
    if (out) std::memset(out, 0, sizeof *out);
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

DWORD GetCurrentProcessorNumber(void) {
#if defined(__APPLE__)
    // thread_info(THREAD_BASIC_INFO) carries cpu_usage, not the cpu id; the portable answer is the mach thread's
    // last run-on processor through thread_get_assignment, which needs a thread port. Report 0 (the only CPU group).
    return 0;
#else
    const int cpu = sched_getcpu();
    return cpu > 0 ? static_cast<DWORD>(cpu) : 0;
#endif
}

DWORD GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER out) {
    if (out) {
        out->Group = 0;
        out->Number = static_cast<BYTE>(GetCurrentProcessorNumber());
        out->Reserved = 0;
    }
    return GetCurrentProcessorNumber();
}

BOOL SetThreadStackGuarantee(PULONG guarantee) {
    // The reserved stack the exception handlers may use on Windows. pthread stacks have no equivalent reservation, and
    // the crash path here runs on a BSD signal handler with its own altstack, so report success and leave the value.
    if (guarantee) *guarantee = 0;
    return TRUE;
}

BOOL IsBadReadPtr(LPCVOID addr, UINT_PTR size) {
    // kernel/svc_thread.cpp validates a guest stack pointer before walking it: walk the page table the same way
    // ReadProcessMemory does, but without copying.
    if (!addr) return TRUE;
    uintptr_t page = reinterpret_cast<uintptr_t>(addr);
    const uintptr_t end = page + (size ? size : 1);
    while (page < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(page), &mbi, sizeof mbi) != sizeof mbi) return TRUE;
        if (mbi.State != MEM_COMMIT) return TRUE;
        const DWORD p = mbi.Protect & 0xFF;
        if (p == PAGE_NOACCESS || p == 0) return TRUE;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= page) return TRUE;
        page = next;
    }
    return FALSE;
}

BOOL IsBadWritePtr(LPVOID addr, UINT_PTR size) {
    if (!addr) return TRUE;
    uintptr_t page = reinterpret_cast<uintptr_t>(addr);
    const uintptr_t end = page + (size ? size : 1);
    while (page < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(page), &mbi, sizeof mbi) != sizeof mbi) return TRUE;
        if (mbi.State != MEM_COMMIT) return TRUE;
        const DWORD p = mbi.Protect & 0xFF;
        if (p != PAGE_READWRITE && p != PAGE_WRITECOPY && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY)
            return TRUE;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= page) return TRUE;
        page = next;
    }
    return FALSE;
}

// The host's cores, one entry per core, in the shape kernel/sched.cpp reads. Windows groups processors above 64 per
// group; the shim keeps one group, which is what a Mac has.
BOOL GetLogicalProcessorInformationEx(int relationship, SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* buffer, PDWORD len) {
    if (!len) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (relationship != RelationProcessorCore) {
        *len = 0;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores <= 0) cores = 1;
    if (cores > 64) cores = 64;   // one GROUP_AFFINITY word

    const DWORD entry = sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);
    // macOS numbers the performance cores first (hw.perflevel0) and the efficiency cores after them (hw.perflevel1),
    // which is the order kernel/sched.cpp wants: a higher EfficiencyClass is a faster core.
    long performance = 0;
#if defined(__APPLE__)
    size_t perf_len = sizeof performance;
    if (sysctlbyname("hw.perflevel0.logicalcpu", &performance, &perf_len, nullptr, 0) != 0) performance = 0;
    if (performance <= 0 || performance > cores) performance = cores;
#endif
    // One entry per PHYSICAL core, its mask holding every hardware thread of that core, as the Windows API reports it:
    // sched.cpp's cores_fastest_first() takes one thread per entry and expects the entries to be distinct cores. One entry
    // per logical CPU made SMT siblings look like separate cores, so a 4-core/8-thread CPU offered 8 entries for 4 cores and
    // take_core_slot() picked 6 of them: two physical cores carried two console cores each while two carried one, the case
    // place_on_few_cores() exists for (#694). That fallback only runs when the core count is below the console's six, so the
    // entry count being inflated kept it from ever running. A CPU with a core per entry (6 cores, 12 threads) landed on six
    // distinct ones by numbering alone, so only the smaller CPUs were affected.
    struct SiblingGroup { DWORD_PTR mask; long first; };
    std::vector<SiblingGroup> groups;
#if defined(__linux__)
    for (long i = 0; i < cores; ++i) {
        // thread_siblings_list is "0,6" or a range such as "0-5": its first number is the group's lowest CPU, which keys the group.
        char path[96], buf[128] = {};
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/topology/thread_siblings_list", i);
        const DWORD_PTR own = static_cast<DWORD_PTR>(1) << i;
        long lowest = i;
        if (std::FILE* f = std::fopen(path, "rb")) {
            if (std::fgets(buf, sizeof buf, f)) lowest = std::strtol(buf, nullptr, 10);
            std::fclose(f);
        }
        if (lowest < 0 || lowest >= cores) lowest = i;   // unreadable, or a group key outside the mask: its own entry
        bool merged = false;
        for (SiblingGroup& g : groups)
            if (g.first == lowest) { g.mask |= own; merged = true; break; }
        if (!merged) groups.push_back({own, lowest});
    }
#endif
    if (groups.empty())      // no topology to read (macOS, or a kernel without sysfs): every logical CPU its own entry, as before
        for (long i = 0; i < cores; ++i) groups.push_back({static_cast<DWORD_PTR>(1) << i, i});
    std::stable_sort(groups.begin(), groups.end(), [](const SiblingGroup& a, const SiblingGroup& b) { return a.first < b.first; });
    const DWORD need = entry * static_cast<DWORD>(groups.size());
    if (!buffer || *len < need) {
        *len = need;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    auto* out = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer);
    for (size_t k = 0; k < groups.size(); ++k) {
        std::memset(out, 0, entry);
        out->Relationship = RelationProcessorCore;
        out->Size = entry;
        out->Processor.EfficiencyClass = groups[k].first < performance ? 1 : 0;
        out->Processor.GroupCount = 1;
        out->Processor.GroupMask[0].Mask = groups[k].mask;
        out = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(reinterpret_cast<unsigned char*>(out) + entry);
    }
    *len = need;
    return TRUE;
}

DWORD SetThreadIdealProcessorEx(HANDLE, const PROCESSOR_NUMBER* ideal, PDWORD previous) {
    // The hint is advisory on Windows and absent on macOS: the scheduler decides. Report the current processor as both
    // the previous and the new ideal, which is what the caller (kernel/sched.cpp) treats as "no change".
    const DWORD current = GetCurrentProcessorNumber();
    if (previous) *previous = current;
    (void)ideal;
    return current;
}

#endif  // !_WIN32
