
🇩🇪 [Deutsche Version / German version](README_DE.md)

---.

## Ship of Harkinian — This Fork: Fully Synchronized Co‑op / MMO Experience

This is a fork of Ship of Harkinian with the goal of implementing a host-authority server/client architecture that fully synchronizes enemy, world, and boss state so the game feels like a modern MMO-style co-op experience.

- Original project: https://github.com/HarbourMasters/Shipwright
- This fork builds on that base and adds extensive network synchronization systems for deterministic multiplayer gameplay.
- Core focus: host-authority room logic, actor state syncing, boss transitions, item progression, and shared world events across connected clients.

---

<a href="features.md">
  <img src="https://img.shields.io/badge/All%20Features-features.md-orange?style=for-the-badge&logo=readme" alt="All Features">
</a>

### 📥 Downloads

<a href="https://github.com/fReEsPiRiT-Spiritsoft/Shipwright-Online/releases">
  <img src="https://img.shields.io/badge/DOWNLOADS-darkgray?style=for-the-badge" alt="Downloads">
</a>

This project aims to turn SoH into a server/client multiplayer version with fully synchronized co-op progression and shared gameplay state.

# Features Added (Host-Authority Co-op Prototype)

This fork implements a host-authority multiplayer architecture designed for enemy and world synchronization during co-op and randomizer play.

### Core Synchronization & Combat Mechanics
* **Host-Authority Enemy Sync:** Forces the freeze flag on all client-side actors (`actor->flags |= ACTOR_FLAG_27` / `Actor_SetFreezeFlags`) when players are in the same room. This prevents physics desyncs and rubberbanding by streaming `world.pos`, `world.rot.y`, and `skelAnime` states directly from the host.
* **Client-to-Host Damage Routing:** Hooks into the collision engine (`acHit` / `ColliderCylinder`) to intercept client-side weapon hits. The local damage calculation is intercepted on the client and forwarded via network packets to the host. The host evaluates the hit using native engine routines and triggers the synced death sequence.
* **Dynamic Aggro Spoofing:** Evaluates distances (`Math3D_Vec3fDistSq`) on the host between enemies and both players. If the client is closer, the enemy AI targeting pointer is swapped to the client dummy, forcing enemies to track and attack the client.
* **Configurable Enemy Sync Radius:** The host can define a world-unit radius within which enemy positions and animations are broadcast. Outside this radius, the client runs its own local AI without network traffic. Radius and tick rate (5 / 10 / 20 Hz) are configurable live from the admin panel.
* **Configurable Enemy Sync Tick Rate:** Enemy position and animation data can be throttled to 5 Hz (every 4th frame), 10 Hz (every 2nd frame), or 20 Hz (every frame) to trade smoothness against bandwidth.

### Dynamic World & Progression Logic
* **Dynamic Room Switching:** Pauses network actor syncing when players separate into different scenes or rooms. Client-side freeze flags are lifted immediately so each client can continue local gameplay without memory or nullpointer crashes.
* **State Merging & Re-Entry Catch-Up:** When players rejoin in the same area, the engine tracks who entered first. If the client cleared out enemies while exploring alone, a death list packet triggers `Actor_Kill` on the corresponding IDs on the host before host-authority sync resumes.
* **Out-of-Radius Kill Persistence:** Enemy kills made by a non-authority player outside the sync radius (or while the authority was in a different room) are queued locally and forwarded the moment the authority enters the same room, so no cleared room can ever reset on the other player's screen.
* **Physical Item Exchange Mode:** When enabled from the admin panel, received items and scene flags are buffered instead of applied instantly. Items are only handed to the receiving player once both players are within roughly one meter of each other. The handover triggers the native hold-item-above-head animation and a localized in-game textbox: *"You received [Item] from [Name]!"*

### Environment & Global State
* **Cross-Zone Day/Time Synchronization:** Synchronizes the global time state (`gSaveContext.dayTime`) across the network, enforcing the host as the primary timekeeper.
* **Global Time-Lock Feature:** Checks `sceneNum` for both players. If either the host or the client enters an area where time naturally stops (for example Kakariko Village, Market, or dungeons), the time counter freezes globally for both players, even if the other player is currently in Hyrule Field.

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
* [Controller Mapping](docs/GAME_CONTROLLER_DB.md)
* [Modding](docs/MODDING.md)
* [Versioning](docs/VERSIONING.md)

<a href="https://github.com/Kenix3/libultraship/">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/poweredbylus.darkmode.png">
    <img alt="Powered by libultraship" src="./docs/poweredbylus.lightmode.png">
  </picture>
</a>
#