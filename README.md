# Ocarina of Time co-op: the whole game, synced

Play **The Legend of Zelda: Ocarina of Time** together on PC, start to finish, in the same
world. Enemies, bosses, switches, puzzles, cutscenes and day and night are shared, not just
items.

This is a fork of **Shipwright-Online** by fReEsPiRiT-Spiritsoft, which itself builds on
**Ship of Harkinian** (the PC port by HarbourMasters) and its **Anchor** multiplayer layer.
Shipwright-Online added host-authority sync (one player's game runs the enemies and bosses,
everyone else follows). This repo fixes the co-op bugs it still had and extends sync to the
rest of the game.

[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/cranebeaker)

## Play in three steps

You need your **own** Ocarina of Time N64 ROM (see "About the ROM" below). Nothing from the
game is included here.

1. Download the latest `OoT-Coop-<version>.zip` from [Releases](../../releases) and unzip it
   anywhere.
2. Double-click **Play Co-op.bat**. The first time, it asks for your name and a room code,
   then the game asks you to pick your ROM file and builds its game data from it (this takes
   a few minutes, once).
3. Everyone who types the **same room code** is in the same world. Start or load a file and
   play.

It connects through the public Anchor server (`anchor.hm64.org`), so there are no IP
addresses or port forwarding. No release is up yet; the first one comes after testing.

### About the ROM

- Use the **NTSC 1.0 (US)** version as a `.z64` file. All players should use the same
  version. Other supported versions are listed in
  [docs/supportedHashes.json](docs/supportedHashes.json).
- You can check your file at https://ship.equipment/.
- Dump it from a cartridge or disc you own. We do not condone piracy, and no ROM, game asset
  or Nintendo file is in this repo or its releases.

## Status

**Early beta.** Every fix here was made by reading the game and network code and is checked
to compile. **Not yet proven:** a real playthrough by two people on separate PCs. Treat
every dungeon after the Deku Tree as untested until someone reports back. Reports are
welcome in [Issues](../../issues).

Fixed so far (details and root causes in [known_bugs.md](known_bugs.md)):

- Eye, floor and crystal switches now update on every player's screen.
- Cutscenes started by an NPC or object no longer leave one player stuck.
- Day and night state is shared, so night enemies match.
- Water levels (Deku Tree, Lake Hylia, Water Temple) stay in step.

In progress: boss death sequences for every boss, King Dodongo's attacks, Deku Tree enemies,
Dodongos that get stuck fleeing, bombable walls and bomb flowers, and the gaps found in
sweeps of the later dungeons and Ganon's Castle.

## Known issues

- Scripted cutscenes (for example the first time you enter an area) can still leave one
  player waiting. Teleporting to the host from the network menu unsticks it.
- More open items are listed in [known_bugs.md](known_bugs.md).

## How it works

- Every player runs the full game. The Anchor server relays packets between players in the
  same room; it does not run any game logic.
- For each room, one player is the "roommaster". Their game runs enemy AI and bosses and
  streams positions, animations and boss state to the others. Hits by other players are sent
  to the roommaster, who applies the damage.
- Switch, chest and clear flags sync both ways, and one-off world events (bombed walls,
  water levels, boss deaths) are sent as room events that every player applies once.
- The co-op code lives in `soh/soh/Network/Anchor/`.

## Building

See [docs/BUILDING.md](docs/BUILDING.md) (Windows: Visual Studio 2022 with the v143 tools,
CMake, Python 3). If `CMAKE_TOOLCHAIN_FILE` is set in your environment for another platform,
unset it before configuring.

## Part of coop-anything

This is one entry in [coop-anything](https://github.com/cranebeaker/coop-anything), a
community playbook and directory for adding co-op to single-player classics.

## Credits

- **Ship of Harkinian** by HarbourMasters: https://github.com/HarbourMasters/Shipwright
- **Anchor** by garrettjoecox: https://github.com/garrettjoecox/anchor
- **Shipwright-Online** by fReEsPiRiT-Spiritsoft:
  https://github.com/fReEsPiRiT-Spiritsoft/Shipwright-Online (its original README is kept as
  [README_SHIPWRIGHT_ONLINE.md](README_SHIPWRIGHT_ONLINE.md))
- The OoT decompilation project (zeldaret).
- The Legend of Zelda: Ocarina of Time is the property of Nintendo. This project is not
  affiliated with or endorsed by Nintendo.

## License

Same as Ship of Harkinian, see [LICENSE](LICENSE) where present and the upstream repository.
Game assets are not included and are never distributed.
