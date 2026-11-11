# SPDX-License-Identifier: GPL-3.0-or-later
"""Downloads a DLL the package does not carry, for the launcher's download buttons (release model b).

    python -I tools/build/fetch_dlss.py --dir <install folder>/upscalers/streamline                    NVIDIA's nvngx_dlss.dll
    python -I tools/build/fetch_dlss.py --file framegen --dir <install folder>/upscalers/fsr3          AMD's amd_fidelityfx_framegeneration_vk.dll

nvngx_dlss.dll:
The package does not carry this file: it is under the NVIDIA RTX SDKs LICENSE (licenses/LICENSE.NVIDIA-RTX-SDKs.txt), whose
redistribution terms do not fit a GPL-3.0 package. The launcher shows that licence and runs this script only when the player asks.
The file comes from NVIDIA's own repository at a fixed tag and must match the pinned sha256 (it is byte-identical to the
bin/x64/nvngx_dlss.dll of the Streamline SDK v2.14.16 release that sl.dlss.dll of this package belongs to).

amd_fidelityfx_framegeneration_vk.dll (FSR 3.1 frame generation) is AMD's signed build from the FidelityFX SDK v2.4.0 (MIT licence), taken from
AMD's own repository at that tag and checked against the pinned sha256; it is the build the loader of this package (amd_fidelityfx_loader_vk.dll)
belongs to.
Output for the launcher, one per line on stdout:  @progress download <done> <total> <text>  /  @error <text>
Exit codes: 0 present and verified, 1 error.
"""
import argparse
import hashlib
import os
import sys
import urllib.request

# file -> (url, sha256, size, name, where it comes from)
FILES = {
    'dlss': ('https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll',
             'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e', 58956912, 'nvngx_dlss.dll', 'NVIDIA (github.com/NVIDIA/DLSS, v310.9.16)'),
    'framegen': ('https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll',
                 'd002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4', 36806928, 'amd_fidelityfx_framegeneration_vk.dll',
                 'AMD (github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, v2.4.0)'),
}


def say(kind, *parts):
    print('@' + kind + ' ' + ' '.join(str(p) for p in parts), flush=True)


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', required=True)
    ap.add_argument('--file', choices=sorted(FILES), default='dlss')
    a = ap.parse_args()
    URL, SHA256, SIZE, NAME, SOURCE = FILES[a.file]
    dest = os.path.join(a.dir, NAME)
    part = dest + '.part'
    try:
        os.makedirs(a.dir, exist_ok=True)
        if os.path.isfile(dest) and os.path.getsize(dest) == SIZE and sha256_of(dest) == SHA256:
            say('progress', 'download', SIZE, SIZE, '%s is already here and matches the release.' % NAME)
            return 0
        say('progress', 'download', 0, SIZE, 'Downloading %s from %s...' % (NAME, SOURCE))
        h = hashlib.sha256()
        done = 0
        req = urllib.request.Request(URL, headers={'User-Agent': 'PokopiaLauncher'})
        with urllib.request.urlopen(req, timeout=30) as r, open(part, 'wb') as out:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                out.write(b)
                h.update(b)
                done += len(b)
                if done > SIZE + (1 << 20):
                    raise RuntimeError('the download is larger than the pinned release; stopped')
                say('progress', 'download', done, SIZE, 'Downloading %s...' % NAME)
        if done != SIZE or h.hexdigest() != SHA256:
            raise RuntimeError('the downloaded file does not match the pinned release (%d bytes, sha256 %s); it was discarded'
                               % (done, h.hexdigest()))
        os.replace(part, dest)
        say('progress', 'download', SIZE, SIZE, '%s downloaded and verified.' % NAME)
        return 0
    except Exception as e:  # noqa: BLE001 - reported to the launcher in one line
        say('error', 'Could not get %s: %s' % (NAME, e))
        return 1
    finally:
        try:
            os.remove(part)
        except OSError:
            pass


if __name__ == '__main__':
    sys.exit(main())
