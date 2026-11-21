// SPDX-License-Identifier: GPL-3.0-or-later
// The entry of the Android game library (libpkpgame.so = this file + src/pkp_stub.cpp + the four lifted libraries, linked against
// libpkp_rt.so). SDLActivity dlopens the library and calls SDL_main on its Java thread "SDLThread", whose stack is ART's default
// (about 1 MiB). The thread that enters pkp_rt_main becomes the guest's main thread, and lifted code nests deeply: the desktop builds
// give it 128 MiB (/STACK, -stack_size, RLIMIT_STACK). So SDL_main runs pkp_stub.cpp's main (compiled with -Dmain=pkp_stub_main) on
// a thread with that stack and waits for it. SDL needs nothing more: the thread that calls SDL_Init(SDL_INIT_VIDEO) is SDL's video
// thread and pumps the events SDLActivity queues, and SDL attaches it to the JVM on its first JNI call and detaches it at exit.
#include <pthread.h>

int pkp_stub_main(int argc, char** argv);

namespace {

struct MainArgs {
    int argc;
    char** argv;
    int rc;
};

void* run_main(void* p) {
    MainArgs* a = static_cast<MainArgs*>(p);
    a->rc = pkp_stub_main(a->argc, a->argv);
    return nullptr;
}

}  // namespace

// C linkage and default visibility: SDL finds it with dlsym(handle, "SDL_main").
extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char** argv) {
    MainArgs a{argc, argv, 1};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 134217728);   // 128 MiB, the desktop main stack
    pthread_t t;
    const int err = pthread_create(&t, &attr, run_main, &a);
    pthread_attr_destroy(&attr);
    if (err != 0) return 1;   // on SDL's own stack the guest would overflow it at once
    pthread_join(t, nullptr);
    return a.rc;
}
