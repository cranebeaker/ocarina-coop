#ifndef ZELDAONLINE_PARTY_HPP
#define ZELDAONLINE_PARTY_HPP

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ByteStream.hpp"

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

		void SetDungeonKeys(int mapIndex, uint8_t count) {
			if (mapIndex >= 0 && mapIndex < 19)
				m_dungeonKeys[mapIndex] = count;
		}

		uint8_t DungeonKeys(int mapIndex) const {
			if (mapIndex >= 0 && mapIndex < 19)
				return m_dungeonKeys[mapIndex];
			return 0;
		}

		Player* GetAdmin() {
			if (m_members.size())
				return m_members[0];
			return nullptr;
		}

	private:
		explicit Party(uint32_t id) : m_id(id)
		{
		}

		uint32_t m_id;
		std::vector<Player*> m_members;
		std::unordered_set<uint64_t> m_invitedPlayers;
		std::string m_settings;
		bool m_privateDungeons = false;
		bool m_storyCoop = false;
		bool m_shareConsumables = false;
		uint8_t m_dungeonKeys[19]{};

		inline static uint32_t s_nextID = 1;
		inline static std::unordered_map<uint32_t, std::weak_ptr<Party>> s_parties;
	};
}

#endif