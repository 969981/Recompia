#!/usr/bin/env python3
"""Checks (and stamps) the translated READMEs.

English sources and their translations:
  README.md                              -> docs/i18n/README.<lang>.md
  game-build/release_b/README.linux.txt  -> game-build/release_b/README.linux.<lang>.txt
Languages: zh-CN es ar hi pt-BR pt-PT

The language bar (the block between <!-- i18n-bar:start --> and <!-- i18n-bar:end --> in README.md) is ignored by the hash and the checks.
Every translation carries a line with the SHA-256 of the English file it was made from (an HTML comment in .md, a '#' line in .txt),
within its first 5 lines. This script:
  (default)   checks each translation that exists: the hash line is there; the hash still matches the English file (a mismatch is a
              WARNING: the translation is stale, and never blocks); the structure matches the English file (same headings, code
              fences, URLs, inline code, file names and options: a mismatch is an ERROR); no em dash.
  --stamp     writes or refreshes the hash line from the current English file (run it after a translation is checked in or the
              English file changes and the translation was updated).
  --fix-links     in the .md translations, rewrite links that are relative to the repository root (docs/assets/logo.png, CHANGELOG.md)
                  so they work from docs/i18n/ (../../docs/assets/logo.png). A relative link that does not resolve is an ERROR in the check.
  --require-all   a missing translation is an error (default: a warning).
  --lang a,b      only these languages.
Standard library only. Exit status 1 if there is any error.
"""
import argparse, hashlib, os, re, sys

LANGS = ["zh-CN", "es", "ar", "hi", "pt-BR", "pt-PT"]
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PAIRS = [
    ("README.md", "docs/i18n/README.{lang}.md", "md"),
    ("game-build/release_b/README.linux.txt", "game-build/release_b/README.linux.{lang}.txt", "txt"),
]
GH = os.environ.get("GITHUB_ACTIONS") == "true"
errors = 0


def warn(path, msg):
    print(f"::warning file={path}::{msg}" if GH else f"WARNING {path}: {msg}")


def err(path, msg):
    global errors
    errors += 1
    print(f"::error file={path}::{msg}" if GH else f"ERROR {path}: {msg}")


BAR_RE = re.compile(r"<!--\s*i18n-bar:start\s*-->.*?<!--\s*i18n-bar:end\s*-->\n?", re.S)   # the language bar is not part of the text to translate


def norm(text):
    text = BAR_RE.sub("", text)
    text = "\n".join(l.rstrip() for l in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"))
    return re.sub(r"\n{3,}", "\n\n", text).strip() + "\n"   # taking the bar out must not change the text (runs of blank lines count as one)


def sha(text):
    return hashlib.sha256(norm(text).encode("utf-8")).hexdigest()


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


HASH_RE = {"md": re.compile(r"<!--\s*source-sha256:\s*([0-9a-f]{64})\s*-->"), "txt": re.compile(r"^#\s*source-sha256:\s*([0-9a-f]{64})\s*$", re.M)}
HASH_LINE = {"md": "<!-- source-sha256: {h} -->", "txt": "# source-sha256: {h}"}


def strip_fences(text):
    out, infence = [], False
    for line in text.split("\n"):
        if line.lstrip().startswith("```"):
            infence = not infence
            continue
        if not infence:
            out.append(line)
    return "\n".join(out)


def facts(text, kind):
    """What a translation must keep exactly as the English file has it."""
    t = norm(text)
    t = HASH_RE[kind].sub("", t)
    fences = sum(1 for l in t.split("\n") if l.lstrip().startswith("```"))
    prose = strip_fences(t)
    urls = set(re.findall(r"https?://[^\s)>\"'\]]+", t))
    f = {"fences": fences, "urls": urls}
    if kind == "md":
        f["headings"] = sum(1 for l in prose.split("\n") if re.match(r"#{1,6} ", l))
        f["code"] = set(re.findall(r"`([^`\n]+)`", prose))
    else:
        f["options"] = set(re.findall(r"(?<![\w-])--[a-z][a-z-]*", t))
        f["files"] = set(re.findall(r"\b[\w.-]+\.(?:AppImage|exe|nup|txt|md|ini|dll)\b", t))
    return f


LINK_RES = [re.compile(r'((?:src|href)=")([^"#][^"]*)(")'), re.compile(r'(\]\()([^)#\s][^)\s]*)(\))')]
SCHEME = re.compile(r"^(?:[a-zA-Z][a-zA-Z0-9+.-]*:|//)")


def relative_links(text):
    for rx in LINK_RES:
        for m in rx.finditer(text):
            if not SCHEME.match(m.group(2)):
                yield m.group(2)


def fix_links(dst_rel):
    path = os.path.join(ROOT, dst_rel)
    here = os.path.dirname(path)
    text = read(dst_rel)

    def sub(m):
        target = m.group(2)
        bare = target.split("#")[0]
        if SCHEME.match(target) or os.path.exists(os.path.join(here, bare)) or not os.path.exists(os.path.join(ROOT, bare)):
            return m.group(0)
        new = os.path.relpath(os.path.join(ROOT, bare), here).replace(os.sep, "/") + target[len(bare):]
        return m.group(1) + new + m.group(3)

    out = text
    for rx in LINK_RES:
        out = rx.sub(sub, out)
    if out != text:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(out)
        print(f"fixed links in {dst_rel}")


def diff_sets(a, b):
    return sorted(a - b), sorted(b - a)


def check_pair(src_rel, dst_rel, kind, lang, require_all):
    if not os.path.exists(os.path.join(ROOT, dst_rel)):
        (err if require_all else warn)(dst_rel, "translation missing")
        return
    src, dst = read(src_rel), read(dst_rel)
    head = "\n".join(dst.split("\n")[:5])
    m = HASH_RE[kind].search(head)
    if not m:
        err(dst_rel, "no source-sha256 line in the first 5 lines (run tools/i18n/check_readmes.py --stamp)")
    elif m.group(1) != sha(src):
        warn(dst_rel, f"stale: {src_rel} changed since this translation was made (update it, then run --stamp)")
    if "\u2014" in dst:
        err(dst_rel, "contains an em dash")
    if kind == "md":
        here = os.path.dirname(os.path.join(ROOT, dst_rel))
        broken = sorted({l for l in relative_links(dst) if not os.path.exists(os.path.join(here, l.split("#")[0]))})
        if broken:
            err(dst_rel, f"relative links that do not resolve from {os.path.dirname(dst_rel)}/ (run --fix-links): {broken[:4]}")
    fs, fd = facts(src, kind), facts(dst, kind)
    for key in fs:
        if isinstance(fs[key], set):
            missing, extra = diff_sets(fs[key], fd[key])
            if missing or extra:
                err(dst_rel, f"{key} differ from {src_rel}: missing {missing[:4]}, unexpected {extra[:4]}")
        elif fs[key] != fd[key]:
            err(dst_rel, f"{key}: {fd[key]} in the translation, {fs[key]} in {src_rel}")


def stamp_pair(src_rel, dst_rel, kind):
    path = os.path.join(ROOT, dst_rel)
    if not os.path.exists(path):
        return
    src, dst = read(src_rel), read(dst_rel)
    line = HASH_LINE[kind].format(h=sha(src))
    if HASH_RE[kind].search(dst):
        dst = HASH_RE[kind].sub(line, dst, count=1)
    else:
        dst = line + "\n" + dst
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(dst)
    print(f"stamped {dst_rel}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stamp", action="store_true")
    ap.add_argument("--fix-links", action="store_true")
    ap.add_argument("--require-all", action="store_true")
    ap.add_argument("--lang", default=",".join(LANGS))
    a = ap.parse_args()
    langs = [l for l in a.lang.split(",") if l]
    for l in langs:
        if l not in LANGS:
            sys.exit(f"unknown language {l} (one of {', '.join(LANGS)})")
    for src_rel, tmpl, kind in PAIRS:
        for lang in langs:
            dst_rel = tmpl.format(lang=lang)
            if a.fix_links:
                if kind == "md" and os.path.exists(os.path.join(ROOT, dst_rel)):
                    fix_links(dst_rel)
            elif a.stamp:
                stamp_pair(src_rel, dst_rel, kind)
            else:
                check_pair(src_rel, dst_rel, kind, lang, a.require_all)
    if not a.stamp and not a.fix_links:
        print("translations: ok" if not errors else f"translations: {errors} error(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
