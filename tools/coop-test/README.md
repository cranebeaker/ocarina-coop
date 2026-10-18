# coop-test: automated two-client Story Co-op harness

Runs real game clients against a local `OOTServer` and checks what each one observed. Nothing here affects normal
play: the in-game test mode only turns on when `--coop-test` or `--coop-test-extract-only` is on the command line.

## Requirements

- The game build (`soh.exe`, `soh.o2r`, `assets/`) and the server build (`OOTServer.exe`).
- Your own OoT NTSC 1.0 ROM, or an `oot.o2r` already generated from it for this build. The ROM is only read to
  generate `oot.o2r` once.
- Python 3. Windows only (uses `taskkill` and hardlinks).

## Run

```
python tools/coop-test/run.py \
    --build-dir <folder with soh.exe> \
    --server <path to OOTServer.exe> \
    --work <scratch folder, outside the repo> \
    --rom <path to oot.z64>          (or --o2r <existing oot.o2r>) \
    [--scenario tools/coop-test/scenarios/smoke.json] [--timeout 240] [--port 21650] [--keep]
```

Each option also has an environment variable: `SOH_BUILD_DIR`, `OOT_SERVER_EXE`, `SOH_TEST_WORK`, `SOH_TEST_ROM`,
`SOH_TEST_O2R`.

What it does:

1. Hardlinks the exe and archives into `<work>/shared`, and the first time obtains `oot.o2r` (reuses `--o2r`, else
   generates it unattended with `soh.exe --coop-test-extract-only --coop-test-rom <rom>`, all popups auto-confirmed).
2. Hardlinks everything into `<work>/inst_A` and `<work>/inst_B` (separate config and saves per client) and writes a
   small windowed config (`--window`, default 480x360).
3. Starts `OOTServer.exe --port <port>` in a fresh `<work>/server` folder with an empty data dir (so no team save
   leaks between runs), then the clients 3 s apart.
4. Waits up to `--timeout` seconds, then kills only the processes it started.
5. Reads `<work>/log_A.json` and `<work>/log_B.json`, evaluates the scenario assertions, prints a summary and exits
   non-zero on any failure. The instance folders are deleted unless `--keep` is given.

The default port is 21650 so a real server on 21050 is never touched. The run aborts if the port is in use. Use
`--no-server` to test against a server you started yourself.

## Scenarios

A scenario is JSON with `server` (host, port), `boot` (entrance and age to start in), per-role `steps` for roles
`A` and `B` (role `name` is the in-game nickname), and `assertions` evaluated by `run.py`. Steps (counts are in game
frames):

| op | fields |
| --- | --- |
| `wait_play` | wait until a save is loaded and no transition is running |
| `connect` | connect to the server (the nickname and endpoint come from the scenario) |
| `warp` | `entrance`, `scene` (waits until that scene is up) |
| `wait_scene` | `scene` |
| `wait_until` | `cond`: `connected`, `net_id`, `other_players` (`n`), `has_party`, `invite_pending`, `party_size` (`n`), `story_active` (`value`), `party_admin` (`value`), `peers_in_scene` (`n`), `peer_puppets` (`n`), `flag` (`type`, `flag`, `value`), `item` (`slot`, `value`), `ammo` (`slot`, `n`), `rupees` / `health` / `health_capacity` (`n`), `quest_bit` (`bit`, `value`), `field` (`field`, `value`, `cmp` `eq`/`ge`/`le`/`ne`), `share_ammo_setting` / `story_coop_setting` (`value`), `story_code_set`, `disconnected` |
| `create_party` | optional `story_coop`, `share_ammo`, `private_dungeons`, `story_code` (`"auto"` generates one like the party window; omit it to let the server generate one); creates the party with those settings |
| `invite` | optional `name`; invites every listed player not in the party |
| `accept_invite` | accepts the first pending invite |
| `set_party_settings` | same fields as `create_party`; sends a settings update (admin) |
| `leave_party` | |
| `wait_frames` | `n` |
| `observe` | `name`, optional `flags` (`type`, `flag`) and `fields` (names as for `set_field`) to record |
| `set_flag`, `unset_flag` | `type` (`switch`, `clear`, `collect`, `treasure`, `event_chk_inf`, `inf_table`, `item_get_inf`, `quest_item`), `flag`; `unset_flag` supports `switch` and `clear` only (the toggle-off path) |
| `reset_inventory` | empties items, ammo, quest items, dungeon items, upgrades; 3 hearts, 0 rupees (the debug save starts full) |
| `give_item` | `item` (item id), optional `map_index`; calls `Item_Give`, so the OnItemReceive hooks fire like a pickup |
| `set_field`, `add_field` | `field`, `value`: a named save field (`world_map`, `high_score:N`, `horse_scene` / `horse_x` / `horse_y` / `horse_z` / `horse_angle`, `scarecrow_spawn_set`, `scarecrow_spawn_note:K`, `scarecrow_long_set`, `scarecrow_long_note:K`, `scene_rooms:S`, `scene_floors:S`, `event_inf:N`, `gs_tokens`, `gs_flags:N`, `sword_health`, `b_button`, `equipment`, ...; see `SaveField` in `AutoTest.inl`). `add_field` adds to the current value, like a delivery or pickup would |
| `set_ammo`, `set_slot` | `slot`, `n` / `item`; direct inventory writes |
| `set_health`, `set_rupees`, `set_time` | `health` / `n` / `day_time`, `night_flag` |
| `disconnect` | drops the connection (pair with `wait_until` cond `disconnected`, then `connect` again) |
| `to_title` | leaves the save and goes back to the title screen, like the `reset` console command |
| `wait_title` | waits until the world is gone |
| `boot` | loads a fresh emptied debug save again from the title screen (optional `entrance`, `age`, `reset`) |
| `log`, `quit` | `msg` / `pass`, `error` |

Every step takes `timeout_frames`; a timeout fails the scenario. Scenarios: `smoke`, `items` (shared non-consumables, per-player ammo/rupees/HP/bottle contents, Share Ammo on, Story Co-op switched off mid session), `items_off` (Story Co-op never on), `flags` (shared story flags, per-player gift flags, Weird Egg, Zelda's Letter gated on Lullaby). Cross-client sync inside a scenario uses a shared infTable flag as a signal.  Optional top-level `cvars` and per-role `cvars` set
arbitrary CVars before the client starts.

Assertion types: `each` (every client, or only `roles`, `obs`/`path`/`op`/`value`, ops `eq ne ge gt le lt truthy`), `count` (number of
clients where `path == value` equals `equals`), `same` (all clients agree on `path`), `cross_equal`, `rel` (compare `a` and `b`, each `role`/`obs`/`path`, with `delta` meaning b - a, or an `op`). Paths are dotted
and can index lists (`peers.0.age`).

An observation records connection, network id, nickname, party id, admin flags, party size, the party settings as the
client sees them (`story_coop`, `share_ammo`, `story_code`), `story_active`, pedestal hold and vote counts, the
player list (`peers`: id, name, in_party, scene, room, age), the number of remote player actors, and in-world state
(scene, room, `link_age` 0 adult / 1 child, health, rupees, position, requested flags).

## In-game flags

```
--coop-test <scenario.json> --coop-test-role <A|B> --coop-test-out <log.json>
--coop-test-host <ip> --coop-test-port <port>
--coop-test-rom <rom.z64> --coop-test-extract-only
```

The runner is implemented in `soh/soh/Enhancements/AutoTest/` (included from `ZeldaOnlineClient.cpp`, so adding it
needed no CMake reconfigure).

## Stages (server restarts, corrupt saves)

A scenario may have `stages`, each with its own `roles`, `assertions` and server handling, run one after another with
fresh client saves. `server` is `fresh` (new empty data dir), `keep` (same running server) or `restart` (the server
is stopped cleanly with Ctrl+Break, `pre` actions run on the saved files, then it starts again on the same data dir).
`pre` actions: `corrupt_bin`, `corrupt_all` (also the backups), `truncate_bin`. `stop_server` stops the server
cleanly after the stage. `server_checks` are `log_contains` / `log_absent` (`text`, `min`) on that stage's server
log, `files` (`glob` under the server folder, `min`), `alive` and `clean_exit`.

`catchup_persist` covers: joining a party that already has progress (own consumables kept), reconnect, going to the
title screen while the team progresses, restoring the team save after a server restart, falling back to backups
when the save file is corrupt, starting empty and staying alive when every file is corrupt, and no save sharing
between unrelated parties whose admins have the same nickname.

## Dungeon scenario

`dungeon` covers permanent dungeon progress: switches and room clears set in the Deku Tree, Forest Temple and Water
Temple are kept when everyone leaves and the server resets the dungeon instance (stage option `server_config`
`{"dungeonsResetTimer": 0}` makes every first entry reset it), a switch that is toggled back off stays off for the
player and for the others, the Forest Temple elevator (0x1B) and Water Temple water level (0x1C) switches are restored too
(as last writer wins values), and everything is restored after a server restart from the team save. A stage may set `server_config` to override keys
of the server `config.json` for a fresh server. The Water Temple is only visited briefly because with the test warp Link
is carried out of the temple about 30 frames after spawning (also offline, so unrelated to the netcode).

## Sync gaps scenario

`sync_gaps` covers the state that was not shared before the audit in `STORY_COOP_SYNC_AUDIT.md`: map areas and map rooms and
floors, the high scores and records (including the Big Poe sum with a simultaneous delivery from both clients), the fishing
record with the per player hat bit, Epona's parking spot (the last writer wins), the scarecrow songs, the Water Temple
level walked through four changes and the Forest Temple elevator toggled twice, and the per player parts that must stay
apart (the Running Man score, the pond owner's hat, `eventInf`, which item is on the B button). It also checks that
Gold Skulltula tokens, the Biggoron's Sword flag, owned sword and shield bits, magic and double defense reach the other
client and come back after a restart. The second stage restarts the server and checks that a
fresh Water Temple and Forest Temple instance start with the party's level and elevator position.

## Offline unit test

`unit/story_progress_test.cpp` checks the team save wire format (a version 1 blob still loads, truncated input is rejected),
the last writer wins rule, the high score merges and the Water and Forest Temple switch handling without a game or a
network. The build line is in the file header.
