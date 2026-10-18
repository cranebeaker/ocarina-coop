# Story Co-op sync audit

Story Co-op has one rule: two players in a party should have one shared single player playthrough. This file lists every
persistent part of the game's `SaveContext` (`soh/include/z64save.h`) and says whether it is shared, kept per player on
purpose, or is run state that the game never saves. The "Gaps closed" section at the end lists what was not shared before
this audit and is now.

Code references: client `soh/soh/Network/ZeldaOnline/ZeldaOnlineStoryCoop.cpp` (`StoryCapture`, `StoryApply`), the wire
format and merge rules in `StoryProgress.hpp` (client and server copy), server `server/src` (`Party.*`, `OOTServer.cpp`,
`Scene.hpp`).

Legend

- **Shared**: a snapshot field or a live relay. A member's gain reaches everyone and is kept in the team save.
- **Per player**: kept per player on purpose, with the reason.
- **Run state**: never written to the save file by the game (it is reset when a save loads), so there is nothing lasting to share.
- **Server clock**: handled by the server's shared world clock.

## 1. Header fields

| Field | Status | Notes |
| --- | --- | --- |
| `entranceIndex`, `savedSceneNum`, `respawn*`, `fw`, `ship.backupFW` | Per player | Where a player is and where Farore's Wind / voids send them. |
| `linkAge` | Shared by vote | Era is kept together by the Master Sword pedestal vote and the server's era state. |
| `cutsceneIndex`, `sceneSetupIndex` | Per player | Position in a cutscene or scene layer of that player. |
| `dayTime`, `nightFlag` | Server clock | The server broadcasts world time (`SERVER_PACKET_WORLD_TIME`). |
| `totalDays` | Derived | Counts dawns on each client from the shared clock. |
| `bgsDayCount` | Per player | Days since that player ordered the Biggoron's Sword (`Environment_ClearBgsDayCount` in `z_en_go.c` and `z_en_go2.c`), part of the adult trade chain (below). |
| `deaths` | Per player | A statistic of the save file. |
| `playerName`, `newf`, `n64ddFlag`, `checksum`, `fileNum` | Per player | File identity. |
| `healthCapacity` + heart piece nibble | Shared | Shared as quarter hearts (`heartQuarters`, maximum merge), so two heart pieces from two different sources add up on whoever picks them up. |
| `health` | Per player | Refills and damage are personal. Shared with the party option "Share Ammo, Rupees & HP". |
| `magicLevel`, `magic` | Per player | Current magic is personal. |
| `isMagicAcquired`, `isDoubleMagicAcquired` | Shared | Great Fairy upgrades (the spin attack uses the same meter, there is no separate flag). |
| `isDoubleDefenseAcquired`, `inventory.defenseHearts` | Shared | Great Fairy of Ganon's Castle. |
| `rupees` | Per player | Shared with the party option. The wallet size is an upgrade and is shared. |
| `swordHealth` | Shared, highest | Giant's Knife durability. The knife counts as intact for everyone once any member holds an intact one; a received knife starts intact. |
| `bgsFlag` | Shared | Marks the Biggoron's Sword as the unbreakable one. |
| `naviTimer` | Per player | Navi's hint timer. |
| `ocarinaGameRoundNum` | Per player | Round counter of the Lost Woods Skull Kid memory game while that player is playing it. The prize flag (`itemGetInf 0x17`) is shared. |
| `childEquips`, `adultEquips`, `equips` (button items, equipped sword/shield/tunic/boots) | Per player | Which owned item is on which button is personal. Ownership is shared (below). This is the same as single player: owning the Kokiri Sword or a shield does not equip it, the player does that in the pause menu. |

## 2. Inventory

| Field | Status | Notes |
| --- | --- | --- |
| `inventory.items[]` (slot items: bow, bombs, hookshot, boomerang, lens, ocarina, hammer, ...) | Shared | Highest item id of a slot wins (upgrade chains ascend: Fairy to Ocarina of Time, Hookshot to Longshot, Fairy Bow). |
| Bottles (`SLOT_BOTTLE_1..4`) | Shared count, contents per player | Every member has as many bottles as the party found. What is inside (milk, fairies, Big Poes) stays personal. Ruto's Letter is shared and turns into an empty bottle once King Zora has moved. |
| Child trade slot | Shared up to Zelda's Letter | Weird Egg, Chicken, Zelda's Letter are story gates. The mask chain after the letter stays per player (see section 5). |
| Adult trade slot | Per player | Pocket Egg to Claim Check is a chain where each step hands over the previous item, so the item and its itemGetInf flags are personal. What the chain unlocks is shared: Biggoron's Sword (`bgsFlag`, sword bit) and the flags the NPCs set in eventChkInf. |
| Magic beans slot | Per player | Beans are bought and counted. Planted beans are switch flags of the scene and are shared. |
| `inventory.ammo[]` | Per player | Unless "Share Ammo, Rupees & HP" is on. Unlocking a slot (first sticks, nuts, bombchus, slingshot) is shared. |
| `inventory.equipment` (owned swords, shields, tunics, boots) | Shared, losses per player | Excluding the Master Sword bit, which belongs to the time travel flow, and the broken Giant's Knife bit, which is per player. Gear a player loses (a shield or tunic eaten by a Like Like, a burned Deku Shield) is lost for that player only: the client remembers the lost bits in its save file and the team union does not hand them back until the player buys or finds the item again. A loss is never sent as a removal, so the teammates keep theirs. Owning the Biggoron's Sword (with `bgsFlag`) removes the broken knife bit. |
| `inventory.upgrades` (quiver, bomb bag, strength, scale, wallet, bullet bag, sticks, nuts) | Shared | Highest level wins. |
| `inventory.questItems` | Shared | Medallions, stones, songs, Stone of Agony, Gerudo Card, Skull Token flag. Excluding the heart piece nibble (shared as `heartQuarters`). |
| `inventory.dungeonItems` (map, compass, boss key) | Shared | |
| `inventory.dungeonKeys` | Shared live | Small keys use their own live relay (`UPDATE_DUNGEON_KEYS`) so a key spent by one member is gone for all. |
| `inventory.gsTokens`, `gsFlags[6]` | Shared | Flags are or-ed, the count is the number of flags. Spider House rewards (itemGetInf and eventChkInf bits) are shared, so the wallet, Stone of Agony and heart piece come once per party. The Bombchu reward at 40 tokens is ammo and goes to whoever talks to the man. |

## 3. Scene flags (`sceneFlags[124]`)

| Part | Status | Notes |
| --- | --- | --- |
| `chest`, `collect` | Shared | Opened chests and picked up collectibles in every scene, overworld included (Kakariko, Hyrule Field, Zora's Domain, ...). The pickup itself (rupees, hearts) is personal because rupees and health are. |
| `swch`, `clear` of overworld scenes | Shared | Permanent switches and room clears (gates, boulders, bean spots, Windmill, ...). |
| `swch`, `clear` of dungeons and boss rooms | Shared | Live through the server's dungeon instance, and kept in the team save for the next visit (see STORY_COOP.md). |
| Water Temple water level (`swch` 0x1C-0x1E), Forest Temple elevator (`swch` 0x1B) | Shared as last writer wins slots | See the verdict in section 9. |
| `rooms`, `floors` (map rooms and floors visited) | Shared | Added in this audit. Map and compass are shared, so what the map shows is shared too. |
| `unk` | Per player | Used by the Bottle Adventure restoration to remember a bottle item per scene. |
| Temporary flags (`tempSwch`, `tempClear`, `tempCollect`) | Run state | Never saved by the game. |

## 4. `eventChkInf` (224 bits)

Shared as a whole, except these bits:

| Bits | Reason |
| --- | --- |
| 0x45, 0x4F, 0xC5, 0xC9 | Pulled the Master Sword, entered the chamber, Sheik at the pedestal, time travelled to adult. They belong to the time travel flow and the pedestal vote. |
| 0x8C-0x8F | Mask fees paid back to the Happy Mask Shop (mask loan chain, per player items). |
| 0x12 (randomizer only) | Weird Egg from Malon; the item is shuffled in the randomizer. |
| 0x40 until Zelda's Lullaby is owned | The game takes the letter back from a save with the flag but no Lullaby. |

Story gates (Lon Lon Ranch and Epona, the Kokiri, Saria, the owl, the Kakariko gate, Zora's Domain, boss and blue warp flags, trials, songs learned, Skulltula rewards) are in this table and are shared. For Lon Lon Ranch: Epona obtained (0x18), rented a horse from Ingo (0x1B), won the cow in Malon's race (0x1E), Talon woken and returned (0x13, 0x14, 0x6A, 0x6B), the Malon talks (0x10, 0x15, 0x16), the Epona's Song flags. The race itself is run state (section 6).

## 5. `itemGetInf` (64 bits) and `infTable` (480 bits)

Both are shared except bits that mean "an NPC already gave this player an item that stays per player" (so every member can still be given it) and the swordless row.

`itemGetInf` bits that stay per player (`STORY_ITEMGETINF_EXCLUDE`):

| Bits | Gift |
| --- | --- |
| 0x02 | Talon's Lon Lon Milk (bottle contents) |
| 0x03-0x0A | Bombchu Shop purchases (ammo) |
| 0x13 | First seed pickup (ammo) |
| 0x23-0x26, 0x2A, 0x3F | Happy Mask Shop loans and the Mask of Truth loan |
| 0x2C, 0x2E | Anju's Pocket Cucco and Cojiro (adult trade chain) |
| 0x30, 0x31 | Odd Potion and Poacher's Saw (adult trade chain) |
| 0x38-0x3B | Masks sold back to their owners (child trade chain) |

The other itemGetInf bits flag one-time rewards whose items are shared: heart pieces (0x0B Deku scrub, 0x10 lab dive, 0x12 Bombchu bowling, 0x15 Kakariko rooftop man, 0x1B treasure chest game, 0x1C Dampe's grave, 0x16 and 0x17 Lost Woods Skull Kid), upgrades (0x0D and 0x0E shooting gallery, 0x0F Gerudo archery, 0x11 Bombchu bowling bomb bag, 0x1D Lost Woods target, 0x1E and 0x1F Deku scrub stick and nut upgrades) and the empty bottle from Anju (0x0C). Each of those items is itself shared, so the flag and the item agree for every member.

`infTable` bits that stay per player: 0x77 (gate guard wears the Keaton Mask), 0xB4 (Broken Goron's Sword hand-off), 0x139 (Eyeball Frog hand-off), 0x17C (Bunny Hood sale dialogue), 0x198 (first magic jar pickup), and the whole swordless row 0x1D0 (it records whether that player took their sword off).

## 6. `eventInf` (4 words, 64 bits): all run state, none shared

`eventInf` is not written to the save file (`SaveManager.cpp` has no entry for it) and the game clears it when a save loads or on a game over (`z_game_over.c`, `z_message_PAL.c`, `z_file_choose.c`). Every bit describes the minigame or race the player is in at this moment. Forcing one on another player would start, skip or end an event for someone who is not playing it, so no bit is shared. What an event leaves behind for good is written to `eventChkInf`, `itemGetInf`, `infTable` or `horseData` and travels there.

| Word / bits | Meaning | Writers | Lasting result (shared) |
| --- | --- | --- | --- |
| `[0] & 0xF` | Horse race state machine ("InRaceSeq"): 1 renting Ingo's horse, 2 racing, 3-7 result states | `z_en_in.c`, `z_en_horse_game_check.c`, `z_en_horse.c`, `z_horse.c`, `z_bg_ingate.c` | Epona obtained (eventChkInf 0x18), rented horse (0x1B) |
| `[0] & 0x10` | Type of horse used in the race (Epona or Ingo's) | `z_en_in.c` | |
| `[0] & 0x20, 0x40` | Race variants (Ingo's challenge for Epona) | `z_en_in.c`, `z_en_horse.c` | |
| `[0] & 0x100` | Gerudo horseback archery in progress; Talon's Cucco game won | `z_en_ge1.c`, `z_en_ta.c` | itemGetInf 0x0F, HBA best score |
| `[0] & 0x400` | Cucco game finished | `z_en_ta.c`, `z_en_ma3.c` | |
| `[0] & 0x8000` | Set together with the race state | `z_en_in.c`, `z_en_horse_game_check.c` | |
| `[1] & 1` | A timed minigame is running (Running Man race); many NPCs clear it to cancel | `z_en_mm2.c`, `z_parameter.c`, `z_en_hs.c`, `z_en_kz.c`, `z_en_mk.c` | Running Man time is scratch state too (section 7) |
| `[2] & 0x1F` | Haggling dialogue progress of the Market man | `z_en_mu.c` | |
| `[3] & 1` | Richard is near his owner (picks one of two text boxes) | `z_en_dog.c`, `z_en_hy.c` | |

`STORY_EVENTINF_SHARE_MASK` stays empty. The Lon Lon Ranch and Epona gap is closed through `horseData` (section 7).

## 7. Remaining save fields

| Field | Status | Notes |
| --- | --- | --- |
| `horseData` (scene and position where Epona waits) | Shared, last writer wins | Added in this audit. Whoever parks Epona last decides where she is for the party. Whether Epona exists at all is eventChkInf 0x18 plus Epona's Song, both shared. |
| `scarecrowSpawnSong(Set)`, `scarecrowLongSong(Set)` | Shared, last writer wins | Added in this audit. The live copies that the ocarina and the scarecrow actors read are updated as well. |
| `worldMapAreaData` (map areas visited) | Shared | Added in this audit (or-ed). |
| `highScores[0]` horseback archery best | Shared, highest | Added in this audit. |
| `highScores[1]` Big Poe points | Shared as a sum | Added in this audit. Deliveries are summed on the server (`CLIENT_PACKET_STORY_ADD_COUNTER`), so two members delivering Big Poes reach the 1000 point reward together. The reward items are shared. Clients take the maximum, so a delivery is never counted twice. |
| `highScores[2]` fishing pond record word | Shared by field | Added in this audit. Longest catch per age and the play counter keep the larger value, "talked to the owner", "won the prize" and "used the sinking lure" are or-ed. The owner losing his hat follows the age of whoever fishes and stays personal. |
| `highScores[3]` Lon Lon race best time, `[6]` Dampe race best time | Shared, lowest nonzero | Added in this audit. |
| `highScores[4]` Running Man | Per player | The man also uses it as scratch state during the race (`z_en_mm2.c` adds and subtracts 1 and resets it to 158 when a run starts). |
| `highScores[5]` | Unused | |
| `sceneFlags[].rooms`, `.floors` | Shared | Section 3. |
| `gsFlags` | Shared | Section 2. |
| `eventChkInf`, `itemGetInf`, `infTable` | Shared with the exclusions above | Sections 4 and 5. |

## 8. Run state that is not saved (no sync needed)

`minigameState`, `minigameScore`, `mapIndex`, `worldMapArea`, `sunsSongState`, `dogParams`, `dogIsLost` (the Market dog follows a player, resets daily), `timerState` and the timers, `nayrusLoveTimer`, button status and alpha fields, `magicState` and the magic targets, `respawnFlag`, `textTriggerFlags`, `seqId` and the other audio and transition fields, `eventInf`. Per player by nature: position, camera, current animation.

## 9. SoH additions (`ship`)

| Field | Status | Notes |
| --- | --- | --- |
| `ship.randomizerInf` | Shared | Randomizer item and check flags (or-ed). |
| `ship.quest.data.randomizer.triforcePiecesCollected`, `bombchuUpgradeLevel` | Shared (randomizer saves only) | Added in this audit, highest wins. |
| `ship.maskMemory` | Per player | The mask a player is wearing. |
| `ship.pendingSale`, `pendingSaleMod`, `pendingIceTrapCount` | Per player | Randomizer shop and ice trap queues of that player. |
| `ship.stats` (timers, counts, timestamps, entrances and scenes discovered) | Per player | Statistics and tracker views of that save. |
| `ship.filenameLanguage` | Per player | |

## 10. Forest Temple elevator and Water Temple water level

Verdict: vanilla single player persists both, so Story Co-op now shares both. The old exclusion ("the actor always starts in its default place when the scene loads") did not match the game.

Evidence from the decomp sources in this repository:

- Water Temple: `z_bg_mizu_water.c`, `BgMizuWater_Init` (type 0) calls `BgMizuWater_GetWaterLevelActionIndex`, which reads switches `WATER_TEMPLE_WATER_F1/F2/F3_FLAG` (0x1C, 0x1D, 0x1E), places the water plane at the matching height (`sWaterLevels`, F3 765, F2 445, F1 -15) and then rewrites exactly one of the three switches. `BgMizuWater_WaitForAction` changes the level when another of the three switches turns on and clears the previous one. The platforms (`z_bg_mizu_movebg.c`, Init) read the same switches. The switches are ordinary permanent scene switches, so leaving the temple saves them (`Play_SaveSceneFlags`) and entering again restores the level. A new game starts with 0x1E set (`SaveManager.cpp`: `sceneFlags[5].swch = 0x40000000`, water at the top), and scene setup copies the saved switches into the scene (`z_actor.c`, `actorCtx->flags.swch = savedSceneFlags->swch`).
- Forest Temple: `z_bg_mori_elevator.c`, `BgMoriElevator_WaitAfterInit` puts the elevator at its lowered height when the actor's switch (`params & 0x3F`, 0x1B in the room data, as recorded by the original exclusion) is already on, and `BgMoriElevator_SetPosition` moves it when the switch changes. So the switch persists in the save and the elevator is rebuilt from it.

What was wrong with sharing them as plain bits: the water level is a one-of-three state and the elevator toggles. A union merge keeps old bits, so a stale level would stay set next to the new one and a lowered elevator could never go back up in the team save. They are now last writer wins slots (`STORY_LWW_WATER_LEVEL`, `STORY_LWW_FOREST_ELEVATOR`) with a stamp, and `StorySceneSwchExclude` keeps the raw bits out of the union merge.

How it behaves:

- Two players in the Water Temple at the same time share one server dungeon instance, so they always see the same level. A level change by one is a set and clear of switches on the instance, which the server relays to the other player live. Their water actors react the same way as in single player (`WaitForAction` notices the other switch and changes the level, with the one point cutscene).
- When a member changes the level (or the elevator), the client publishes the new value with the next stamp. The server keeps the highest stamp, so the last change wins on every member and in the saved team file.
- When the first member enters an empty dungeon instance on the server (a new instance after a server restart, an instance that was reset after everyone left, or one that another party used), `Scene::ApplyStoryState` sets the Water Temple level switch and the Forest Temple elevator switch from that member's team save before the scene loads, so the game rebuilds the temple exactly as a single player save would. Once someone is inside, the live instance switches are the truth and are not overwritten.
- A save that was played alone and already has a lowered temple seeds the slot with stamp 0 when it joins, so any change made in the party replaces it.

## 11. Gaps closed in this audit

Before: not shared although vanilla keeps them in the save and they are story progress.

| Gap | Now |
| --- | --- |
| Epona's parking spot (`horseData`) | Last writer wins slot |
| Scarecrow spawn song and long song | Last writer wins slots |
| Water Temple water level, Forest Temple elevator (persistent in vanilla, excluded by mistake) | Last writer wins slots, applied by the server to a new dungeon instance |
| Map areas visited (`worldMapAreaData`) | Or-ed |
| Map rooms and floors visited (`rooms`, `floors`) | Or-ed |
| Big Poe points | Sum on the server |
| Horseback archery best score | Highest |
| Fishing pond record, prize and visit bits | Merged by field |
| Lon Lon race and Dampe race records | Lowest nonzero |
| Randomizer triforce pieces and Bombchu bag level | Highest (randomizer saves) |

Kept per player on purpose (the reasons are in the sections above): ammo, rupees, health, bottle contents, button assignments and equipped items, the adult trade chain, the mask loan chain and its gift flags, Talon's milk, Bombchu shop purchases, the Running Man scratch score, the pond owner's hat, `eventInf` (all run state), per-save statistics and trackers.
