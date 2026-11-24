// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_named.cpp: the part of a Win32 object name that other processes see. On Windows a named mutex lives in a
// namespace every process of the session shares, so CreateMutex in a second process answers ERROR_ALREADY_EXISTS;
// the shim's names (os_posix_sync.cpp) are a list inside one process. kernel/sched.cpp claims its console core slot that way,
// and with the list alone two games side by side both took slot 0 and put their console cores on the same processors.
//
// A name is present in a process while it holds a shared flock on the name's lock file; the kernel drops it when the
// last handle closes the file or the process ends, crash included, so a dead process never keeps a name. Testing for
// other holders and taking our own shared lock is one step under an exclusive flock on the user's guard file: flock
// cannot turn an exclusive lock into a shared one atomically, and without the guard two processes starting at the same
// moment could both find the name free. One fd per name per process: os_posix_sync.cpp's list hands every further create of
// the name in this process the same object, and two fds of one process would see each other's flock.
//
// The lock files stay behind (removing one could race a process that is opening it); they are empty and live in
// $XDG_RUNTIME_DIR (Linux, a per-user tmpfs), $TMPDIR (macOS, per user) or /tmp, with the uid in the name.
#include "nx_os_internal.h"

#if !defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include <string>

namespace {

std::string lock_dir() {
    for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR"}) {
        const char* d = getenv(var);
        if (d && *d) return d;
    }
    return "/tmp";
}

// "Local\\name" and "Global\\name" share one namespace here (one user's processes); the rest of the name becomes a file
// name made of [A-Za-z0-9._-].
std::string file_part(const std::string& name) {
    std::string s = name;
    for (const char* prefix : {"Local\\", "Global\\"})
        if (s.rfind(prefix, 0) == 0) {
            s.erase(0, strlen(prefix));
            break;
        }
    for (char& c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
            c = '_';
    return s;
}

int flock_retry(int fd, int op) {
    int r;
    while ((r = ::flock(fd, op)) != 0 && errno == EINTR) {}
    return r;
}

}  // namespace

namespace nxos {

int named_claim(const std::string& name, bool* others) {
    *others = false;
    const std::string base = lock_dir() + "/nxrt-" + std::to_string(static_cast<unsigned long>(getuid())) + "-";
    const int guard = ::open((base + "names.guard").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (guard < 0) return -1;
    if (flock_retry(guard, LOCK_EX) != 0) {
        ::close(guard);
        return -1;
    }
    int fd = ::open((base + file_part(name) + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) {
        // Exclusive fails only while another process holds the name shared: nobody holds it exclusive outside the guard.
        const bool alone = flock_retry(fd, LOCK_EX | LOCK_NB) == 0;
        const bool held = !alone && errno == EWOULDBLOCK;
        if ((alone || held) && flock_retry(fd, LOCK_SH | LOCK_NB) == 0) {
            *others = held;
        } else {
            ::close(fd);
            fd = -1;
        }
    }
    ::close(guard);
    return fd;
}

}  // namespace nxos

#endif  // !_WIN32
