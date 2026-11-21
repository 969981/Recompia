# SPDX-License-Identifier: GPL-3.0-or-later
"""The part of CPython's _multiprocessing C module that multiprocessing.synchronize needs, for Android.

CPython for Android ships no _multiprocessing: bionic has no named POSIX semaphores (sem_open fails), so every Lock,
Semaphore and multiprocessing.Pool fails to import. The lifter (recomp/nxrecomp/driver.py) runs a Pool. Here a SemLock is
a pipe holding one byte per available unit: acquire reads a byte, release writes one. The pipe's ends are inherited over
fork(), so the lock works between the parent and the pool's workers. Only the 'fork' start method can use it (a pipe has
no name to rebuild from after pickling): sitecustomize.py makes 'fork' the default.
"""
import fcntl
import os
import select
import struct
import termios
import threading
import time

flags = {}


def sem_unlink(name):
    pass


class SemLock:
    SEM_VALUE_MAX = 1 << 30
    RECURSIVE_MUTEX, SEMAPHORE = 0, 1

    def __init__(self, kind, value, maxvalue, name, unlink):
        self.kind, self.maxvalue = kind, maxvalue
        self.name = None                    # no name: nothing to register with the resource tracker
        self._r, self._w = os.pipe()
        os.set_blocking(self._r, False)
        if value:
            os.write(self._w, b'x' * value)
        self._owner = None
        self._depth = 0                     # acquisitions held by this process (and thread, for the mutex)

    @property
    def handle(self):
        return self._r

    @classmethod
    def _rebuild(cls, *state):
        raise RuntimeError('SemLock on Android works with the fork start method only')

    def _me(self):
        return (os.getpid(), threading.get_ident())

    def _is_mine(self):
        return self._depth > 0 and self._owner == self._me()

    def acquire(self, block=True, timeout=None):
        if self.kind == self.RECURSIVE_MUTEX and self._is_mine():
            self._depth += 1
            return True
        end = None if not block else (None if timeout is None else time.monotonic() + max(0.0, timeout))
        while True:
            try:
                if os.read(self._r, 1):
                    break
            except BlockingIOError:
                pass
            if not block:
                return False
            left = None if end is None else end - time.monotonic()
            if left is not None and left <= 0:
                return False
            select.select([self._r], [], [], left)
        self._owner = self._me()
        self._depth += 1
        return True

    def release(self):
        if self.kind == self.RECURSIVE_MUTEX:
            if not self._is_mine():
                raise AssertionError('attempt to release recursive lock not owned by thread')
            self._depth -= 1
            if self._depth:
                return
        else:
            if self.maxvalue == 1 and self._get_value() >= 1:
                raise ValueError('semaphore or lock released too many times')
            self._depth = max(0, self._depth - 1)
        os.write(self._w, b'x')

    def _count(self):
        return self._depth

    def _get_value(self):
        return struct.unpack('i', fcntl.ioctl(self._r, termios.FIONREAD, b'\0\0\0\0'))[0]

    def _is_zero(self):
        return self._get_value() == 0

    def _after_fork(self):
        self._depth = 0
        self._owner = None

    def __enter__(self):
        return self.acquire()

    def __exit__(self, *args):
        self.release()
