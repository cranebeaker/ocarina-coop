#ifndef SCENEH
#define SCENEH

#include <unordered_map>
#include <memory>
#include <vector>
#include <set>
#include <cstdio>
#include <chrono>

#include "Player.hpp"
#include "Room.hpp"
#include "ActorListHelper.hpp"
#include "ActorRegistry.hpp"
#include "ByteStream.hpp"
#include "StoryProgress.hpp"
#include "Packet.hpp"
#include "PacketTypes.hpp"
#include "Flags.hpp"
#include "Guid.hpp"
#include "SceneID.hpp"

namespace ZeldaOnline
{
	class OOTServer;
	using namespace Scenes;
	static bool IsDungeonScene(int sceneNum) {
		return sceneNum == SCENE_DEKU_TREE || sceneNum == SCENE_DODONGOS_CAVERN || sceneNum == SCENE_JABU_JABU ||
			sceneNum == SCENE_FOREST_TEMPLE || sceneNum == SCENE_FIRE_TEMPLE || sceneNum == SCENE_WATER_TEMPLE ||
			sceneNum == SCENE_SPIRIT_TEMPLE || sceneNum == SCENE_SHADOW_TEMPLE || sceneNum == SCENE_BOTTOM_OF_THE_WELL ||
			sceneNum == SCENE_ICE_CAVERN || sceneNum == SCENE_THIEVES_HIDEOUT || sceneNum == SCENE_INSIDE_GANONS_CASTLE;
	}

	static bool IsBossScene(int sceneNum) {
		return sceneNum == SCENE_DEKU_TREE_BOSS || sceneNum == SCENE_DODONGOS_CAVERN_BOSS ||
			sceneNum == SCENE_JABU_JABU_BOSS || sceneNum == SCENE_FOREST_TEMPLE_BOSS ||
			sceneNum == SCENE_FIRE_TEMPLE_BOSS || sceneNum == SCENE_WATER_TEMPLE_BOSS ||
			sceneNum == SCENE_SPIRIT_TEMPLE_BOSS || sceneNum == SCENE_SHADOW_TEMPLE_BOSS ||
			sceneNum == SCENE_GANONDORF_BOSS || sceneNum == SCENE_GANON_BOSS;
	}

	static bool IsPartyScene(int sceneNum) {
		return IsDungeonScene(sceneNum) || IsBossScene(sceneNum);
	}



	static auto DUNGEON_RESET_TIMER = std::chrono::minutes(5);

	class Scene
	{

	protected:


		OOTServer* m_server = nullptr;
		ActorRegistry* m_registry = nullptr;

		uint64_t m_sceneKey = 0;

		int m_sceneNum = 0;

		int m_isFuture = 0;

		int m_otherVariant = 0;

		uint16_t m_mapIndex;

		std::unordered_map<int, Player*> m_players;
		std::unordered_map<int, bool> m_eventINFFlags;
		std::unordered_map<int, bool> m_INFFlags;
		std::unordered_map<int, std::unique_ptr<Room>> m_rooms;

		// Owns every actor in the scene, in PARENT-BEFORE-CHILD order. A child
		// spawn packet is useless to a client that has not seen its parent yet,
		// and sorting at send time only works one level deep.
		std::vector<std::unique_ptr<WorldActor>> m_actors;

	

		int m_dungeonKeyCount = 0;
		uint32_t m_sceneFlags = 0U;
		uint32_t m_clearFlags = 0U;
		uint32_t m_tempFlags = 0U;
		uint32_t m_lockedDoorsMask = 0U;
		uint64_t m_sessionID = NewGuid();
		std::chrono::steady_clock::time_point	m_lastPlayerLeftTime{};

		bool m_pendingDeletion = false;



		ByteStream MakePlayerSpawnPacket(Player* subject) const
		{
			ByteStream packet = newPacket(SERVER_PACKET_ACTOR_SPAWN);
			packet << PackedUInt4((unsigned int)(ClientSceneKey()));
			packet << PackedUInt1(PLAYER_PUPPET_ROOM);
			packet << PackedUInt2((unsigned int)(subject->NetworkID()));
			packet << PackedUInt2(0u);
			packet << PackedInt2(0);
			packet << PackedFloat4(subject->X()) << PackedFloat4(subject->Y()) << PackedFloat4(subject->Z());
			packet << PackedInt2(subject->RotX()) << PackedInt2(subject->RotY()) << PackedInt2(subject->RotZ());
			packet << PackedFloat4(subject->X()) << PackedFloat4(subject->Y()) << PackedFloat4(subject->Z());
			packet << PackedInt2(subject->RotX()) << PackedInt2(subject->RotY()) << PackedInt2(subject->RotZ());
			packet << PackedUInt2(0u);

			auto& appearance = subject->Appearance();
			auto properties = subject->FullProperties();
			packet.WriteVarUInt(appearance.Length());
			packet.Write(appearance);
			packet.Write(properties);
			return packet;
		}

		virtual WorldActor* MakeWorldActor(int networkID, Room* room, int actorID, int params,
			float x, float y, float z,
			short rotX, short rotY, short rotZ, float homeX, float homeY, float homeZ,
			short homeRotX, short homeRotY, short homeRotZ, int parentID)
		{
			return new WorldActor(networkID, this, room, actorID, params, x, y, z, rotX, rotY, rotZ, homeX, homeY, homeZ, homeRotX, homeRotY, homeRotZ, parentID);
		}

		virtual Room* MakeRoom(int roomIndex)
		{
			return new Room(this, m_registry, m_sceneKey, roomIndex);
		}



		ByteStream BuildEventINFFlagsPacket()
		{
			ByteStream retval;
			retval.WriteVarUInt((unsigned int)m_eventINFFlags.size());
			for (auto& flag : m_eventINFFlags)
				retval << PackedUInt1(5) << PackedUInt2(flag.first) << PackedUInt1(flag.second);
			return retval;
		}

		ByteStream BuildINFFlagsPacket()
		{
			ByteStream retval;
			retval.WriteVarUInt((unsigned int)m_INFFlags.size());
			for (auto& flag : m_INFFlags)
				retval << PackedUInt1(7) << PackedUInt2(flag.first) << PackedUInt1(flag.second);
			return retval;

		}

	public:
		Scene(OOTServer* server, ActorRegistry* registry,
			uint64_t sceneKey, int sceneNum, int isFuture, int otherVariant, uint16_t mapIndex) :
			m_server(server), m_registry(registry), m_sceneKey(sceneKey), m_sceneNum(sceneNum),
			m_isFuture(isFuture), m_otherVariant(otherVariant), m_mapIndex(mapIndex)
		{
			m_lastPlayerLeftTime = std::chrono::steady_clock::now();


		}


		
		void MarkForDeletion()
		{
			m_pendingDeletion = true;
		}

		bool PendingDeletion() const
		{
			return m_pendingDeletion;
		}


		uint32_t PartyID() const
		{
			return (uint32_t)(m_sceneKey >> 32);
		}

		uint64_t SceneKey() const {
			return m_sceneKey;
		}

		uint32_t ClientSceneKey() const {
			return (uint32_t)m_sceneKey;
		}

		OOTServer* Server() {
			return m_server;
		}

		const OOTServer* Server() const {
			return m_server;
		}

		int SceneNum() const {
			return m_sceneNum;
		}

		int IsFuture() const {
			return m_isFuture;
		}

		int OtherVariant() const {
			return m_otherVariant;
		}

		uint16_t MapIndex() const {
			return m_mapIndex;
		}

		void SetDungeonKeyCount(int count) {
			m_dungeonKeyCount = count;
		}

		int DungeonKeyCount() const {
			return m_dungeonKeyCount;
		}

		virtual bool IsDungeon() const {
			return IsDungeonScene(m_sceneNum);
		}

		virtual bool IsBoss() const {
			return IsBossScene(m_sceneNum);
		}

		void AppendLockedDoorMask(unsigned int flag) {
			if (flag < 32)
				m_lockedDoorsMask |= 1U << flag;
		}

		void SetEventINFFlag(int flag, bool set) {
			m_eventINFFlags[flag] = set;
		}

		void RemoveEventINFFlag(int flag) {
			m_eventINFFlags.erase(flag);
		}

		void SetINFFlag(int flag, bool set) {
			m_INFFlags[flag] = set;
		}

		void RemoveINFFlag(int flag) {
			m_INFFlags.erase(flag);
		}

		virtual bool CanSyncINFFlag(int flag, bool* persistant) const
		{
			return false;
		}

		virtual bool CanSyncEventINFFlag(int flag, bool* persistant) const
		{
			switch (flag)
			{
			case EVENTCHKINF_TALON_RETURNED_FROM_CASTLE:
			case EVENTCHKINF_TALON_WOKEN_IN_CASTLE:
			case EVENTCHKINF_DISPELLED_GANONS_TOWER_BARRIER:
			case EVENTCHKINF_COMPLETED_WATER_TRIAL:
			case EVENTCHKINF_COMPLETED_LIGHT_TRIAL:
			case EVENTCHKINF_COMPLETED_FIRE_TRIAL:
			case EVENTCHKINF_COMPLETED_SHADOW_TRIAL:
			case EVENTCHKINF_COMPLETED_SPIRIT_TRIAL:
			case EVENTCHKINF_COMPLETED_FOREST_TRIAL:


				*persistant = true;
				return true;
			}
			return false;
		}

		virtual bool CanSyncSceneFlag(int flag) const {
			return true;
		}

		virtual bool CanSyncTempFlag(int flag) const {
			return true;
		}

		virtual bool CanOverrideSpawnAuthorize(int actorID, int params) {
			return false;
		}


		virtual void OnFirstPlayerEnters();

		virtual void ResetScene() {
			printf("RESETTING THE SCENE\n");
			m_sessionID = NewGuid();
			m_sceneFlags = m_clearFlags = m_tempFlags = 0U;
			m_INFFlags.clear();
			m_eventINFFlags.clear();
			ResetActors();

			InitializeSceneFlags();
		}

		void ResetActors()
		{
			for (auto& entry : m_rooms)
				entry.second->ClearActors();
			m_rooms.clear();

			for (auto& owned : m_actors)
				m_registry->Remove(owned.get());
			m_actors.clear();
		}

		virtual void InitializeSceneFlags() {

		}

		// Story Co-op: puts the party's saved state of switches that cannot be merged as plain bits into a dungeon
		// instance that was just created. The game rebuilds the Water Temple water level and the Forest Temple elevator
		// from these switches when the scene loads, which is how a single player save restores them.
		void ApplyStoryState(const StoryProgress& story)
		{
			if (m_sceneNum == STORY_SCENE_WATER_TEMPLE)
			{
				const StoryLwwSlot* level = story.FindLww(STORY_LWW_WATER_LEVEL);
				if (level != nullptr && level->data.size() == 1 && level->data[0] >= 1 && level->data[0] <= 3)
				{
					const uint32_t levelBits = 0x70000000u; // switches 0x1C-0x1E
					m_sceneFlags = (m_sceneFlags & ~levelBits) | (1u << (STORY_WATER_LEVEL_FIRST_SWITCH - 1 + level->data[0]));
				}
			}
			else if (m_sceneNum == STORY_SCENE_FOREST_TEMPLE)
			{
				const StoryLwwSlot* elevator = story.FindLww(STORY_LWW_FOREST_ELEVATOR);
				if (elevator != nullptr && elevator->data.size() == 1)
				{
					const uint32_t bit = 1u << STORY_FOREST_ELEVATOR_SWITCH;
					m_sceneFlags = (m_sceneFlags & ~bit) | (elevator->data[0] != 0 ? bit : 0u);
				}
			}
		}

		virtual void OnAllPlayersLeave() {

		}

		void AddPlayer(Player* player, bool isRefresh);

		void RemovePlayer(Player* player, bool sceneRefresh = false)
		{

			{
				auto& created = player->CreatedRooms();
				for (auto it = created.begin(); it != created.end(); )
				{
					Room* room = *it;
					if (room->SceneKey() != m_sceneKey)
					{
						++it;
						continue;
					}
					it = created.erase(it);
					if (room->Creator() == player)
						room->SetCreatorRaw(nullptr);
				}
			}

			if (Room* room = player->CurrentRoom())
			{
				int roomIndex = room->RoomIndex();
				room->RemovePlayer(player);
				DestroyRoomIfEmpty(roomIndex);
			}

			{
				std::vector<WorldActor*> carried;
				for (auto& owned : m_actors)
					if (owned->OwningRoom() == nullptr && owned->Owner() == player)
						carried.push_back(owned.get());
				for (WorldActor* actor : carried)
					DestroyActor(m_registry, actor);
			}

			std::printf("[trace] scene %llu: remove p%d\n", m_sceneKey, player->NetworkID());
			m_players.erase(player->NetworkID());
			if (player->CurrentScene() == this)
				player->SetCurrentScene(nullptr);

			ByteStream packet = newPacket(SERVER_PACKET_DESTROY_ACTOR);
			packet << PackedUInt2((unsigned int)(player->NetworkID()));
			SendToAll(packet);

			if (m_players.empty() && !sceneRefresh)
			{
				m_lastPlayerLeftTime = std::chrono::steady_clock::now();

				// Nobody is left to lead these, and a non-dungeon scene resets
				// on the next entry anyway. Dungeons keep their actors until
				// DUNGEON_RESET_TIMER expires, which OnFirstPlayerEnters checks.
				if (!IsDungeon())
					ResetActors();

				OnAllPlayersLeave();
			}
		}

		void SetPlayerRoom(Player* player, int roomIndex)
		{
			if (player == nullptr)
				return;

			Room* current = player->CurrentRoom();
			if (current && current->RoomIndex() == roomIndex)
				return;

			Room* entered = GetOrCreateRoom(roomIndex);
			if (entered == nullptr)
				return;

			entered->AddPlayer(player);

		}

		void SendSceneScopedActors(Player* player)
		{
			for (auto& owned : m_actors)
			{
				WorldActor* actor = owned.get();
				if (actor->OwningRoom() != nullptr)
					continue;
				if (actor->Owner() == player)
					continue;
				player->SendPacket(MakeActorSpawnPacket(ClientSceneKey(), *actor));
			}
		}

		void DestroyRoomIfEmpty(int roomIndex)
		{
			auto it = m_rooms.find(roomIndex);
			if (it != m_rooms.end() && it->second->PlayerCount() == 0)
			{
				std::printf("[trace] scene %llu: room %d empty and leaderless, destroyed\n", m_sceneKey, roomIndex);
				it->second->ClearActors();
				m_rooms.erase(it);
			}
		}

		WorldActor* CreateActor(int networkID, Room* room, int actorID, int params,
			float x, float y, float z, short rotX, short rotY, short rotZ, float homeX, float homeY, float homeZ,
			short homeRotX, short homeRotY, short homeRotZ, int parentID)
		{
			auto owned = std::unique_ptr<WorldActor>(
				MakeWorldActor(networkID, room, actorID, params, x, y, z, rotX, rotY, rotZ, homeX, homeY, homeZ, homeRotX, homeRotY, homeRotZ, parentID));
			WorldActor* raw = owned.get();
			InsertActorOrdered(m_actors, std::move(owned));
			return raw;
		}

		WorldActor* GetActor(int networkID)
		{
			return FindActorOrdered(m_actors, networkID);
		}

		void EraseActor(int networkID)
		{
			EraseActorOrdered(m_actors, networkID);
		}

		void MakeActorRoomScoped(WorldActor* actor)
		{
			Room* room = GetOrCreateRoom(actor->RoomIndex());
			if (!room)
				return;

			room->AdoptActor(actor);

			ByteStream destroy = newPacket(SERVER_PACKET_DESTROY_ACTOR);
			destroy << PackedUInt2((unsigned int)(actor->NetworkID()));
			for (auto& entry : m_players)
			{
				Player* p = entry.second;
				if (p->CurrentRoom() == room)
					continue;
				p->SendPacket(destroy);
			}

			std::printf("[trace] actor %d went room-scoped (room %d)\n",
				actor->NetworkID(), room->RoomIndex());
		}
		void MakeActorSceneScoped(WorldActor* actor)
		{
			Room* old = actor->OwningRoom();
			if (old == nullptr && !actor->IsSceneScoped())
				old = GetRoom(actor->RoomIndex());
			if (old)
				old->ReleaseActor(actor->NetworkID());
			actor->SetOwningRoom(nullptr);

			if (actor->CreatorFlagClaimed())
			{
				ByteStream spawn = MakeActorSpawnPacket(ClientSceneKey(), *actor);
				for (auto& entry : m_players)
				{
					Player* p = entry.second;
					if (p->CurrentRoom() == old)
						continue;
					p->SendPacket(spawn);
				}
			}
			else
			{
				for (auto& entry : m_players)
				{
					Player* p = entry.second;
					if (p->CurrentRoom() == old)
						continue;
					p->SendPacket(MakeActorSpawnPacket(ClientSceneKey(), *actor));
				}
			}

			if (old)
				DestroyRoomIfEmpty(old->RoomIndex());
		}

		void DestroyActor(ActorRegistry* registry, WorldActor* actor)
		{
			int id = actor->NetworkID();

			ByteStream packet = newPacket(SERVER_PACKET_DESTROY_ACTOR);
			packet << PackedUInt2((unsigned int)(id));
			actor->Broadcast(packet);

			for (auto& owned : m_actors)
				if (owned->ParentID() == id)
					owned->ClearParent();

			if (Room* room = actor->OwningRoom())
			{
				room->ReleaseActor(id);
				registry->Remove(actor);
				EraseActor(id);
				DestroyRoomIfEmpty(room->RoomIndex());
				return;
			}

			registry->Remove(actor);
			EraseActor(id);
		}

		Room* GetOrCreateRoom(int roomIndex)
		{
			auto it = m_rooms.find(roomIndex);
			if (it != m_rooms.end())
				return it->second.get();

			auto room = std::unique_ptr<Room>(MakeRoom(roomIndex));
			Room* raw = room.get();
			m_rooms.emplace(roomIndex, std::move(room));
			return raw;
		}

		Room* GetRoom(int roomIndex)
		{
			auto it = m_rooms.find(roomIndex);
			return it != m_rooms.end() ? it->second.get() : nullptr;
		}

		bool SetSceneFlag(unsigned int flag)
		{
			if (flag >= 32)
				return false;

			bool wasSet = (m_sceneFlags & (1u << flag)) != 0;
			m_sceneFlags |= 1u << flag;
			return !wasSet;
		}

		bool UnsetSceneFlag(unsigned int flag)
		{
			if (flag >= 32)
				return false;

			bool wasSet = (m_sceneFlags & (1u << flag)) != 0;
			m_sceneFlags &= ~(1u << flag);
			return wasSet;

		}

		bool SetSceneClearFlag(unsigned int flag)
		{
			if (flag >= 32)
				return false;

			bool wasSet = (m_clearFlags & (1u << flag)) != 0;
			m_clearFlags |= 1u << flag;
			return !wasSet;
		}

		bool UnsetSceneClearFlag(unsigned int flag)
		{
			if (flag >= 32)
				return false;

			bool wasSet = (m_clearFlags & (1u << flag)) != 0;
			m_clearFlags &= ~(1u << flag);
			return wasSet;

		}

		void SetTempFlag(int flag, bool on)
		{
			if (flag < 0x20 || flag >= 0x38)
				return;

			unsigned int bit = 1u << (flag - 0x20);
			if (on)
				m_tempFlags |= bit;
			else
				m_tempFlags &= ~bit;
		}

		uint32_t TempFlags() const {
			return m_tempFlags;
		}


		void SetSceneFlags(unsigned int sceneFlags, unsigned int clearFlags) {
			m_sceneFlags = sceneFlags;
			m_clearFlags = clearFlags;
		}

		virtual void NightDayTransition(bool wasNight)
		{
			std::vector<Player*> present;
			for (auto& entry : m_players)
				present.push_back(entry.second);

			for (Player* player : present)
				RemovePlayer(player, true);

			for (auto& entry : m_rooms)
				entry.second->ClearActors();

			for (auto& owned : m_actors)
				m_registry->Remove(owned.get());
			m_actors.clear();
		}

		const std::unordered_map<int, std::unique_ptr<Room>>& Rooms() const {
			return m_rooms;
		}

		Player* GetPlayer(int networkID) const
		{
			auto it = m_players.find(networkID);
			return it != m_players.end() ? it->second : nullptr;
		}

		const std::unordered_map<int, Player*>& Players() const {
			return m_players;
		}

		int PlayerCount() const {
			return (int)(m_players.size());
		}

		void SendToAll(const ByteStream& payload, Player* except = nullptr)
		{
			for (auto& entry : m_players)
			{
				if (entry.second == except)
					continue;
				entry.second->SendPacket(payload);
			}
		}


		void AdoptOwnerlessActors(Player* player)
		{
			if (m_players.find(player->NetworkID()) == m_players.end())
				return;

			if (player->IsPaused())
				return;

			std::printf("[trace] AdoptOwnerlessActors(%i)\n", player->NetworkID());


			bool adoptedAny = false;
			for (const auto& actor : m_actors)
			{
				if (!actor->IsSceneScoped())
					continue;

				if (!actor->AwaitingLeader() && actor->Owner() != nullptr)
					continue;

				if (actor->HasDeclinedAssignment(player->Guid()))
					continue;

				actor->ReassignOwner(player);
				adoptedAny = true;
			}
		}


		WorldActor* FindActorByLocalId(Player* owner, unsigned int localId) {
			for (auto& actor : m_actors)
				if (actor->Owner() == owner && actor->LocalID() == localId)
					return actor.get();
			return nullptr;
		}

		Player* ClosestOtherEligiblePlayer(Player* except, WorldActor* target) const
		{
			Player* best = nullptr;
			float bestDistSq = 0.0f;

			for (auto& entry : m_players)
			{
				Player* candidate = entry.second;
				if (candidate == except || candidate->IsPaused())
					continue;
				if (!target->HasDeclinedAssignment(candidate->Guid()))
				{
					float dx = candidate->X() - target->X();
					float dy = candidate->Y() - target->Y();
					float dz = candidate->Z() - target->Z();
					float distSq = dx * dx + dy * dy + dz * dz;
					if (best == nullptr || distSq < bestDistSq)
					{
						best = candidate;
						bestDistSq = distSq;
					}
				}
			}
			return best;
		}
	};
}

#endif