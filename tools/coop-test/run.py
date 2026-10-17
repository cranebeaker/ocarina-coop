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
import glob
import json
import os
import re
import shutil
import signal
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
                if "roles" in a and role not in a["roles"]:
                    continue
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
        elif kind == "rel":
            # compare two observations, possibly of different roles: b - a == delta, or `op` between b and a
            sides = []
            for side in (a["a"], a["b"]):
                lg = logs.get(side["role"])
                ob = find_obs(lg, side["obs"]) if lg else None
                sides.append(dig(ob, side["path"]) if ob else None)
            va, vb = sides
            if va is None or vb is None:
                ok = False
            elif "delta" in a:
                ok = (vb - va) == a["delta"]
            else:
                ok = compare(a.get("op", "eq"), vb, va)
            want = ("b - a == %r" % a["delta"]) if "delta" in a else ("b %s a" % a.get("op", "eq"))
            results.append((ok, "%s: a=%r b=%r, want %s" % (label, va, vb, want)))
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


def start_server(args, sdir, logf):
    """Starts OOTServer in `sdir` (its own config.json and data dir). Returns the Popen or None."""
    if port_open("127.0.0.1", args.port):
        print("[run] ERROR: port %d already in use; use --port or --no-server" % args.port, file=sys.stderr)
        return None
    flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)  # lets us send CTRL_BREAK for a clean stop
    server = subprocess.Popen([os.path.abspath(args.server), "--port", str(args.port)], cwd=sdir,
                              stdout=logf, stderr=subprocess.STDOUT, creationflags=flags)
    for _ in range(50):
        if port_open("127.0.0.1", args.port):
            print("[run] server up (pid %d, port %d)" % (server.pid, args.port))
            return server
        if server.poll() is not None:
            break
        time.sleep(0.2)
    print("[run] ERROR: server did not open port", args.port, file=sys.stderr)
    kill_tree(server)
    return None


def stop_server_clean(server, timeout=20):
    """Ctrl+Break the server so it flushes the team saves. Returns (exited, exit code)."""
    if server is None:
        return False, None
    if server.poll() is not None:
        return True, server.returncode
    try:
        server.send_signal(getattr(signal, "CTRL_BREAK_EVENT", signal.SIGTERM))
        server.wait(timeout=timeout)
        return True, server.returncode
    except subprocess.TimeoutExpired:
        kill_tree(server)
        return False, None


def corrupt_file(path):
    """Overwrite bytes in the middle of the file, same size, so only the checksum can notice."""
    with open(path, "r+b") as f:
        data = bytearray(f.read())
        if len(data) < 8:
            data = bytearray(b"garbage!")
        mid = len(data) // 2
        for i in range(mid, min(len(data) - 4, mid + 16)):
            data[i] = 0xA5  # not a XOR, so corrupting a copy of an already corrupt file keeps it corrupt
        f.seek(0)
        f.write(data)
        f.truncate()


def do_pre(action, sdir):
    story = os.path.join(sdir, "data", "story")
    if action == "corrupt_bin":
        files = glob.glob(os.path.join(story, "*.bin"))
        for f in files:
            corrupt_file(f)
        return ["corrupted %d .bin file(s): %s" % (len(files), ", ".join(os.path.basename(f) for f in files))]
    if action == "corrupt_all":
        files = glob.glob(os.path.join(story, "*.bin")) + glob.glob(os.path.join(story, "*.bak*"))
        for f in files:
            corrupt_file(f)
        return ["corrupted %d save file(s) incl. backups: %s" % (len(files), ", ".join(os.path.basename(f) for f in files))]
    if action == "truncate_bin":
        files = glob.glob(os.path.join(story, "*.bin"))
        for f in files:
            with open(f, "r+b") as fh:
                fh.truncate(10)
        return ["truncated %d .bin file(s)" % len(files)]
    raise ValueError("unknown pre action " + action)


def check_server(stage, sdir, log_text, stopped):
    """Stage level checks on the server: log text, files, liveness and clean exit. Returns list of (ok, msg)."""
    results = []
    for c in stage.get("server_checks", []):
        kind = c["type"]
        label = c.get("label", kind)
        if kind == "log_contains":
            n = log_text.count(c["text"])
            results.append((n >= c.get("min", 1), "server log: %s (%d x %r)" % (label, n, c["text"])))
        elif kind == "log_absent":
            n = log_text.count(c["text"])
            results.append((n == 0, "server log: %s (%d x %r)" % (label, n, c["text"])))
        elif kind == "files":
            found = glob.glob(os.path.join(sdir, c["glob"]))
            results.append((len(found) >= c.get("min", 1), "server files: %s (%d match %s)" % (label, len(found), c["glob"])))
        elif kind == "alive":
            results.append((bool(stopped.get("alive_at_end")), "server still running at end of stage: " + label))
        elif kind == "clean_exit":
            ok = bool(stopped.get("exited")) and stopped.get("code") == 0
            results.append((ok, "server clean exit: %s (exited=%s code=%s)" % (label, stopped.get("exited"), stopped.get("code"))))
        else:
            results.append((False, "unknown server check " + kind))
    return results


def run_clients(args, scenario_path, roles, folders, out, width, height, build_dir, extra):
    """Launch the clients of one stage, wait with a hard timeout, kill them. Returns the Popen objects."""
    for r in roles:
        # Fresh saves/config/logs each stage, keep the hardlinked binaries.
        for sub in ("Save", "logs"):
            shutil.rmtree(os.path.join(folders[r], sub), ignore_errors=True)
        for fn in ("shipofharkinian.json", "log.json"):
            try:
                os.remove(os.path.join(folders[r], fn))
            except OSError:
                pass
        populate(build_dir, folders[r], extra)
        write_client_config(folders[r], width, height)
    for p in out.values():
        try:
            os.remove(p)
        except OSError:
            pass
    procs = {}
    opened = []
    try:
        for i, r in enumerate(roles):
            cl = open(os.path.join(args.work, "stdout_%s.log" % r), "w")
            opened.append(cl)
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
        for f in opened:
            f.close()
    return procs


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
    ap.add_argument("--timeout", type=int, default=240, help="hard timeout for the clients of one stage, seconds")
    ap.add_argument("--extract-timeout", type=int, default=600)
    ap.add_argument("--no-server", action="store_true", help="use an already running server on --port (no server checks)")
    ap.add_argument("--stagger", type=float, default=3.0, help="seconds between launching clients")
    ap.add_argument("--window", default="480x360", help="client window size WxH")
    ap.add_argument("--stage", default=None, help="run only stages whose name contains this text (later stages may need earlier ones)")
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

    # A scenario is either one stage (roles + assertions at the top level) or "stages", each with its own roles,
    # assertions and server handling: "server": "fresh" (new empty data dir), "keep" (same server and data) or
    # "restart" (clean stop, then start again on the same data dir); "pre": actions on the saved files while the
    # server is down (restart only); "stop_server": stop it cleanly after the stage; "server_checks": checks on
    # the server log, files, liveness and exit.
    stages = scenario.get("stages") or [{"name": scenario.get("name", "run"), "server": "fresh"}]
    base = {k: v for k, v in scenario.items() if k not in ("stages", "assertions", "roles")}

    if args.stage:
        stages = [st for st in stages if args.stage in st.get("name", "")]
        if not stages:
            ap.error("no stage matches --stage")
        stages[0] = dict(stages[0], server="fresh")
    sdir = os.path.join(args.work, "server")
    log_path = os.path.join(sdir, "server.log")
    server = None
    server_log = None
    all_results = []
    all_roles = set()
    try:
        for si, stage in enumerate(stages):
            sname = stage.get("name", "stage%d" % si)
            multi = len(stages) > 1
            stage_scn = dict(base)
            stage_scn.update({k: v for k, v in stage.items() if k in ("roles", "boot", "cvars")})
            if "roles" not in stage_scn:
                stage_scn["roles"] = scenario["roles"]
            stage_scn["name"] = scenario.get("name", "run") + ("/" + sname if multi else "")
            stage_path = os.path.join(args.work, "stage_%d.json" % si)
            with open(stage_path, "w") as f:
                json.dump(stage_scn, f)
            roles = list(stage_scn["roles"].keys())
            all_roles.update(roles)
            print("\n[run] ---- stage %d/%d: %s ----" % (si + 1, len(stages), sname))

            log_start = 0
            stopped = {}
            if not args.no_server:
                mode = stage.get("server", "fresh")
                if mode in ("restart", "fresh") and server is not None:
                    exited, code = stop_server_clean(server)
                    print("[run] server stopped between stages: exited=%s code=%s" % (exited, code))
                    server = None
                if mode == "restart":
                    for action in stage.get("pre", []):
                        for line in do_pre(action, sdir):
                            print("[run] pre:", line)
                if server is None:
                    if mode == "fresh":
                        shutil.rmtree(sdir, ignore_errors=True)
                    os.makedirs(sdir, exist_ok=True)
                    server_cfg = os.path.join(os.path.dirname(os.path.abspath(args.server)), "config.json")
                    if mode == "fresh" and os.path.exists(server_cfg):
                        with open(server_cfg) as f:
                            cfg_text = f.read()
                        if stage.get("server_config"):
                            # scenario overrides, e.g. {"dungeonsResetTimer": 0} to reset a dungeon instance on every first entry
                            merged = json.loads(cfg_text)
                            merged.update(stage["server_config"])
                            cfg_text = json.dumps(merged, indent=1)
                        with open(os.path.join(sdir, "config.json"), "w") as f:
                            f.write(cfg_text)
                    if server_log is not None:
                        server_log.close()
                    # one server log per fresh start (server_<stage>.log), appended to across restarts
                    if mode == "fresh" or not log_path:
                        log_path = os.path.join(args.work, "server_%s.log" % re.sub(r"[^A-Za-z0-9_]", "_", sname))
                    server_log = open(log_path, "w" if mode == "fresh" else "a")
                    server = start_server(args, sdir, server_log)
                    if server is None:
                        return 2
                try:
                    log_start = os.path.getsize(log_path)
                except OSError:
                    log_start = 0

            folders = {r: os.path.join(args.work, "inst_" + r) for r in roles}
            out = {r: os.path.join(args.work, "log_%s%s.json" % (r, "_s%d" % si if multi else "")) for r in roles}
            procs = run_clients(args, stage_path, roles, folders, out, width, height, build_dir, extra)

            logs = {}
            for r in roles:
                try:
                    with open(out[r]) as f:
                        logs[r] = json.load(f)
                except (OSError, ValueError):
                    logs[r] = None
            stage_results = check({"assertions": stage.get("assertions", [] if multi else scenario.get("assertions", []))}, logs)

            if not args.no_server:
                stopped["alive_at_end"] = server is not None and server.poll() is None
                if stage.get("stop_server", False):
                    exited, code = stop_server_clean(server)
                    stopped.update(exited=exited, code=code)
                    server = None
                try:
                    with open(log_path, "r", errors="replace") as f:
                        f.seek(log_start)
                        log_text = f.read()
                except OSError:
                    log_text = ""
                stage_results += check_server(stage, sdir, log_text, stopped)

            print("\n=== %s ===" % stage_scn["name"])
            for r in roles:
                print("client %s exit code: %s" % (r, procs[r].returncode if r in procs else "not started"))
            for ok, msg in stage_results:
                print("%s  %s" % ("PASS" if ok else "FAIL", msg))
                all_results.append((ok, ("[%s] " % sname if multi else "") + msg))
    finally:
        if server is not None:
            kill_tree(server)
        if server_log is not None:
            server_log.close()

    failed = sum(1 for ok, _ in all_results if not ok)
    print("\nRESULT: %s (%d/%d checks)" % ("PASS" if failed == 0 else "FAIL", len(all_results) - failed, len(all_results)))
    for ok, msg in all_results:
        if not ok:
            print("  FAILED:", msg)
    print("logs in", args.work)
    if not args.keep:
        for r in all_roles:
            shutil.rmtree(os.path.join(args.work, "inst_" + r), ignore_errors=True)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
