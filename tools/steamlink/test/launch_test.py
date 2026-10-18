#!/usr/bin/env python3
"""Real game launch path: OOTSteamLink host (mock Steam) + OOTSteamLink join that starts soh.exe itself.

The join tool finds soh.exe next to it, sets OOT_SERVER=127.0.0.1:<relay port> for it and starts it. The game
(no test flags, plain launch) must then connect through the relay and the server log must show the player.

Usage: python launch_test.py --work <steamlink-test dir with H, J and server_run> --build-dir <soh.exe dir>
       --o2r <oot.o2r> [--server-exe <OOTServer.exe>] [--wait 90]
The game is started from <work>/JG (hardlinks of the build plus the join tool and the mock DLL).
"""
import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "coop-test"))
sys.path.insert(0, HERE)
import run as coop_run  # noqa: E402  (tools/coop-test/run.py: populate, write_client_config, kill_tree)
import e2e_test  # noqa: E402  (spawn, wait_line)

SERVER_PORT = 22350
LOCAL_PORT = 22351


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--o2r", required=True)
    ap.add_argument("--server-exe", default=None)
    ap.add_argument("--wait", type=int, default=90)
    a = ap.parse_args()
    e2e_test.MOCK_BASE = "48600"
    server_exe = os.path.normpath(os.path.abspath(a.server_exe or os.path.join(a.work, "server_run", "OOTServer.exe")))
    log_path = os.path.join(os.path.dirname(server_exe), "OOTServer.log")
    if os.path.exists(log_path):
        os.remove(log_path)
    shutil.rmtree(os.path.join(os.path.dirname(server_exe), "data"), ignore_errors=True)

    H, J = os.path.join(a.work, "H"), os.path.join(a.work, "J")
    G = os.path.join(a.work, "JG")
    for sub in ("Save", "logs"):
        shutil.rmtree(os.path.join(G, sub), ignore_errors=True)
    coop_run.populate(a.build_dir, G, [a.o2r])
    coop_run.write_client_config(G, 480, 360)
    for f in ("OOTSteamLink.exe", "steam_api64.dll", "steam_appid.txt"):
        coop_run.link_or_copy(os.path.join(J, f), os.path.join(G, f))

    host = join = None
    game_pids = []
    ok = False
    try:
        host = e2e_test.spawn(H, "Host", ["host", "--no-game", "--port", str(SERVER_PORT), "--server-exe", server_exe])
        if not e2e_test.wait_line(host, r"Lobby id: \d+"):
            print("FAIL host did not come up:\n" + "\n".join(host.lines))
            return 1
        # no --no-launch: the tool starts soh.exe from its own folder
        join = e2e_test.spawn(G, "Client", ["join", "--local-port", str(LOCAL_PORT)])
        if not e2e_test.wait_line(join, r"Started the game \(server 127\.0\.0\.1:%d\)" % LOCAL_PORT):
            print("FAIL join tool did not start the game:\n" + "\n".join(join.lines))
            return 1
        print("join tool started soh.exe with OOT_SERVER=127.0.0.1:%d" % LOCAL_PORT, flush=True)
        end = time.time() + a.wait
        auth = conns = 0
        text = ""
        while time.time() < end:
            time.sleep(2)
            text = open(log_path, errors="replace").read() if os.path.exists(log_path) else ""
            auth = len(re.findall(r"player \d+ authenticated", text))
            conns = len(re.findall(r"connection from 127\.0\.0\.1", text))
            if auth >= 1:
                break
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq soh.exe", "/FO", "CSV", "/NH"], capture_output=True,
                             text=True).stdout
        print("server log: %d connection(s), %d authenticated player(s)" % (conns, auth))
        for l in text.splitlines():
            if "authenticated" in l or "connection from" in l:
                print("  " + l)
        ok = auth >= 1
    finally:
        # stop only what this test started: the join tool's game child, then the tools
        if join is not None:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(join.pid)], capture_output=True)
        if host is not None and host.poll() is None:
            host.send_signal(signal.CTRL_BREAK_EVENT)
            try:
                host.wait(40)
            except subprocess.TimeoutExpired:
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(host.pid)], capture_output=True)
        print("--- host log ---\n" + "\n".join(host.lines if host else []))
        print("--- join log ---\n" + "\n".join(join.lines if join else []))
    print("LAUNCH", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
