"""Unit tests for edengdb (GDB remote-protocol client, game-state reader, trace loop).

Everything runs against an in-process fake RSP stub: a threading TCP server on 127.0.0.1
with an ephemeral port. No Eden, no game, no real GDB.

Run from the tools test folder:
  ..\\.venv\\Scripts\\python.exe -I -m unittest discover -s tests -p "test_edengdb.py" -v
"""
import json
import math
import os
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest

_TOOLS_TEST = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if _TOOLS_TEST not in sys.path:
    sys.path.insert(0, _TOOLS_TEST)

import edengdb  # noqa: E402
from edengdb import Gdb, GdbError  # noqa: E402

BASE = 0x804CA000  # GameFramework.nss load address in INFO_TEXT

INFO_TEXT = (
    "Process:     0x51 (Application)\n"
    "Program Id:  0x010012101468c000\n"
    "Layout:\n"
    "  Alias: 0x1106400000 - 0x21063fffff\n"
    "Modules:\n"
    "  0x00804c6000 - 0x00804c9fff nnrtld\n"
    "  0x00804ca000 - 0x0085007fff GameFramework.nss\n"
    "  0x0085008000 - 0x00856a1fff multimedia\n"
)
INFO_NO_MAIN = (
    "Process:     0x51 (Application)\n"
    "Modules:\n"
    "  0x00804c6000 - 0x00804c9fff nnrtld\n"
    "  0x0085008000 - 0x00856a1fff multimedia\n"
)

# fake heap addresses for the state chain (any unmapped values would do)
GS = 0x7100000000      # CGameStateMP1 header
ARR = 0x7100001000     # CWorldStateMP1 array
PS = 0x7100002000      # player-state
MGR = 0x7200000000     # CStateManager
XS = 0x7300000000      # state-manager +8 object
CGS = 0x7400000000     # CGameState
LOGIC = 0x7500000000   # logic object
PLAYER = 0x7600000000  # CPlayerMP1
PA = 0x7700000000      # player +8 object
PB = 0x7800000000      # PA +0x20 object
XF = 0x7900000000      # PB +0x40 -> CTransform4f (12 floats)

WORLD = bytes.fromhex("00112233445566778899aabbccddeeff")
OTHER_WORLD = bytes(range(16))
AREA = bytes.fromhex("0f1e2d3c4b5a69788796a5b4c3d2e1f0")


def pkt(payload):
    return b"$" + payload + b"#" + b"%02x" % (sum(payload) & 0xFF)


def pack_transform(x, y, z, rot=(0.6, 0.8)):
    # row-major 4x3: rotation terms at [1] and [5], origin at [3], [7], [11]
    return struct.pack("<12f", 1.0, rot[0], 0.0, x, 0.0, rot[1], 0.0, y, 0.0, 0.0, 1.0, z)


class FakeStub:
    """Minimal RSP server. One client connection, handled sequentially on a worker thread.

    vCont;c with a breakpoint set runs the on_resume(n) hook (n = resume index, 0-based) and then
    sends T05. vCont;c with no breakpoint set sends nothing (the game runs free).
    vCont;s always answers T05.
    """

    def __init__(self, info_text=INFO_TEXT, short_reply=False):
        self.lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.lsock.bind(("127.0.0.1", 0))
        self.lsock.listen(1)
        self.port = self.lsock.getsockname()[1]
        self.info_text = info_text
        self.short_reply = short_reply     # answer memory reads with half the requested bytes
        self.regions = []                  # [start, bytearray]
        self.regs = [0] * 33
        self.breakpoints = set()
        self.requests = []                 # decoded payloads, arrival order
        self.bad_checksums = 0
        self.stray_acks = 0                # '+' bytes received after QStartNoAckMode beyond the one ack
        self.resumes = 0
        self.on_resume = None
        self._allowed_acks = 0
        self._no_ack = False
        self._conn = None
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    # ---- memory image
    def map(self, addr, data):
        self.regions.append([addr, bytearray(data)])

    def write(self, addr, data):
        for start, buf in self.regions:
            if start <= addr and addr + len(data) <= start + len(buf):
                off = addr - start
                buf[off:off + len(data)] = data
                return
        raise KeyError(hex(addr))

    def _peek(self, addr, n):
        for start, buf in self.regions:
            if start <= addr and addr + n <= start + len(buf):
                off = addr - start
                return bytes(buf[off:off + n])
        return None

    # ---- wire
    def _send(self, payload):
        data = payload.encode() if isinstance(payload, str) else payload
        with self._lock:
            self._conn.sendall(b"$" + data + b"#" + b"%02x" % (sum(data) & 0xFF))

    def _junk(self, data):
        n = data.count(b"+")
        if not n:
            return
        use = min(n, self._allowed_acks)
        self._allowed_acks -= use
        if self._no_ack:
            self.stray_acks += n - use

    def _serve(self):
        try:
            self.lsock.settimeout(10)
            conn, _ = self.lsock.accept()
        except OSError:
            self.lsock.close()
            return
        self._conn = conn
        conn.settimeout(10)
        buf = b""
        try:
            while True:
                chunk = conn.recv(65536)
                if not chunk:
                    break
                buf += chunk
                while True:
                    i = buf.find(b"$")
                    if i < 0:
                        self._junk(buf)
                        buf = b""
                        break
                    self._junk(buf[:i])
                    buf = buf[i:]
                    j = buf.find(b"#")
                    if j < 0 or len(buf) < j + 3:
                        break
                    payload = buf[1:j]
                    want = int(buf[j + 1:j + 3], 16)
                    buf = buf[j + 3:]
                    if sum(payload) & 0xFF != want:
                        self.bad_checksums += 1
                        continue
                    self._handle(payload)
        except (OSError, ValueError):
            pass
        finally:
            conn.close()
            self.lsock.close()

    def _handle(self, payload):
        p = payload.decode("latin-1")
        self.requests.append(p)
        if p.startswith("qSupported"):
            self._send("PacketSize=4000;swbreak+;hwbreak+;vContSupported+;QStartNoAckMode+")
        elif p == "QStartNoAckMode":
            self._send("OK")
            self._no_ack = True
            self._allowed_acks = 1          # the client acks this OK once, before it stops acking
        elif p.startswith("qRcmd,"):
            cmd = bytes.fromhex(p[len("qRcmd,"):]).decode()
            self._send(self.info_text.encode().hex() if cmd == "get info" else "E01")
        elif p == "g":
            self._send(struct.pack("<33Q", *self.regs).hex())
        elif p.startswith("m"):
            addr, n = (int(v, 16) for v in p[1:].split(","))
            data = self._peek(addr, n)
            if data is None:
                self._send("E01")
            else:
                if self.short_reply:
                    data = data[:len(data) // 2]
                self._send(data.hex())
        elif p.startswith("Z0,"):
            self.breakpoints.add(int(p.split(",")[1], 16))
            self._send("OK")
        elif p.startswith("z0,"):
            self.breakpoints.discard(int(p.split(",")[1], 16))
            self._send("OK")
        elif p == "vCont;c":
            self._resume()
        elif p == "vCont;s":
            self._send("T05")
        elif p == "qOffsets":
            self._send("TextSeg=804ca000;DataSeg=804ca000")
        else:
            self._send("")

    def _resume(self):
        n = self.resumes
        self.resumes += 1
        if self.breakpoints:
            if self.on_resume is not None:
                self.on_resume(n)
            self._send("T05")

    def wait_until(self, pred, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            if pred():
                return True
            time.sleep(0.01)
        return pred()

    def close(self):
        try:
            self.lsock.close()
        except OSError:
            pass
        self._thread.join(timeout=5)


def build_scene(stub, *, world=WORLD, area=AREA, hp=87.5, igt=1234.5678, state=4,
                pos=(10.5, -3.25, 7.0), matching_world=True, transform=True):
    """Lay out the same chain read_state walks (see edengdb.read_state / tt_probe.cpp)."""
    stub.map(BASE + edengdb.GP_GAME_STATE, struct.pack("<Q", GS))
    stub.map(GS, world + struct.pack("<i4xQQ", 1, ARR, PS))        # count 1 at +0x10, arr +0x18, ps +0x20
    entry_world = world if matching_world else OTHER_WORLD
    stub.map(ARR, entry_world + area + bytes(edengdb.WORLD_STATE_SIZE - 32))
    stub.map(PS + 0xC, struct.pack("<f", hp))
    stub.map(MGR + 8, struct.pack("<Q", XS))
    stub.map(XS + 8, struct.pack("<Q", CGS))
    stub.map(CGS + 0xFFB08, struct.pack("<d", igt))
    stub.map(MGR + 0x4E0150, struct.pack("<Q", LOGIC))
    stub.map(LOGIC + 0x18, struct.pack("<Q", PLAYER))
    stub.map(PLAYER + 8, struct.pack("<Q", PA))
    stub.map(PLAYER + 0x3F0, struct.pack("<I", state))
    stub.map(PA + 0x20, struct.pack("<Q", PB))
    stub.map(PB + 0x40, struct.pack("<Q", XF))
    if transform:
        stub.map(XF, pack_transform(*pos))


class EdenClientTestCase(unittest.TestCase):
    def setUp(self):
        self.stub = FakeStub()
        self.g = Gdb(port=self.stub.port, timeout=10.0)

    def tearDown(self):
        self.g.close()
        self.stub.close()


class ConnectTests(EdenClientTestCase):
    def test_handshake_sends_qsupported_then_noack(self):
        self.assertEqual(self.stub.requests[0],
                         "qSupported:multiprocess+;swbreak+;hwbreak+;vContSupported+")
        self.assertEqual(self.stub.requests[1], "QStartNoAckMode")

    def test_qsupported_reply_with_plus_signs_is_parsed(self):
        reply = self.g.send("qSupported:multiprocess+")
        self.assertEqual(reply, "PacketSize=4000;swbreak+;hwbreak+;vContSupported+;QStartNoAckMode+")

    def test_no_acks_sent_after_noackmode(self):
        self.g.modules()
        self.g.regs()
        self.g.send("qOffsets")
        self.assertEqual(self.stub.stray_acks, 0)
        self.assertEqual(self.stub.bad_checksums, 0)

    def test_stub_counts_stray_acks(self):
        # sanity check of the harness itself: the stub must notice extra '+' bytes in no-ack mode.
        # The stub takes one connection only, so this uses its own stub and raw socket.
        stub = FakeStub()
        s = socket.create_connection(("127.0.0.1", stub.port), timeout=5)
        try:
            s.sendall(pkt(b"QStartNoAckMode"))
            self.assertIn(b"OK", s.recv(64))
            s.sendall(b"+" + b"+++")           # one allowed ack, then three stray ones
            s.sendall(pkt(b"qOffsets"))
            self.assertIn(b"TextSeg", s.recv(256))
        finally:
            s.close()
            stub.close()
        self.assertEqual(stub.stray_acks, 3)


class ModulesTests(EdenClientTestCase):
    def test_modules_parses_the_three_modules(self):
        mods, txt = self.g.modules()
        self.assertEqual(mods, {
            "nnrtld": (0x804C6000, 0x804C9FFF),
            "GameFramework.nss": (0x804CA000, 0x85007FFF),
            "multimedia": (0x85008000, 0x856A1FFF),
        })
        self.assertIn("Program Id:  0x010012101468c000", txt)

    def test_main_base_is_gameframework_module(self):
        self.assertEqual(edengdb.main_base(self.g), BASE)

    def test_main_base_falls_back_to_qoffsets(self):
        stub = FakeStub(info_text=INFO_NO_MAIN)
        g = Gdb(port=stub.port, timeout=10.0)
        try:
            self.assertEqual(edengdb.main_base(g), 0x804CA000)
        finally:
            g.close()
            stub.close()


class MemoryTests(EdenClientTestCase):
    def setUp(self):
        super().setUp()
        self.pattern = bytes((i * 7 + 3) & 0xFF for i in range(0x3000))
        self.stub.map(0x70000000, self.pattern)

    def test_read_across_0x1000_boundary(self):
        data = self.g.read(0x70000800, 0x1800)
        self.assertEqual(data, self.pattern[0x800:0x800 + 0x1800])
        memreqs = [r for r in self.stub.requests if r.startswith("m")]
        self.assertEqual(memreqs, ["m70000800,1000", "m70001800,800"])

    def test_read_inside_one_page(self):
        self.assertEqual(self.g.read(0x70000FF0, 0x20), self.pattern[0xFF0:0x1010])

    def test_try_read_returns_data_when_mapped(self):
        self.assertEqual(self.g.try_read(0x70000000, 4), self.pattern[:4])

    def test_read_unmapped_raises(self):
        with self.assertRaises(GdbError):
            self.g.read(0x90000000, 8)

    def test_read_running_off_the_region_raises(self):
        with self.assertRaises(GdbError):
            self.g.read(0x70002FF8, 0x10)

    def test_try_read_unmapped_is_none(self):
        self.assertIsNone(self.g.try_read(0x90000000, 8))
        self.assertIsNone(self.g.try_read(0x70002FF8, 0x10))

    
    def test_short_memory_reply_is_not_silently_accepted(self):
        # BUG (edengdb.read, lines 93-103): read() never checks the length of an 'm' reply.
        # RSP allows a short reply, so read() returns fewer bytes than asked with no error.
        # read_state() then unpacks the short data and fails with struct.error, which try_read()
        # does not catch.
        # RSP allows an 'm' reply to carry fewer bytes than asked for. read() must then either
        # return the full length or raise GdbError. It currently returns the short data.
        stub = FakeStub()
        stub.short_reply = True
        stub.map(0x70000000, bytes(range(16)))
        g = Gdb(port=stub.port, timeout=10.0)
        try:
            try:
                data = g.read(0x70000000, 16)
            except GdbError:
                return
            self.assertEqual(len(data), 16)
        finally:
            g.close()
            stub.close()


class RegisterTests(EdenClientTestCase):
    def test_regs_decodes_x0_to_x30_sp_pc(self):
        xs = [0x0102030405060700 + i for i in range(31)]
        sp = 0x0000_7FFF_FFFF_F000
        pc = BASE + 0x1234
        self.stub.regs = xs + [sp, pc]
        r = self.g.regs()
        self.assertEqual(len(r["x"]), 31)
        self.assertEqual(r["x"], xs)
        self.assertEqual(r["sp"], sp)
        self.assertEqual(r["pc"], pc)


class ReadStateTests(EdenClientTestCase):
    def test_read_state_full_chain(self):
        build_scene(self.stub)
        st = edengdb.read_state(self.g, BASE, MGR)
        f = struct.unpack("<12f", pack_transform(10.5, -3.25, 7.0))
        self.assertEqual(st["world"], WORLD.hex())
        self.assertEqual(st["area"], AREA.hex())
        self.assertEqual(st["hp"], 87.5)
        self.assertEqual(st["igt"], 1234.5678)
        self.assertEqual(st["pos"], [10.5, -3.25, 7.0])
        self.assertAlmostEqual(st["yaw"], math.atan2(-f[1], f[5]), places=6)
        self.assertEqual(st["state"], 4)
        self.assertEqual(set(st), {"world", "area", "hp", "igt", "pos", "yaw", "state"})

    def test_read_state_without_state_manager_skips_igt_and_pos(self):
        build_scene(self.stub)
        st = edengdb.read_state(self.g, BASE)
        self.assertEqual(st["world"], WORLD.hex())
        self.assertEqual(st["hp"], 87.5)
        self.assertNotIn("igt", st)
        self.assertNotIn("pos", st)

    def test_read_state_empty_memory_is_empty_dict(self):
        self.assertEqual(edengdb.read_state(self.g, BASE), {})
        self.assertEqual(edengdb.read_state(self.g, BASE, MGR), {})

    def test_read_state_no_matching_world_entry(self):
        build_scene(self.stub, matching_world=False)
        st = edengdb.read_state(self.g, BASE, MGR)
        self.assertNotIn("world", st)
        self.assertNotIn("area", st)
        self.assertEqual(st["hp"], 87.5)
        self.assertEqual(st["pos"], [10.5, -3.25, 7.0])

    def test_read_state_missing_transform_drops_pos_yaw_state(self):
        build_scene(self.stub, transform=False)
        st = edengdb.read_state(self.g, BASE, MGR)
        self.assertEqual(st["igt"], 1234.5678)
        self.assertNotIn("pos", st)
        self.assertNotIn("yaw", st)
        self.assertNotIn("state", st)


class TraceTests(EdenClientTestCase):
    def _run_trace(self, frames=5):
        build_scene(self.stub, pos=(0.0, 0.0, 0.0))
        # frame n is the stop from resume n; the game moves 0.5 in x and 0.25 in y per frame
        self.stub.on_resume = lambda n: self.stub.write(XF, pack_transform(0.5 * n, 2.0 + 0.25 * n, 3.0))
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        out = os.path.join(tmp.name, "trace.jsonl")
        n = edengdb.trace(self.g, BASE, out, frames, MGR, timeout=10.0)
        with open(out) as f:
            lines = [json.loads(x) for x in f if x.strip()]
        return n, lines

    def test_trace_writes_header_and_one_line_per_frame(self):
        n, lines = self._run_trace()
        self.assertEqual(n, 5)
        self.assertEqual(len(lines), 6)
        self.assertEqual(lines[0], {"trace": 1, "source": "eden"})

    def test_trace_frame_lines_carry_state_and_velocity(self):
        _, lines = self._run_trace()
        for k, line in enumerate(lines[1:]):
            self.assertEqual(line["f"], k)
            self.assertEqual(line["pos"], [0.5 * k, 2.0 + 0.25 * k, 3.0])
            self.assertEqual(line["world"], WORLD.hex())
            self.assertIsInstance(line["ms"], (int, float))
            if k == 0:
                self.assertNotIn("vel", line)
            else:
                prev = lines[k]["pos"]
                expected = [(a - b) * 60.0 for a, b in zip(line["pos"], prev)]
                self.assertEqual(line["vel"], expected)
        self.assertEqual(lines[2]["vel"], [30.0, 15.0, 0.0])

    def test_trace_sets_and_clears_update_breakpoint(self):
        self._run_trace()
        bp = f"Z0,{BASE + edengdb.FN_UPDATE:x},4"
        clr = f"z0,{BASE + edengdb.FN_UPDATE:x},4"
        self.assertTrue(self.stub.wait_until(lambda: clr in self.stub.requests))
        self.assertIn(bp, self.stub.requests)
        self.assertNotIn(BASE + edengdb.FN_UPDATE, self.stub.breakpoints)

    
    def test_trace_leaves_connection_in_sync(self):
        # BUG (edengdb.trace, lines 276-281): after the last frame the loop has already issued
        # cont(), so the game is running with the breakpoint still set. The following
        # clear_bp() then sends z0 while a stop may be in flight: it reads that T05 as its reply,
        # and the real 'OK' is left unread in the socket. Every later reply is then off by one.
        self._run_trace()
        self.assertEqual(self.g.send("qOffsets"), "TextSeg=804ca000;DataSeg=804ca000")


if __name__ == "__main__":
    unittest.main()
