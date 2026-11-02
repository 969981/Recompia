r"""Guest address -> demangled symbol name (+offset) for the four modules of the Metroid Prime NSOs.
  tools\.venv\Scripts\python tools\boot\addrname.py 0x7500148900 0x7100123456 ...
Reads the .dynsym of work\elf\{main,sdk,subsdk0,rtld}.elf (nx2elf output, module-relative values) and caches a sorted table in
work\run\symcache.pkl. Importable: addrname.annotate(text) appends '<name+off>' after every 0x71..0x77xxxxxxxx / f_77xxxxxxxx address."""
import os, pickle, re, subprocess, sys, bisect
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BASES = {'main': 0x7100000000, 'sdk': 0x7500000000, 'subsdk0': 0x7600000000, 'rtld': 0x7700000000}
CACHE = os.path.join(ROOT, 'work', 'run', 'symcache.pkl')
CXXFILT = r'C:\Program Files\LLVM\bin\llvm-cxxfilt.exe'
_tab = None


def _build():
    from elftools.elf.elffile import ELFFile
    rows = []
    for name, base in BASES.items():
        f = ELFFile(open(os.path.join(ROOT, 'work', 'elf', name + '.elf'), 'rb'))
        ds = f.get_section_by_name('.dynsym')
        for s in ds.iter_symbols():
            if s['st_shndx'] != 'SHN_UNDEF' and s['st_value'] and s['st_info']['type'] in ('STT_FUNC', 'STT_OBJECT', 'STT_NOTYPE'):
                rows.append((base + s['st_value'], s['st_size'], s.name))
    rows.sort()
    names = [r[2] for r in rows]
    out = subprocess.run([CXXFILT], input=chr(10).join(names), capture_output=True, text=True).stdout.splitlines()
    dem = dict(zip(names, out)) if len(out) == len(names) else {}
    return [(a, sz, dem.get(n, n)) for a, sz, n in rows]


def table():
    global _tab
    if _tab is None:
        if os.path.exists(CACHE):
            _tab = pickle.load(open(CACHE, 'rb'))
        else:
            _tab = _build()
            os.makedirs(os.path.dirname(CACHE), exist_ok=True)
            pickle.dump(_tab, open(CACHE, 'wb'))
    return _tab


_keys = None
def lookup(addr):
    global _keys
    t = table()
    if _keys is None:
        _keys = [r[0] for r in t]
    i = bisect.bisect_right(_keys, addr) - 1
    # prefer a sized symbol that contains the address; else the nearest preceding one within 64 KiB
    while i >= 0 and addr - t[i][0] < 0x10000:
        a, sz, n = t[i]
        if addr < a + max(sz, 1) or addr - a < 0x400 and sz == 0:
            return '%s+0x%x' % (n[:90], addr - a)
        i -= 1
        if addr - t[max(i, 0)][0] > 0x2000: break
    return None


ADDR = re.compile(r'(?:0x|f_|u_)(7[1-7][0-9a-f]{8})\b')
def annotate(text):
    def rep(m):
        a = int(m.group(1), 16)
        n = lookup(a)
        return m.group(0) + (' <%s>' % n if n else '')
    return ADDR.sub(rep, text)


if __name__ == '__main__':
    for x in sys.argv[1:]:
        print(x, lookup(int(x, 16)))
