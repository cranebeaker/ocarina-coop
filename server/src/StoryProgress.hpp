#ifndef STORYPROGRESS_HPP
#define STORYPROGRESS_HPP

// Compact snapshot of the party-wide ("team save") progress used by the Story Co-op party setting.
// This header is mirrored byte for byte between server/src and soh/soh/Network/ZeldaOnline.
// It must stay free of game headers.

#include <cstdint>
#include <cstring>
#include <vector>
#include "ByteStream.hpp"

namespace ZeldaOnline
{
	static const int STORY_SCENE_COUNT = 124;
	static const uint8_t STORY_PROGRESS_VERSION = 2; // 1 = original layout, still read
	static const uint8_t STORY_FLAG_CATCH_UP = (1 << 0); // server -> client: sent to a member who just joined
	static const uint8_t STORY_FLAG_SEED = (1 << 0);     // client -> server: snapshot uploaded to seed or re-seed the team save

	// Exclusion masks. Bits listed here are never shared (they belong to the time travel flow or are per player).
	static const uint16_t STORY_EQUIPMENT_EXCLUDE = (1 << 1);               // Master Sword inventory bit (EQUIP_INV_SWORD_MASTER)
	static const uint32_t STORY_QUEST_ITEMS_EXCLUDE = 0xF0000000u;          // heart piece counter nibble, shared as heartQuarters
	static const int STORY_INFTABLE_EXCLUDE_INDEX = 29;                     // swordless flag row
	static const uint32_t STORY_WATER_TEMPLE_SWCH_EXCLUDE = 0x70000000u;    // swch 0x1C-0x1E
	static const uint32_t STORY_FOREST_TEMPLE_SWCH_EXCLUDE = 0x08000000u;   // swch 0x1B
	static const int STORY_SCENE_FOREST_TEMPLE = 0x03;
	static const int STORY_SCENE_WATER_TEMPLE = 0x05;
	static const int STORY_WATER_LEVEL_FIRST_SWITCH = 0x1C; // level n (1..3) is switch 0x1B + n
	static const int STORY_FOREST_ELEVATOR_SWITCH = 0x1B;

	// Last writer wins slots: small blobs of state that is not a flag (a position, a recorded song, which of several
	// switches is the live one). The higher stamp wins, an equal stamp falls back to the larger bytes so every side
	// ends up with the same value. A stamp of 0 means "seeded from a save, never changed in this party".
	static const uint8_t STORY_LWW_HORSE = 1;            // s16 scene, s16 x, y, z, s16 angle (horseData, where Epona waits)
	static const uint8_t STORY_LWW_SCARECROW_SPAWN = 2; // u8 set flag, 16 ocarina notes of 8 bytes
	static const uint8_t STORY_LWW_SCARECROW_LONG = 3;  // u8 set flag, 108 ocarina notes of 8 bytes
	static const uint8_t STORY_LWW_WATER_LEVEL = 4;     // u8: 1, 2 or 3 = switch 0x1C, 0x1D or 0x1E is the live level
	static const uint8_t STORY_LWW_FOREST_ELEVATOR = 5; // u8: 1 when switch 0x1B is on
	static const int STORY_LWW_MAX_SLOTS = 16;
	static const unsigned int STORY_LWW_MAX_BYTES = 1024;

	// highScores indices (z64save.h) and how each one merges between members
	static const int STORY_HS_COUNT = 7;
	static const int STORY_HS_HBA = 0;        // horseback archery best score: highest
	static const int STORY_HS_POE_POINTS = 1; // Big Poe points: a sum of deliveries, see STORY_COUNTER_POE_POINTS
	static const int STORY_HS_FISHING = 2;    // bit field, see StoryMergeFishing
	static const int STORY_HS_HORSE_RACE = 3; // Lon Lon Ranch race best time: lowest, 0 means none
	static const int STORY_HS_MARATHON = 4;   // not shared: the Running Man also uses it as scratch state during the race
	static const int STORY_HS_UNK_05 = 5;     // unused by the game
	static const int STORY_HS_DAMPE_RACE = 6; // Dampe race best time: lowest, 0 means none
	static const int STORY_POE_POINTS_MAX = 1100;

	// client -> server counter additions (CLIENT_PACKET_STORY_ADD_COUNTER)
	static const uint8_t STORY_COUNTER_POE_POINTS = 1;

	// Switch flags of a scene that are not merged as plain bits. The game keeps them on purpose (the Water Temple water
	// level and the Forest Temple elevator are rebuilt from these switches whenever the scene loads, so a single player
	// save restores them), but they are one-of-N or toggling states, which a union merge would corrupt: an old level
	// would stay set next to the new one. They travel as last writer wins slots instead (STORY_LWW_WATER_LEVEL and
	// STORY_LWW_FOREST_ELEVATOR) and the server puts them back into a fresh dungeon instance.
	static inline uint32_t StorySceneSwchExclude(int sceneNum)
	{
		if (sceneNum == STORY_SCENE_WATER_TEMPLE)
			return STORY_WATER_TEMPLE_SWCH_EXCLUDE;
		if (sceneNum == STORY_SCENE_FOREST_TEMPLE)
			return STORY_FOREST_TEMPLE_SWCH_EXCLUDE;
		return 0;
	}

	// eventChkInf bit numbers set only by the Master Sword time travel flow
	// (PULLED_MASTER_SWORD_FROM_PEDESTAL, ENTERED_MASTER_SWORD_CHAMBER, SHEIK_SPAWNED_AT_MASTER_SWORD_PEDESTAL, TIME_TRAVELED_TO_ADULT)
	static const int STORY_EVENTCHKINF_EXCLUDE[] = { 0x45, 0x4F, 0xC5, 0xC9 };

	// eventChkInf bits meaning "an NPC already gave this player an item that stays per player" (mask loans).
	// Sharing them would stop the other members from ever receiving that item.
	//   (0x12 weird egg from Malon and 0x40 Zelda's Letter are story gates and are shared, together with the child trade slot)
	//   0x8C-0x8F mask fees paid back to the Happy Mask Shop (mask loan chain)
	static const int STORY_EVENTCHKINF_ITEM_EXCLUDE[] = { 0x8C, 0x8D, 0x8E, 0x8F };

	// itemGetInf bits meaning "already got / bought item X" for things that stay per player.
	//   0x02 Talon's Lon Lon Milk (bottle contents)
	//   0x03-0x0A Bombchu shop purchases (ammo)
	//   0x13 first seed pickup (ammo)
	//   0x23-0x26 Happy Mask Shop loans (Keaton, Skull, Spooky, Bunny Hood), 0x2A / 0x3F Mask of Truth loan
	//   0x2C / 0x2E Anju's Pocket Cucco and Cojiro (adult trade chain)
	//   0x30 Odd Potion, 0x31 Poacher's Saw (adult trade chain)
	//   0x38-0x3B masks sold back to their owners (child trade chain)
	static const int STORY_ITEMGETINF_EXCLUDE[] = { 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x13, 0x23, 0x24,
	                                                0x25, 0x26, 0x2A, 0x2C, 0x2E, 0x30, 0x31, 0x38, 0x39, 0x3A, 0x3B, 0x3F };

	// infTable bits tied to per player item hand-offs (the swordless row is excluded separately by index).
	//   0x77 gate guard wearing the Keaton Mask, 0x17C Bunny Hood sale dialogue, 0x139 Eyeball Frog hand-off,
	//   0xB4 Broken Goron's Sword hand-off, 0x198 first magic jar pickup
	static const int STORY_INFTABLE_ITEM_EXCLUDE[] = { 0x77, 0xB4, 0x139, 0x17C, 0x198 };

	// Clear every listed flag bit (flag = row * 16 + bit) from a row array
	static inline void StoryClearFlagBits(uint16_t* rows, int rowCount, const int* flags, int flagCount)
	{
		for (int i = 0; i < flagCount; i++)
		{
			if ((flags[i] >> 4) < rowCount)
				rows[flags[i] >> 4] &= (uint16_t)~(1u << (flags[i] & 15));
		}
	}

	// Fishing pond record word (highScores[HS_FISHING]). Record lengths and the play counter keep the larger value, the
	// "talked to the owner", "won the prize" and "used the sinking lure" bits are or-ed. The owner losing his hat
	// (0x1000) goes back and forth with the age of the player who is fishing, so it stays whatever the local save has.
	static inline uint32_t StoryMergeFishing(uint32_t mine, uint32_t other)
	{
		static const uint32_t kLengthChild = 0x0000007F;
		static const uint32_t kLengthAdult = 0x7F000000;
		static const uint32_t kPlayCount = 0x00FF0000;
		static const uint32_t kOrBits = 0x80000000u | 0x00000800u | 0x00000400u | 0x00000200u | 0x00000100u | 0x00000080u;
		static const uint32_t kKeep = 0x00001000u | 0x0000E000u;

		uint32_t out = mine & kKeep;
		out |= (mine & kLengthChild) > (other & kLengthChild) ? (mine & kLengthChild) : (other & kLengthChild);
		out |= (mine & kLengthAdult) > (other & kLengthAdult) ? (mine & kLengthAdult) : (other & kLengthAdult);
		out |= (mine & kPlayCount) > (other & kPlayCount) ? (mine & kPlayCount) : (other & kPlayCount);
		out |= (mine | other) & kOrBits;
		return out;
	}

	// Value of one highScores entry after combining two. Entries that are not listed here are never shared.
	static inline int32_t StoryMergeHighScore(int index, int32_t mine, int32_t other)
	{
		switch (index)
		{
		case STORY_HS_HBA:
			return other > mine ? other : mine;
		case STORY_HS_POE_POINTS:
		{
			int32_t best = other > mine ? other : mine;
			return best > STORY_POE_POINTS_MAX ? STORY_POE_POINTS_MAX : best;
		}
		case STORY_HS_FISHING:
			return (int32_t)StoryMergeFishing((uint32_t)mine, (uint32_t)other);
		case STORY_HS_HORSE_RACE:
		case STORY_HS_DAMPE_RACE:
			if (other <= 0)
				return mine;
			if (mine <= 0)
				return other;
			return other < mine ? other : mine;
		default:
			return mine;
		}
	}

	static inline bool StoryHighScoreShared(int index)
	{
		return index == STORY_HS_HBA || index == STORY_HS_POE_POINTS || index == STORY_HS_FISHING ||
		       index == STORY_HS_HORSE_RACE || index == STORY_HS_DAMPE_RACE;
	}

	// eventInf bits that are safe to share between party members. eventInf is the run state of the minigame or race a
	// player is in right now (Lon Lon Ranch race, horse state, cucco game, shooting gallery); the game never writes it
	// to the save file and clears it when a save loads. Forcing a bit on another player would start or end a race for
	// them, so no bit is shared. What those events leave behind permanently is in eventChkInf, itemGetInf, infTable
	// and horseData, which are shared. See STORY_COOP_SYNC_AUDIT.md for the reading of every bit.
	static const uint16_t STORY_EVENTINF_SHARE_MASK[4] = { 0, 0, 0, 0 };

	// upgrades are packed into a u32 as 3 bit fields, except the wallet which uses 2 bits
	static const uint8_t STORY_UPGRADE_SHIFTS[8] = { 0, 3, 6, 9, 12, 14, 17, 20 };
	static const uint32_t STORY_UPGRADE_MASKS[8] = {
		0x00000007, 0x00000038, 0x000001C0, 0x00000E00, 0x00003000, 0x0001C000, 0x000E0000, 0x00700000
	};

	struct StorySceneFlags
	{
		uint8_t sceneNum = 0;
		uint32_t chest = 0;
		uint32_t swch = 0;
		uint32_t clear = 0;
		uint32_t collect = 0;
		uint32_t rooms = 0;  // map rooms visited
		uint32_t floors = 0; // map floors visited
	};

	struct StoryLwwSlot
	{
		uint8_t id = 0;
		uint32_t stamp = 0;
		std::vector<uint8_t> data;
	};

	struct StoryProgress
	{
		uint8_t items[24];
		uint16_t equipment = 0;
		uint32_t upgrades = 0;
		uint32_t questItems = 0;
		uint8_t dungeonItems[20];
		int8_t dungeonKeys[19];

		uint8_t heartQuarters = 0;
		uint8_t magicLevel = 0;
		uint8_t isMagicAcquired = 0;
		uint8_t isDoubleMagicAcquired = 0;
		uint8_t isDoubleDefenseAcquired = 0;
		uint8_t defenseHearts = 0;
		uint8_t bgsFlag = 0;
		uint16_t swordHealth = 0;
		uint16_t gsTokens = 0;

		uint8_t bottleCount = 0;
		uint8_t hasRutoLetter = 0;

		uint16_t eventChkInf[14];
		uint16_t itemGetInf[4];
		uint16_t infTable[30];
		uint16_t eventInf[4];
		int32_t gsFlags[6];
		std::vector<uint16_t> randomizerInf;

		uint32_t worldMapAreaData = 0;      // map areas visited
		int32_t highScores[STORY_HS_COUNT]; // only the entries StoryHighScoreShared lists are used
		uint8_t randoTriforcePieces = 0;
		uint8_t randoBombchuLevel = 0;

		std::vector<StoryLwwSlot> lww;       // sorted by id
		std::vector<StorySceneFlags> scenes; // sorted by sceneNum, only entries with a non-zero bit

		StoryProgress()
		{
			std::memset(items, 0xFF, sizeof(items));
			std::memset(dungeonItems, 0, sizeof(dungeonItems));
			std::memset(dungeonKeys, 0, sizeof(dungeonKeys));
			std::memset(eventChkInf, 0, sizeof(eventChkInf));
			std::memset(itemGetInf, 0, sizeof(itemGetInf));
			std::memset(infTable, 0, sizeof(infTable));
			std::memset(eventInf, 0, sizeof(eventInf));
			std::memset(gsFlags, 0, sizeof(gsFlags));
			std::memset(highScores, 0, sizeof(highScores));
		}

		const StoryLwwSlot* FindLww(uint8_t id) const
		{
			for (const StoryLwwSlot& slot : lww)
			{
				if (slot.id == id)
					return &slot;
			}
			return nullptr;
		}

		// True when other's slot should replace this one: a higher stamp, or the same stamp with larger bytes
		static bool LwwWins(const StoryLwwSlot& other, const StoryLwwSlot& mine)
		{
			if (other.stamp != mine.stamp)
				return other.stamp > mine.stamp;
			return other.data > mine.data;
		}

		// Sets a slot unconditionally (a side adopting its own change)
		void SetLww(uint8_t id, uint32_t stamp, const std::vector<uint8_t>& data)
		{
			size_t pos = 0;
			while (pos < lww.size() && lww[pos].id < id)
				pos++;
			if (pos == lww.size() || lww[pos].id != id)
			{
				StoryLwwSlot fresh;
				fresh.id = id;
				lww.insert(lww.begin() + pos, fresh);
			}
			lww[pos].stamp = stamp;
			lww[pos].data = data;
		}

		bool MergeLww(const StoryLwwSlot& other)
		{
			if (other.data.empty() || other.data.size() > STORY_LWW_MAX_BYTES)
				return false;
			const StoryLwwSlot* mine = FindLww(other.id);
			if (mine != nullptr && !LwwWins(other, *mine))
				return false;
			SetLww(other.id, other.stamp, other.data);
			return true;
		}

		// Adds delta to a shared counter (Big Poe points). The server is the only place that adds, clients only
		// ever take the maximum, so the same delivery can never be counted twice.
		bool AddPoePoints(int delta)
		{
			if (delta <= 0)
				return false;
			int32_t next = highScores[STORY_HS_POE_POINTS] + delta;
			if (next > STORY_POE_POINTS_MAX)
				next = STORY_POE_POINTS_MAX;
			if (next == highScores[STORY_HS_POE_POINTS])
				return false;
			highScores[STORY_HS_POE_POINTS] = next;
			return true;
		}

		void Serialize(ByteStream& out) const
		{
			out.Write((const char*)&STORY_PROGRESS_VERSION, 1);
			out.Write((const char*)items, sizeof(items));
			out.Write(equipment);
			out.Write(upgrades);
			out.Write(questItems);
			out.Write((const char*)dungeonItems, sizeof(dungeonItems));
			out.Write((const char*)dungeonKeys, sizeof(dungeonKeys));
			out.Write(heartQuarters);
			out.Write(magicLevel);
			out.Write(isMagicAcquired);
			out.Write(isDoubleMagicAcquired);
			out.Write(isDoubleDefenseAcquired);
			out.Write(defenseHearts);
			out.Write(bgsFlag);
			out.Write(swordHealth);
			out.Write(gsTokens);
			out.Write(bottleCount);
			out.Write(hasRutoLetter);
			out.Write((const char*)eventChkInf, sizeof(eventChkInf));
			out.Write((const char*)itemGetInf, sizeof(itemGetInf));
			out.Write((const char*)infTable, sizeof(infTable));
			out.Write((const char*)eventInf, sizeof(eventInf));
			out.Write((const char*)gsFlags, sizeof(gsFlags));

			out.WriteVarUInt((unsigned int)randomizerInf.size());
			for (uint16_t word : randomizerInf)
				out.Write(word);

			out.WriteVarUInt((unsigned int)scenes.size());
			for (const StorySceneFlags& scene : scenes)
			{
				out.Write(scene.sceneNum);
				out.Write(scene.chest);
				out.Write(scene.swch);
				out.Write(scene.clear);
				out.Write(scene.collect);
				out.Write(scene.rooms);
				out.Write(scene.floors);
			}

			out.Write(worldMapAreaData);
			for (int i = 0; i < STORY_HS_COUNT; i++)
				out.Write(highScores[i]);
			out.Write(randoTriforcePieces);
			out.Write(randoBombchuLevel);
			out.WriteVarUInt((unsigned int)lww.size());
			for (const StoryLwwSlot& slot : lww)
			{
				out.Write(slot.id);
				out.Write(slot.stamp);
				out.WriteVarUInt((unsigned int)slot.data.size());
				if (!slot.data.empty())
					out.Write((const char*)slot.data.data(), (unsigned int)slot.data.size());
			}
		}

		// Returns false on a short or malformed blob. The object is left in an unspecified (but safe) state then.
		bool Deserialize(ByteStream& in)
		{
			// bytes before the two variable length tables
			static const unsigned int FIXED_SIZE = 1 + 24 + 2 + 4 + 4 + 20 + 19 + 7 + 2 + 2 + 2 + 28 + 8 + 60 + 8 + 24;

			if (in.BytesLeft() < FIXED_SIZE)
				return false;

			uint8_t version = in.Read<uint8_t>();
			if (version != 1 && version != STORY_PROGRESS_VERSION)
				return false;

			in.Read((char*)items, sizeof(items));
			equipment = in.Read<uint16_t>();
			upgrades = in.Read<uint32_t>();
			questItems = in.Read<uint32_t>();
			in.Read((char*)dungeonItems, sizeof(dungeonItems));
			in.Read((char*)dungeonKeys, sizeof(dungeonKeys));
			heartQuarters = in.Read<uint8_t>();
			magicLevel = in.Read<uint8_t>();
			isMagicAcquired = in.Read<uint8_t>();
			isDoubleMagicAcquired = in.Read<uint8_t>();
			isDoubleDefenseAcquired = in.Read<uint8_t>();
			defenseHearts = in.Read<uint8_t>();
			bgsFlag = in.Read<uint8_t>();
			swordHealth = in.Read<uint16_t>();
			gsTokens = in.Read<uint16_t>();
			bottleCount = in.Read<uint8_t>();
			hasRutoLetter = in.Read<uint8_t>();
			in.Read((char*)eventChkInf, sizeof(eventChkInf));
			in.Read((char*)itemGetInf, sizeof(itemGetInf));
			in.Read((char*)infTable, sizeof(infTable));
			in.Read((char*)eventInf, sizeof(eventInf));
			in.Read((char*)gsFlags, sizeof(gsFlags));

			unsigned int randCount = in.ReadVarUInt();
			if (randCount > 256 || in.BytesLeft() < randCount * 2)
				return false;

			randomizerInf.assign(randCount, 0);
			for (unsigned int i = 0; i < randCount; i++)
				randomizerInf[i] = in.Read<uint16_t>();

			unsigned int sceneCount = in.ReadVarUInt();
			const unsigned int sceneSize = version >= 2 ? 25 : 17;
			if (sceneCount > (unsigned int)STORY_SCENE_COUNT || in.BytesLeft() < sceneCount * sceneSize)
				return false;

			scenes.clear();
			for (unsigned int i = 0; i < sceneCount; i++)
			{
				StorySceneFlags scene;
				scene.sceneNum = in.Read<uint8_t>();
				scene.chest = in.Read<uint32_t>();
				scene.swch = in.Read<uint32_t>();
				scene.clear = in.Read<uint32_t>();
				scene.collect = in.Read<uint32_t>();
				if (version >= 2)
				{
					scene.rooms = in.Read<uint32_t>();
					scene.floors = in.Read<uint32_t>();
				}

				if (scene.sceneNum >= STORY_SCENE_COUNT)
					return false;

				MergeScene(scene);
			}

			if (version < 2)
				return true;

			// 4 + 7 * 4 + 1 + 1 + 1 bytes before the slot data
			if (in.BytesLeft() < 35)
				return false;

			worldMapAreaData = in.Read<uint32_t>();
			for (int i = 0; i < STORY_HS_COUNT; i++)
				highScores[i] = in.Read<int32_t>();
			randoTriforcePieces = in.Read<uint8_t>();
			randoBombchuLevel = in.Read<uint8_t>();

			unsigned int slotCount = in.ReadVarUInt();
			if (slotCount > (unsigned int)STORY_LWW_MAX_SLOTS)
				return false;

			lww.clear();
			for (unsigned int i = 0; i < slotCount; i++)
			{
				if (in.BytesLeft() < 6)
					return false;

				StoryLwwSlot slot;
				slot.id = in.Read<uint8_t>();
				slot.stamp = in.Read<uint32_t>();
				unsigned int len = in.ReadVarUInt();
				if (len > STORY_LWW_MAX_BYTES || in.BytesLeft() < len)
					return false;
				slot.data.resize(len);
				if (len > 0)
					in.Read((char*)slot.data.data(), len);
				MergeLww(slot);
			}
			return true;
		}

		// Monotone union of other into this. Returns true when anything changed.
		bool MergeFrom(const StoryProgress& other)
		{
			bool changed = false;

			for (int i = 0; i < 24; i++)
			{
				// 0xFF (empty) ranks lowest, otherwise the higher item id wins (upgrade chains ascend)
				if (other.items[i] != 0xFF && (items[i] == 0xFF || other.items[i] > items[i]))
				{
					items[i] = other.items[i];
					changed = true;
				}
			}

			changed |= OrInto(equipment, (uint16_t)(other.equipment & ~STORY_EQUIPMENT_EXCLUDE));
			changed |= OrInto(questItems, (uint32_t)(other.questItems & ~STORY_QUEST_ITEMS_EXCLUDE));

			for (int i = 0; i < 8; i++)
			{
				uint32_t mine = (upgrades & STORY_UPGRADE_MASKS[i]) >> STORY_UPGRADE_SHIFTS[i];
				uint32_t theirs = (other.upgrades & STORY_UPGRADE_MASKS[i]) >> STORY_UPGRADE_SHIFTS[i];
				if (theirs > mine)
				{
					upgrades = (upgrades & ~STORY_UPGRADE_MASKS[i]) | (theirs << STORY_UPGRADE_SHIFTS[i]);
					changed = true;
				}
			}

			for (int i = 0; i < 20; i++)
				changed |= OrInto(dungeonItems[i], other.dungeonItems[i]);

			// dungeon keys are only seeded, never merged: the live UPDATE_DUNGEON_KEYS mechanism owns them

			changed |= MaxInto(heartQuarters, other.heartQuarters);
			changed |= MaxInto(magicLevel, other.magicLevel);
			changed |= OrInto(isMagicAcquired, other.isMagicAcquired);
			changed |= OrInto(isDoubleMagicAcquired, other.isDoubleMagicAcquired);
			changed |= OrInto(isDoubleDefenseAcquired, other.isDoubleDefenseAcquired);
			changed |= MaxInto(defenseHearts, other.defenseHearts);
			changed |= OrInto(bgsFlag, other.bgsFlag);
			changed |= MaxInto(swordHealth, other.swordHealth);
			changed |= MaxInto(gsTokens, other.gsTokens);
			changed |= MaxInto(bottleCount, other.bottleCount);
			changed |= OrInto(hasRutoLetter, other.hasRutoLetter);

			{
				uint16_t incomingEvents[14];
				std::memcpy(incomingEvents, other.eventChkInf, sizeof(incomingEvents));
				for (int bit : STORY_EVENTCHKINF_EXCLUDE)
					incomingEvents[bit >> 4] &= (uint16_t)~(1u << (bit & 15));
				StoryClearFlagBits(incomingEvents, 14, STORY_EVENTCHKINF_ITEM_EXCLUDE,
				                   (int)(sizeof(STORY_EVENTCHKINF_ITEM_EXCLUDE) / sizeof(int)));

				for (int i = 0; i < 14; i++)
					changed |= OrInto(eventChkInf[i], incomingEvents[i]);
			}
			{
				uint16_t incomingItemGet[4];
				std::memcpy(incomingItemGet, other.itemGetInf, sizeof(incomingItemGet));
				StoryClearFlagBits(incomingItemGet, 4, STORY_ITEMGETINF_EXCLUDE,
				                   (int)(sizeof(STORY_ITEMGETINF_EXCLUDE) / sizeof(int)));
				for (int i = 0; i < 4; i++)
					changed |= OrInto(itemGetInf[i], incomingItemGet[i]);

				uint16_t incomingInfTable[30];
				std::memcpy(incomingInfTable, other.infTable, sizeof(incomingInfTable));
				StoryClearFlagBits(incomingInfTable, 30, STORY_INFTABLE_ITEM_EXCLUDE,
				                   (int)(sizeof(STORY_INFTABLE_ITEM_EXCLUDE) / sizeof(int)));
				for (int i = 0; i < 30; i++)
				{
					if (i != STORY_INFTABLE_EXCLUDE_INDEX)
						changed |= OrInto(infTable[i], incomingInfTable[i]);
				}
			}
			for (int i = 0; i < 4; i++)
				changed |= OrInto(eventInf[i], other.eventInf[i]);
			for (int i = 0; i < 6; i++)
			{
				if ((gsFlags[i] | other.gsFlags[i]) != gsFlags[i])
				{
					gsFlags[i] |= other.gsFlags[i];
					changed = true;
				}
			}

			if (randomizerInf.size() < other.randomizerInf.size())
				randomizerInf.resize(other.randomizerInf.size(), 0);
			for (size_t i = 0; i < other.randomizerInf.size(); i++)
				changed |= OrInto(randomizerInf[i], other.randomizerInf[i]);

			for (const StorySceneFlags& scene : other.scenes)
				changed |= MergeScene(scene);

			changed |= OrInto(worldMapAreaData, other.worldMapAreaData);
			for (int i = 0; i < STORY_HS_COUNT; i++)
			{
				if (!StoryHighScoreShared(i))
					continue;
				int32_t merged = StoryMergeHighScore(i, highScores[i], other.highScores[i]);
				if (merged != highScores[i])
				{
					highScores[i] = merged;
					changed = true;
				}
			}
			changed |= MaxInto(randoTriforcePieces, other.randoTriforcePieces);
			changed |= MaxInto(randoBombchuLevel, other.randoBombchuLevel);
			for (const StoryLwwSlot& slot : other.lww)
				changed |= MergeLww(slot);

			return changed;
		}

		bool operator==(const StoryProgress& other) const
		{
			ByteStream a, b;
			Serialize(a);
			other.Serialize(b);
			return a.Length() == b.Length() && memcmp(a.Text(), b.Text(), a.Length()) == 0;
		}

		bool operator!=(const StoryProgress& other) const
		{
			return !(*this == other);
		}

		// Switches bits off again in the permanent switch and clear flags of one scene (a toggle that was turned back
		// off). The union merge cannot do this, so the server calls it when a member unsets a flag in a dungeon.
		bool ClearSceneBits(int sceneNum, uint32_t swchMask, uint32_t clearMask)
		{
			for (StorySceneFlags& scene : scenes)
			{
				if (scene.sceneNum != sceneNum)
					continue;
				uint32_t swch = scene.swch & ~swchMask;
				uint32_t clear = scene.clear & ~clearMask;
				bool changed = swch != scene.swch || clear != scene.clear;
				scene.swch = swch;
				scene.clear = clear;
				return changed;
			}
			return false;
		}

		// OR one scene entry into the sorted table. Returns true when anything changed.
		bool MergeScene(const StorySceneFlags& incoming)
		{
			StorySceneFlags scene = incoming;
			scene.swch &= ~StorySceneSwchExclude(scene.sceneNum);

			if (scene.chest == 0 && scene.swch == 0 && scene.clear == 0 && scene.collect == 0 && scene.rooms == 0 &&
			    scene.floors == 0)
				return false;

			size_t pos = 0;
			while (pos < scenes.size() && scenes[pos].sceneNum < scene.sceneNum)
				pos++;

			if (pos == scenes.size() || scenes[pos].sceneNum != scene.sceneNum)
			{
				StorySceneFlags fresh;
				fresh.sceneNum = scene.sceneNum;
				scenes.insert(scenes.begin() + pos, fresh);
			}

			StorySceneFlags& mine = scenes[pos];
			bool changed = false;
			changed |= OrInto(mine.chest, scene.chest);
			changed |= OrInto(mine.swch, scene.swch);
			changed |= OrInto(mine.clear, scene.clear);
			changed |= OrInto(mine.collect, scene.collect);
			changed |= OrInto(mine.rooms, scene.rooms);
			changed |= OrInto(mine.floors, scene.floors);
			return changed;
		}

	private:
		template <class T>
		static bool OrInto(T& dst, T src)
		{
			T merged = (T)(dst | src);
			if (merged == dst)
				return false;
			dst = merged;
			return true;
		}

		template <class T>
		static bool MaxInto(T& dst, T src)
		{
			if (src <= dst)
				return false;
			dst = src;
			return true;
		}
	};
}

#endif
