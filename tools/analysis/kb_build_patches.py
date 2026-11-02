#!/usr/bin/env python
"""Build docs/mpr/patches.json (+ frag/web_patches.json symbol fragment) from the community patch sources.
Inputs: work/kb/web/fl4sh_patches.json (transcribed from the Citron-Mods repo pages) plus the small sets embedded below
(transcribed from the pages named in each 'source').  Offsets are main-relative (pchtxt offset_shift 0x100 = NSO header).
pchtxt bytes are memory byte order; Atmosphere cheat values are the numeric 32-bit word (little-endian in memory).
"""
import json, re, pathlib, struct, collections
ROOT = pathlib.Path(__file__).resolve().parents[2]
W = ROOT / 'work/kb/web'
CIT = 'https://github.com/citron-neo/Citron-Mods/blob/b935a8e919b7ec3af26469d6805f0c0b68057d08/010012101468C000/1.0.0 by Fl4sh_/'
FEAT = {'120FPS': ['fps', 'timestep'], '240FPS': ['fps', 'timestep'], 'Unlocked': ['fps', 'timestep'], 'x': ['resolution'], 'LOD': ['render']}


def mov_w(rd, imm, hw=0):
    return struct.pack('<I', 0x52800000 | (hw << 21) | (imm << 5) | rd).hex().upper()


def word_to_bytes(w): return struct.pack('<I', int(w, 16)).hex().upper()


sets = []
d = json.load(open(W / 'fl4sh_patches.json'))
for f in d['files']:
    n = f['dir']
    feats = ['resolution']
    if 'FPS' in n: feats = ['fps', 'timestep', 'physics']
    if 'Ultrawide' in n: feats = ['resolution', 'aspect', 'hud', 'fov']
    sets.append({'id': 'fl4sh/' + n, 'title': f['title_line'], 'author': 'Fl4sh_ (Fl4sh9174)', 'url': f['url'], 'repo': d['source'], 'commit': d['commit'],
                 'features': feats, 'header_comments': f['header_comments'], 'entries': f['entries']})

# cucholix/NX-IPS-romfs-compilation, M/Metroid Prime Remastered/1.0.0.pchtxt @ d524be33 (comments give original | new instruction)
cu = [
 ('0x00001248', mov_w(0, 0x780), 'mov w0, #0x640 | mov w0, #0x780 | docked resX'),
 ('0x00001250', mov_w(0, 0x438), 'mov w0, #0x384 | mov w0, #0x438 | docked resY'),
 ('0x00001258', mov_w(0, 0x500), 'mov w0, #0x400 | mov w0, #0x500 | handheld resX'),
 ('0x00001260', mov_w(0, 0x2d0), 'mov w0, #0x240 | mov w0, #0x2d0 | handheld resY'),
 ('0x00001268', mov_w(0, 0x500), 'mov w0, #0x400 | mov w0, #0x500 | handheld resX'),
 ('0x00001270', mov_w(0, 0x2d0), 'mov w0, #0x240 | mov w0, #0x2d0 | handheld resY'),
 ('0x0010dc88', '00103E1E', 'ldr s0, [x21,#0x20] | fmov s0, #-1.0 | LOD bias'),
 ('0x001fbe40', '3D000014', 'cbz x8, #0x118 | b | improved LOD'),
 ('0x001fbb5c', '27000014', 'mov x9, xzr | b | improved LOD'),
 ('0x00bdb81c', mov_w(0, 0x640), 'bl GetScreenWidth | mov w0, #0x640 | UI resX'),
 ('0x00bdb844', mov_w(0, 0x384), 'bl GetScreenHeight | mov w0, #0x384 | UI resY'),
 ('0x00e5324c', mov_w(0, 0x384), 'bl | mov w0, #0x384 | UI resY'),
 ('0x00e53254', mov_w(0, 0x384), 'bl | mov w0, #0x384 | UI resY'),
 ('0x00e5325c', mov_w(0, 0x640), 'bl | mov w0, #0x640 | UI resX'),
 ('0x00e53264', mov_w(0, 0x384), 'bl | mov w0, #0x384 | UI resY'),
 ('0x00e54c40', mov_w(0, 0x640), 'bl | mov w0, #0x640 | UI resX'),
 ('0x00e54c50', mov_w(0, 0x384), 'bl | mov w0, #0x384 | UI resY'),
]
sets.append({'id': 'cucholix/1.0.0.pchtxt', 'title': 'Metroid Prime Remastered [010012101468C000]', 'author': 'cucholix compilation', 'repo': 'https://github.com/cucholix/NX-IPS-romfs-compilation',
             'commit': 'd524be33d85c60ad6fdde969d878f79b72737df8', 'url': 'https://github.com/cucholix/NX-IPS-romfs-compilation/blob/d524be33d85c60ad6fdde969d878f79b72737df8/M/Metroid Prime Remastered/1.0.0.pchtxt',
             'features': ['resolution', 'render'], 'header_comments': [], 'entries': [{'offset': o, 'bytes': b, 'comment': c} for o, b, c in cu]})
sets.append({'id': 'kenji/disable-dynamic-resolution', 'title': 'Disable Dynamic Resolution', 'author': 'KeatonTheBot', 'repo': 'https://github.com/Kenji-NX/switch-pchtxt-mods',
             'commit': '81e79b55505648ff20fd9c0ed4540d5634c60f0e',
             'url': 'https://github.com/Kenji-NX/switch-pchtxt-mods/blob/81e79b55505648ff20fd9c0ed4540d5634c60f0e/Metroid Prime Remastered/010012101468C000/Disable Dynamic Resolution/exefs/1.0.0.pchtxt',
             'features': ['resolution', 'render'], 'header_comments': ['// Disable Dynamic Resolution'],
             'entries': [{'offset': '0x00aff5d4', 'bytes': 'C0035FD6', 'comment': 'ret   // function head replaced by ret'}]})
sets.append({'id': 'fl4sh/Improved LODBIAS', 'title': 'Improved LODBIAS', 'author': 'Fl4sh_', 'repo': d['source'], 'commit': d['commit'], 'url': CIT + 'Improved LODBIAS/exefs/1.0.0.pchtxt',
             'features': ['render'], 'header_comments': ['// Improves the overall texture quality'], 'entries': [{'offset': '0x001fc6d4', 'bytes': '20008012', 'comment': ''}]})
sets.append({'id': 'fl4sh/1920x1080', 'title': '1920x1080 Docked', 'author': 'Fl4sh_', 'repo': d['source'], 'commit': d['commit'], 'url': CIT + '1920x1080/exefs/1.0.0.pchtxt',
             'features': ['resolution', 'fov'], 'header_comments': ['// 1920x1080 Docked'],
             'entries': [{'offset': '0x00afe3d8', 'bytes': '00902F1E', 'comment': 'fmov s0, #1.75000000 //'},
                         {'offset': '0x00001248', 'bytes': 'mov0', 'comment': 'mov w0, #0x780 // GetScreenWidth'}]})
# fix the 1080p set to full entries as printed on the page
sets[-1]['entries'] = [
 {'offset': '0x00afe3d8', 'bytes': '00902F1E', 'comment': 'fmov s0, #1.75000000 //'},
 {'offset': '0x00001248', 'bytes': mov_w(0, 0x780), 'comment': 'mov w0, #0x780 // GetScreenWidth'},
 {'offset': '0x00001250', 'bytes': mov_w(0, 0x438), 'comment': 'mov w0, #0x438 // GetScreenHeight'},
 {'offset': '0x00001258', 'bytes': mov_w(0, 0x500), 'comment': 'mov w0, #0x500 // HandheldFullWidth'},
 {'offset': '0x00001260', 'bytes': mov_w(0, 0x2d0), 'comment': 'mov w0, #0x2d0 // HandheldFullHeight'},
 {'offset': '0x00001268', 'bytes': mov_w(0, 0x500), 'comment': 'mov w0, #0x500 // HandheldContentWidth'},
 {'offset': '0x00001270', 'bytes': mov_w(0, 0x2d0), 'comment': 'mov w0, #0x2d0 // HandheldContentHeight'},
 {'offset': '0x00c8c8ac', 'bytes': '0854A852', 'comment': 'mov w8, #0x42a00000 // DefaultThirdPersonVerticalFOV s0 = 80.0'},
 {'offset': '0x00c8c8b8', 'bytes': '8852A852', 'comment': 'mov w8, #0x42940000 //'},
 {'offset': '0x00c8c8bc', 'bytes': '0001271E', 'comment': 'fmov s0, w8 // DefaultFirstPersonVerticalFOV s0 = 74'},
 {'offset': '0x001fbe40', 'bytes': '3D000014', 'comment': 'b #0x11c //'},
 {'offset': '0x001fbb5c', 'bytes': '27000014', 'comment': 'b #0xc8 //'}]

# Atmosphere cheats for 1.0.0 (xenocard/CheatsHunter ...67C9CA1428222752.txt @ d03a6651): numeric instruction words.
CHEAT_URL = 'https://github.com/xenocard/CheatsHunter/blob/d03a66511e1e3df7847abd8739c23f813f62bdd3/010012101468C000/cheats/67C9CA1428222752.txt'
ch = [  # offset, original word (given by an 'off'/restore line, else None), new word, purpose
 ('0x00c89130', '4B020129', '51000129', 'Inf Ammo: ammo decrement `sub w9,w9,w2` becomes `sub w9,w9,#0`'),
 ('0x00c89134', None, '52801FE9', 'Infinite Missiles: `mov w9,#0xff` after the decrement'),
 ('0x00c69bc0', 'BD400000', '140F022C', 'Inf HP: `ldr s0,[x0]` (health read) becomes b to cave at 0x102a470'),
 ('0x00c5dbb8', '1E283800', 'D503201F', 'Freeze time: `fsub s0,s0,s8` becomes nop'),
 ('0x00d802e4', 'F9400069', '140AA85D', 'ZL hover: ldr x9,[x3,...] becomes b to cave at 0x102a458'),
 ('0x00c62f70', 'BD000369', '140F1FE8', 'Invincible: instruction at 0xc62f70 becomes b to cave at 0x102a310'),
 ('0x00c62f74', 'B40003B9', None, 'Invincible off (restore line shows original word)'),
 ('0x00c16e40', 'F9400128', 'D2E81408', 'Moon Jump (B)(morph ball): `ldr x8,[x9]` replaced by movz x8,#0x40a0,lsl#48'),
 ('0x00c9aa98', None, 'D503201F', 'Infinite bombs: nop'),
 ('0x00c88efc', None, '52800020', 'Can use all abilities: mov w0,#1'),
 ('0x00c89180', None, '528003C0', 'Can use all abilities: mov w0,#0x1e'),
 ('0x00c89450', None, '52800020', 'Can use all abilities: mov w0,#1'),
 ('0x00c88ef0', None, '140E8804', 'Get Samus Data pointer: b to cave at 0x102a... stores the player-state pointer into 0x4b3df00'),
]
sets.append({'id': 'cheats/xenocard-1.0.0', 'title': 'Metroid Prime Remastered 1.0.0 cheats (TomSwitch, Gamerjin, Crazy_p, ReclaimerShawn)', 'author': 'various', 'repo': 'https://github.com/xenocard/CheatsHunter',
             'commit': 'd03a66511e1e3df7847abd8739c23f813f62bdd3', 'url': CHEAT_URL, 'features': ['cheat', 'player_state'], 'header_comments': ['values are numeric u32 instruction words (memory is little-endian)'],
             'entries': [{'offset': o, 'bytes': word_to_bytes(n) if n else None, 'orig_bytes': word_to_bytes(orig) if orig else None, 'comment': c} for o, orig, n, c in ch]})
# data-side facts from the cheats page
cheat_facts = [
 {'what': 'cheat pointer base for power-ups', 'detail': 'IceBeam at +0x38, ChargeBeam +0x80, MorphBall +0xb0, SpiderBall +0xc8: every item is 0x14 below the practice-mod layout (CPlayerStateMP1 + 0x44 + 8*item). Implies the cheat base pointer = CPlayerStateMP1 + 0x14.', 'status': 'derived, unverified'},
 {'what': 'data global 0x4b3df00', 'detail': 'cheat "Get Samus Data pointer" stores a pointer at main+0x4b3df00 (bss); also main+0x4b3d000..0x4b3d080 is scratch used by the 120FPS patch', 'status': 'source claim'},
 {'what': 'code caves', 'detail': 'cheat caves at main+0x102a390..0x102a47c (end of text segment, text ends at 0x102a390); 120FPS patch caves at 0x3c0430..0x3c0900 and 0xc32890..0xc328dc replace existing code', 'status': 'source claim'},
]
out = {'meta': {'build_id': '67C9CA1428222752F951021464A7B3597EB98FEC', 'offset_note': 'offsets are main-relative (== main.elf vaddr). pchtxt "bytes" are memory byte order; cheats use numeric words (converted to bytes here).',
                'transcription_note': 'fl4sh_* sets were retyped by hand from fetched pages; validate_kb decodes each entry and compares with its own comment to catch typos.'},
       'sets': sets, 'facts': cheat_facts}
json.dump(out, open(ROOT / 'docs/mpr/patches.json', 'w', encoding='utf-8'), indent=1)

# snapshot text + symbol fragment ------------------------------------------------
snap, sym = [], []
def addsym(name, off, kind, role, tags, line, ev, mangled=None, contains=None):
    it = {'type': 'symbol', 'name': name, 'mangled': mangled, 'kind': kind, 'address': off, 'addr_form': 'main_offset' if off else 'unknown', 'claimed_version': '1.0.0',
          'signature': None, 'role': role, 'tags': tags, 'used_by': 'offset patch in community mod', 'src': {'path': 'work/kb/web/patches_snapshot.txt', 'line': line, 'evidence': ev}}
    if contains: it['contains'] = contains
    sym.append(it)
infn = collections.defaultdict(list)
seen = set()
for st in sets:
    snap.append(f'# {st["id"]} {st["url"]}')
    for e in st['entries']:
        snap.append(f'{e["offset"]} {e.get("bytes") or ""} // {e["comment"]}')
        ln = len(snap)
        c = e['comment']
        key = (e['offset'])
        m = re.search(r'(GetScreenWidth|GetScreenHeight)\s+(\w+::\w+)\s*$', c)
        if m: infn[m.group(2)].append((e['offset'], m.group(1), ln, f'{e["offset"]} {e.get("bytes") or ""} // {c}'))
        for nm, off, role in [('GetScreenWidth', '0x00001248', 'getter returning docked width constant (orig 1600, mov w0,#0x640; ret)'), ('GetScreenHeight', '0x00001250', 'getter returning docked height constant (orig 900)'),
                              ('HandheldFullWidth', '0x00001258', 'handheld width getter (orig 1024)'), ('HandheldFullHeight', '0x00001260', 'handheld height getter (orig 576)'),
                              ('HandheldContentWidth', '0x00001268', 'handheld content width getter (orig 1024)'), ('HandheldContentHeight', '0x00001270', 'handheld content height getter (orig 576)'),
                              ('DefaultThirdPersonVerticalFOV', '0x00c8c8ac', 'patched to 80.0 deg; function returns default 3rd-person vertical FOV'),
                              ('DefaultFirstPersonVerticalFOV', '0x00c8c8b8', 'patched to 74.0 deg (first-person vertical FOV default)'),
                              ('ResolutionScaleOrAspect hook (aspect constant load)', '0x00afe3d8', 'patch replaces aspect-ratio float load (1.75 or 2.3889 cave)'),
                              ('Dynamic resolution update (patched to ret)', '0x00aff5d4', 'disabling this function disables dynamic resolution')]:
            if e['offset'] == off and (nm, off) not in seen:
                seen.add((nm, off))
                addsym(nm, off, 'function' if off not in ('0x00afe3d8',) else 'patch_site', role, ['resolution' if 'FOV' not in nm else 'fov'], ln, e['offset'], None)
for fn, sites in infn.items():
    first = sites[0]
    addsym(fn, None, 'function', 'contains call sites of ' + ', '.join(sorted({s[1] for s in sites})) + ' patched in the ultrawide/res mods', ['resolution', 'hud'], first[2], first[0],
           contains=sorted({s[0].lower() for s in sites}))
(W).mkdir(parents=True, exist_ok=True)
(W / 'patches_snapshot.txt').write_text('\n'.join(snap) + '\n', encoding='utf-8')
json.dump({'source_id': 'web_patches', 'items': sym}, open(ROOT / 'docs/mpr/frag/web_patches.json', 'w', encoding='utf-8'), indent=1)
print('sets', len(sets), 'entries', sum(len(s['entries']) for s in sets), 'symbols', len(sym))
