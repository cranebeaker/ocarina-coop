"""Builds the Windows player zip for Ocarina of Time Story Co-op.

python tools/release/package.py --steam-dll <steam_api64.dll> [--build-dir x64/Release] [--out dist] [--version v1.0.0]

The zip holds the game (no ROM, no game assets: the game builds them from the player's own ROM on the first start),
the server, the Steam relay helper and the two shortcuts. steam_api64.dll is Valve's Steamworks runtime from the
Steamworks.NET Standalone release 2025.164.1 (Windows-x64); its SHA-256 is checked.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile

STEAM_DLL_SHA256 = "eb17909a76668cf9ae0b92a618a34a50f6c73d3a6787cb4dd8ce36a8b10bfb75"
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

README = """Ocarina of Time Story Co-op
===========================

Play the whole of Ocarina of Time with friends: one shared story, one team save, and you time travel together.
Built on Ship of Harkinian (HarbourMasters) and ShipwrightOnline (lukegrahamSydney).
Project page: https://github.com/cranebeaker/ocarina-coop

You need your own supported Ocarina of Time ROM. No game files are included.

First start
-----------
1. Start soh.exe once. When it asks for your ROM, pick it (or put the .z64 file in this folder first).
   The game builds oot.o2r from it; this takes a minute and only happens once.
2. Close the game.

Play over Steam (no IP addresses, no port forwarding; Steam must be running)
------------------------------------------------------------------------------
Host:  double-click "Host over Steam.bat". It starts the server, opens a friends-only Steam lobby and your game.
Join:  double-click "Join over Steam.bat". It finds a friend who is hosting and starts your game connected to them.
       With it open, right-click your friend in Steam and Join Game works too.
Then the host opens the online window, creates a party with "Story Co-op" turned on, and invites everyone.

Play by address instead
-----------------------
Run server\\OOTServer.exe (port 21050) and connect from the online window in the game.

Files
-----
soh.exe, soh.o2r, assets\\        the game (Ship of Harkinian with ShipwrightOnline and Story Co-op)
server\\OOTServer.exe, config.json the server; team saves go to server\\data\\story
OOTSteamLink.exe, steam_api64.dll  the Steam relay helper (Valve's free "Spacewar" app, AppID 480)
"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "x64", "Release"))
    ap.add_argument("--server-exe", default=os.path.join(ROOT, "server", "build", "OOTServer.exe"))
    ap.add_argument("--steam-dll", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--version", default="v1.0.0")
    args = ap.parse_args()

    if sha256(args.steam_dll) != STEAM_DLL_SHA256:
        sys.exit("steam_api64.dll does not match the pinned SHA-256")

    link_dir = os.path.join(ROOT, "tools", "steamlink")
    link_exe = os.path.join(link_dir, "out", "OOTSteamLink.exe")
    if subprocess.call(["cmd", "/c", os.path.join(link_dir, "build.bat")]) != 0 or not os.path.exists(link_exe):
        sys.exit("OOTSteamLink build failed")

    name = "OcarinaStoryCoop-" + args.version + "-windows"
    stage = os.path.join(args.out, name)
    if os.path.exists(stage):
        shutil.rmtree(stage)
    os.makedirs(os.path.join(stage, "server"))

    for f in ("soh.exe", "soh.o2r"):
        shutil.copy2(os.path.join(args.build_dir, f), stage)
    shutil.copytree(os.path.join(args.build_dir, "assets"), os.path.join(stage, "assets"))
    pad_db = os.path.join(ROOT, "build", "x64", "gamecontrollerdb.txt")
    if os.path.exists(pad_db):
        shutil.copy2(pad_db, stage)

    shutil.copy2(args.server_exe, os.path.join(stage, "server"))
    shutil.copy2(os.path.join(ROOT, "server", "config.json"), os.path.join(stage, "server"))

    shutil.copy2(link_exe, stage)
    shutil.copy2(args.steam_dll, os.path.join(stage, "steam_api64.dll"))
    for bat in ("Host over Steam.bat", "Join over Steam.bat"):
        shutil.copy2(os.path.join(link_dir, bat), stage)
    with open(os.path.join(stage, "steam_appid.txt"), "w") as f:
        f.write("480")
    with open(os.path.join(stage, "README.txt"), "w", newline="\r\n") as f:
        f.write(README)

    zip_path = stage + ".zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _, files in os.walk(stage):
            for f in files:
                full = os.path.join(base, f)
                z.write(full, os.path.join(name, os.path.relpath(full, stage)))
    print(zip_path, os.path.getsize(zip_path) // (1 << 20), "MB", sha256(zip_path))


if __name__ == "__main__":
    main()
