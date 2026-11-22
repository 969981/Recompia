// SPDX-License-Identifier: GPL-3.0-or-later
// The python executable for Android: python.org's Android release ships libpython only. Packaged as
// lib/arm64-v8a/libpython_exec.so (an executable named like a library, so the app may run it).
// The stdlib lives in app storage, not beside the executable, so its home is taken here from RECOMPIA_PYTHONHOME, else
// PYTHONHOME: Python itself ignores PYTHONHOME under -I, which pkp_build.py passes to every Python it starts.
#include <Python.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    PyStatus st = PyConfig_SetBytesArgv(&config, argc, argv);
    const char *home = getenv("RECOMPIA_PYTHONHOME");
    if (!home)
        home = getenv("PYTHONHOME");
    if (!PyStatus_Exception(st) && home)
        st = PyConfig_SetBytesString(&config, &config.home, home);
    if (!PyStatus_Exception(st))
        st = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(st))
        Py_ExitStatusException(st);
    return Py_RunMain();
}
