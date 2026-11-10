# SPDX-License-Identifier: GPL-3.0-or-later
"""Writing generated files."""
import os


def write_if_changed(path, content):
    if os.path.exists(path):
        with open(path, encoding='utf-8', newline='') as f:
            if f.read() == content:
                return False
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(content)
    return True

