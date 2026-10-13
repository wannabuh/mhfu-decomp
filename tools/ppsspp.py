"""PPSSPP as an oracle: run the game in PPSSPPHeadless (with a throwaway home directory, so your own PPSSPP settings
are not touched) and drive it through its WebSocket debugger.

    with Headless(iso) as pp:            # connected, the game running
        pp.pause()
        regs = pp.request("cpu.getAllRegs")

Notes from getting this to work (PPSSPP 1.20.4):
  - the debugger's web server takes about 80 s to answer its first connection; the game waits at its entry point
    meanwhile (--debugger breaks at start), and is resumed once connected;
  - the software renderer is needed: with the default (null) graphics, or the SDL build offscreen, the game stops
    getting vblanks once the debugger is attached, and goes idle for good;
  - pausing and resuming are acknowledged by broadcast events (cpu.stepping, cpu.resume), not replies.
Headless runs unthrottled - about 100 times the PSP's speed.

Requests get their reply (matched by ticket); events PPSSPP sends meanwhile (a breakpoint hit: "cpu.stepping") are
queued for wait_event().
"""
import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import time


class Debugger:
    def __init__(self, port, timeout=60):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET /debugger HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
                          .encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("debugger closed the connection")
            buf += chunk
        if b" 101 " not in buf.split(b"\r\n", 1)[0]:
            raise ConnectionError(buf.decode(errors="replace"))
        self.buf = buf.split(b"\r\n\r\n", 1)[1]
        self.events = []
        self.ticket = 0
        self.partial = b""

    def _send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        n = len(data)
        if n < 126:
            hdr = bytes([0x81, 0x80 | n])
        elif n < 65536:
            hdr = bytes([0x81, 0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr = bytes([0x81, 0x80 | 127]) + struct.pack(">Q", n)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(data)) if n < 4096 else \
            (int.from_bytes(data, "little") ^ int.from_bytes((mask * (n // 4 + 1))[:n], "little")).to_bytes(n, "little")
        self.sock.sendall(hdr + mask + masked)

    def _frame(self):
        """A complete frame from the buffer (fin, opcode, payload), consumed - or None if not all here yet"""
        if len(self.buf) < 2:
            return None
        b0, b1 = self.buf[0], self.buf[1]
        n, off = b1 & 0x7F, 2
        if n == 126:
            if len(self.buf) < 4:
                return None
            n, off = struct.unpack(">H", self.buf[2:4])[0], 4
        elif n == 127:
            if len(self.buf) < 10:
                return None
            n, off = struct.unpack(">Q", self.buf[2:10])[0], 10
        if len(self.buf) < off + n:
            return None
        payload, self.buf = self.buf[off:off + n], self.buf[off + n:]
        return b0 & 0x80, b0 & 0x0F, payload

    def _recv(self):
        """The next message. Frames are only consumed whole, and fragments kept, so a read timing out (in
        wait_event) loses nothing."""
        while True:
            f = self._frame()
            if f is None:
                chunk = self.sock.recv(1 << 22)
                if not chunk:
                    raise ConnectionError("debugger closed the connection")
                self.buf += chunk
                continue
            fin, opcode, payload = f
            if opcode in (0x8, 0x9, 0xA):       # close / ping / pong
                continue
            self.partial += payload
            if fin:
                msg, self.partial = self.partial, b""
                return json.loads(msg)

    def request(self, event, **args):
        self.ticket += 1
        t = str(self.ticket)
        self._send(json.dumps({"event": event, "ticket": t, **args}))
        while True:
            m = self._recv()
            if m.get("ticket") == t:
                if m.get("event") == "error":
                    raise RuntimeError(f"{event}: {m.get('message')}")
                return m
            self.events.append(m)

    def send(self, event, **args):
        """A request answered by a broadcast event rather than a reply (cpu.stepping, cpu.resume)."""
        self._send(json.dumps({"event": event, **args}))

    def pause(self):
        self.send("cpu.stepping")
        return self.wait_event("cpu.stepping")

    def resume(self):
        self.send("cpu.resume")
        return self.wait_event("cpu.resume")

    def wait_event(self, name, timeout=30.0):
        """The next event called name (others are dropped)."""
        deadline = time.time() + timeout
        while True:
            while self.events:
                m = self.events.pop(0)
                if m.get("event") == name:
                    return m
            self.sock.settimeout(max(0.1, deadline - time.time()))
            try:
                self.events.append(self._recv())
            except socket.timeout:
                return None
            finally:
                self.sock.settimeout(60)
            if time.time() > deadline and not self.events:
                return None

    def read(self, address, size):
        return base64.b64decode(self.request("memory.read", address=address, size=size)["base64"])

    def write(self, address, data):
        self.request("memory.write", address=address, base64=base64.b64encode(data).decode())


class Headless:
    """PPSSPPHeadless running the ISO with the software renderer and the WebSocket debugger on a free port."""

    def __init__(self, iso, extra=("-i",)):
        self.iso = iso
        self.extra = list(extra)    # the interpreter by default: breakpoints are not reliably hit under the JIT

    def __enter__(self):
        self.home = tempfile.mkdtemp(prefix="mhfu-ppsspp-")
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        self.log = open(os.path.join(self.home, "ppsspp.log"), "w")
        self.proc = subprocess.Popen(["PPSSPPHeadless", self.iso, f"--debugger={self.port}", "--graphics=software", *self.extra,
                                      "--timeout=86400"], cwd=self.home, env=dict(os.environ, HOME=self.home),
                                     stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 180
        while time.time() < deadline:
            try:
                self.dbg = Debugger(self.port, timeout=2)
            except OSError:
                if self.proc.poll() is not None:
                    raise RuntimeError("PPSSPPHeadless exited; see " + self.log.name)
                time.sleep(0.5)
                continue
            self.dbg.sock.settimeout(60)
            self.dbg.resume()
            return self.dbg
        raise RuntimeError("could not connect to PPSSPP's debugger")

    def __exit__(self, *exc):
        self.proc.kill()
        self.proc.wait()
        self.log.close()
        shutil.rmtree(self.home, ignore_errors=True)
