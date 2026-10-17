# OOTSteamLink: play over Steam with no IP addresses or port forwarding

OOTSteamLink is a small helper program that sits next to the game. It carries the game's connection to
`OOTServer` over Steam (Valve's free "Spacewar" test app, AppID 480), so friends can join each other from the
Steam friends list. Nobody needs to know an IP address, open a port or own anything extra. Both players need Steam
running and logged in.

The game and the server are not changed. The game still talks TCP to `OOTServer` (default port 21050); the helper
relays that TCP stream through Steam. The only client change is an optional environment variable, `OOT_SERVER`
(`host:port`), which the helper sets so the game connects to the helper instead of its saved server. When it is not
set, the game behaves exactly as before.

## Using it

Put these files in one folder, next to `soh.exe` (the host also needs `OOTServer.exe`, with its `config.json`, in the
same folder or in a `server` subfolder):

- `OOTSteamLink.exe` (built by `build.bat`, or from a release)
- `steam_api64.dll` (see below)
- `Host over Steam.bat` and `Join over Steam.bat`

**Host:** double-click `Host over Steam.bat`. It starts `OOTServer.exe`, creates a friends-only Steam lobby, and
starts your game connected to your own server. Leave the window open while you play; press `Q` to stop hosting
(the server is stopped cleanly so team saves are written). Your friends then either:

- start `Join over Steam.bat` (it finds a friend who is hosting by itself, or lists them to pick from), or
- with `Join over Steam.bat` already open, right-click you in the Steam friends list and choose Join Game, or
  accept your invite.

**Join:** double-click `Join over Steam.bat`. It joins the lobby, opens a local relay port, and starts the game
pointed at it; the game connects by itself. Close the game (or press `Q`) to leave. If the host closes the game
or stops hosting, the window says so.

Running `OOTSteamLink.exe` with no arguments shows a small menu (1 = Host, 2 = Join).

Command line, for scripts:

```
OOTSteamLink.exe host [--port 21050] [--server-exe <path>] [--game-exe <path>] [--no-server] [--no-game]
OOTSteamLink.exe join [lobby id] [--local-port <n>] [--game-exe <path>] [--no-launch]
```

- `--no-server` relays to a server that is already running on `--port` instead of starting one.
- `--no-game` / `--no-launch` do not start `soh.exe`. Start the game yourself with the environment variable
  `OOT_SERVER=127.0.0.1:<port>` (the join window prints the port), or type that host and port into the online window.
- The server's console output goes to `OOTServer.log` in the server folder.

## What it does

- The host creates a friends-only lobby tagged `ootsteam=1` with the relay version, and sets the Steam rich presence
  `connect` string so Join Game works. Only people who joined that lobby are accepted by the host.
- Each TCP connection from a game becomes one stream with its own id. Streams are cut into chunks of at most
  16000 bytes and sent as reliable, ordered Steam messages (channel 0), then rebuilt in order on the other side, so
  large transfers and partial reads are handled. Open and close of a connection are carried too, so a closed game
  connection closes the server connection and the other way round, and the game can reconnect at any time.
- Channel 1 carries a keep-alive every second and a goodbye. A peer that is silent for 20 seconds is dropped and
  its connections are closed.

## steam_api64.dll

The helper loads Steam's runtime library from its own folder (no Steam SDK is needed to build). Use
`steam_api64.dll` from the Steamworks.NET "Standalone" release 2025.164.1
(`github.com/rlabrecque/Steamworks.NET/releases`, folder `Windows-x64`). It is a 64-bit DLL:

- SHA-256 `eb17909a76668cf9ae0b92a618a34a50f6c73d3a6787cb4dd8ce36a8b10bfb75`

`steam_appid.txt` (containing `480`) is created next to the exe if missing. If Steam is not running, or the DLL is
missing, the helper says so in plain words and exits.

## Build

`build.bat` builds `out\OOTSteamLink.exe` and the mock `out\mock_steam_api64.dll` with Visual Studio 2022
(64-bit, plain C, no dependencies).

## Tests (one PC, no Steam accounts needed)

`mock/mock_steam_api.c` is a stand-in for `steam_api64.dll` that passes the Steam messages between local processes
over UDP and fakes lobbies and a friends list. Put it next to a copy of `OOTSteamLink.exe` under the name
`steam_api64.dll`, one folder for the host (`H`) and one for the joiner (`J`), and run:

- `python test/stream_test.py --work <dir with H and J>`: byte-exact round trips (16 MB, odd sizes), parallel
  connections, close propagation in both directions, reconnects, restart of the join side, and the host vanishing.
- `python test/e2e_test.py --work <dir> --build-dir <soh.exe dir> --o2r <oot.o2r>`: starts the real `OOTServer.exe`
  through the host helper and runs the two-client `tools/coop-test` smoke scenario with both real game clients
  connected through the join helper.

Which environment variables the mock reads: `STEAMLINK_MOCK_NAME` (`Host` or `Client`),
`STEAMLINK_MOCK_PORT_BASE`, `STEAMLINK_MOCK_LOG`.
