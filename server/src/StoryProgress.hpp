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
	static const uint8_t STORY_PROGRESS_VERSION = 1;
	static const uint8_t STORY_FLAG_CATCH_UP = (1 << 0); // server -> client: sent to a member who just joined
	static const uint8_t STORY_FLAG_SEED = (1 << 0);     // client -> server: snapshot uploaded to seed or re-seed the team save

	// Exclusion masks. Bits listed here are never shared (they belong to the time travel flow or are per player).
	static const uint16_t STORY_EQUIPMENT_EXCLUDE = (1 << 1);               // Master Sword inventory bit (EQUIP_INV_SWORD_MASTER)
	static const uint32_t STORY_QUEST_ITEMS_EXCLUDE = 0xF0000000u;          // heart piece counter nibble, shared as heartQuarters
	static const int STORY_INFTABLE_EXCLUDE_INDEX = 29;                     // swordless flag row
	static const uint32_t STORY_WATER_TEMPLE_SWCH_EXCLUDE = 0x70000000u;    // swch 0x1C-0x1E
	static const uint32_t STORY_FOREST_TEMPLE_SWCH_EXCLUDE = 0x08000000u;   // swch 0x1B

	// eventChkInf bit numbers set only by the Master Sword time travel flow
	// (PULLED_MASTER_SWORD_FROM_PEDESTAL, ENTERED_MASTER_SWORD_CHAMBER, SHEIK_SPAWNED_AT_MASTER_SWORD_PEDESTAL, TIME_TRAVELED_TO_ADULT)
	static const int STORY_EVENTCHKINF_EXCLUDE[] = { 0x45, 0x4F, 0xC5, 0xC9 };

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
			if (version != STORY_PROGRESS_VERSION)
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
			if (sceneCount > (unsigned int)STORY_SCENE_COUNT || in.BytesLeft() < sceneCount * 17)
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

				if (scene.sceneNum >= STORY_SCENE_COUNT)
					return false;

				MergeScene(scene);
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

				for (int i = 0; i < 14; i++)
					changed |= OrInto(eventChkInf[i], incomingEvents[i]);
			}
			for (int i = 0; i < 4; i++)
				changed |= OrInto(itemGetInf[i], other.itemGetInf[i]);
			for (int i = 0; i < 30; i++)
			{
				if (i != STORY_INFTABLE_EXCLUDE_INDEX)
					changed |= OrInto(infTable[i], other.infTable[i]);
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

		// OR one scene entry into the sorted table. Returns true when anything changed.
		bool MergeScene(const StorySceneFlags& scene)
		{
			if (scene.chest == 0 && scene.swch == 0 && scene.clear == 0 && scene.collect == 0)
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
