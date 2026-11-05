#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
r"""Fails when the added lines or commit messages of a PR carry text that must not be public.

    public_text_check.py BASE HEAD            (git revisions; checks BASE...HEAD, and the commits BASE..HEAD)

Built-in rules: session trailers and claude.ai session links, 192.168.x.x LAN addresses, user-profile paths (C:\Users\<name>,
/home/<name>). Private names and hostnames are NOT kept here: put one regular expression per line in the PUBLIC_NAMING_PATTERNS
repository secret (or variable); it is read from the environment and matched case-insensitively. Secrets are empty on PRs
from forks, so only the built-in rules run there. PR_TITLE and PR_BODY are checked too when set.
"""
import os
import re
import subprocess
import sys

SELF = '.github/scripts/public_text_check.py'   # holds the rules themselves
SAFE_NAMES = r'(?:Public|Default|runner|user|username|name|you|me|All Users|<[^>]*>|%[^%]*%|\$\S*|\{[^}]*\}|\*)'
RULES = [
    ('session trailer', r'Claude-' + r'Session\s*:'),
    ('claude.ai session link', r'claude\.ai/code/' + r'session'),
    ('LAN address', r'\b192\.168\.\d{1,3}\.\d{1,3}\b'),
    ('Windows user path', r'[A-Za-z]:[\\/]+Users[\\/]+(?!' + SAFE_NAMES + r'(?:[\\/\s"\'`]|$))[^\\/\s"\'`]+'),
    ('Unix home path', r'(?<![\w.])/home/(?!' + SAFE_NAMES + r'(?:[/\s"\'`]|$))[^/\s"\'`]+'),
]


def git(*args):
    r = subprocess.run(['git', *args], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode:
        sys.exit('git %s failed: %s' % (' '.join(args), r.stderr.strip()))
    return r.stdout


def main():
    base, head = sys.argv[1], sys.argv[2]
    rules = [(n, re.compile(p, re.I)) for n, p in RULES]
    extra = os.environ.get('PUBLIC_NAMING_PATTERNS', '')
    for i, line in enumerate(extra.splitlines(), 1):
        line = line.strip()
        if line and not line.startswith('#'):
            rules.append(('private pattern #%d' % i, re.compile(line, re.I)))   # never echo the pattern itself

    # (where, text) pairs: added diff lines per file, commit messages, PR text
    items = []
    where = None
    for line in git('diff', '--no-color', '-U0', '--no-ext-diff', base + '...' + head, '--', '.', ':!' + SELF).splitlines():
        if line.startswith('+++ '):
            where = line[6:] if line.startswith('+++ b/') else None
        elif line.startswith('+') and where:
            items.append((where, line[1:]))
    for sha in git('rev-list', base + '..' + head).split():
        for line in git('show', '-s', '--format=%an <%ae>%n%cn <%ce>%n%B', sha).splitlines():
            items.append(('commit ' + sha[:10], line))
    for line in os.environ.get('PR_TITLE', '').splitlines() + os.environ.get('PR_BODY', '').splitlines():
        items.append(('PR title/body', line))

    bad = 0
    for where, text in items:
        for name, rx in rules:
            if rx.search(text):
                bad += 1
                shown = '(line hidden)' if name.startswith('private') else text.strip()[:120]
                print('%s: %s: %s' % (where, name, shown))
    if bad:
        print('\n%d public-text problem(s). Reword or remove them.' % bad)
        return 1
    print('public-text check: clean (%d lines)' % len(items))
    return 0


if __name__ == '__main__':
    sys.exit(main())
