# SPDX-License-Identifier: GPL-3.0-or-later
"""The `cover` command: per-instruction-word translation coverage of a module."""
import collections

from .lift import Lifter, Unimpl
from .env import Env


def cover(img, cfg=None, log=print, limit_examples=3):
    """Per-instruction-word translation coverage of .text, with the reasons for what is not translated."""
    env = Env(img, cfg)
    fn = Lifter(img.base, img.end, env)
    ok = bad = 0
    reasons = collections.Counter()
    examples = {}
    cache = {}
    for i, w in enumerate(img.words()):
        r = cache.get(w)
        if r is None:
            try:
                fn.lift(w, env.text_lo + 0x1000000)
                r = ''
            except Unimpl as e:
                r = str(e)
            except Exception as e:                       # a decoder bug counts as not translated
                r = 'error:%s' % type(e).__name__
            cache[w] = r
        if r:
            bad += 1
            reasons[r] += 1
            examples.setdefault(r, '%x:%08x' % (env.text_lo + 4 * i, w))
        else:
            ok += 1
    total = ok + bad
    log('instruction words: %d   translated: %d (%.2f%%)   not translated: %d (%.2f%%)'
        % (total, ok, 100.0 * ok / max(1, total), bad, 100.0 * bad / max(1, total)))
    for r, n in reasons.most_common(30):
        log('  %-16s %9d  %.2f%%   e.g. %s' % (r, n, 100.0 * n / total, examples[r]))
    return ok, bad, reasons
