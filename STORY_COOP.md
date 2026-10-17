# Story Co-op

Story Co-op is an opt-in party setting for ShipwrightOnline. It turns a party into a shared playthrough of Ocarina of Time: the party shares progression, story flags and time travel, while each player keeps their own consumables. With the setting off (the default), the game and server behave exactly as before.

## Enabling it

Open the party window, then tick **Story Co-op** in the party settings (party admin only). A second option, **Share Ammo, Rupees & HP**, is available once Story Co-op is on and is off by default.

## What is shared

- **Items:** equipment, upgrades, quest items, songs, medallions, stones, heart containers and pieces, magic, dungeon items, small keys, boss keys, bottles themselves and Ruto's Letter. When one member gets one, every member has it.
- **Stay per player (unless "Share Ammo, Rupees & HP" is on):** ammo counts, rupees, health refills and bottle contents.
- **Story flags:** event check flags, item get flags, info tables, shared event inf bits, scene flags (switch, chest, clear, collect) and gold skulltula tokens.
- **Team save:** the server keeps the party's merged progress. A member who joins or reconnects is caught up on arrival, and the merge keeps their own consumables. The team save is stored with the server's other persistent data.

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

## Code map

- Client: `soh/soh/Network/ZeldaOnline/ZeldaOnlineStoryCoop.cpp`, plus additions in `ZeldaOnlineClient.*`, `PartySettings.hpp` and `ZeldaOnlineRoomWindow.hpp`.
- Server: `server/src` (party team save, vote and era state machine, story packets).
- Vanilla hooks: `VB_MASTER_SWORD_PEDESTAL_USE` (`z_bg_toki_swd.c`) and `VB_SWITCH_AGE` (`SwitchAge.cpp`).

## Credits

- ShipwrightOnline by lukegrahamSydney, the netcode and server this builds on.
- Ship of Harkinian (HarbourMasters), the PC port underneath.
- The Anchor based ocarina-coop project, whose ideas for shared items, flag sync and team save catch-up are ported here.
