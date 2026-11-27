// SPDX-License-Identifier: GPL-3.0-or-later
// Linked only into the test version of an optimized build (pkp_build.py --optimize, pgo/pkp_pgo.py): saves the profile counters to
// PKP_PROFILE_FILE (a define pkp_build.py sets) every 10 s. The game never returns from main: a scripted run ends in
// TerminateProcess / _exit and Android kills the process, so the profile runtime's own write at exit never runs. Each write goes to
// PKP_PROFILE_FILE.tmp and is then renamed over PKP_PROFILE_FILE, so the end of the run (which may come in the middle of a write)
// leaves the last whole file, never a cut one. The name has no %m: with %m the runtime adds each write to the file, counting the
// start of the session again at every write.
#include <chrono>
#include <cstdio>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);

namespace {
struct ProfileWriter {
    ProfileWriter() {
        std::thread([] {
            __llvm_profile_set_filename(PKP_PROFILE_FILE ".tmp");
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                if (__llvm_profile_write_file() != 0) continue;
#if defined(_WIN32)
                MoveFileExA(PKP_PROFILE_FILE ".tmp", PKP_PROFILE_FILE, MOVEFILE_REPLACE_EXISTING);
#else
                std::rename(PKP_PROFILE_FILE ".tmp", PKP_PROFILE_FILE);
#endif
            }
        }).detach();
    }
} writer;
}  // namespace
