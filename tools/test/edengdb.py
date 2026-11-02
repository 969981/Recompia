"""Minimal GDB remote-protocol client for Eden's GDB stub (work area S5), plus the game-state reader that mirrors runtime/src/testtools/tt_probe.cpp.

Eden (v0.2.1, src/core/debugger) listens on [Debugging] gdbstub_port (default 6543) when use_gdbstub is on. A client connecting PAUSES the game; it keeps
running only after a continue. Supported packets used here: qSupported, QStartNoAckMode, qRcmd (monitor get info / mappings), m (memory), g/p (registers),
Z0/z0 (software breakpoints), vCont (c, s), 0x03 interrupt.

  python edengdb.py info [--port 6543]                      module list and the main module base
  python edengdb.py state [--port 6543]                     read the probe fields once (game paused while reading, then continued)
  python edengdb.py trace out.jsonl --frames 600 [--port]   break on every CGameArchitectureSupport::Update, write one trace line per frame (same JSONL as runtime tt)
"""
import argparse
import json
import math
import socket
import struct
import sys
import time

# ---- the probe layout: main-module offsets of v1.0.0, see runtime/src/testtools/tt_probe.cpp for where each comes from ----
GP_GAME_STATE = 0x459CC70
WORLD_STATE_SIZE = 0x50
FN_UPDATE = 0x3C81C8              # CGameArchitectureSupport::Update(float*): the frame boundary
FN_PROCESS_INPUT = 0xC70334       # CPlayerMP1::ProcessInput(CFinalInput const&, CStateManager&): x0 = player, x2 = state manager


class GdbError(RuntimeError):
    pass


class Gdb:
    def __init__(self, host="127.0.0.1", port=6543, timeout=30.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""
        self.no_ack = False
        self.send("qSupported:multiprocess+;swbreak+;hwbreak+;vContSupported+")
        self.send("QStartNoAckMode")
        self.no_ack = True

    # ---- framing
    @staticmethod
    def _cksum(b):
        return sum(b) & 0xFF

    def _write(self, payload):
        data = payload.encode() if isinstance(payload, str) else payload
        self.sock.sendall(b"$" + data + b"#" + b"%02x" % self._cksum(data))

    def _read_packet(self, timeout=None):
        if timeout is not None:
            self.sock.settimeout(timeout)
        while True:
            i = self.buf.find(b"$")
            if i >= 0:
                j = self.buf.find(b"#", i)
                if j >= 0 and len(self.buf) >= j + 3:
                    payload = self.buf[i + 1:j]
                    self.buf = self.buf[j + 3:]
                    if not self.no_ack:
                        self.sock.sendall(b"+")
                    return payload
            chunk = self.sock.recv(65536)
            if not chunk:
                raise GdbError("connection closed")
            # drop acks
            self.buf += chunk.replace(b"+", b"") if b"$" not in self.buf and b"$" not in chunk else chunk

    def send(self, payload, timeout=30.0):
        self._write(payload)
        r = self._read_packet(timeout)
        return r.decode(errors="replace")

    # ---- high level
    def monitor(self, cmd):
        self._write("qRcmd," + cmd.encode().hex())
        out = b""
        while True:
            r = self._read_packet(30)
            if r.startswith(b"O") and len(r) > 1:
                out += bytes.fromhex(r[1:].decode())
            elif r == b"OK":
                break
            elif r.startswith(b"E"):
                raise GdbError("monitor " + cmd + ": " + r.decode())
            else:
                try:                      # Eden answers a monitor command with the text hex-encoded in one packet
                    out += bytes.fromhex(r.decode())
                except ValueError:
                    out += r
                break
        return out.decode(errors="replace")

    def read(self, addr, n):
        out = b""
        while n > 0:
            c = min(n, 0x1000)
            r = self.send("m%x,%x" % (addr, c))
            if r.startswith("E"):
                raise GdbError("read %x+%x failed" % (addr, c))
            chunk = bytes.fromhex(r)
            if len(chunk) != c:
                raise GdbError("short read at %x: %d of %d bytes" % (addr, len(chunk), c))
            out += chunk
            addr += c
            n -= c
        return out

    def try_read(self, addr, n):
        try:
            return self.read(addr, n)
        except (GdbError, ValueError):
            return None

    def regs(self):
        """x0..x30, sp, pc of the active thread (aarch64 'g' layout: 8 bytes each)."""
        r = self.send("g")
        b = bytes.fromhex(r)
        vals = struct.unpack("<" + "Q" * 33, b[:33 * 8])
        return {"x": list(vals[:31]), "sp": vals[31], "pc": vals[32]}

    def set_bp(self, addr):
        r = self.send("Z0,%x,4" % addr)
        if r != "OK":
            raise GdbError("Z0 failed: " + r)

    def clear_bp(self, addr):
        self.send("z0,%x,4" % addr)

    def cont(self):
        self._write("vCont;c")

    def step(self):
        self._write("vCont;s")
        return self._read_packet(30)

    def interrupt(self):
        self.sock.sendall(b"\x03")

    def wait_stop(self, timeout=30.0):
        return self._read_packet(timeout)

    def modules(self):
        """name -> (base, end) from `monitor get info`."""
        txt = self.monitor("get info")
        mods = {}
        in_mods = False
        for line in txt.splitlines():
            if line.strip().startswith("Modules:"):
                in_mods = True
                continue
            if in_mods:
                parts = line.split()
                if len(parts) >= 4 and parts[0].startswith("0x") and parts[1] == "-":
                    name = " ".join(parts[3:])
                    mods.setdefault(name, (int(parts[0], 16), int(parts[2], 16)))
        return mods, txt

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def main_base(g):
    mods, txt = g.modules()
    for k, (b, e) in mods.items():
        if k.lower() in ("main", "gameframework.nss"):
            return b
    # fall back to qOffsets (TextSeg = main module entry)
    r = g.send("qOffsets")
    if r.startswith("TextSeg="):
        return int(r[8:].split(";")[0], 16)
    raise GdbError("cannot find the main module in: " + txt[:400])


def u64(b, o=0):
    return struct.unpack_from("<Q", b, o)[0]


def read_state(g, base, mgr=0, debug=False):
    """The same fields as tt_probe.cpp read_state: dict with world/area (32 hex), hp, igt, pos, yaw, state (what could be read)."""
    out = {}
    dbg = {"mgr": mgr}
    gp = g.try_read(base + GP_GAME_STATE, 8)
    gs = u64(gp) if gp else 0
    if gs:
        hdr = g.try_read(gs, 0x28)
        if hdr:
            world = hdr[:16]
            count = struct.unpack_from("<i", hdr, 0x10)[0]
            arr = u64(hdr, 0x18)
            ps = u64(hdr, 0x20)
            if 0 < count < 64 and arr:
                for i in range(count):
                    e = g.try_read(arr + i * WORLD_STATE_SIZE, 0x20)
                    if e and e[:16] == world:
                        out["world"] = world.hex()
                        out["area"] = e[16:32].hex()
                        break
            if ps:
                h = g.try_read(ps + 0xC, 4)
                if h:
                    out["hp"] = struct.unpack("<f", h)[0]
    if mgr:
        w = g.try_read(mgr + 8, 8)
        x = g.try_read(u64(w) + 8, 8) if w else None
        if x:
            ig = g.try_read(u64(x) + 0xFFB08, 8)
            if ig:
                v = struct.unpack("<d", ig)[0]
                if math.isfinite(v):
                    out["igt"] = v
        lg = g.try_read(mgr + 0x4E0150, 8)
        pl = g.try_read(u64(lg) + 0x18, 8) if lg and u64(lg) else None
        player = u64(pl) if pl else 0
        dbg["logic"] = u64(lg) if lg else None
        dbg["player"] = player
        if player:
            a = g.try_read(player + 8, 8)
            b = g.try_read(u64(a) + 0x20, 8) if a else None
            c = g.try_read(u64(b) + 0x40, 8) if b else None
            m = g.try_read(u64(c), 48) if c else None
            dbg["a"], dbg["b"], dbg["c"] = (u64(a) if a else None), (u64(b) if b else None), (u64(c) if c else None)
            dbg["xf_raw"] = m.hex() if m else None
            if m:
                f = struct.unpack("<12f", m)
                out["pos"] = [f[3], f[7], f[11]]
                out["yaw"] = math.atan2(-f[1], f[5])
                st = g.try_read(player + 0x3F0, 4)
                if st:
                    out["state"] = struct.unpack("<I", st)[0]
    if debug:
        out["_dbg"] = {k: (hex(v) if isinstance(v, int) else v) for k, v in dbg.items()}
    return out


def raw_dump(g, base, mgr):
    """Hex dumps of the objects on the player chain, for checking the probe layout against the live game."""
    r = {}

    def grab(name, addr, n):
        b = g.try_read(addr, n) if addr else None
        r[name] = {"addr": hex(addr or 0), "hex": b.hex() if b else None}
        return b

    grab("mgr_0x4e0140", mgr + 0x4E0140, 0x60)
    grab("mgr_0", mgr, 0x20)
    lg = grab("logic_ptr", mgr + 0x4E0150, 8)
    logic = u64(lg) if lg else 0
    grab("logic", logic, 0x60)
    pl = g.try_read(logic + 0x18, 8) if logic else None
    player = u64(pl) if pl else 0
    grab("player_0", player, 0x40)
    grab("player_0x3e0", player + 0x3E0, 0x30)
    a = u64(g.try_read(player + 8, 8) or bytes(8)) if player else 0
    grab("a", a, 0x60)
    b = u64(g.try_read(a + 0x20, 8) or bytes(8)) if a else 0
    grab("b", b, 0x80)
    c = u64(g.try_read(b + 0x40, 8) or bytes(8)) if b else 0
    grab("c", c, 0x60)
    grab("b_plus_0x40_inline", b + 0x40, 0x30)
    return r


def capture_manager(g, base, wait_s=30.0):
    """Break at ProcessInput until the state manager (x2) is known; leaves the game running. Returns the CStateManager pointer or 0."""
    bp = base + FN_PROCESS_INPUT
    g.set_bp(bp)
    g.cont()
    try:
        g.wait_stop(wait_s)
    except (socket.timeout, OSError):
        g.interrupt()
        try:
            g.wait_stop(5)
        except (socket.timeout, OSError):
            pass
        g.clear_bp(bp)
        g.cont()
        return 0
    r = g.regs()
    mgr = r["x"][2]
    g.clear_bp(bp)
    g.step()
    g.cont()
    return mgr


def trace(g, base, out_path, frames, mgr=0, timeout=60.0):
    """One JSONL line per game frame, taken at every call of CGameArchitectureSupport::Update."""
    bp = base + FN_UPDATE
    prev = None
    with open(out_path, "w") as f:
        f.write(json.dumps({"trace": 1, "source": "eden"}) + "\n")
        g.set_bp(bp)
        g.cont()
        k = 0
        t0 = time.time()
        while k < frames:
            try:
                g.wait_stop(timeout)
            except socket.timeout:
                break
            st = read_state(g, base, mgr)
            line = {"f": k}
            line.update(st)
            if prev is not None and "pos" in st and "pos" in prev:
                line["vel"] = [(a - b) * 60.0 for a, b in zip(st["pos"], prev["pos"])]
            prev = st
            line["ms"] = round((time.time() - t0) * 1000.0, 3)
            f.write(json.dumps(line) + "\n")
            k += 1
            g.clear_bp(bp)
            if k >= frames:
                g.cont()                 # the game stays paused at the breakpoint otherwise
                return k
            g.step()
            g.set_bp(bp)
            g.cont()
        g.clear_bp(bp)
        g.cont()
    return k


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["info", "state", "trace"])
    ap.add_argument("out", nargs="?")
    ap.add_argument("--port", type=int, default=6543)
    ap.add_argument("--frames", type=int, default=600)
    a = ap.parse_args(argv)
    g = Gdb(port=a.port)
    try:
        if a.cmd == "info":
            mods, txt = g.modules()
            print(txt)
            print("main base:", hex(main_base(g)))
            g.cont()
        elif a.cmd == "state":
            base = main_base(g)
            st = read_state(g, base, 0, debug=True)
            print(json.dumps(st))
            g.cont()
        else:
            base = main_base(g)
            mgr = capture_manager(g, base)
            print("state manager:", hex(mgr))
            n = trace(g, base, a.out, a.frames, mgr)
            print("frames:", n)
    finally:
        g.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
