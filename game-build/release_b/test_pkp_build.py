# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for pkp_build.py's object cache, with tiny fake sources and a fake compiler (no Zig, no game).

    python -I game-build/release_b/test_pkp_build.py
"""
import os
import shutil
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pkp_build as pb  # noqa: E402

# the fake compiler: records the source name as a file in $FAKE_LOG, sleeps $FAKE_SLEEP seconds, writes "OBJ" + the source to -o
# (exit 1 when $FAKE_FAIL is part of the source path)
FAKE = r"""
import os, sys, time
a = sys.argv[1:]
src, out = a[a.index('-c') + 1], a[a.index('-o') + 1]
open(os.path.join(os.environ['FAKE_LOG'], os.path.basename(src)), 'w').close()   # one file per run: appends can race
time.sleep(float(os.environ.get('FAKE_SLEEP', '0')))
if os.environ.get('FAKE_FAIL') and os.environ['FAKE_FAIL'] in src:
    sys.exit(1)
open(out, 'wb').write(b'OBJ' + open(src, 'rb').read())
"""


class CacheTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = self.tmp.name
        self.gen, self.obj, self.cdir = [os.path.join(t, n) for n in ('gen', 'obj', 'cache')]
        fake = os.path.join(t, 'fake_cc.py')
        with open(fake, 'w') as f:
            f.write(FAKE)
        os.makedirs(os.path.join(t, 'recomp', 'include'))
        with open(os.path.join(t, 'recomp', 'include', 'x.h'), 'w') as f:
            f.write('// h1')
        self.saved = (pb.RECOMP, pb.COMPILER, pb.CXXFLAGS, pb.progress)
        pb.RECOMP = os.path.join(t, 'recomp')
        pb.COMPILER = [sys.executable, '-I', fake]
        pb.progress = lambda *a: None
        os.environ['FAKE_LOG'] = os.path.join(t, 'log')
        os.makedirs(os.environ['FAKE_LOG'])
        os.environ.pop('FAKE_FAIL', None)
        os.environ['FAKE_SLEEP'] = '0'
        for name, *_ in pb.MODULES:
            os.makedirs(os.path.join(self.gen, name))
            self.write(name, 'funcs.h', '// decls')
        for i in range(6):
            self.write('main', 'f%d.cpp' % i, 'int f%d;' % i)
        self.write('sdk', 's0.cpp', 'int s0;')
        self.runner = pb.Runner(os.path.join(t, 'cancel'))

    def tearDown(self):
        pb.RECOMP, pb.COMPILER, pb.CXXFLAGS, pb.progress = self.saved
        self.tmp.cleanup()

    def write(self, mod, name, text):
        with open(os.path.join(self.gen, mod, name), 'w') as f:
            f.write(text)

    def cache(self, cap_mb=100):
        return pb.make_cache(self.cdir, cap_mb)

    def build(self, cache='default'):
        """Compile everything into a fresh obj folder; the sorted names of the sources the compiler really ran on."""
        shutil.rmtree(self.obj, ignore_errors=True)
        shutil.rmtree(os.environ['FAKE_LOG'], ignore_errors=True)
        os.makedirs(os.environ['FAKE_LOG'])
        pb.compile_all(self.runner, self.gen, self.obj, 2, dict(os.environ), self.cache() if cache == 'default' else cache)
        return sorted(os.listdir(os.environ['FAKE_LOG']))

    def entries(self):
        return sorted(os.listdir(self.cdir))

    def test_second_build_compiles_nothing(self):
        self.assertEqual(len(self.build()), 7)
        self.assertEqual(self.build(), [])
        with open(os.path.join(self.obj, 'main', 'f3.o'), 'rb') as f:
            self.assertEqual(f.read(), b'OBJint f3;')

    def test_changed_source_recompiles_only_it(self):
        self.build()
        self.write('main', 'f2.cpp', 'int f2_changed;')
        self.assertEqual(self.build(), ['f2.cpp'])

    def test_generated_header_or_include_change_misses(self):
        self.build()
        self.write('sdk', 'funcs.h', '// decls2')
        self.assertEqual(self.build(), ['s0.cpp'])        # only the module whose header changed
        with open(os.path.join(pb.RECOMP, 'include', 'x.h'), 'w') as f:
            f.write('// h2')
        self.assertEqual(len(self.build()), 7)

    def test_flags_or_compiler_change_misses(self):
        self.build()
        pb.CXXFLAGS = pb.CXXFLAGS + ['-DX']
        self.assertEqual(len(self.build()), 7)
        pb.COMPILER = pb.COMPILER + ['--new']
        self.assertEqual(len(self.build()), 7)

    def test_compiler_binary_change_misses(self):
        exe = os.path.join(self.tmp.name, 'cc.bin')
        with open(exe, 'wb') as f:
            f.write(b'v1')
        pb.COMPILER = [exe]
        c1 = self.cache()
        with open(exe, 'wb') as f:
            f.write(b'v2')
        c2 = self.cache()
        src = os.path.join(self.gen, 'sdk', 's0.cpp')
        self.assertNotEqual(c1.key(src), c2.key(src))

    def test_damaged_entry_is_not_used_and_removed(self):
        self.build()
        for n in self.entries():
            with open(os.path.join(self.cdir, n), 'ab') as f:     # no longer the size in its name
                f.write(b'junk')
        self.assertEqual(len(self.build()), 7)            # all recompiled
        self.assertEqual(self.build(), [])                # and cached again, whole

    def test_same_size_damage_is_not_detectable_but_truncation_is(self):
        self.build()
        n = self.entries()[0]
        with open(os.path.join(self.cdir, n), 'r+b') as f:
            f.truncate(3)
        self.assertEqual(len(self.build()), 1)

    def test_partial_tmp_file_is_ignored_and_swept(self):
        self.build()
        with open(os.path.join(self.cdir, 'deadbeef-9.o.123.tmp'), 'wb') as f:
            f.write(b'x')
        self.assertEqual(self.build(), [])
        self.assertFalse([n for n in self.entries() if n.endswith('.tmp')])

    def test_zero_size_entry_never_stored(self):
        c = self.cache()
        empty = os.path.join(self.tmp.name, 'e.o')
        open(empty, 'wb').close()
        c.store('k' * 64, empty)
        self.assertEqual(self.entries(), [])

    def test_failed_compile_is_not_cached(self):
        os.environ['FAKE_FAIL'] = 'f1.cpp'
        with self.assertRaises(pb.Fail):
            self.build()
        os.environ.pop('FAKE_FAIL')
        self.assertEqual(self.build(), ['f1.cpp'])        # the others were cached, f1 was not

    def test_lru_trim_keeps_recent_and_respects_cap(self):
        c = self.cache()
        c.cap = 25
        now = time.time()
        for i in range(5):
            p = os.path.join(self.cdir, '%064d-10.o' % i)
            with open(p, 'wb') as f:
                f.write(b'0123456789')
            os.utime(p, (now - 100 + i, now - 100 + i))
        c.trim()
        self.assertEqual(self.entries(), ['%064d-10.o' % i for i in (3, 4)])

    def test_hit_refreshes_recency(self):
        self.build()
        old = time.time() - 1000
        for n in self.entries():
            os.utime(os.path.join(self.cdir, n), (old, old))
        self.build()                                      # all hits: every entry is recent again
        c = self.cache()
        c.cap = 10 ** 9
        os.utime(os.path.join(self.cdir, self.entries()[0]), (old, old))
        keep = self.entries()[1:]
        c.cap = sum(os.path.getsize(os.path.join(self.cdir, n)) for n in keep)
        c.trim()
        self.assertEqual(self.entries(), keep)

    def test_disabled_cache(self):
        self.assertIsNone(self.cache(0))
        self.assertEqual(len(self.build(cache=None)), 7)
        self.assertEqual(len(self.build(cache=None)), 7)

    def test_synthetic_speedup(self):
        for i in range(40):
            self.write('main', 'g%d.cpp' % i, 'int g%d;' % i)
        os.environ['FAKE_SLEEP'] = '0.15'
        t = time.time()
        self.build()
        cold = time.time() - t
        self.write('main', 'g7.cpp', 'int g7_x;')
        t = time.time()
        n = self.build()
        warm = time.time() - t
        self.assertEqual(n, ['g7.cpp'])
        print('\nsynthetic 47 units, 2 jobs, 0.15 s per compile: cold %.1f s, one unit changed %.1f s' % (cold, warm))
        self.assertLess(warm, cold / 3)


if __name__ == '__main__':
    unittest.main()
