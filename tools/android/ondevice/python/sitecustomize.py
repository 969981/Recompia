# SPDX-License-Identifier: GPL-3.0-or-later
# Android only (installed into the on-device Python's site-packages by build-python.sh): the default multiprocessing start
# method is 'fork', the only one the pipe-based _multiprocessing.SemLock beside this file can serve. Python 3.14 would pick
# 'forkserver', which pickles locks by name.
import multiprocessing

multiprocessing.set_start_method('fork')
