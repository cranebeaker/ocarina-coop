#!/usr/bin/env python3
"""End-to-end check: real OOTServer + two real soh.exe clients, all game traffic through OOTSteamLink
(mock steam_api64.dll, so one PC is enough).

  host tool  ->  starts OOTServer.exe on SERVER_PORT, lobby, relays to it
  join tool  ->  listens on LOCAL_PORT, relays over the mock Steam to the host
  tools/coop-test/run.py runs the smoke scenario with both clients pointed at LOCAL_PORT (--no-server)

Passes when the scenario passes and the server log shows both players authenticated.

Usage: python e2e_test.py --work <steamlink-test dir> --build-dir <soh.exe dir> --o2r <oot.o2r>
       [--scenario <scenario json>] [--server-exe <OOTServer.exe>]
Layout of --work: H and J (tool + mock DLL as steam_api64.dll), server_run (OOTServer.exe + config.json).
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SERVER_PORT = 22250
LOCAL_PORT = 22251
MOCK_BASE = "48500"


def spawn(folder, name, args):
    env = dict(os.environ, STEAMLINK_MOCK_NAME=name, STEAMLINK_MOCK_PORT_BASE=MOCK_BASE)
    p = subprocess.Popen([os.path.join(folder, "OOTSteamLink.exe")] + args + ["--quiet-errors"], cwd=folder, env=env,
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    p.lines = []
    threading.Thread(target=lambda: [p.lines.append(l.rstrip()) for l in p.stdout], daemon=True).start()
    return p


def wait_line(p, pattern, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        if any(re.search(pattern, l) for l in list(p.lines)):
            return True
        if p.poll() is not None:
            return False
        time.sleep(0.2)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--o2r", required=True)
    ap.add_argument("--scenario", default=os.path.join(REPO, "tools", "coop-test", "scenarios", "smoke.json"))
    ap.add_argument("--server-exe", default=None)
    ap.add_argument("--timeout", default="300")
    a = ap.parse_args()
    server_exe = a.server_exe or os.path.join(a.work, "server_run", "OOTServer.exe")
    log_path = os.path.join(os.path.dirname(server_exe), "OOTServer.log")
    if os.path.exists(log_path):
        os.remove(log_path)
    shutil.rmtree(os.path.join(os.path.dirname(server_exe), "data"), ignore_errors=True)

    with open(a.scenario) as f:
        sc = json.load(f)
    sc["server"]["host"] = "127.0.0.1"
    sc["server"]["port"] = LOCAL_PORT
    scen = os.path.join(a.work, "e2e_scenario.json")
    with open(scen, "w") as f:
        json.dump(sc, f)

    H, J = os.path.join(a.work, "H"), os.path.join(a.work, "J")
    host = join = None
    ok = False
    try:
        host = spawn(H, "Host", ["host", "--no-game", "--port", str(SERVER_PORT), "--server-exe", server_exe])
        if not wait_line(host, r"Lobby id: \d+"):
            print("FAIL host did not come up:\n" + "\n".join(host.lines))
            return 1
        join = spawn(J, "Client", ["join", "--no-launch", "--local-port", str(LOCAL_PORT)])
        if not wait_line(join, r"Relaying 127\.0\.0\.1"):
            print("FAIL join did not come up:\n" + "\n".join(join.lines))
            return 1
        print("relay up, running scenario through it", flush=True)
        r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "coop-test", "run.py"),
                            "--build-dir", a.build_dir, "--o2r", a.o2r, "--server", server_exe,
                            "--work", os.path.join(a.work, "e2e_work"), "--scenario", scen,
                            "--port", str(LOCAL_PORT), "--no-server", "--timeout", a.timeout])
        scenario_ok = r.returncode == 0
        print("scenario exit code", r.returncode)
        # stop the host tool cleanly: it Ctrl+Breaks the server so the log is flushed
        host.send_signal(signal.CTRL_BREAK_EVENT)
        try:
            host.wait(40)
        except subprocess.TimeoutExpired:
            print("host tool did not stop on Ctrl+Break")
        time.sleep(1)
        text = open(log_path, errors="replace").read() if os.path.exists(log_path) else ""
        auth = len(re.findall(r"player \d+ authenticated", text))
        conns = len(re.findall(r"connection from 127\.0\.0\.1", text))
        print("server log: %d connection(s) from 127.0.0.1, %d authenticated player(s)" % (conns, auth))
        for l in text.splitlines():
            if "authenticated" in l or "connection from" in l:
                print("  " + l)
        ok = scenario_ok and auth >= 2
    finally:
        for p in (join, host):
            if p and p.poll() is None:
                subprocess.run(["taskkill", "/F", "/PID", str(p.pid)], capture_output=True)
        print("--- host log ---\n" + "\n".join(host.lines if host else []))
        print("--- join log ---\n" + "\n".join(join.lines if join else []))
    print("E2E", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
