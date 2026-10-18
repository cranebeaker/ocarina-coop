# Story Co-op

Story Co-op is an opt-in party setting for ShipwrightOnline. It turns a party into a shared playthrough of Ocarina of Time: the party shares progression, story flags and time travel, while each player keeps their own consumables. With the setting off (the default), the game and server behave exactly as before.

## Enabling it

Open the party window, then tick **Story Co-op** in the party settings (party admin only). A second option, **Share Ammo, Rupees & HP**, is available once Story Co-op is on and is off by default.

## What is shared

- **Items:** equipment, upgrades, quest items, songs, medallions, stones, heart containers and pieces, magic, dungeon items, small keys, boss keys, bottles themselves, Ruto's Letter and the child trade items up to Zelda's Letter (Weird Egg, Chicken, Zelda's Letter). When one member gets one, every member has it.
- **Stay per player (unless "Share Ammo, Rupees & HP" is on):** ammo counts, rupees, health refills and bottle contents.
- **Story flags:** event check flags, item get flags, info tables, shared event inf bits, scene flags (switch, chest, clear, collect) and gold skulltula tokens.
- **Dungeon progress:** the permanent switch and clear flags of dungeon and boss scenes (doors opened, rooms cleared) are part of the team save too, kept as they were when each member left the scene. They are added to the scene whenever a member loads it, including after the dungeon instance on the server has been reset or recreated, and chests stay open. Temporary switches and room flags are never saved by the game and are not shared. The Water Temple water level (switches 0x1C-0x1E) and the Forest Temple elevator (switch 0x1B) are saved by the game and rebuilt from those switches when the scene loads, so they are shared too, as last writer wins values instead of plain flags (see below). A member who is inside a dungeon when someone else's progress arrives sees it the next time they load that scene. A switch that is turned back off again (a crystal switch or other toggle) is dropped from the team save when the member who switched it off is in a party dungeon: the server removes it and tells the other members, so it does not come back on the next visit. A member who was offline or on the title screen at that moment can still bring the old state back when they reconnect, because the team save is merged by union.
- **Lon Lon Ranch and Epona:** Epona obtained, the rented horse, Talon, Malon and the race prizes are story flags and are shared. Where Epona waits (her parking spot) is shared too: whoever parks her last decides where she is for the party. The race itself (the Ingo and Malon events, the Cucco game, the shooting gallery, Richard's walk) is run state that the game never saves, so it is never forced on another player.
- **Scarecrow songs:** the spawn song and the long song are shared. The last one recorded is the one every member hears.
- **Records and counters:** Big Poe points add up across all members (both players' deliveries count toward the 1000 point reward), the horseback archery best score is the highest, the Lon Lon and Dampe race records are the lowest times, and the fishing pond record (longest catches, prize and visit bits) is merged. The Running Man's score and the pond owner's hat stay personal.
- **Map progress:** map areas visited, and the rooms and floors visited in every scene, are shared along with the map and compass.
- **Water Temple and Forest Temple:** the last water level any member set (or the elevator position) is what the next dungeon instance starts with, and two members inside at once always see the same level.
- **Stay personal:** which item is on which button and what is equipped, bottle contents, the adult trade chain, the mask loan chain, Talon's milk, Bombchu shop purchases, deaths and statistics.
- **Gift flags stay per player too:** flags that mean an NPC already gave you an item that is not shared (the adult trade chain, the mask loans, Talon's milk, Bombchu shop purchases) are not shared, so every member can still receive those items.
- **Zelda's Letter and the Weird Egg:** the Malon egg flag and the courtyard flag (Zelda's Letter) are story gates for Saria, the owl, Mido, the Kokiri, Talon and the Kakariko gate, so they are shared together with the child trade slot. A member who never saw the courtyard gets the flag, the letter and Zelda's Lullaby, so the NPCs act the same for everyone. The letter and its flag only travel once Zelda's Lullaby is owned, because the game takes the letter back from a save that has the flag but no Lullaby. A member further along the mask chain counts as having the letter and keeps their masks, and the trade items stay per player in the randomizer.
- **Loading a save:** a team snapshot is only applied to the save that was loaded when it was requested. Going to the title screen or loading another save drops anything pending, and the newly loaded save is uploaded again, which brings back what the team has that it lacks.
- **Team save:** the server keeps the party's merged progress (team save format 2; files written by earlier servers still load, but client and server must be updated together because an older server ignores format 2 snapshots). A member who joins or reconnects is caught up on arrival, and the merge keeps their own consumables. The team save is stored with the server's other persistent data.

Sharing is state based: clients send a compact progress snapshot, the server merges it (union / max) into the team save and relays the result. Echoes are harmless and nobody loses progress.

## Time travel together

- Pulling the Master Sword (child) or placing it back (adult) at the pedestal puts you on hold: `Waiting for the others (n/N): <names>. Press B to cancel.`
- B cancels and withdraws your vote. Leaving the scene also withdraws it.
- When every party member has voted, all of them travel together and each client runs the game's own sequence (the first pull plays the Chamber of Sages).
- Members who disconnect or leave the party are removed from the count, and nobody is left on hold.
- A member who joins while the party is in a different era is moved to the party's era.
- Other age changes (the SwitchAge cheat or hotkey, the Ocarina Time Travel enhancement) are blocked while Story Co-op is active, with a notification pointing to the pedestal.
- A remote Song of Time never changes another player's age. Remote ocarinas are audio only, and only the local player's own age changes locally.

The party list shows each member's era (Adult or Child).

## Server save

The team save is written to disk, so it survives a server restart. Clients still keep their own progress in their own saves and still re-seed the team save by union when they join, so nothing depends on the file being present.

- **Where:** `<dataDir>/story/<name>-<hash>.bin`. `dataDir` is set in `config.json` (default `data`, relative to where the server is started, empty disables saving).
- **Which file:** the key is the admin's nickname plus the party's team code (up to 32 characters, the `storyCode` setting). The code is always set while Story Co-op is on: the party window shows it under **Team code** and fills in a random one when you tick Story Co-op (you can type your own, or paste the code of an earlier playthrough to resume it). A client that sends none gets one from the server, so two unrelated parties whose admins share a nickname can never load each other's save. The key is chosen when Story Co-op is switched on and the matching file is loaded then, merged by union with anything the party already holds. Changing the code while Story Co-op stays on does nothing, switch it off and on again. To resume a playthrough after a server restart, create the party again with the same admin nickname and the same code (the admin's client remembers the last code). If another running party already uses the key, the second one is simply not persisted.
- **When:** a few seconds after the last change, when Story Co-op is switched off, when the party is dropped and on a clean shutdown (Ctrl+C or SIGTERM).
- **Safety:** each write goes to a temporary file, is flushed to disk and renamed over the old one. The file has a magic, a format version and a checksum. Up to three rotating backups (`.bak1` newest) are kept, rotated at most every 10 minutes.
- **Bad files:** a missing, truncated, corrupt or unknown version file is logged and skipped, falling back to the newest good backup, then to an empty team save. The server never stops because of it.

## Audit

`STORY_COOP_SYNC_AUDIT.md` lists every part of the save file and whether it is shared, personal or run state, with the reason and the code that shows it.

## Code map

- Client: `soh/soh/Network/ZeldaOnline/ZeldaOnlineStoryCoop.cpp`, plus additions in `ZeldaOnlineClient.*`, `PartySettings.hpp` and `ZeldaOnlineRoomWindow.hpp`.
- Server: `server/src` (party team save, vote and era state machine, story packets).
- Vanilla hooks: `VB_MASTER_SWORD_PEDESTAL_USE` (`z_bg_toki_swd.c`) and `VB_SWITCH_AGE` (`SwitchAge.cpp`).

## Credits

- ShipwrightOnline by lukegrahamSydney, the netcode and server this builds on.
- Ship of Harkinian (HarbourMasters), the PC port underneath.
- The Anchor based ocarina-coop project, whose ideas for shared items, flag sync and team save catch-up are ported here.
