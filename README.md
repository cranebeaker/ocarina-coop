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

https://github.com/user-attachments/assets/fedb77e0-4b35-44a7-9120-dce0d7005fcd
<img width="1073" height="803" alt="image" src="https://github.com/user-attachments/assets/9d737741-d8ca-4d7c-bf03-0547e75d3ab9" />



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
