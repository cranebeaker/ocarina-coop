#include "Party.hpp"
#include "Player.hpp"
#include "Scene.hpp"
#include "nlohmann/json.hpp"
#include <cstdio>
#include <random>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <system_error>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ZeldaOnline
{
	namespace
	{
		// Client supplied JSON is never trusted: a wrong typed key must not throw out of the packet handler
		bool ParseSettingsObject(const std::string& text, nlohmann::json& out)
		{
			try
			{
				out = nlohmann::json::parse(text, nullptr, false);
				return out.is_object();
			}
			catch (const std::exception&)
			{
				return false;
			}
		}

		bool ReadBoolKey(const nlohmann::json& object, const char* key, bool fallback)
		{
			try
			{
				auto it = object.find(key);
				if (it == object.end() || !it->is_boolean())
					return fallback;
				return it->get<bool>();
			}
			catch (const std::exception&)
			{
				return fallback;
			}
		}

		// Optional party code that names the on-disk team save together with the admin nickname
		std::string ReadCodeKey(const nlohmann::json& object, const char* key)
		{
			try
			{
				auto it = object.find(key);
				if (it == object.end() || !it->is_string())
					return std::string();
				std::string code = it->get<std::string>();
				if (code.size() > 32)
					code.resize(32);
				return code;
			}
			catch (const std::exception&)
			{
				return std::string();
			}
		}

		// Random team code for a party that switches Story Co-op on without one: two parties must never end up with the
		// same save key just because their admins picked the same nickname.
		std::string GenerateStoryCode()
		{
			static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
			static std::mt19937_64 rng{ std::random_device{}() ^ (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() };
			std::string code;
			for (int i = 0; i < 8; i++)
				code += alphabet[rng() % (sizeof(alphabet) - 1)];
			return code;
		}

		std::string DumpSettings(const nlohmann::json& object)
		{
			return object.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
		}
	}

	void Party::SetSettings(const std::string& settings) {
		const bool wasStoryCoop = m_storyCoop;

		if (settings.empty())
		{
			m_settings.clear();
			m_privateDungeons = false;
			m_storyCoop = false;
			m_shareConsumables = false;
			m_storyCode.clear();
			StorySettingsChanged(wasStoryCoop);
			return;
		}

		nlohmann::json parsed;
		if (!ParseSettingsObject(settings, parsed))
		{
			std::printf("Party: settings json is not a valid object, ignoring\n");
			return;
		}

		m_privateDungeons = ReadBoolKey(parsed, "privateDungeons", false);
		m_storyCoop = ReadBoolKey(parsed, "storyCoop", false);
		m_shareConsumables = m_storyCoop && ReadBoolKey(parsed, "shareAmmoRupeesHp", false);
		const std::string sentCode = ReadCodeKey(parsed, "storyCode");
		if (wasStoryCoop && m_storyCoop && !m_storyCode.empty())
		{
			// the save was chosen when Story Co-op was switched on, changing the code now would not move it
		}
		else
			m_storyCode = sentCode;
		if (m_storyCoop && m_storyCode.empty())
			m_storyCode = GenerateStoryCode();
		if (m_storyCoop && m_storyCode != sentCode)
		{
			parsed["storyCode"] = m_storyCode;
			m_settingsAdjusted = true;
		}
		try
		{
			m_settings = DumpSettings(parsed);
		}
		catch (const std::exception& e)
		{
			std::printf("Party: failed to store settings json (%s)\n", e.what());
			m_settings.clear();
		}
		StorySettingsChanged(wasStoryCoop);
	}

	bool Party::SetSettingsFromMember(const std::string& settings, bool isAdmin)
	{
		if (isAdmin)
		{
			SetSettings(settings);
			return true;
		}

		nlohmann::json parsed;
		if (!ParseSettingsObject(settings, parsed))
			return false;

		// a non-admin may not change the Story Co-op keys: they keep the party's current values
		bool same = ReadBoolKey(parsed, "storyCoop", false) == m_storyCoop &&
			ReadBoolKey(parsed, "shareAmmoRupeesHp", false) == m_shareConsumables;

		parsed["storyCoop"] = m_storyCoop;
		parsed["shareAmmoRupeesHp"] = m_shareConsumables;
		parsed["storyCode"] = m_storyCode;
		try
		{
			SetSettings(DumpSettings(parsed));
		}
		catch (const std::exception&)
		{
			return false;
		}
		return same;
	}

	bool Party::MergeStory(const StoryProgress& incoming)
	{
		bool changed = false;

		if (!m_storySeeded)
		{
			// the first snapshot also seeds the small key counts, afterwards UPDATE_DUNGEON_KEYS owns them
			for (int i = 0; i < 19; i++)
			{
				if (incoming.dungeonKeys[i] > 0)
					m_dungeonKeys[i] = (uint8_t)incoming.dungeonKeys[i];
			}
			m_storySeeded = true;
			changed = true;
		}

		if (m_story.MergeFrom(incoming))
			changed = true;

		if (changed)
			MarkStoryDirty();

		return changed;
	}

	void Party::SendToAll(const ByteStream& payload, Player* except)
	{
		for (auto& other : m_members)
		{
			if (other == except)
				continue;
			other->SendPacket(payload);
		}
	}

	void Party::SendToScene(Scene* scene, const ByteStream& payload, Player* except)
	{
		for (auto& other : m_members)
		{
			if (other == except || other->CurrentScene() != scene)
				continue;
			other->SendPacket(payload);
		}
	}
}


namespace ZeldaOnline
{
	// A travel that has not finished by then (Chamber of Sages included) is considered done
	static const int STORY_TRAVEL_TIMEOUT_SECONDS = 180;
	// Unsaved team save changes are written this long after the first change
	static const int STORY_SAVE_DEBOUNCE_SECONDS = 3;
	static const int STORY_BACKUP_COUNT = 3;
	static const int STORY_BACKUP_INTERVAL_MINUTES = 10;

	bool Party::HasVoted(uint16_t netID) const
	{
		for (uint16_t id : m_votes)
		{
			if (id == netID)
				return true;
		}
		return false;
	}

	uint8_t Party::EffectiveEra() const
	{
		return m_travelling ? m_travelTarget : m_era;
	}

	void Party::ResetVotes()
	{
		m_votes.clear();
		m_travelling = false;
	}

	void Party::StorySettingsChanged(bool wasStoryCoop)
	{
		if (!wasStoryCoop && m_storyCoop)
		{
			ResetVotes();
			m_era = ERA_UNKNOWN;
			m_era = ComputeEra();
			SelectStorySave();
		}
		else if (wasStoryCoop && !m_storyCoop)
		{
			// release every hold
			ResetVotes();
			m_era = ERA_UNKNOWN;
			SendVoteStatusTo(nullptr, false);
			if (m_storyDirty)
				WriteStorySave();
		}
	}

	void Party::SendVoteStatusTo(Player* player, bool active)
	{
		ByteStream packet = newPacket(SERVER_PACKET_STORY_VOTE_STATUS);

		std::vector<std::string> missing;
		if (active)
		{
			for (Player* member : m_members)
			{
				if (!HasVoted((uint16_t)member->NetworkID()))
					missing.push_back(member->NickName().substr(0, 255));
			}
		}

		packet << PackedUInt1(active ? 1u : 0u)
			<< PackedUInt1((unsigned int)(active ? m_votes.size() : 0))
			<< PackedUInt1((unsigned int)m_members.size())
			<< PackedUInt1(m_travelling ? 1u : 0u)
			<< PackedUInt1((unsigned int)missing.size());

		for (auto& name : missing)
		{
			packet << PackedUInt1((unsigned int)name.length());
			packet << name;
		}

		if (player)
			player->SendPacket(packet);
		else
			SendToAll(packet);
	}

	void Party::BroadcastVoteStatus()
	{
		SendVoteStatusTo(nullptr, !m_votes.empty() && !m_travelling);
	}

	void Party::HandleVote(Player* player, bool vote)
	{
		if (!m_storyCoop || !HasMember(player))
			return;

		const uint16_t netID = (uint16_t)player->NetworkID();

		if (!vote)
		{
			for (size_t i = 0; i < m_votes.size(); i++)
			{
				if (m_votes[i] == netID)
				{
					m_votes.erase(m_votes.begin() + i);
					break;
				}
			}

			EvaluateVotes(true);
			return;
		}

		if (m_travelling)
		{
			// too late to join this travel: release the hold, the sender is moved when it is over
			SendVoteStatusTo(player, false);
			return;
		}

		// only members standing in the party's era may vote; the others are moved there first
		const uint8_t era = EffectiveEra();
		if (era != ERA_UNKNOWN && player->Age() != era)
		{
			player->SendPacket(newPacket(SERVER_PACKET_STORY_SET_ERA) << PackedUInt1(era));
			SendVoteStatusTo(player, false);
			return;
		}

		if (!HasVoted(netID))
			m_votes.push_back(netID);

		EvaluateVotes(true);
	}

	void Party::EvaluateVotes(bool forceBroadcast)
	{
		if (!m_storyCoop || m_travelling)
			return;

		bool allVoted = !m_members.empty() && !m_votes.empty();
		for (Player* member : m_members)
		{
			if (!HasVoted((uint16_t)member->NetworkID()))
			{
				allVoted = false;
				break;
			}
		}

		if (allVoted)
			StartTravel();
		else if (forceBroadcast)
			BroadcastVoteStatus();
	}

	void Party::StartTravel()
	{
		m_travelling = true;
		// the target is always the opposite of the party's era
		if (m_era == ERA_UNKNOWN)
			m_era = ComputeEra();
		m_travelTarget = (m_era == 0) ? 1 : 0;
		m_travelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(STORY_TRAVEL_TIMEOUT_SECONDS);
		m_votes.clear();

		SendToAll(newPacket(SERVER_PACKET_STORY_TRAVEL) << PackedUInt1(m_travelTarget));
		SendVoteStatusTo(nullptr, false);
	}

	void Party::CheckTravelDone(bool timedOut)
	{
		if (!m_travelling)
			return;

		size_t atTarget = 0;
		for (Player* member : m_members)
		{
			if (member->Age() == m_travelTarget)
				atTarget++;
		}

		if (!timedOut && atTarget != m_members.size())
			return;

		m_travelling = false;

		// on timeout the majority decides where the party stands, everybody else is moved there
		m_era = (atTarget * 2 >= m_members.size()) ? m_travelTarget : (uint8_t)(1 - m_travelTarget);

		for (Player* member : m_members)
		{
			if (member->Age() != m_era)
				member->SendPacket(newPacket(SERVER_PACKET_STORY_SET_ERA) << PackedUInt1(m_era));
		}

		SendVoteStatusTo(nullptr, false);
	}

	void Party::OnMemberAdded(Player* player)
	{
		if (!m_storyCoop)
			return;

		if (m_era == ERA_UNKNOWN)
			m_era = ComputeEra();

		// a late joiner stands in the party's era
		uint8_t era = EffectiveEra();
		if (era != ERA_UNKNOWN && player->Age() != era)
			player->SendPacket(newPacket(SERVER_PACKET_STORY_SET_ERA) << PackedUInt1(era));

		// N changed: votes cast so far no longer cover everybody
		if (!m_votes.empty() && !m_travelling)
			BroadcastVoteStatus();
	}

	void Party::OnMemberRemoved(Player* player)
	{
		if (!m_storyCoop)
			return;

		// the leaver never stays on hold (their client also clears it when the party goes away)
		SendVoteStatusTo(player, false);

		bool hadVote = false;
		for (size_t i = 0; i < m_votes.size(); i++)
		{
			if (m_votes[i] == (uint16_t)player->NetworkID())
			{
				m_votes.erase(m_votes.begin() + i);
				hadVote = true;
				break;
			}
		}

		if (m_members.empty())
		{
			ResetVotes();
			return;
		}

		if (m_travelling)
			CheckTravelDone(false);
		else
			EvaluateVotes(hadVote || !m_votes.empty());
	}

	void Party::OnMemberAgeChanged(Player* player)
	{
		if (!m_storyCoop)
			return;

		if (m_travelling)
		{
			CheckTravelDone(false);
			return;
		}

		if (m_members.empty())
			return;

		// the party era is the one most members are in (a tie keeps the current era); a member standing
		// in the other era is moved back by the late joiner path and loses its vote
		m_era = ComputeEra();
		if (player && player->Age() != m_era)
		{
			for (size_t i = 0; i < m_votes.size(); i++)
			{
				if (m_votes[i] == (uint16_t)player->NetworkID())
				{
					m_votes.erase(m_votes.begin() + i);
					break;
				}
			}

			player->SendPacket(newPacket(SERVER_PACKET_STORY_SET_ERA) << PackedUInt1(m_era));
			if (!m_votes.empty())
				BroadcastVoteStatus();
		}
	}

	uint8_t Party::ComputeEra() const
	{
		size_t count[2] = { 0, 0 };
		for (Player* member : m_members)
		{
			if (member->Age() < 2)
				count[member->Age()]++;
		}

		if (count[0] > count[1])
			return 0;
		if (count[1] > count[0])
			return 1;

		// tie (or nobody): keep the current era, else the admin's, else adult
		if (m_era < 2)
			return m_era;
		if (!m_members.empty() && m_members[0]->Age() < 2)
			return m_members[0]->Age();
		return 1;
	}

	void Party::TickAll(std::chrono::steady_clock::time_point now)
	{
		std::vector<std::shared_ptr<Party>> parties;
		for (auto& entry : s_parties)
		{
			if (auto party = entry.second.lock())
			{
				if (party->m_storyCoop && (party->m_travelling || party->m_storyDirty))
					parties.push_back(party);
			}
		}

		for (auto& party : parties)
		{
			if (party->m_travelling && now >= party->m_travelDeadline)
				party->CheckTravelDone(true);

			if (party->m_storyDirty && now - party->m_storyDirtySince >= std::chrono::seconds(STORY_SAVE_DEBOUNCE_SECONDS))
				party->WriteStorySave();
		}
	}
}


// Story Co-op team save on disk
//
// File: <dataDir>/story/<sanitized key>-<hash>.bin, with STORY_BACKUP_COUNT rotating backups (.bak1 is the newest).
// Layout: magic "OOTSCSAV", u8 file version, u8 seeded, u8 dungeonKeys[19], u32 payload size, payload (StoryProgress::Serialize,
// which carries its own version byte), u32 FNV-1a checksum of everything before it.
namespace ZeldaOnline
{
	namespace
	{
		namespace fs = std::filesystem;

		const char STORY_SAVE_MAGIC[8] = { 'O', 'O', 'T', 'S', 'C', 'S', 'A', 'V' };
		const uint8_t STORY_SAVE_FILE_VERSION = 1;
		const unsigned int STORY_SAVE_MAX_PAYLOAD = 1 << 20;

		std::string s_storyDir;
		std::unordered_map<std::string, uint32_t> s_saveKeyOwners;

		uint32_t Fnv1a(const char* data, size_t size)
		{
			uint32_t hash = 2166136261u;
			for (size_t i = 0; i < size; i++)
			{
				hash ^= (uint8_t)data[i];
				hash *= 16777619u;
			}
			return hash;
		}

		std::string MakeFileStem(const std::string& key)
		{
			std::string stem;
			for (char c : key)
			{
				if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
					stem += c;
				else if (c >= 'A' && c <= 'Z')
					stem += (char)(c - 'A' + 'a');
				else
					stem += '_';
				if (stem.size() >= 40)
					break;
			}

			char hash[16];
			std::snprintf(hash, sizeof(hash), "-%08x", Fnv1a(key.data(), key.size()));
			return stem + hash;
		}

		bool WriteFileAtomic(const fs::path& path, const char* data, size_t size)
		{
			fs::path temp = path;
			temp += ".tmp";

			FILE* f = std::fopen(temp.string().c_str(), "wb");
			if (f == nullptr)
				return false;

			bool ok = std::fwrite(data, 1, size, f) == size && std::fflush(f) == 0;
			if (ok)
			{
#if defined(_WIN32)
				_commit(_fileno(f));
#else
				fsync(fileno(f));
#endif
			}
			ok = (std::fclose(f) == 0) && ok;

			std::error_code ec;
			if (ok)
				fs::rename(temp, path, ec);
			if (!ok || ec)
			{
				fs::remove(temp, ec);
				return false;
			}
			return true;
		}

		bool ReadWholeFile(const fs::path& path, std::string& out)
		{
			FILE* f = std::fopen(path.string().c_str(), "rb");
			if (f == nullptr)
				return false;

			out.clear();
			char chunk[4096];
			size_t got;
			while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
			{
				out.append(chunk, got);
				if (out.size() > STORY_SAVE_MAX_PAYLOAD + 1024)
					break;
			}
			std::fclose(f);
			return true;
		}

		// Magic, version, sizes and checksum of a team save file are intact (the payload itself is not parsed).
		bool IsIntactSaveFile(const fs::path& file)
		{
			std::string blob;
			if (!ReadWholeFile(file, blob) || blob.size() < sizeof(STORY_SAVE_MAGIC) + 2 + 19 + 4 + 4)
				return false;
			const size_t sumPos = blob.size() - 4;
			uint32_t sum;
			std::memcpy(&sum, blob.data() + sumPos, 4);
			return sum == Fnv1a(blob.data(), sumPos) &&
				std::memcmp(blob.data(), STORY_SAVE_MAGIC, sizeof(STORY_SAVE_MAGIC)) == 0 &&
				(uint8_t)blob[sizeof(STORY_SAVE_MAGIC)] == STORY_SAVE_FILE_VERSION;
		}

		fs::path BackupPath(const fs::path& path, int index)
		{
			fs::path result = path;
			result += ".bak" + std::to_string(index);
			return result;
		}
	}

	void Party::SetStoryDataDir(const std::string& dir)
	{
		s_storyDir = dir.empty() ? std::string() : (fs::path(dir) / "story").string();
		if (!s_storyDir.empty())
			std::printf("Story Co-op saves: %s\n", s_storyDir.c_str());
		else
			std::printf("Story Co-op saves: disabled (memory only)\n");
	}

	Party::~Party()
	{
		if (m_storyDirty)
			WriteStorySave();
		ReleaseSaveKey();
		s_parties.erase(m_id);
	}

	void Party::SaveAllStory()
	{
		for (auto& entry : s_parties)
		{
			if (auto party = entry.second.lock())
			{
				if (party->m_storyDirty)
					party->WriteStorySave();
			}
		}
	}

	void Party::ReleaseSaveKey()
	{
		if (m_saveKey.empty())
			return;
		auto it = s_saveKeyOwners.find(m_saveKey);
		if (it != s_saveKeyOwners.end() && it->second == m_id)
			s_saveKeyOwners.erase(it);
		m_saveKey.clear();
	}

	void Party::MarkStoryDirty()
	{
		if (m_saveKey.empty())
			return;
		if (!m_storyDirty)
			m_storyDirtySince = std::chrono::steady_clock::now();
		m_storyDirty = true;
	}

	// Save key: the admin nickname plus the optional "storyCode" party setting. Chosen when Story Co-op is switched on.
	void Party::SelectStorySave()
	{
		if (s_storyDir.empty())
			return;

		Player* admin = GetAdmin();
		std::string key = (admin ? admin->NickName() : std::string("Player")) + "|" + m_storyCode;
		if (key == m_saveKey)
			return;

		if (m_storyDirty)
			WriteStorySave();
		ReleaseSaveKey();

		if (s_saveKeyOwners.find(key) != s_saveKeyOwners.end())
		{
			std::printf("Story Co-op: save key already used by another running party, this party is not persisted\n");
			return;
		}

		s_saveKeyOwners[key] = m_id;
		m_saveKey = key;
		LoadStorySave();
	}

	void Party::LoadStorySave()
	{
		fs::path path = fs::path(s_storyDir) / (MakeFileStem(m_saveKey) + ".bin");

		for (int attempt = 0; attempt <= STORY_BACKUP_COUNT; attempt++)
		{
			fs::path file = attempt == 0 ? path : BackupPath(path, attempt);
			std::error_code ec;
			if (!fs::exists(file, ec))
				continue;

			try
			{
				std::string blob;
				if (!ReadWholeFile(file, blob) || blob.size() < sizeof(STORY_SAVE_MAGIC) + 2 + 19 + 4 + 4)
					throw std::runtime_error("too short");

				const size_t sumPos = blob.size() - 4;
				uint32_t sum;
				std::memcpy(&sum, blob.data() + sumPos, 4);
				if (sum != Fnv1a(blob.data(), sumPos))
					throw std::runtime_error("checksum mismatch");

				if (std::memcmp(blob.data(), STORY_SAVE_MAGIC, sizeof(STORY_SAVE_MAGIC)) != 0)
					throw std::runtime_error("bad magic");

				size_t pos = sizeof(STORY_SAVE_MAGIC);
				if ((uint8_t)blob[pos++] != STORY_SAVE_FILE_VERSION)
					throw std::runtime_error("unsupported file version");

				const bool seeded = blob[pos++] != 0;
				uint8_t keys[19];
				std::memcpy(keys, blob.data() + pos, 19);
				pos += 19;

				uint32_t payloadSize;
				std::memcpy(&payloadSize, blob.data() + pos, 4);
				pos += 4;
				if (payloadSize > STORY_SAVE_MAX_PAYLOAD || payloadSize != sumPos - pos)
					throw std::runtime_error("bad payload size");

				ByteStream payload(blob.data() + pos, payloadSize);
				StoryProgress loaded;
				if (!loaded.Deserialize(payload))
					throw std::runtime_error("team save payload rejected");

				if (!seeded)
					return;

				// union into whatever the party already holds; key counts are only taken when the party has none yet
				m_story.MergeFrom(loaded);
				for (int i = 0; i < 19; i++)
				{
					if (!m_storySeeded || m_dungeonKeys[i] == 0)
						m_dungeonKeys[i] = keys[i];
				}
				m_storySeeded = true;
				std::printf("Story Co-op: loaded team save %s%s\n", file.filename().string().c_str(), attempt ? " (backup)" : "");
				return;
			}
			catch (const std::exception& e)
			{
				std::printf("Story Co-op: team save %s is unusable (%s)\n", file.filename().string().c_str(), e.what());
			}
		}
	}

	bool Party::WriteStorySave()
	{
		m_storyDirty = false;
		if (s_storyDir.empty() || m_saveKey.empty() || !m_storySeeded)
			return false;

		try
		{
			ByteStream payload;
			m_story.Serialize(payload);

			std::string blob;
			blob.append(STORY_SAVE_MAGIC, sizeof(STORY_SAVE_MAGIC));
			blob += (char)STORY_SAVE_FILE_VERSION;
			blob += (char)1;
			blob.append((const char*)m_dungeonKeys, 19);
			uint32_t payloadSize = payload.Length();
			blob.append((const char*)&payloadSize, 4);
			blob.append(payload.Text(), payload.Length());
			uint32_t sum = Fnv1a(blob.data(), blob.size());
			blob.append((const char*)&sum, 4);

			std::error_code ec;
			fs::create_directories(s_storyDir, ec);
			fs::path path = fs::path(s_storyDir) / (MakeFileStem(m_saveKey) + ".bin");

			// rotate the backups at most every few minutes
			const auto now = std::chrono::steady_clock::now();
			// A damaged main file is never copied over the backups, they are what is left of the team save.
			const bool backupDue = m_lastBackup == std::chrono::steady_clock::time_point{} ||
				now - m_lastBackup >= std::chrono::minutes(STORY_BACKUP_INTERVAL_MINUTES);
			if (backupDue && fs::exists(path, ec) && !IsIntactSaveFile(path))
				std::printf("Story Co-op: team save %s is damaged, keeping the backups as they are\n", path.filename().string().c_str());
			else if (backupDue && fs::exists(path, ec))
			{
				fs::remove(BackupPath(path, STORY_BACKUP_COUNT), ec);
				for (int i = STORY_BACKUP_COUNT - 1; i >= 1; i--)
				{
					if (fs::exists(BackupPath(path, i), ec))
						fs::rename(BackupPath(path, i), BackupPath(path, i + 1), ec);
				}
				fs::copy_file(path, BackupPath(path, 1), fs::copy_options::overwrite_existing, ec);
				m_lastBackup = now;
			}

			if (!WriteFileAtomic(path, blob.data(), blob.size()))
			{
				std::printf("Story Co-op: failed to write team save %s\n", path.string().c_str());
				m_storyDirty = true;
				m_storyDirtySince = now;
				return false;
			}
			return true;
		}
		catch (const std::exception& e)
		{
			std::printf("Story Co-op: team save write failed (%s)\n", e.what());
			return false;
		}
	}
}
