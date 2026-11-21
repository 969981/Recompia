#!/usr/bin/env python3
"""Checks the game layer's own translated strings (runtime/games/pkp/core/pkp_strings_*).

The keys and their English text are in pkp_strings_table.h (S(Key, Draw, "English")); each other game language has
pkp_strings_<lang>.inc (T(Key, "...")). The compiler already refuses a language with a missing, extra or misordered key
(pkp_strings.cpp). This script also checks, for every language:
  - the same keys in the same order as the table;
  - the same {n} placeholders as English (each one as often);
  - no '<' or '>' (they start the game's text tags in a message-log line);
  - a Log line fits the message log's string argument (127 bytes, pkp_coop_notice.cpp) with {0} filled by NAME_BYTES bytes (a
    player or item name: 12 Latin letters, some accented, or 6 CJK characters) and every other placeholder by OTHER_BYTES (a count,
    a short ability name). A longer line is cut at a whole character, so this is a budget, not a crash guard;
  - an Overlay string, in a language the overlay draws (Latin), uses only letters the overlay's font has (ImGui's default vector
    font, ProggyForever minimal: its cmap below). In the other languages the code shows Overlay strings in English.
Exit 1 on any error. Usage: python tools/i18n/check_strings.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "runtime" / "games" / "pkp" / "core"
LANGS = ["ja", "fr", "it", "de", "es", "zh_hant", "ko", "zh_hans"]
OVERLAY_LANGS = {"fr", "it", "de", "es"}          # kLanguages[].overlay_glyphs in pkp_strings.cpp (English too)
LOG_MAX = 127                                      # kArgText - 1 in pkp_coop_notice.cpp
NAME_BYTES = 24
OTHER_BYTES = 8
# The cmap of ImGui 1.92.9b's ProggyForever-Regular-minimal.ttf (AddFontDefaultVector), read with fontTools.
OVERLAY_CMAP = [(0x20, 0x7E), (0x85, 0x85), (0xA1, 0xAB), (0xAE, 0xFF), (0x20AC, 0x20AC)]

S_RE = re.compile(r'^S\((\w+),\s*(Overlay|Log),\s*"((?:[^"\\]|\\.)*)"\)\s*$')
T_RE = re.compile(r'^T\((\w+),\s*"((?:[^"\\]|\\.)*)"\)\s*$')
PH_RE = re.compile(r"\{(\d)\}")


def unescape(s):
    return s.replace('\\"', '"').replace("\\\\", "\\")


def read(path, rx):
    rows = []
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("//"):
            continue
        m = rx.match(line)
        if not m:
            raise SystemExit(f"{path.name}:{n}: not a string row: {line}")
        rows.append(m.groups())
    return rows


def in_cmap(ch):
    c = ord(ch)
    return any(a <= c <= b for a, b in OVERLAY_CMAP)


def filled_bytes(text):
    phs = PH_RE.findall(text)
    return len(PH_RE.sub("", text).encode("utf-8")) + sum(NAME_BYTES if p == "0" else OTHER_BYTES for p in phs)


def main():
    errors = []
    table = [(k, d, unescape(e)) for k, d, e in read(CORE / "pkp_strings_table.h", S_RE)]
    keys = [k for k, _, _ in table]
    if len(set(keys)) != len(keys):
        errors.append("pkp_strings_table.h: a key is declared twice")
    for k, draw, en in table:
        if "<" in en or ">" in en:
            errors.append(f"en {k}: '<' or '>'")
        if draw == "Log" and filled_bytes(en) > LOG_MAX:
            errors.append(f"en {k}: {filled_bytes(en)} bytes filled, the log takes {LOG_MAX}")
        if draw == "Overlay" and not all(in_cmap(ch) for ch in en):
            errors.append(f"en {k}: a letter the overlay font does not have")
    for lang in LANGS:
        path = CORE / f"pkp_strings_{lang}.inc"
        rows = [(k, unescape(t)) for k, t in read(path, T_RE)]
        got = [k for k, _ in rows]
        if got != keys:
            missing = [k for k in keys if k not in got]
            extra = [k for k in got if k not in keys]
            errors.append(f"{lang}: keys differ from the table (missing {missing}, extra {extra}, or out of order)")
            continue
        for (k, draw, en), (_, text) in zip(table, rows):
            if sorted(PH_RE.findall(text)) != sorted(PH_RE.findall(en)):
                errors.append(f"{lang} {k}: placeholders {PH_RE.findall(text)}, English has {PH_RE.findall(en)}")
            if "<" in text or ">" in text:
                errors.append(f"{lang} {k}: '<' or '>'")
            if not text.strip():
                errors.append(f"{lang} {k}: empty")
            if draw == "Log" and filled_bytes(text) > LOG_MAX:
                errors.append(f"{lang} {k}: {filled_bytes(text)} bytes filled, the log takes {LOG_MAX}")
            if draw == "Overlay" and lang in OVERLAY_LANGS:
                bad = sorted({ch for ch in text if not in_cmap(ch)})
                if bad:
                    errors.append(f"{lang} {k}: letters the overlay font does not have: {' '.join(f'U+{ord(c):04X}' for c in bad)}")
    # Two lines whose {1} is long: the unlocks batch (all five kinds, 2-digit counts) and an ability line (its longest name).
    langs = {"en": dict((k, e) for k, _, e in table)}
    for lang in LANGS:
        langs[lang] = dict((k, unescape(t)) for k, t in read(CORE / f"pkp_strings_{lang}.inc", T_RE))
    for lang, d in langs.items():
        if set(d) != set(keys):
            continue
        parts = [d[f"Coop{k}Many"].replace("{0}", "12") for k in ("Moves", "Emotes", "Areas", "Buildings", "Gates")]
        batch = d["CoopUnlockedBatch"].replace("{1}", d["CoopListComma"].join(parts[:-1]) + d["CoopListAnd"] + parts[-1])
        ability = d["CoopAbilityNamed"].replace("{1}", max(d["CoopAbilityCreatureList"], d["CoopAbilityWater"], key=lambda t: len(t.encode())))
        for name, text in (("CoopUnlockedBatch (five kinds)", batch), ("CoopAbilityNamed (longest name)", ability)):
            if filled_bytes(text) > LOG_MAX and not d[name.split()[0]].endswith("{1}"):
                errors.append(f"{lang} {name}: {filled_bytes(text)} bytes filled; over {LOG_MAX}, so its {{1}} must end the line (a cut drops list items only)")
    for e in errors:
        print("ERROR:", e)
    print(f"{len(keys)} keys, {len(LANGS)} languages besides English: {'FAILED' if errors else 'ok'}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
