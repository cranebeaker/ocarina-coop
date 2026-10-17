#ifndef ZELDAONLINE_PARTY_HPP
#define ZELDAONLINE_PARTY_HPP

#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ByteStream.hpp"
#include "StoryProgress.hpp"

namespace ZeldaOnline
{
	class Player;
	class Scene;

	class Party
	{
	public:
		static std::shared_ptr<Party> Create()
		{
			uint32_t id = s_nextID++;

			if (s_nextID == 0)
				s_nextID = 1;

			auto party = std::shared_ptr<Party>(new Party(id));
			s_parties[id] = party;
			return party;
		}

		static std::shared_ptr<Party> FindByID(uint32_t id)
		{
			if (id == 0)
				return nullptr;

			auto it = s_parties.find(id);

			if (it == s_parties.end())
				return nullptr;

			return it->second.lock();
		}

		~Party()
		{
			s_parties.erase(m_id);
		}

		uint32_t ID() const
		{
			return m_id;
		}

		size_t MemberCount() const
		{
			return m_members.size();
		}

		const std::vector<Player*>& Members() const
		{
			return m_members;
		}

		bool HasMember(Player* player) const
		{
			for (Player* member : m_members)
			{
				if (member == player)
					return true;
			}
			return false;
		}

		void AddMember(Player* player)
		{
			if (player == nullptr || HasMember(player))
				return;

			m_members.push_back(player);
		}

		void AddInvitedPlayer(uint64_t guid)
		{
			if (guid != 0)
				m_invitedPlayers.insert(guid);
		}

		bool HasInvitedPlayer(uint64_t guid) const
		{
			return m_invitedPlayers.find(guid) != m_invitedPlayers.end();
		}

		void RemoveInvitedPlayer(uint64_t guid)
		{
			m_invitedPlayers.erase(guid);
		}

		void RemoveMember(Player* player)
		{
			for (size_t i = 0; i < m_members.size(); i++)
			{
				if (m_members[i] == player)
				{
					m_members.erase(m_members.begin() + i);
					return;
				}
			}
		}

		void SetSettings(const std::string& settings);

		// Settings change coming from a member. The Story Co-op keys can only be changed by the admin; a
		// non-admin keeps the current values. Returns false when the update had to be altered or ignored.
		bool SetSettingsFromMember(const std::string& settings, bool isAdmin);

		const std::string& Settings() const {
			return m_settings;
		}
		void SendToAll(const ByteStream& payload, Player* except = nullptr);
		void SendToScene(Scene* scene, const ByteStream& payload, Player* except = nullptr);

		bool PrivateDungeons() const {
			return m_privateDungeons;
		}

		bool StoryCoop() const {
			return m_storyCoop;
		}

		bool ShareConsumables() const {
			return m_shareConsumables;
		}

		// Story Co-op team save. Lives and dies with the Party; clients re-seed it by union (see STORY_PROGRESS).
		const StoryProgress& Story() const {
			return m_story;
		}

		bool StorySeeded() const {
			return m_storySeeded;
		}

		// Unions a member's snapshot into the team save. Returns true when the team save changed.
		bool MergeStory(const StoryProgress& incoming);

		void SetDungeonKeys(int mapIndex, uint8_t count) {
			if (mapIndex >= 0 && mapIndex < 19)
				m_dungeonKeys[mapIndex] = count;
		}

		uint8_t DungeonKeys(int mapIndex) const {
			if (mapIndex >= 0 && mapIndex < 19)
				return m_dungeonKeys[mapIndex];
			return 0;
		}

		// Story Co-op time travel. Members vote at the Master Sword pedestal; when every current member has
		// voted the server tells all of them to run the native travel sequence together.
		static const uint8_t ERA_UNKNOWN = 0xFF;

		void HandleVote(Player* player, bool vote);
		void OnMemberAdded(Player* player);
		void OnMemberRemoved(Player* player);
		void OnMemberAgeChanged(Player* player);
		void BroadcastVoteStatus();
		static void TickAll(std::chrono::steady_clock::time_point now);

		Player* GetAdmin() {
			if (m_members.size())
				return m_members[0];
			return nullptr;
		}

	private:
		explicit Party(uint32_t id) : m_id(id)
		{
		}

		void StorySettingsChanged(bool wasStoryCoop);
		void EvaluateVotes(bool forceBroadcast);
		void StartTravel();
		void CheckTravelDone(bool timedOut);
		void ResetVotes();
		uint8_t EffectiveEra() const;
		void SendVoteStatusTo(Player* player, bool active);
		bool HasVoted(uint16_t netID) const;

		uint32_t m_id;
		std::vector<Player*> m_members;
		std::unordered_set<uint64_t> m_invitedPlayers;
		std::string m_settings;
		bool m_privateDungeons = false;
		bool m_storyCoop = false;
		bool m_shareConsumables = false;
		uint8_t m_dungeonKeys[19]{};
		StoryProgress m_story;
		bool m_storySeeded = false;
		std::vector<uint16_t> m_votes;
		uint8_t m_voteEra = ERA_UNKNOWN;
		uint8_t m_era = ERA_UNKNOWN;
		bool m_travelling = false;
		uint8_t m_travelTarget = 0;
		std::chrono::steady_clock::time_point m_travelDeadline;

		inline static uint32_t s_nextID = 1;
		inline static std::unordered_map<uint32_t, std::weak_ptr<Party>> s_parties;
	};
}

#endif