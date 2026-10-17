#include "Party.hpp"
#include "Player.hpp"
#include "Scene.hpp"
#include "nlohmann/json.hpp"

namespace ZeldaOnline
{
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

		try
		{
			auto parsed = nlohmann::json::parse(settings);

			if (!parsed.is_object())
			{
				std::printf("Party: settings json is not an object, ignoring\n");
				return;
			}

			m_privateDungeons = parsed.value("privateDungeons", false);
			m_storyCoop = parsed.value("storyCoop", false);
			m_shareConsumables = m_storyCoop && parsed.value("shareAmmoRupeesHp", false);
			m_settings = parsed.dump();
			StorySettingsChanged(wasStoryCoop);
		}
		catch (const std::exception& e)
		{
			std::printf("Party: failed to parse settings json (%s), ignoring\n", e.what());
		}
	}

	bool Party::SetSettingsFromMember(const std::string& settings, bool isAdmin)
	{
		if (isAdmin || !m_storyCoop)
		{
			// a non-admin may not switch Story Co-op on, so check the incoming keys when it is currently off
			if (!isAdmin)
			{
				try
				{
					auto parsed = nlohmann::json::parse(settings, nullptr, false);
					if (parsed.is_object() && (parsed.value("storyCoop", false) || parsed.value("shareAmmoRupeesHp", false)))
					{
						parsed["storyCoop"] = false;
						parsed["shareAmmoRupeesHp"] = false;
						SetSettings(parsed.dump());
						return false;
					}
				}
				catch (const std::exception&)
				{
				}
			}

			SetSettings(settings);
			return true;
		}

		// Story Co-op is active and the sender is not the admin: keep the current Story Co-op keys
		auto parsed = nlohmann::json::parse(settings, nullptr, false);
		if (!parsed.is_object())
			return false;

		bool same = parsed.value("storyCoop", false) == m_storyCoop &&
			parsed.value("shareAmmoRupeesHp", false) == m_shareConsumables;

		parsed["storyCoop"] = m_storyCoop;
		parsed["shareAmmoRupeesHp"] = m_shareConsumables;
		SetSettings(parsed.dump());
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
		m_voteEra = ERA_UNKNOWN;
		m_travelling = false;
	}

	void Party::StorySettingsChanged(bool wasStoryCoop)
	{
		if (!wasStoryCoop && m_storyCoop)
		{
			ResetVotes();
			Player* admin = GetAdmin();
			m_era = admin ? admin->Age() : ERA_UNKNOWN;
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

			if (m_votes.empty())
				m_voteEra = ERA_UNKNOWN;

			EvaluateVotes(true);
			return;
		}

		if (m_travelling)
		{
			// too late to join this travel: release the hold, the sender is moved when it is over
			SendVoteStatusTo(player, false);
			return;
		}

		if (m_votes.empty())
			m_voteEra = player->Age();

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
		m_travelTarget = (m_voteEra == 0) ? 1 : 0;
		m_travelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(STORY_TRAVEL_TIMEOUT_SECONDS);
		m_votes.clear();
		m_voteEra = ERA_UNKNOWN;

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
		{
			Player* admin = GetAdmin();
			m_era = admin ? admin->Age() : player->Age();
		}

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

		if (m_votes.empty())
			m_voteEra = ERA_UNKNOWN;

		if (m_travelling)
			CheckTravelDone(false);
		else
			EvaluateVotes(hadVote || !m_votes.empty());
	}

	void Party::OnMemberAgeChanged(Player* player)
	{
		(void)player;
		if (!m_storyCoop)
			return;

		if (m_travelling)
		{
			CheckTravelDone(false);
			return;
		}

		// everybody agrees on a new era (e.g. the last straggler arrived)
		if (m_members.empty())
			return;

		uint8_t common = m_members[0]->Age();
		for (Player* member : m_members)
		{
			if (member->Age() != common)
				return;
		}
		m_era = common;
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
