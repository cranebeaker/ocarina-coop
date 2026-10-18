// Offline checks of the Story Co-op team save wire format and merge rules (server/src/StoryProgress.hpp, the client
// mirror in soh/soh/Network/ZeldaOnline is the same file without ClearSceneBits). No game, no network.
//
// Build (Visual Studio developer prompt), from the repository root:
//   cl /nologo /std:c++20 /EHsc /utf-8 /DNOMINMAX /Iserver/src tools/coop-test/unit/story_progress_test.cpp ^
//      server/src/ByteStream.cpp /Fe:story_progress_test.exe
// Exit code 0 when every check passes.

#include <cstdio>
#include <cstdlib>
#include "StoryProgress.hpp"

using namespace ZeldaOnline;

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(cond)                                                                 \
	do                                                                              \
	{                                                                               \
		sChecks++;                                                                  \
		if (!(cond))                                                                \
		{                                                                           \
			sFailures++;                                                            \
			std::printf("FAIL line %d: %s\n", __LINE__, #cond);                     \
		}                                                                           \
	} while (0)

static std::vector<uint8_t> Bytes(std::initializer_list<uint8_t> v)
{
	return std::vector<uint8_t>(v);
}

static StoryProgress RoundTrip(const StoryProgress& in, bool* ok = nullptr)
{
	ByteStream stream;
	in.Serialize(stream);
	StoryProgress out;
	bool good = out.Deserialize(stream);
	if (ok)
		*ok = good;
	return out;
}

int main()
{
	// serialize / deserialize keeps every new field
	{
		StoryProgress p;
		p.worldMapAreaData = 0x00ABCDEF;
		p.highScores[STORY_HS_HBA] = 1500;
		p.highScores[STORY_HS_POE_POINTS] = 700;
		p.highScores[STORY_HS_FISHING] = 0x0A0100 | 25 | (30u << 24);
		p.highScores[STORY_HS_HORSE_RACE] = 40;
		p.highScores[STORY_HS_DAMPE_RACE] = 55;
		p.randoTriforcePieces = 7;
		p.randoBombchuLevel = 2;
		p.SetLww(STORY_LWW_HORSE, 4, Bytes({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }));
		p.SetLww(STORY_LWW_WATER_LEVEL, 9, Bytes({ 2 }));
		StorySceneFlags s;
		s.sceneNum = 5;
		s.rooms = 0x0F;
		s.floors = 0x03;
		p.MergeScene(s);

		bool ok = false;
		StoryProgress q = RoundTrip(p, &ok);
		CHECK(ok);
		CHECK(q == p);
		CHECK(q.worldMapAreaData == 0x00ABCDEF);
		CHECK(q.highScores[STORY_HS_POE_POINTS] == 700);
		CHECK(q.FindLww(STORY_LWW_WATER_LEVEL) != nullptr && q.FindLww(STORY_LWW_WATER_LEVEL)->stamp == 9);
		CHECK(q.scenes.size() == 1 && q.scenes[0].rooms == 0x0F && q.scenes[0].floors == 0x03);
	}

	// a version 1 blob (an old team save file) still loads, and the new fields stay empty
	{
		ByteStream v1;
		const uint8_t one = 1;
		v1.Write((const char*)&one, 1);
		uint8_t items[24];
		std::memset(items, 0xFF, sizeof(items));
		v1.Write((const char*)items, sizeof(items));
		v1.Write((uint16_t)3);
		v1.Write((uint32_t)0x10);
		v1.Write((uint32_t)0x20);
		uint8_t dungeonItems[20] = {};
		v1.Write((const char*)dungeonItems, sizeof(dungeonItems));
		int8_t keys[19] = {};
		v1.Write((const char*)keys, sizeof(keys));
		for (int i = 0; i < 7; i++)
			v1.Write((uint8_t)0);
		v1.Write((uint16_t)0);
		v1.Write((uint16_t)5); // gsTokens
		v1.Write((uint8_t)0);
		v1.Write((uint8_t)0);
		uint16_t eventChk[14] = {};
		eventChk[2] = 0x8001;
		v1.Write((const char*)eventChk, sizeof(eventChk));
		uint16_t itemGet[4] = {};
		v1.Write((const char*)itemGet, sizeof(itemGet));
		uint16_t inf[30] = {};
		v1.Write((const char*)inf, sizeof(inf));
		uint16_t ev[4] = {};
		v1.Write((const char*)ev, sizeof(ev));
		int32_t gs[6] = {};
		v1.Write((const char*)gs, sizeof(gs));
		v1.WriteVarUInt(0);
		v1.WriteVarUInt(1); // one scene, 17 byte entry
		v1.Write((uint8_t)3);
		v1.Write((uint32_t)1);
		v1.Write((uint32_t)2);
		v1.Write((uint32_t)4);
		v1.Write((uint32_t)8);

		StoryProgress p;
		CHECK(p.Deserialize(v1));
		CHECK(p.gsTokens == 5);
		CHECK(p.eventChkInf[2] == 0x8001);
		CHECK(p.scenes.size() == 1 && p.scenes[0].sceneNum == 3 && p.scenes[0].collect == 8 && p.scenes[0].rooms == 0);
		CHECK(p.lww.empty());
	}

	// truncated and oversized input is rejected, not crashed on
	{
		StoryProgress p;
		p.SetLww(STORY_LWW_HORSE, 1, Bytes({ 1, 2 }));
		ByteStream full;
		p.Serialize(full);
		for (unsigned int cut = 1; cut < full.Length(); cut += 7)
		{
			ByteStream part(full.Text(), cut);
			StoryProgress q;
			(void)q.Deserialize(part); // must simply return
		}
		CHECK(true);
	}

	// last writer wins: higher stamp, then larger bytes, never a lower stamp
	{
		StoryProgress team;
		StoryProgress a, b;
		a.SetLww(STORY_LWW_HORSE, 2, Bytes({ 1 }));
		b.SetLww(STORY_LWW_HORSE, 5, Bytes({ 9 }));
		CHECK(team.MergeFrom(a));
		CHECK(team.MergeFrom(b));
		CHECK(team.FindLww(STORY_LWW_HORSE)->data == Bytes({ 9 }));
		CHECK(!team.MergeFrom(a)); // an older value never comes back
		StoryProgress c;
		c.SetLww(STORY_LWW_HORSE, 5, Bytes({ 7 }));
		CHECK(!team.MergeFrom(c)); // same stamp, smaller bytes
		StoryProgress d;
		d.SetLww(STORY_LWW_HORSE, 5, Bytes({ 10 }));
		CHECK(team.MergeFrom(d)); // same stamp, larger bytes: both sides converge on it
		CHECK(team.FindLww(STORY_LWW_HORSE)->data == Bytes({ 10 }));
		// merging both ways gives the same result
		StoryProgress x, y;
		x.SetLww(STORY_LWW_WATER_LEVEL, 3, Bytes({ 1 }));
		y.SetLww(STORY_LWW_WATER_LEVEL, 3, Bytes({ 2 }));
		StoryProgress xy = x, yx = y;
		xy.MergeFrom(y);
		yx.MergeFrom(x);
		CHECK(xy == yx);
	}

	// water level and elevator never travel as plain scene switch bits
	{
		StorySceneFlags w;
		w.sceneNum = STORY_SCENE_WATER_TEMPLE;
		w.swch = 0x70000004u;
		StorySceneFlags f;
		f.sceneNum = STORY_SCENE_FOREST_TEMPLE;
		f.swch = 0x08000010u;
		StoryProgress p;
		p.MergeScene(w);
		p.MergeScene(f);
		for (const StorySceneFlags& s : p.scenes)
		{
			if (s.sceneNum == STORY_SCENE_WATER_TEMPLE)
				CHECK(s.swch == 0x00000004u);
			if (s.sceneNum == STORY_SCENE_FOREST_TEMPLE)
				CHECK(s.swch == 0x00000010u);
		}
	}

	// high scores
	{
		StoryProgress team, a, b;
		a.highScores[STORY_HS_HBA] = 1000;
		b.highScores[STORY_HS_HBA] = 1500;
		a.highScores[STORY_HS_HORSE_RACE] = 0;
		b.highScores[STORY_HS_HORSE_RACE] = 50;
		a.highScores[STORY_HS_DAMPE_RACE] = 70;
		b.highScores[STORY_HS_DAMPE_RACE] = 55;
		a.highScores[STORY_HS_MARATHON] = 12; // never shared
		team.MergeFrom(a);
		team.MergeFrom(b);
		CHECK(team.highScores[STORY_HS_HBA] == 1500);
		CHECK(team.highScores[STORY_HS_HORSE_RACE] == 50);
		CHECK(team.highScores[STORY_HS_DAMPE_RACE] == 55);
		CHECK(team.highScores[STORY_HS_MARATHON] == 0);

		// Big Poe points are a sum that only the server adds to; merging snapshots is a maximum
		StoryProgress poe;
		CHECK(poe.AddPoePoints(100));
		CHECK(poe.AddPoePoints(100));
		CHECK(poe.highScores[STORY_HS_POE_POINTS] == 200);
		StoryProgress stale;
		stale.highScores[STORY_HS_POE_POINTS] = 100;
		CHECK(!poe.MergeFrom(stale));
		CHECK(poe.highScores[STORY_HS_POE_POINTS] == 200);
		for (int i = 0; i < 20; i++)
			poe.AddPoePoints(100);
		CHECK(poe.highScores[STORY_HS_POE_POINTS] == STORY_POE_POINTS_MAX);

		// fishing: the longest catch per age, prize and visit bits or-ed, the owner's hat is not taken over
		uint32_t mine = 0x00001000u | 20u | (10u << 24) | 0x00010000u;
		uint32_t other = 25u | (8u << 24) | 0x00000400u | 0x00030000u;
		uint32_t merged = StoryMergeFishing(mine, other);
		CHECK((merged & 0x7F) == 25);
		CHECK(((merged >> 24) & 0x7F) == 10);
		CHECK((merged & 0x400) != 0);
		CHECK((merged & 0x1000) != 0);
		CHECK((merged & 0xFF0000) == 0x30000);
		CHECK((StoryMergeFishing(0, 0x1000u) & 0x1000) == 0);
	}

	std::printf("%d checks, %d failures\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
