#!/usr/bin/env python3
"""Two-client Story Co-op test runner for the ZeldaOnline fork of Ship of Harkinian.

Starts the C++ OOTServer on a local port, prepares one instance folder per client role (hardlinked exe and
archives, separate config and saves), launches the clients with --coop-test, waits with a hard timeout, kills only
the processes it started, then checks the scenario's assertions against the observation logs.

Usage:
  python tools/coop-test/run.py --build-dir <dir with soh.exe + soh.o2r> --rom <oot.z64> \
      --server <OOTServer.exe> --work <scratch dir> [--scenario smoke.json] [--timeout 240]

Exit code 0 only if every client ran, every script finished with pass, and every assertion held.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def link_or_copy(src, dst):
    if os.path.exists(dst):
        try:
            if os.path.samefile(src, dst):
                return
        except OSError:
            pass
        os.remove(dst)  # stale link from an earlier build
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def link_tree(src_dir, dst_dir):
    for root, _dirs, files in os.walk(src_dir):
        rel = os.path.relpath(root, src_dir)
        for f in files:
            link_or_copy(os.path.join(root, f), os.path.join(dst_dir, rel, f))


def populate(build_dir, folder, extra_o2r=None):
    """Hardlink everything a client needs from the build dir into `folder`."""
    os.makedirs(folder, exist_ok=True)
    for name in os.listdir(build_dir):
        src = os.path.join(build_dir, name)
        low = name.lower()
        if os.path.isfile(src) and low.endswith((".exe", ".dll", ".o2r")):
            if low in ("zapd.exe",):
                continue
            link_or_copy(src, os.path.join(folder, name))
        elif os.path.isdir(src) and name in ("assets", "mods"):
            link_tree(src, os.path.join(folder, name))
    if extra_o2r:
        for p in extra_o2r:
            link_or_copy(p, os.path.join(folder, os.path.basename(p)))


def write_client_config(folder, width, height):
    """Small windowed client, DX11 backend. Everything else is set by the in-game test mode."""
    cfg = {
        "Window": {"Width": width, "Height": height, "Fullscreen": {"Enabled": False},
                   "Backend": {"Id": 0, "Name": "DirectX 11"}},
        "CVars": {"gOpenWindows": {"ModalWindow": 1}},
    }
    with open(os.path.join(folder, "shipofharkinian.json"), "w") as f:
        json.dump(cfg, f, indent=2)


def port_open(host, port):
    try:
        with socket.create_connection((host, port), timeout=0.5):
            return True
    except OSError:
        return False


def kill_tree(proc):
    if proc is None or proc.poll() is not None:
        return
    subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass


def ensure_extracted(args, shared):
    if os.path.exists(os.path.join(shared, "oot.o2r")) or os.path.exists(os.path.join(shared, "oot-mq.o2r")):
        print("[run] oot.o2r already present in", shared)
        return True
    if args.o2r and os.path.exists(args.o2r):
        link_or_copy(args.o2r, os.path.join(shared, "oot.o2r"))
        print("[run] reusing", args.o2r)
        return True
    if not args.rom or not os.path.exists(args.rom):
        print("[run] ERROR: no oot.o2r in shared dir and --rom/--o2r missing", file=sys.stderr)
        return False
    print("[run] extracting oot.o2r (one time)...")
    log = open(os.path.join(args.work, "extract.log"), "w")
    p = subprocess.Popen([os.path.join(shared, "soh.exe"), "--coop-test-extract-only",
                          "--coop-test-rom", os.path.abspath(args.rom)],
                         cwd=shared, stdout=log, stderr=subprocess.STDOUT)
    try:
        p.wait(timeout=args.extract_timeout)
    except subprocess.TimeoutExpired:
        print("[run] extraction timed out", file=sys.stderr)
        kill_tree(p)
        return False
    finally:
        log.close()
    ok = os.path.exists(os.path.join(shared, "oot.o2r"))
    print("[run] extraction", "ok" if ok else "FAILED", "(exit %s)" % p.returncode)
    return ok


def dig(obj, path):
    cur = obj
    for part in path.split("."):
        if isinstance(cur, dict) and part in cur:
            cur = cur[part]
        elif isinstance(cur, list) and part.isdigit() and int(part) < len(cur):
            cur = cur[int(part)]
        else:
            return None
    return cur


def compare(op, got, want):
    if op == "eq":
        return got == want
    if op == "ne":
        return got != want
    if got is None:
        return False
    if op == "ge":
        return got >= want
    if op == "gt":
        return got > want
    if op == "le":
        return got <= want
    if op == "lt":
        return got < want
    if op == "truthy":
        return bool(got)
    raise ValueError("unknown op " + op)


def find_obs(log, name):
    for o in log.get("observations", []):
        if o.get("name") == name:
            return o
    return None


def check(scenario, logs):
    """logs: {role: log dict or None}. Returns list of (ok, message)."""
    results = []
    for role, log in logs.items():
        if log is None:
            results.append((False, "client %s produced no log" % role))
            continue
        results.append((bool(log.get("finished")), "client %s script finished" % role))
        results.append((bool(log.get("pass")), "client %s script pass%s" % (
            role, "" if log.get("pass") else " (error: %s)" % log.get("error"))))
    for a in scenario.get("assertions", []):
        kind = a["type"]
        label = a.get("label") or "%s %s %s" % (kind, a.get("obs"), a.get("path", ""))
        if kind == "each":
            for role, log in logs.items():
                o = find_obs(log, a["obs"]) if log else None
                got = dig(o, a["path"]) if o else None
                ok = o is not None and compare(a.get("op", "eq"), got, a.get("value"))
                results.append((ok, "%s [%s]: got %r, want %s %r" % (label, role, got, a.get("op", "eq"), a.get("value"))))
        elif kind == "count":
            n = 0
            for role, log in logs.items():
                o = find_obs(log, a["obs"]) if log else None
                if o is not None and dig(o, a["path"]) == a.get("value", True):
                    n += 1
            results.append((n == a["equals"], "%s: %d client(s) have %s==%r, want %d" % (
                label, n, a["path"], a.get("value", True), a["equals"])))
        elif kind == "same":
            vals = []
            for role, log in logs.items():
                o = find_obs(log, a["obs"]) if log else None
                vals.append(dig(o, a["path"]) if o else None)
            ok = len(vals) > 0 and all(v is not None and v == vals[0] for v in vals)
            results.append((ok, "%s: values %r" % (label, vals)))
        elif kind == "cross_equal":
            # path_a in role A's observation must equal path_b in role B's observation
            oa = find_obs(logs.get("A"), a["obs"]) if logs.get("A") else None
            ob = find_obs(logs.get("B"), a["obs"]) if logs.get("B") else None
            va = dig(oa, a["path_a"]) if oa else None
            vb = dig(ob, a["path_b"]) if ob else None
            results.append((va is not None and va == vb, "%s: A.%s=%r B.%s=%r" % (label, a["path_a"], va, a["path_b"], vb)))
        else:
            results.append((False, "unknown assertion type " + kind))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=os.environ.get("SOH_BUILD_DIR"),
                    help="folder with soh.exe, soh.o2r (and assets/) [env SOH_BUILD_DIR]")
    ap.add_argument("--rom", default=os.environ.get("SOH_TEST_ROM"), help="OoT NTSC 1.0 z64 [env SOH_TEST_ROM]")
    ap.add_argument("--o2r", default=os.environ.get("SOH_TEST_O2R"),
                    help="an already generated oot.o2r to reuse instead of extracting [env SOH_TEST_O2R]")
    ap.add_argument("--server", default=os.environ.get("OOT_SERVER_EXE"),
                    help="OOTServer executable [env OOT_SERVER_EXE]")
    ap.add_argument("--work", default=os.environ.get("SOH_TEST_WORK"), help="scratch dir [env SOH_TEST_WORK]")
    ap.add_argument("--scenario", default=os.path.join(HERE, "scenarios", "smoke.json"))
    ap.add_argument("--port", type=int, default=21650, help="server port (default 21650, not the game default)")
    ap.add_argument("--timeout", type=int, default=240, help="hard timeout for the clients, seconds")
    ap.add_argument("--extract-timeout", type=int, default=600)
    ap.add_argument("--no-server", action="store_true", help="use an already running server on --port")
    ap.add_argument("--stagger", type=float, default=3.0, help="seconds between launching clients")
    ap.add_argument("--window", default="480x360", help="client window size WxH")
    ap.add_argument("--keep", action="store_true", help="keep the instance folders after the run")
    args = ap.parse_args()

    for need in ("build_dir", "work"):
        if not getattr(args, need):
            ap.error("--%s is required" % need.replace("_", "-"))
    if not args.no_server and not args.server:
        ap.error("--server is required (or --no-server)")
    args.work = os.path.abspath(args.work)
    build_dir = os.path.abspath(args.build_dir)
    os.makedirs(args.work, exist_ok=True)
    scenario_path = os.path.abspath(args.scenario)
    with open(scenario_path) as f:
        scenario = json.load(f)
    width, height = (int(x) for x in args.window.lower().split("x"))

    shared = os.path.join(args.work, "shared")
    populate(build_dir, shared)
    if not ensure_extracted(args, shared):
        return 2
    extra = [os.path.join(shared, n) for n in ("oot.o2r", "oot-mq.o2r") if os.path.exists(os.path.join(shared, n))]

    roles = list(scenario["roles"].keys())
    folders = {}
    for r in roles:
        folders[r] = os.path.join(args.work, "inst_" + r)
        # Fresh saves/config/logs each run, keep the hardlinked binaries.
        for sub in ("Save", "logs"):
            shutil.rmtree(os.path.join(folders[r], sub), ignore_errors=True)
        for fn in ("shipofharkinian.json", "log.json"):
            try:
                os.remove(os.path.join(folders[r], fn))
            except OSError:
                pass
        populate(build_dir, folders[r], extra)
        write_client_config(folders[r], width, height)
    out = {r: os.path.join(args.work, "log_%s.json" % r) for r in roles}
    for p in out.values():
        try:
            os.remove(p)
        except OSError:
            pass

    procs = {}
    server = None
    logs_open = []
    sdir = os.path.join(args.work, "server")
    try:
        if not args.no_server:
            if port_open("127.0.0.1", args.port):
                print("[run] ERROR: port %d already in use; use --port or --no-server" % args.port, file=sys.stderr)
                return 2
            # Fresh server dir: its own config.json and an empty data dir, so no team save leaks between runs.
            shutil.rmtree(sdir, ignore_errors=True)
            os.makedirs(sdir, exist_ok=True)
            server_cfg = os.path.join(os.path.dirname(os.path.abspath(args.server)), "config.json")
            if os.path.exists(server_cfg):
                with open(server_cfg) as f:
                    cfg_text = f.read()
                with open(os.path.join(sdir, "config.json"), "w") as f:
                    f.write(cfg_text)
            slog = open(os.path.join(sdir, "server.log"), "w")
            logs_open.append(slog)
            server = subprocess.Popen([os.path.abspath(args.server), "--port", str(args.port)], cwd=sdir,
                                      stdout=slog, stderr=subprocess.STDOUT)
            for _ in range(50):
                if port_open("127.0.0.1", args.port):
                    break
                time.sleep(0.2)
            else:
                print("[run] ERROR: server did not open port", args.port, file=sys.stderr)
                return 2
            print("[run] server up (pid %d, port %d)" % (server.pid, args.port))

        for i, r in enumerate(roles):
            cl = open(os.path.join(args.work, "stdout_%s.log" % r), "w")
            logs_open.append(cl)
            cmd = [os.path.join(folders[r], "soh.exe"), "--coop-test", scenario_path, "--coop-test-role", r,
                   "--coop-test-out", out[r], "--coop-test-host", "127.0.0.1", "--coop-test-port", str(args.port)]
            procs[r] = subprocess.Popen(cmd, cwd=folders[r], stdout=cl, stderr=subprocess.STDOUT)
            print("[run] started client %s (pid %d)" % (r, procs[r].pid))
            if i + 1 < len(roles):
                time.sleep(args.stagger)

        deadline = time.time() + args.timeout
        while time.time() < deadline and any(p.poll() is None for p in procs.values()):
            time.sleep(0.5)
        timed_out = [r for r, p in procs.items() if p.poll() is None]
        if timed_out:
            print("[run] HARD TIMEOUT, killing clients:", ",".join(timed_out))
    finally:
        for p in procs.values():
            kill_tree(p)
        kill_tree(server)
        for f in logs_open:
            f.close()

    logs = {}
    for r in roles:
        try:
            with open(out[r]) as f:
                logs[r] = json.load(f)
        except (OSError, ValueError):
            logs[r] = None
    results = check(scenario, logs)
    print("\n=== %s ===" % scenario.get("name", os.path.basename(scenario_path)))
    for r in roles:
        print("client %s exit code: %s" % (r, procs[r].returncode if r in procs else "not started"))
    failed = 0
    for ok, msg in results:
        print("%s  %s" % ("PASS" if ok else "FAIL", msg))
        failed += 0 if ok else 1
    print("\nRESULT: %s (%d/%d checks)" % ("PASS" if failed == 0 else "FAIL", len(results) - failed, len(results)))
    print("logs in", args.work)
    if not args.keep:
        for r in roles:
            shutil.rmtree(folders[r], ignore_errors=True)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
