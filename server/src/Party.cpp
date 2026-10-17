#include "Party.hpp"
#include "Player.hpp"
#include "Scene.hpp"
#include "nlohmann/json.hpp"

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
		}
		else if (wasStoryCoop && !m_storyCoop)
		{
			// release every hold
			ResetVotes();
			m_era = ERA_UNKNOWN;
			SendVoteStatusTo(nullptr, false);
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
				if (party->m_storyCoop && party->m_travelling)
					parties.push_back(party);
			}
		}

		for (auto& party : parties)
		{
			if (now >= party->m_travelDeadline)
				party->CheckTravelDone(true);
		}
	}
}
