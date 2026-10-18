# Story Co-op QA report

All results come from runs with real game clients (soh.exe) and a real OOTServer on loopback, driven by `tools/coop-test/run.py`. A check is listed as passing only if a run log showed it. Counts are as reported by the runner.

## How to run

Prerequisites: a built game (`x64/Release/soh.exe`), a built server (`server/build/OOTServer.exe`), an `oot.o2r` generated for this SoH version (or a legal ROM via `--rom`), and a work directory outside the repo. Never commit ROMs, o2r files, saves or logs. Windows only (uses taskkill and hardlinks). Small game windows appear during runs. The runner kills only the PIDs it starts.

```
python tools/coop-test/run.py --build-dir x64/Release --server server/build/OOTServer.exe \
  --work <work dir> --o2r <path to oot.o2r> --rom <path to ROM> \
  --scenario tools/coop-test/scenarios/<name>.json --timeout 300
```

Environment variables SOH_BUILD_DIR, OOT_SERVER_EXE, SOH_TEST_WORK, SOH_TEST_ROM, SOH_TEST_O2R replace the flags. Extra flags: `--stage <name part>`, `--keep`. See `tools/coop-test/README.md`.

Timeouts: `timetravel` needs `--timeout 1100` (800 passed once; 400 and 300 failed by hard timeout mid-run). `dungeon` about 500, `catchup_persist` about 200 per stage (about 8 min total). Others about 300.

Steam link (mock Steam DLL): `python tools/steamlink/test/stream_test.py --work <work>/steamlink-test`, `e2e_test.py` and `launch_test.py` with `--work`, `--build-dir x64/Release`, `--o2r`.

## Results

| Scenario / check | Result | Checks |
|---|---|---|
| Game build (soh, Release) | pass | exit 0 |
| Server build (OOTServer) | pass | exit 0 |
| smoke | pass | 32/32 |
| smoke negative control (story_coop false) | fail as intended | 1/31, timed out at story_active |
| items (Story Co-op on) | pass | 36/36 (first run 34/36, test errors, fixed) |
| items_off | pass | 16/16 |
| flags | pass | 30/30 |
| catchup_persist (join catch-up, reconnect, title screen, restart, corrupt saves, same-nickname parties) | pass | 101/101 |
| dungeon (3 stages) | pass | 73/73 (baseline before fix: 46/50, fail) |
| sync_gaps | pass | 115/115 |
| dungeon_live (incl. private dungeons stage) | pass | 40/40 |
| dungeon_crash | pass | 21/21 |
| time_field | pass | 65/65 |
| timetravel | pass | 55/55 with --timeout 1100 (22/55 at 300 s and 48/55 at 400 s, hard timeout) |
| coop_features (teleport to member, enemy sync, Story Co-op on and off) | pass | 28/28 |
| single_player (no server) | pass | 7/7 |
| cutscenes (area intro together, scene cutscene together, member elsewhere keeps the intro, trigger drops mid-cutscene, Story Co-op off) | pass | 39/39 |
| npc_shield (gear loss per player, key NPC dialogue shown to the party, Story Co-op off) | pass | 37/37 |
| steamlink stream_test (mock) | pass | 10/10 |
| steamlink e2e_test (mock, real server, 2 clients) | pass | smoke 32/32 |
| steamlink launch_test | pass | game started with OOT_SERVER, player authenticated |


## Bugs found and fixed by the suite

1. Client dropped the `storyCode` party setting, so no client could pick the server team-save key and an admin settings update could clear the server's code. Fixed (90bc3a0eb).
2. Two unrelated parties whose admins shared a nickname and set no code loaded each other's save. Story Co-op now always has a team code (client generated, or server generated and echoed). Fixed (4a4d90ed9).
3. Backup rotation copied a damaged team save over the newest good backup. Server now rotates only when the main file checksum is intact. Fixed (4a4d90ed9).
4. Server could not be stopped cleanly on Windows by a script. Now handles SIGBREAK and flushes team saves; stdout unbuffered. Fixed (4a4d90ed9).
5. Toggled-off dungeon switches and clear flags came back on, because the persisted copy was OR-only and the team save is a union. Clients now track set and unset, the server drops the bit and broadcasts packet 41. Fixed (6ae25297d).
6. Upstream bug: the client scene-flag UNSET packet omitted scene key and room, so the server dropped every unset as malformed. Fixed (6ae25297d).
7. Uncaught filesystem_error on save temp-file remove or rename (Windows file lock) killed the game from the worker thread. Now retries, then logs. Fixed (f00334bd9).
8. Steamlink e2e and launch tests failed with error 267 on forward-slash server paths. Fixed in the test tools (a36de4e3b).

Test-only flaws fixed along the way: empty inventory slot reads 255 not 0; bow ammo 30 matched the default; corruption helper XOR repaired already-corrupt files; title-screen wait assumed no world; enemy-kill assertion gave false failures because Stalchildren respawn (now dense sampling).
