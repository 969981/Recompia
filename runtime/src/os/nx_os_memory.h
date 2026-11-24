// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_memory.h: part of src/os/nx_os.h for the POSIX build -- virtual memory, sections and process memory (os_posix_mem.cpp).
#pragma once

#if !defined(NX_OS_H_PARTS)
#error "nx_os_memory.h is a part of nx_os.h: include nx_os.h"
#endif

// ---- memory ------------------------------------------------------------------------------------------------------
#define MEM_COMMIT 0x00001000
#define MEM_RESERVE 0x00002000
#define MEM_DECOMMIT 0x00004000
#define MEM_RELEASE 0x00008000
#define MEM_FREE 0x00010000
#define MEM_PRIVATE 0x00020000
#define MEM_MAPPED 0x00040000
#define MEM_RESET 0x00080000
#define MEM_TOP_DOWN 0x00100000
#define MEM_LARGE_PAGES 0x20000000

#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100
#define PAGE_NOCACHE 0x200
#define PAGE_WRITECOMBINE 0x400

#define SEC_FILE 0x00800000
#define SEC_IMAGE 0x01000000
#define SEC_RESERVE 0x04000000
#define SEC_COMMIT 0x08000000
#define SEC_NOCACHE 0x10000000
#define SEC_LARGE_PAGES 0x80000000

#define FILE_MAP_WRITE 0x0002
#define FILE_MAP_READ 0x0004
#define FILE_MAP_ALL_ACCESS 0x000F001F
#define FILE_MAP_COPY 0x00000001
#define FILE_MAP_EXECUTE 0x0020

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION, *LPMEMORY_BASIC_INFORMATION;

typedef struct _SYSTEM_INFO {
    union {
        DWORD dwOemId;
        struct {
            WORD wProcessorArchitecture;
            WORD wReserved;
        };
    };
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress;
    LPVOID lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;

#define PROCESSOR_ARCHITECTURE_ARM64 12
#define PROCESSOR_ARCHITECTURE_AMD64 9

typedef struct _MEMORYSTATUSEX {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    ULONGLONG ullTotalPhys;
    ULONGLONG ullAvailPhys;
    ULONGLONG ullTotalPageFile;
    ULONGLONG ullAvailPageFile;
    ULONGLONG ullTotalVirtual;
    ULONGLONG ullAvailVirtual;
    ULONGLONG ullAvailExtendedVirtual;
} MEMORYSTATUSEX, *LPMEMORYSTATUSEX;

LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
LPVOID VirtualAllocEx(HANDLE proc, LPVOID addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, PDWORD old_protect);
BOOL VirtualProtectEx(HANDLE proc, LPVOID addr, SIZE_T size, DWORD protect, PDWORD old_protect);
SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION* mbi, SIZE_T len);
SIZE_T VirtualQueryEx(HANDLE proc, LPCVOID addr, MEMORY_BASIC_INFORMATION* mbi, SIZE_T len);
// macOS: [start, end) of the executable's __GUEST segment (cmake/guest_segment.py, 0x7000000000 + 72 GiB, slid by dyld).
// False when the binary has none (a dylib, a unit test). The guest layout must fit inside it: see src/mem/memory.cpp.
// Android: the whole guest layout (nxrt/memory.h), reserved PROT_NONE by the first question. Linux: none.
bool nxos_guest_reservation(uint64_t* start, uint64_t* end);

// Gives this thread a signal stack (64 KiB), so a handler can run when the thread's own stack is exhausted. Without it a
// stack overflow is reported as a second fault inside the handler (the report then runs on the exhausted stack and dies),
// which is exactly the case the report is needed for. Idempotent; the main thread gets one in install_signals().
void nxos_install_altstack(void);

// The name the runtime gave this thread (log_set_thread_name), for the signal handler: it reads it as a thread-local
// pointer instead of calling pthread_getname_np, which is not safe on an exhausted stack. The pointer must stay valid.
void nxos_set_thread_name(const char* name);
void GetSystemInfo(LPSYSTEM_INFO si);
void GetNativeSystemInfo(LPSYSTEM_INFO si);
BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX ms);
BOOL FlushInstructionCache(HANDLE proc, LPCVOID addr, SIZE_T size);

// <malloc.h>'s aligned allocation. posix_memalign wants a power of two that is at least a pointer.
NX_OS_INLINE void* _aligned_malloc(size_t size, size_t alignment) {
    void* p = NULL;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    return posix_memalign(&p, alignment, size) == 0 ? p : NULL;
}
NX_OS_INLINE void _aligned_free(void* p) { free(p); }

// Section objects (memory.cpp: the guest heap is a SEC_RESERVE pagefile section with views at fixed addresses).
HANDLE CreateFileMappingW(HANDLE file, void* sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name);
HANDLE CreateFileMappingA(HANDLE file, void* sa, DWORD protect, DWORD hi, DWORD lo, LPCSTR name);
LPVOID MapViewOfFileEx(HANDLE section, DWORD access, DWORD hi, DWORD lo, SIZE_T size, LPVOID at);
LPVOID MapViewOfFile(HANDLE section, DWORD access, DWORD hi, DWORD lo, SIZE_T size);
BOOL UnmapViewOfFile(LPCVOID view);
BOOL FlushViewOfFile(LPCVOID view, SIZE_T size);
