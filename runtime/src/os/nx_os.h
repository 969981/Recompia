// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os.h: the Win32 API the runtime is written against, for the POSIX (macOS) build.
//
// The runtime, the game modules and the vendored Link Play module are written against Win32: HANDLEs, SRWLOCK +
// CONDITION_VARIABLE, CreateThread, waitable timers, QueryPerformanceCounter, VirtualAlloc, CreateFileW/ReadFile.
// 90 of the 101 Win32 calls in the tree are one of those, and every one of them has a direct POSIX equivalent
// (docs/macos-port.md, D6). Rather than rewrite 80 files, this header declares the same names with the same
// signatures on top of pthreads, mmap/mach, clock_gettime and the POSIX file API, and the CMake build puts
// src/os/compat/ first on the include path so `<windows.h>` resolves here. The Windows build never sees this
// header at all: it keeps the real SDK headers, byte for byte (D12).
//
// Two things are NOT emulated, because emulating them would be a lie:
//   - section objects. CreateFileMappingW/MapViewOfFile* exist and behave (SEC_RESERVE, views at a fixed
//     address, commit through VirtualAlloc), implemented with mach_vm on top of an anonymous backing store.
//   - vectored exception handlers. AddVectoredExceptionHandler installs a mach EXC_BAD_ACCESS/EXC_BAD_INSTRUCTION
//     port handler. The handler runs on a helper thread, not on the faulting thread (that is the mach model), so
//     a handler may inspect and report but only a thread_state rewrite can resume the fault (write_watch.cpp).
//
// C11 and C++: the vendored pokopia-coop module is C11 and uses SRWLOCK/HANDLE/CreateThread, so the core types and
// those calls are plain C. The path and string calls are C++ inline overloads, because std::filesystem::path::c_str()
// is const char* here and const wchar_t* on Windows, and both must compile at the same call site.
//
// The POSIX declarations are split by job into the nx_os_<part>.h files below, included here in order: each relies on
// the parts before it, so they are never included on their own (each part stops with an #error if it is).
#pragma once

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#else  // ---------------------------------------------------------------- POSIX

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <sys/types.h>
#endif

#define NX_OS_H_PARTS 1   // the parts check it: they are included from here only
#include "nx_os_types.h"
#include "nx_os_memory.h"
#include "nx_os_thread.h"
#include "nx_os_sync.h"
#include "nx_os_time.h"
#include "nx_os_file.h"
#include "nx_os_module.h"
#include "nx_os_string.h"
#include "nx_os_process.h"
#include "nx_os_diag.h"
#include "nx_os_seh.h"
#include "nx_os_overloads.h"

#endif  // _WIN32
