#!/usr/bin/env python
"""Survey audio / codec symbols in the Metroid Prime Switch ELF binaries.

Reads .dynsym and .symtab (if present) from work/elf/{main,sdk,subsdk0}.elf with
pyelftools, demangles names with llvm-cxxfilt (one batched process per ELF),
splits them into IMPORTS (SHN_UNDEF) and DEFINED, and writes:

  work/survey/audio/<elf>_imports.txt     demangled<TAB>mangled, sorted
  work/survey/audio/<elf>_defined.txt     demangled<TAB>mangled, sorted
  work/survey/audio/audio_imports_summary.txt
  work/survey/audio/keyword_table.txt
  work/survey/audio/audio_layer_main.txt
  work/survey/audio/opus_symbols.txt

Run from the project folder: tools/.venv/Scripts/python.exe tools/survey/audio_symbols.py
"""
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

def _project_root():
    """The project folder: NXRT_ROOT, else the nearest parent of this file that holds both tools/ and work/."""
    import os, pathlib
    env = os.environ.get('NXRT_ROOT')
    if env:
        return pathlib.Path(env).as_posix()
    here = pathlib.Path(__file__).resolve()
    for p in here.parents:
        if (p / 'tools').is_dir() and (p / 'work').is_dir():
            return p.as_posix()
    return here.parents[2].as_posix()


ROOT = _project_root()
ELF_DIR = ROOT + "/work/elf"
OUT_DIR = ROOT + "/work/survey/audio"
ELFS = ["main", "sdk", "subsdk0"]
KEEP_TYPES = {"STT_FUNC", "STT_OBJECT", "STT_NOTYPE", "STT_GNU_IFUNC", "STT_TLS", "STT_COMMON"}

CXXFILT = "C:/Program Files/LLVM/bin/llvm-cxxfilt.exe"
if not os.path.exists(CXXFILT):
    CXXFILT = shutil.which("llvm-cxxfilt") or shutil.which("c++filt")
if not CXXFILT:
    raise SystemExit("no llvm-cxxfilt / c++filt found")


def read_symbols(path):
    """Return {(name, undef): {"type": str, "srcs": set}} over .dynsym and .symtab."""
    recs = {}
    with open(path, "rb") as f:
        elf = ELFFile(f)
        for sec in elf.iter_sections():
            if not isinstance(sec, SymbolTableSection):
                continue
            for sym in sec.iter_symbols():
                name = sym.name
                if not name:
                    continue
                stype = sym["st_info"]["type"]
                if stype not in KEEP_TYPES:
                    continue
                shndx = sym["st_shndx"]
                undef = shndx == "SHN_UNDEF" or shndx == 0
                key = (name, undef)
                if key not in recs:
                    recs[key] = {"type": stype, "srcs": set()}
                recs[key]["srcs"].add(sec.name)
    return recs


def demangle_all(names):
    """Demangle every _Z name in one llvm-cxxfilt call. Returns {mangled: demangled}."""
    mangled = sorted({n for n in names if n.startswith("_Z")})
    if not mangled:
        return {}
    proc = subprocess.run(
        [CXXFILT],
        input="\n".join(mangled) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=True,
    )
    out = proc.stdout.split("\n")
    if out and out[-1] == "":
        out.pop()
    if len(out) != len(mangled):
        raise SystemExit("demangler line count mismatch: %d in, %d out" % (len(mangled), len(out)))
    return dict(zip(mangled, out))


def build_lists(elf_name):
    path = "%s/%s.elf" % (ELF_DIR, elf_name)
    recs = read_symbols(path)
    names = [k[0] for k in recs]
    dmap = demangle_all(names)
    imports, defined = [], []
    for (name, undef), info in recs.items():
        dem = dmap.get(name, name)
        entry = (dem, name, info)
        (imports if undef else defined).append(entry)
    imports.sort(key=lambda e: (e[0], e[1]))
    defined.sort(key=lambda e: (e[0], e[1]))
    return imports, defined


def write_list(path, entries):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for dem, mangled, _ in entries:
            f.write("%s\t%s\n" % (dem, mangled))


def count_matching(entries, pattern):
    rx = re.compile(pattern, re.IGNORECASE)
    return sum(1 for dem, _, _ in entries if rx.search(dem))


# (label, regex on demangled name, case-insensitive)
KEYWORDS = [
    ("nn::audio (incl. ctrl/out)", r"nn::audio"),
    ("nn::audioctrl", r"nn::audioctrl"),
    ("nn::audioout / AudioOut", r"audioout"),
    ("AudioRenderer", r"audiorenderer"),
    ("nn::codec", r"nn::codec"),
    ("nn::mmnv", r"nn::mmnv"),
    ("Opus / opus_", r"opus"),
    ("ADPCM", r"adpcm"),
    ("nn::hac", r"nn::hac"),
    ("nn::vfx", r"nn::vfx"),
    ("FMOD", r"fmod"),
    ("Wwise / AK::", r"wwise|\bak::"),
    ("CRI / ADX", r"criware|\bcri_|\bcri::|\badx"),
    ("Bink", r"bink"),
    ("XAudio", r"xaudio"),
    ("vorbis", r"vorbis"),
    ("mp3", r"mp3"),
    ("aac", r"aac"),
    ("fdk", r"fdk"),
    ("movie::", r"movie::"),
    ("sead::", r"sead::"),
    ("CAudio", r"caudio"),
    ("CSound", r"csound"),
    ("CSample", r"csample"),
    ("CStreamed", r"cstreamed"),
    ("CMusic", r"cmusic"),
    ("CSfx", r"csfx"),
    ("CAram", r"caram"),
    ("CAudioSys", r"caudiosys"),
    ("CSMP", r"csmp"),
    ("'Audio' anywhere", r"audio"),
]

# Game audio-layer filter (step 4)
AUDIO_LAYER_RX = re.compile(
    r"Audio|Sound|Sfx|SampleBank|CSMP|CAUD|Voice|Mixer|Opus|Adpcm|Decoder|Stream.*Audio|Audio.*Stream",
    re.IGNORECASE,
)


def strip_params(dem):
    """Cut the demangled name at the parameter list '(' that sits at template depth 0."""
    s = dem.replace("(anonymous namespace)", "{anon}")
    depth_a = 0
    for i, ch in enumerate(s):
        if ch == "<":
            depth_a += 1
        elif ch == ">":
            depth_a = max(0, depth_a - 1)
        elif ch == "(" and depth_a == 0 and i > 0:
            return s[:i]
    return s


def split_class_method(qual):
    """Split 'a::b::C<T>::method' at the last top-level '::'."""
    depth = 0
    last = -1
    i = 0
    while i < len(qual):
        ch = qual[i]
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth = max(0, depth - 1)
        elif ch == ":" and depth == 0 and i + 1 < len(qual) and qual[i + 1] == ":":
            last = i
            i += 2
            continue
        i += 1
    if last < 0:
        return "(global)", qual
    cls = re.sub(r"<.*>", "", qual[:last])
    return cls, qual[last + 2:]


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    print("demangler:", CXXFILT)

    lists = {}
    for elf in ELFS:
        imports, defined = build_lists(elf)
        lists[elf] = (imports, defined)
        write_list("%s/%s_imports.txt" % (OUT_DIR, elf), imports)
        write_list("%s/%s_defined.txt" % (OUT_DIR, elf), defined)
        print("%-9s imports=%6d  defined=%6d" % (elf, len(imports), len(defined)))

    # ---- Step 2: keyword table -------------------------------------------
    cols = []
    for elf in ELFS:
        cols.append((elf + " imp", lists[elf][0]))
        cols.append((elf + " def", lists[elf][1]))
    header = "%-30s" % "keyword" + "".join("%13s" % c[0] for c in cols)
    rows = [header, "-" * len(header)]
    for label, pat in KEYWORDS:
        counts = [count_matching(entries, pat) for _, entries in cols]
        rows.append("%-30s" % label + "".join("%13d" % c for c in counts))
    table = "\n".join(rows)
    print("\n== Keyword counts (demangled, case-insensitive) ==")
    print(table)
    with open(OUT_DIR + "/keyword_table.txt", "w", encoding="utf-8", newline="\n") as f:
        f.write(table + "\n")

    # ---- Step 3: nn::audio / nn::codec / nn::audioctrl / nn::mmnv imports --
    prefixes = ("nn::audio", "nn::codec", "nn::audioctrl", "nn::mmnv")
    summary_lines = []
    print("\n== nn::audio / nn::codec / nn::audioctrl / nn::mmnv IMPORTS ==")
    for elf in ELFS:
        seen = []
        seen_set = set()
        for dem, mangled, _ in lists[elf][0]:
            if dem.startswith(prefixes) and dem not in seen_set:
                seen_set.add(dem)
                seen.append(dem)
        seen.sort()
        total_imports = len(lists[elf][0])
        header = "== %s.elf: %d unique matching imports (of %d total imports) ==" % (elf, len(seen), total_imports)
        summary_lines.append(header)
        summary_lines.extend(seen)
        summary_lines.append("")
        print("\n" + header)
        for line in seen[:60]:
            print("  " + line)
        if len(seen) > 60:
            print("  ... (%d more, see audio_imports_summary.txt)" % (len(seen) - 60))
    with open(OUT_DIR + "/audio_imports_summary.txt", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(summary_lines) + "\n")

    # ---- Step 4: game audio layer in main.elf defined list -----------------
    main_defined = lists["main"][1]
    hits = {}
    for dem, mangled, _ in main_defined:
        if not AUDIO_LAYER_RX.search(dem):
            continue
        hits.setdefault(dem, mangled)
    hit_names = sorted(hits)
    with open(OUT_DIR + "/audio_layer_main.txt", "w", encoding="utf-8", newline="\n") as f:
        for dem in hit_names:
            f.write("%s\t%s\n" % (dem, hits[dem]))

    print("\n== main.elf audio-layer candidate functions: %d unique ==" % len(hit_names))
    print("-- first 80 --")
    for dem in hit_names[:80]:
        print("  " + dem)

    groups = defaultdict(list)
    for dem in hit_names:
        qual = strip_params(dem)
        cls, method = split_class_method(qual)
        groups[cls].append(method)
    ordered = sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    print("\n== Grouped by class (count, 3 example methods) ==")
    print("%-6s %-60s %s" % ("count", "class", "examples"))
    for cls, methods in ordered:
        uniq = []
        for m in methods:
            if m not in uniq:
                uniq.append(m)
        print("%-6d %-60s %s" % (len(methods), cls[:60], " | ".join(uniq[:3])))

    # ---- Step 5: opus symbols -------------------------------------------------
    print("\n== Opus symbols per ELF ==")
    opus_rx = re.compile(r"opus", re.IGNORECASE)
    opus_lines = []
    for elf in ELFS:
        imp = [e for e in lists[elf][0] if opus_rx.search(e[0])]
        dfn = [e for e in lists[elf][1] if opus_rx.search(e[0])]
        line = "%-9s defined=%4d  imported=%4d" % (elf, len(dfn), len(imp))
        print(line)
        opus_lines.append("== %s.elf ==" % elf)
        opus_lines.append(line)
        for dem, _, _ in dfn:
            opus_lines.append("  DEF  " + dem)
        for dem, _, _ in imp:
            opus_lines.append("  IMP  " + dem)
        opus_lines.append("")
    with open(OUT_DIR + "/opus_symbols.txt", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(opus_lines) + "\n")
    print("(full opus name lists in opus_symbols.txt)")

    print("\nWrote files under", OUT_DIR)


if __name__ == "__main__":
    main()
