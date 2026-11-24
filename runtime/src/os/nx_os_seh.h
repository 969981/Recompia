// SPDX-License-Identifier: GPL-3.0-or-later
// nx_os_seh.h: part of src/os/nx_os.h for the POSIX build -- structured exceptions, thread contexts and the guarded call (os_posix_seh.cpp).
#pragma once

#if !defined(NX_OS_H_PARTS)
#error "nx_os_seh.h is a part of nx_os.h: include nx_os.h"
#endif

// ---- structured exceptions (mach exception port, see os_posix.cpp) -----------------------------------------------
#define CONTEXT_AMD64 0x00100000
#define CONTEXT_ARM64 0x00400000
#define CONTEXT_CONTROL 0x00100001
#define CONTEXT_INTEGER 0x00100002
#define CONTEXT_SEGMENTS 0x00100004
#define CONTEXT_FLOATING_POINT 0x00100008
#define CONTEXT_DEBUG_REGISTERS 0x00100010
#define CONTEXT_FULL (CONTEXT_CONTROL | CONTEXT_INTEGER)
#define CONTEXT_ALL 0x0010001F
#define EXCEPTION_NONCONTINUABLE 0x01
#define EXCEPTION_MAXIMUM_PARAMETERS 15
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005L
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001DL
#define EXCEPTION_PRIV_INSTRUCTION 0xC0000096L
#define EXCEPTION_BREAKPOINT 0x80000003L
#define EXCEPTION_STACK_OVERFLOW 0xC00000FDL
#define EXCEPTION_IN_PAGE_ERROR 0xC0000006L

typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD* ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD;

typedef struct _CONTEXT {
    // AArch64 register file, in the order a crash report prints it. Only pc/sp/lr/fp and x0..x28 are filled in.
    ULONGLONG x[29];
    ULONGLONG fp;
    ULONGLONG lr;
    ULONGLONG sp;
    ULONGLONG pc;
    DWORD Cpsr;
    DWORD ContextFlags;   // CONTEXT_*: the arm64 file has no separately selectable parts
} CONTEXT, *PCONTEXT, *LPCONTEXT;

typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

BOOL GetThreadContext(HANDLE thread, LPCONTEXT ctx);
BOOL SetThreadContext(HANDLE thread, const CONTEXT* ctx);

typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS*);
typedef LONG (*PTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS*);
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS*);

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
ULONG RemoveVectoredExceptionHandler(PVOID handle);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);
void RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR* args);

// The POSIX stand-in for MSVC __try/__except. The renderer guards a memory copy, a texture prepare or a user hook
// because the guest pages under it can be decommitted; clang has no SEH, so the callable runs under a sigsetjmp in
// the SIGSEGV/SIGBUS handler and a fault inside it returns 0 instead of killing the process. Vectored handlers run
// first, exactly as on Windows, so write_watch's un-protect-and-retry still wins. The callable must not own
// resources: a fault leaves it without unwinding. Callers use nxrt/fault_guard.h, which is SEH on Windows and this here.
int nxos_guarded_call(void (*fn)(void*), void* arg);   // 1 = returned normally, 0 = faulted
