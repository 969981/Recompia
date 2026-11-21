// SPDX-License-Identifier: GPL-3.0-or-later
// Measurement helper (not shipped): a NativeActivity that runs <filesDir>/run.sh with /system/bin/sh in a real app process
// (untrusted_app domain, unlike run-as), with NATIVE_LIB_DIR and FILES_DIR set, output to <filesDir>/run.log, and the
// screen kept on while the activity is visible (FLAG_KEEP_SCREEN_ON). Writes <filesDir>/run.done with the exit status.
#include <android/native_activity.h>
#include <dlfcn.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char files[512];

static void *run(void *arg) {
    (void)arg;
    Dl_info info;
    char self[512];
    dladdr((void *)run, &info);
    snprintf(self, sizeof self, "%s", info.dli_fname);
    setenv("NATIVE_LIB_DIR", dirname(self), 1);
    setenv("FILES_DIR", files, 1);
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "cd %s && /system/bin/sh ./run.sh > run.log 2>&1; echo $? > run.done", files);
    system(cmd);
    return NULL;
}

void ANativeActivity_onCreate(ANativeActivity *a, void *state, size_t size) {
    (void)state; (void)size;
    ANativeActivity_setWindowFlags(a, 0x80 /* FLAG_KEEP_SCREEN_ON */, 0);
    static int started;
    if (started++) return;
    snprintf(files, sizeof files, "%s", a->internalDataPath);
    pthread_t t;
    pthread_create(&t, NULL, run, NULL);
    pthread_detach(t);
}
