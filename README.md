# Ocarina of Time Story Co-op

**Play the whole of Ocarina of Time with a friend: one shared story, one team save, and you time travel together.**

[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/cranebeaker)

Story Co-op is a party mode for [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) (the PC port of
Ocarina of Time) built on the online netcode of [ShipwrightOnline](https://github.com/lukegrahamSydney/ShipwrightOnline)
by lukegrahamSydney. Two or more players go through the adventure together as one playthrough.

## How to play!

1. Download the latest zip from [Releases](https://github.com/cranebeaker/ocarina-coop/releases/latest) and extract it.
   You need your own supported Ocarina of Time ROM (no game files are included; on the first start the game builds
   its assets from your ROM, see Quick Start below).
2. **Host:** double-click `Host over Steam.bat`. It starts the server, opens a friends-only Steam lobby and starts
   your game.
3. **Join:** your friend double-clicks `Join over Steam.bat`; it finds you on their friends list by itself (with it
   open, right-click you in Steam and Join Game works too). Then the host creates a party in the online window with **Story Co-op** turned on and invites
   them.

No IP addresses or port forwarding: the connection goes through Steam's relay network (Valve's free "Spacewar" app).
Both players need Steam running. You can also run `OOTServer.exe` yourself and connect by address as in
ShipwrightOnline.

## What is shared

- **Items and progress:** weapons, items, songs, medallions, stones, heart containers and pieces, magic, upgrades,
  story flags, Gold Skulltulas, Big Poe points, scarecrow songs, map progress. One player finds the Hookshot, everyone
  has it.
- **Kept per player:** rupees, ammo and health (a party setting shares them too), bottle contents, one-time NPC gifts.
- **Dungeons:** keys, switches, chests, cleared rooms and pickups update live for everyone in the dungeon, and are
  saved for the team. The Water Temple water level and the Forest Temple elevator are shared like in single player.
- **Story cutscenes together:** when a story cutscene starts for one player, everyone in the same area watches it at
  the same moment. Area intros still play on your own first visit if you were elsewhere.
- **Time travel together:** pulling the Master Sword puts you on hold ("Waiting for the others") until every player
  has pulled it, then the whole party travels. The party is always in the same era.
- **Team save on the server:** survives server restarts, keeps backups, and recovers from a damaged file. Late
  joiners and reconnects catch up straight away. Every Story Co-op party has a team code shown in the party window.
- **Enemies and bosses** are synced by the ShipwrightOnline netcode: everyone fights the same enemy.

## How it works

- The client sends a compact progress snapshot; the server merges it into the party's team save and sends the result
  back. Merges are unions and maximums, so nobody loses progress.
- Story cutscenes are caught where the game starts them (a cutscene script, or a transition into a scene's cutscene
  layer) and started for the party members in the same scene.
- The Master Sword pedestal runs a party vote on the server before anyone changes era.
- `OOTSteamLink.exe` relays the game's TCP connection through Steam

## Part of Co-op Anything

This mod is part of [Co-op Anything](https://github.com/cranebeaker/coop-anything)

## Credits

- [ShipwrightOnline](https://github.com/lukegrahamSydney/ShipwrightOnline) by lukegrahamSydney: the online netcode,
  enemy and boss sync, parties and skins this mod is built on.
- [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) by HarbourMasters: the PC port.
- Steam relay: Steamworks.NET's `steam_api64.dll` (Valve's Steamworks runtime).

---

The rest of this README is the original ShipwrightOnline and Ship of Harkinian readme.

ShipwrightOnline: a multiplayer/online mod for SOH. Time, enemies, bosses, various actors, horses, bombs, arrows,
boomerangs etc are all synced. Original releases: https://github.com/lukegrahamSydney/ShipwrightOnline/releases/latest

https://youtu.be/BCDZvdghA6A?si=D5fk3nhATs81vnCC

<img width="1073" height="803" alt="image" src="https://github.com/user-attachments/assets/9d737741-d8ca-4d7c-bf03-0547e75d3ab9" />

<img width="1073" height="803" alt="image" src="https://github.com/user-attachments/assets/4a272801-bbf2-4615-ba7e-dc672440cf54" />

<img width="1073" height="803" alt="image" src="https://github.com/user-attachments/assets/6374876b-b7ac-4388-a685-292d00048b18" />

<img width="1073" height="803" alt="image" src="https://github.com/user-attachments/assets/15cba0b7-f31e-4e53-82e4-7b28f0e74c15" />

Thank you to pengy_vt and bladeedge02 for testing

![Ship of Harkinian](docs/shiptitle.darkmode.png#gh-dark-mode-only)
![Ship of Harkinian](docs/shiptitle.lightmode.png#gh-light-mode-only)


## Website

Official Website: https://www.shipofharkinian.com/

## Discord

Official Discord: https://discord.com/invite/shipofharkinian

If you're having any trouble after reading through this `README`, feel free to ask for help in the Support text channels. Please keep in mind that we do not condone piracy.

# Quick Start

The Ship does not include any copyrighted assets.  You are required to provide a supported copy of the game.

### 1. Verify your ROM dump
You can verify you have dumped a supported copy of the game by using the compatibility checker at https://ship.equipment/. If you'd prefer to manually validate your ROM dump, you can cross-reference its `sha1` hash with the hashes [here](docs/supportedHashes.json).

### 2. Download The Ship of Harkinian from [Releases](https://github.com/HarbourMasters/Shipwright/releases)

### 3. Launch the Game!
#### Windows
* Extract the zip
* Launch `soh.exe`

#### Linux
* Place your supported copy of the game in the same folder as the appimage.
* Execute `soh.appimage`.  You may have to `chmod +x` the appimage via terminal.

#### macOS
* Run `soh.app`. When prompted, select your supported copy of the game.
* You should see a notification saying `Processing OTR`, then, once the process is complete, you should get a notification saying `OTR Successfully Generated`, then the game should start.

#### Nintendo Switch
* Run one of the PC releases to generate an `oot.o2r` and/or `oot-mq.o2r` file. After launching the game on PC, you will be able to find these files in the same directory as `soh.exe` or `soh.appimage`. On macOS, these files can be found in `/Users/<username>/Library/Application Support/com.shipofharkinian.soh/`
* Copy the files to your sd card
```
sdcard
└── switch
    └── soh
        ├── oot-mq.o2r
        ├── oot.o2r
        ├── soh.nro
        └── soh.o2r
```
* Launch via Atmosphere's `Game+R` launcher method.

### 4. Play!

Congratulations, you are now sailing with the Ship of Harkinian! Have fun!

# Configuration

### Default keyboard configuration
| N64 | A | B | Z | Start | Analog stick | C buttons | D-Pad |
| - | - | - | - | - | - | - | - |
| Keyboard | X | C | Z | Space | WASD | Arrow keys | TFGH |

### Other shortcuts
| Keys | Action |
| - | - |
| ESC | Toggle menu |
| F2 | Toggle capture mouse input |
| F5 | Save state |
| F6 | Change state |
| F7 | Load state |
| F9 | Toggle Text-to-Speech (Windows and Mac only) |
| F11 | Fullscreen |
| Tab | Toggle Alternate assets |
| Ctrl+R | Reset |

# Project Overview
Ship of Harkinian (SOH) is built atop a custom library dubbed libultraship (LUS). Back in the N64 days, there was an SDK distributed to developers named libultra; LUS is designed to mimic the functionality of libultra on modern hardware. In addition, we are dependant on the source code provided by the OOT decompilation project.

In order for the game to function, you will require a **legally acquired** ROM for Ocarina of Time. Click [here](https://ship.equipment/) to check the compatibility of your specific rom. Any copyrighted assets are extracted from the ROM and reformatted as a .o2r archive file which the code uses.

### Graphics Backends
Currently, there are three rendering APIs supported: DirectX11 (Windows), OpenGL (all platforms), and Metal (MacOS). You can change which API to use in the `Settings` menu of the menubar, which requires a restart.  If you're having an issue with crashing, you can change the API in the `shipofharkinian.json` file by finding the line `gfxbackend:""` and changing the value to `sdl` for OpenGL. DirectX 11 is the default on Windows.

# Custom Assets

Custom assets are packed in `.otr` archive files. To use custom assets, place them in the `mods` folder.

If you're interested in creating and/or packing your own custom asset `.otr` files, check out the following tools:
* [**retro - OTR generator**](https://github.com/HarbourMasters64/retro)
* [**fast64 - Blender plugin**](https://github.com/HarbourMasters/fast64)

# Development
### Building

If you want to manually compile SoH, please consult the [building instructions](docs/BUILDING.md).

### Playtesting
If you want to playtest a continuous integration build, you can find them at the links below. Keep in mind that these are for playtesting only, and you will likely encounter bugs and possibly crashes. 

* [Windows](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-windows.zip)
* [macOS](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-mac.zip)
* [Linux](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-linux.zip)

### Further Reading
More detailed documentation can be found in the 'docs' directory, including the aforementioned [building instructions](docs/BUILDING.md).

* [Credits](docs/CREDITS.md)
* [Custom Music](docs/CUSTOM_MUSIC.md)
* [Formatting](docs/FORMATTING.md)
* [Controller Mapping](docs/GAME_CONTROLLER_DB.md)
* [Modding](docs/MODDING.md)
* [Versioning](docs/VERSIONING.md)

<a href="https://github.com/Kenix3/libultraship/">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/poweredbylus.darkmode.png">
    <img alt="Powered by libultraship" src="./docs/poweredbylus.lightmode.png">
  </picture>
</a>
