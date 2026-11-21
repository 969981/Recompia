# SPDX-License-Identifier: GPL-3.0-or-later
# Android: CPython ships no _posixshmem (bionic has no shm_open). multiprocessing.resource_tracker imports it on every POSIX
# system at import time, so a fork Pool cannot start without it. Shared memory stays unavailable (the lifter does not use it).


def shm_open(path, flags, mode=0o777):
    raise OSError(38, 'shm_open is not available on Android')


def shm_unlink(path):
    raise OSError(38, 'shm_unlink is not available on Android')
