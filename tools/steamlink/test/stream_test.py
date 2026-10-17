#!/usr/bin/env python3
"""OOTSteamLink stream tests over the mock steam_api64.dll (one PC, no Steam needed).

Needs two folders, each with OOTSteamLink.exe and the mock DLL renamed to steam_api64.dll:
  <work>/H (host side), <work>/J (join side)
Usage: python stream_test.py --work <dir with H and J> [--quick]

Checks:
  1 a 16 MB random stream round-trips intact through host -> echo server -> back (both directions at once)
  2 two parallel connections do not mix their bytes
  3 a server side close reaches the game side, and a game side close reaches the server
  4 reconnect: new connections work after the old ones closed
  5 the join side restarts (hard kill) and reconnects
  6 the host disappears: the join side closes its local connections
Prints PASS/FAIL per check and exits non-zero on any failure.
"""
import argparse
import hashlib
import os
import random
import re
import socket
import subprocess
import sys
import threading
import time

ECHO_PORT = 22150
LOCAL_PORT = 22151
MOCK_BASE = "48400"

results = []


def report(name, ok, detail=""):
    results.append(ok)
    print("%s  %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


class Echo(threading.Thread):
    """TCP echo server. A connection that sends b'BYE!' is closed by the server. Tracks closes seen."""

    def __init__(self, port):
        super().__init__(daemon=True)
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(16)
        self.client_closed = 0
        self.accepted = 0

    def run(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            self.accepted += 1
            threading.Thread(target=self.handle, args=(c,), daemon=True).start()

    def handle(self, c):
        try:
            while True:
                d = c.recv(65536)
                if not d:
                    self.client_closed += 1
                    break
                if d.endswith(b"BYE!"):
                    c.sendall(d[:-4])
                    break
                c.sendall(d)
        except OSError:
            pass
        finally:
            c.close()


def spawn(folder, name, args):
    env = dict(os.environ, STEAMLINK_MOCK_NAME=name, STEAMLINK_MOCK_PORT_BASE=MOCK_BASE)
    p = subprocess.Popen([os.path.join(folder, "OOTSteamLink.exe")] + args + ["--quiet-errors"], cwd=folder, env=env,
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    p.lines = []

    def pump():
        for line in p.stdout:
            p.lines.append(line.rstrip())
    threading.Thread(target=pump, daemon=True).start()
    return p


def wait_line(p, pattern, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        for l in list(p.lines):
            if re.search(pattern, l):
                return l
        if p.poll() is not None:
            break
        time.sleep(0.1)
    return None


def connect(retries=50):
    for _ in range(retries):
        try:
            s = socket.create_connection(("127.0.0.1", LOCAL_PORT), timeout=5)
            return s
        except OSError:
            time.sleep(0.2)
    return None


def roundtrip(s, data, chunk=100000):
    """Send `data` while receiving the echo concurrently. Returns the received bytes (len(data) expected)."""
    got = bytearray()
    err = []

    def rx():
        try:
            while len(got) < len(data):
                d = s.recv(1 << 20)
                if not d:
                    break
                got.extend(d)
        except OSError as e:
            err.append(e)
    t = threading.Thread(target=rx)
    t.start()
    try:
        for i in range(0, len(data), chunk):
            s.sendall(data[i:i + chunk])
    except OSError as e:
        err.append(e)
    t.join(120)
    return bytes(got)


def kill(p):
    if p and p.poll() is None:
        subprocess.run(["taskkill", "/F", "/PID", str(p.pid)], capture_output=True)
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--quick", action="store_true")
    a = ap.parse_args()
    H, J = os.path.join(a.work, "H"), os.path.join(a.work, "J")
    echo = Echo(ECHO_PORT)
    echo.start()
    host = join = None
    try:
        host = spawn(H, "Host", ["host", "--no-server", "--no-game", "--port", str(ECHO_PORT)])
        line = wait_line(host, r"Lobby id: \d+")
        report("host creates a lobby", bool(line), line or "\n".join(host.lines))
        if not line:
            return
        join = spawn(J, "Client", ["join", "--no-launch", "--local-port", str(LOCAL_PORT)])
        report("join finds the hosting friend and joins", bool(wait_line(join, r"Relaying 127\.0\.0\.1")),
               "\n".join(join.lines) if not wait_line(join, r"Relaying", 1) else "")

        # 1: big stream, both directions at once
        size = (2 if a.quick else 16) * 1024 * 1024
        rnd = random.Random(1)
        data = bytes(rnd.getrandbits(8) for _ in range(4096)) * (size // 4096)
        data = bytearray(data)
        for i in range(0, len(data), 997):  # make it non-periodic
            data[i] = rnd.getrandbits(8)
        data = bytes(data)
        s = connect()
        t0 = time.time()
        got = roundtrip(s, data)
        dt = time.time() - t0
        report("1 %d MB round-trip intact" % (size >> 20), hashlib.sha256(got).digest() == hashlib.sha256(data).digest(),
               "(%d of %d bytes, %.1f s)" % (len(got), len(data), dt))
        # odd sizes: 1 byte, exactly one chunk, chunk +- 1
        ok = True
        for n in (1, 15999, 16000, 16001, 32000, 65537):
            blob = os.urandom(n)
            if roundtrip(s, blob) != blob:
                ok = False
                print("   size %d mismatch" % n)
        report("1b odd-sized writes (1, chunk-1, chunk, chunk+1, 2*chunk, 65537)", ok)
        s.close()

        # 2: parallel connections
        conns = [connect() for _ in range(3)]
        blobs = [os.urandom(3 * 1024 * 1024) for _ in conns]
        outs = [None] * 3

        def work(i):
            outs[i] = roundtrip(conns[i], blobs[i])
        ts = [threading.Thread(target=work, args=(i,)) for i in range(3)]
        [t.start() for t in ts]
        [t.join(120) for t in ts]
        report("2 three parallel connections keep their own bytes", all(outs[i] == blobs[i] for i in range(3)))
        for c in conns:
            c.close()

        # 3: close propagation
        time.sleep(2)  # let closes of earlier connections arrive
        before = echo.client_closed
        s = connect()
        s.sendall(b"hello")
        s.settimeout(10)
        r = s.recv(100)
        s.sendall(b"BYE!")
        eof = False
        try:
            while True:
                d = s.recv(100)
                if not d:
                    eof = True
                    break
        except OSError:
            pass
        report("3a server side close reaches the game side", r == b"hello" and eof)
        s.close()
        s = connect()
        s.sendall(b"x")
        s.recv(10)
        s.close()
        time.sleep(1.5)
        report("3b game side close reaches the server", echo.client_closed == before + 1,
               "(%d -> %d)" % (before, echo.client_closed))

        # 4: reconnect several times
        ok = True
        for i in range(5):
            s = connect()
            blob = os.urandom(50000)
            ok = ok and roundtrip(s, blob) == blob
            s.close()
        report("4 five reconnects work", ok)

        # 5: join side hard-killed and restarted
        kill(join)
        join = spawn(J, "Client", ["join", "--no-launch", "--local-port", str(LOCAL_PORT)])
        wait_line(join, r"Relaying 127\.0\.0\.1")
        s = connect()
        blob = os.urandom(200000)
        report("5 join side restart then reconnect", roundtrip(s, blob) == blob)
        s.close()

        # 6: host disappears
        s = connect()
        s.sendall(b"alive")
        s.recv(10)
        kill(host)
        host = None
        s.settimeout(40)
        closed = False
        t0 = time.time()
        try:
            while True:
                d = s.recv(100)
                if not d:
                    closed = True
                    break
        except OSError:
            closed = True if time.time() - t0 < 39 else False
        report("6 host vanishes: local connection closes", closed, "(after %.0f s)" % (time.time() - t0))
    finally:
        kill(join)
        kill(host)
        if host:
            print("--- host log ---\n" + "\n".join(host.lines))
        if join:
            print("--- join log ---\n" + "\n".join(join.lines))
    print("\n%d/%d checks passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) and results else 1)


if __name__ == "__main__":
    main()
