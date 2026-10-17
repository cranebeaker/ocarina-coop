#include <cstdio>
#include <ctime>
#include <vector>


#include "OOTServer.hpp"
#include "nlohmann/json.hpp"
#include "Packet.hpp"
#include "PacketTypes.hpp"
#include "ActorID.hpp"
#include "PacketStrings.hpp"
#include "StoryProgress.hpp"


namespace ZeldaOnline
{
	static const int POLL_TIMEOUT_MICROS = 50 * 1000;
	static const int IDLE_TIMEOUT_SECONDS = 10;
	static const int HANDSHAKE_TIMEOUT_SECONDS = 10;

	//Must be 10 characters
	static const std::string ALLOWED_VERSION = "BETA000003";

#ifndef OOT_MAX_POLL_SOCKETS
#define OOT_MAX_POLL_SOCKETS FD_SETSIZE
#endif

	static const int MIN_BATCH_TIMEOUT_MICROS = 1000;



	static ByteStream MakeStoryStatePacket(const StoryProgress& story, uint8_t flags)
	{
		ByteStream packet = newPacket(SERVER_PACKET_STORY_STATE);
		packet << PackedUInt1(flags);
		story.Serialize(packet);
		return packet;
	}

	static std::string ReadFile(const char* path) {
		FILE* f = fopen(path, "rb");
		if (f == nullptr)
			return "";

		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);

		if (size <= 0)
		{
			fclose(f);
			return "";
		}

		std::string text;
		text.resize((size_t)(size));

		size_t read = fread(&text[0], 1, (size_t)(size), f);
		text.resize(read);
		fclose(f);

		return text;
	}

	OOTServer::OOTServer(int listenPort)
		: m_port(listenPort), m_listenSocket(INVALID_SOCKET), m_running(false)
	{
		
	}

	OOTServer::~OOTServer()
	{
		m_newConnections.clear();
		m_players.clear();
		if (m_listenSocket != INVALID_SOCKET)
			soxCloseSocket(m_listenSocket);
		soxCleanup();
	}
	
	void OOTServer::LoadConfig()
	{
		try
		{
			std::string configText = ReadFile("config.json");
			if (configText.empty())
			{
				std::printf("OOTServer: config.json missing or empty, using defaults\n");
				return;
			}

			auto cfg = nlohmann::json::parse(configText, nullptr, true, true);

			if (!cfg.is_object())
			{
				std::printf("OOTServer: config.json is not an object, using defaults\n");
				return;
			}

			m_daySpeed = (unsigned int)cfg.value("daySpeed", 320);
			m_nightSpeed = (unsigned int)cfg.value("nightSpeed", 640);
			m_dungeonsReset = cfg.value("dungeonsReset", true);
			m_dungeonsResetTimer = cfg.value("dungeonsResetTimer", 10);
			m_dataDir = cfg.value("dataDir", std::string("data"));

			if (cfg.contains("skins") && cfg["skins"].is_array())
			{
				for (const auto& entry : cfg["skins"]) {
					if (!entry.is_object())
						continue;

					PlayerSkin skin;
					skin.displayName = entry.value("displayName", std::string());
					skin.referenceName = entry.value("referenceName", std::string());
					skin.pitch = entry.value("pitch", 1.0f);

					if (skin.referenceName.empty())
						continue;

					m_skins.push_back(skin);
				}
			}

		}
		catch (const std::exception& e)
		{
			std::printf("OOTServer: failed to read config.json (%s), using defaults\n", e.what());
			m_skins.clear();
		}
	}

	int OOTServer::run()
	{
		soxInitialize();
		LoadConfig();
		Party::SetStoryDataDir(m_dataDir);

		std::printf("Day Speed: %u (units per second)\n", m_daySpeed);
		std::printf("Night Speed: %u (units per second)\n", m_nightSpeed);
		std::printf("Dungeons Reset: %s\n", m_dungeonsReset ? "true" : "false");
		std::printf("Dungeons Reset Timer: %i (minutes)\n", m_dungeonsResetTimer);
		std::printf("Using port %d (override with --port <number>)\n", m_port);

		m_listenSocket = soxCreateTcpSocket();
		if (m_listenSocket == INVALID_SOCKET)
		{
			std::fprintf(stderr, "OOTServer: failed to create listen socket (error %d)\n", soxLastError());
			return 1;
		}

		if (!soxTcpListen(m_listenSocket, m_port))
		{
			std::fprintf(stderr, "OOTServer: failed to listen on port %d (error %d)\n", m_port, soxLastError());
			soxCloseSocket(m_listenSocket);
			m_listenSocket = INVALID_SOCKET;
			return 1;
		}

		soxUnblockSocket(m_listenSocket);

		std::printf("OOTServer: listening on port %d\n", m_port);

		m_running = true;
		while (m_running)
		{
			tickWorldTime();
			Party::TickAll(std::chrono::steady_clock::now());
			acceptPendingConnections();
			pollConnections();
			reapConnections();
		}

		Party::SaveAllStory();
		return 0;
	}

	Player* OOTServer::AddPlayer()
	{
		int id = m_actors.NewID();
		if (id < 0)
			return nullptr;

		auto result = m_players.emplace(id, std::unique_ptr<Player>(new Player(id)));
		m_actors.Add(result.first->second.get());
		return result.first->second.get();
	}

	void OOTServer::RemovePlayer(Player* player)
	{
		player->MarkForRemoval();
		if (ClientConnection* connection = player->GetConnection())
			connection->Disconnect();
	}

	Player* OOTServer::GetPlayer(int id) const
	{
		auto it = m_players.find(id);
		return it != m_players.end() ? it->second.get() : nullptr;
	}

	Scene* OOTServer::GetOrCreateScene(int sceneNum, int isFuture, int otherVariant, uint16_t mapIndex, unsigned int partyHash)
	{
		static const int MAX_SCENE_NUM = 0x6D;

		if (sceneNum < 0 || sceneNum > Scenes::SCENE_TESTROOM)
		{
			std::printf("OOTServer: rejecting out-of-range scene %d (variant %d)\n", sceneNum, otherVariant);
			return nullptr;
		}

		uint64_t key = MakeSceneKey(sceneNum, isFuture, otherVariant);
		if (partyHash != 0 && IsPartyScene(sceneNum))
			key = MakeServerSceneKey((uint32_t)key, partyHash);

		auto it = m_scenes.find(key);
		if (it != m_scenes.end())
			return it->second.get();

		auto scene = CreateScene(sceneNum, key, isFuture, otherVariant, mapIndex);
		Scene* raw = scene.get();
		m_scenes.emplace(key, std::move(scene));
		return raw;
	}

	Scene* OOTServer::GetScene(uint64_t sceneKey)
	{
		auto it = m_scenes.find(sceneKey);
		return it != m_scenes.end() ? it->second.get() : nullptr;
	}

	void OOTServer::RemovePlayerByGUID(uint64_t guid)
	{
		for (auto& entry : m_players)
		{
			Player* player = entry.second.get();
			ClientConnection* connection = player->GetConnection();

			if (connection != nullptr && connection->ClientGUID() == guid)
			{
				connection->Disconnect();
				player->MarkForRemoval();
				break;
			}
		}

	}

	void OOTServer::acceptPendingConnections()
	{
		for (;;)
		{
			SoxHandle clientSocket = soxTcpAccept(m_listenSocket);
			if (clientSocket == INVALID_SOCKET)
				break;

			m_newConnections.emplace_back(new ClientConnection(clientSocket));
			onClientConnected(m_newConnections.back().get());
		}
	}

	void OOTServer::pollConnections()
	{
		struct PollEntry
		{
			ClientConnection* connection;
			Player* player;
		};

		std::vector<PollEntry> pending;
		pending.reserve(m_newConnections.size() + m_players.size());
		for (auto& c : m_newConnections)
		{
			if (c->Connected())
				pending.push_back({ c.get(), nullptr });
		}
		for (auto& entry : m_players)
		{
			Player* player = entry.second.get();
			ClientConnection* connection = player->GetConnection();
			if (connection && connection->Connected())
				pending.push_back({ connection, player });
		}

		const std::size_t maxPerBatch = OOT_MAX_POLL_SOCKETS - 1;
		const std::size_t numBatches = pending.empty()
			? 1
			: (pending.size() + maxPerBatch - 1) / maxPerBatch;

		int batchTimeout = POLL_TIMEOUT_MICROS / (int)(numBatches);
		if (batchTimeout < MIN_BATCH_TIMEOUT_MICROS)
			batchTimeout = MIN_BATCH_TIMEOUT_MICROS;

		std::vector<ClientConnection*> promote;

		for (std::size_t batch = 0; batch < numBatches; ++batch)
		{
			const std::size_t begin = batch * maxPerBatch;
			std::size_t end = begin + maxPerBatch;
			if (end > pending.size())
				end = pending.size();

			sox::SoxPoller poller;

			if (batch == 0)
				poller.AddSocket(m_listenSocket, POLL_READ);

			for (std::size_t i = begin; i < end; ++i)
			{
				ClientConnection* c = pending[i].connection;
				int options = POLL_READ | POLL_ERROR;
				if (c->HasPendingSend())
					options |= POLL_WRITE;
				poller.AddSocket(c->Socket(), options);
			}

			if (poller.Poll(batchTimeout) <= 0)
				continue;

			for (std::size_t i = begin; i < end; ++i)
			{
				ClientConnection* c = pending[i].connection;
				if (!c->Connected())
					continue;

				if (poller.IsSet(c->Socket(), POLL_ERROR))
				{
					c->Disconnect();
					continue;
				}

				if (poller.IsSet(c->Socket(), POLL_READ) && c->ReceiveData() > 0)
				{
					if (Player* player = pending[i].player)
					{
						servicePlayer(player);
					}
					else if (serviceNewConnection(c))
					{
						promote.push_back(c);
					}
				}

				if (c->Connected() && poller.IsSet(c->Socket(), POLL_WRITE))
					c->FlushSendBuffer();
			}
		}

		for (ClientConnection* c : promote)
		{
			for (auto it = m_newConnections.begin(); it != m_newConnections.end(); ++it)
			{
				if (it->get() != c)
					continue;

				int id = m_actors.NewID();
				if (id < 0)
				{
					std::printf("OOTServer: server full, rejecting connection\n");
					c->Disconnect();
					break;
				}

				auto result = m_players.emplace(id,
					std::unique_ptr<Player>(new Player(id, std::move(*it))));
				m_actors.Add(result.first->second.get());
				m_newConnections.erase(it);

				Player* player = result.first->second.get();
				onPlayerAuthenticated(player);
				servicePlayer(player);
				break;
			}
		}
	}

	bool OOTServer::serviceNewConnection(ClientConnection* connection)
	{
		ByteStream packet;
		if (!connection->NextPacket(packet))
			return false;

		std::string error = "";
		if (!onHandshake(connection, packet, &error))
		{
			connection->SendPacket(newPacket(SERVER_PACKET_DISCONNECT) << error);
			connection->FlushSendBuffer();
			connection->Disconnect();
			return false;
		}
		return true;
	}

	void OOTServer::servicePlayer(Player* player)
	{
		ClientConnection* connection = player->GetConnection();
		if (!connection)
			return;

		ByteStream packet;
		while (connection->Connected() && !player->PendingRemoval()
			&& connection->NextPacket(packet))
		{
			try
			{
				onIncomingData(player, packet);
			}
			catch (const std::exception& e)
			{
				// a malformed packet must never kill the server thread
				std::printf("OOTServer: dropped malformed packet from player %d (%s)\n", player->NetworkID(), e.what());
			}
		}
	}

	void OOTServer::reapConnections()
	{
		time_t now = time(nullptr);

		for (auto& c : m_newConnections)
		{
			if (!c->Connected())
				continue;

			if (now - c->ConnectedAt() > HANDSHAKE_TIMEOUT_SECONDS)
			{
				std::printf("OOTServer: dropping connection (handshake timeout)\n");
				c->Disconnect();
			}
			else if (c->SecondsSinceLastReceived() > IDLE_TIMEOUT_SECONDS)
			{
				std::printf("OOTServer: dropping connection (idle %llds before handshake)\n",
					(long long)(c->SecondsSinceLastReceived()));
				c->Disconnect();
			}
		}

		for (auto it = m_newConnections.begin(); it != m_newConnections.end(); )
		{
			if (!(*it)->Connected())
			{
				onClientDisconnected(it->get());
				it = m_newConnections.erase(it);
			}
			else
			{
				++it;
			}
		}

		for (auto it = m_players.begin(); it != m_players.end(); )
		{
			Player* player = it->second.get();
			ClientConnection* connection = player->GetConnection();

			if (connection != nullptr && connection->Connected() && !player->PendingRemoval() &&
				connection->SecondsSinceLastReceived() > IDLE_TIMEOUT_SECONDS)
			{
				std::printf("OOTServer: dropping player %d (idle %llds)\n", player->NetworkID(),
					(long long)(connection->SecondsSinceLastReceived()));
				connection->Disconnect();
			}

			if (player->PendingRemoval() || (connection && !connection->Connected()))
			{
				onPlayerDisconnected(player);
				m_actors.Remove(player);
				it = m_players.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	bool OOTServer::CanSpawnActor(Player* player, int spawnerNetID, int actorID, int parentID, int params)
	{
		using namespace ActorID;

		//0 means either the player spawned it or a non-networked actor spawned it
		if (spawnerNetID == 0)
		{
			switch (actorID)
			{
				case ACTOR_EN_BOM:
				case ACTOR_EN_ARROW:
				case ACTOR_EN_DOG:
				case ACTOR_EN_BOMBF:
				case ACTOR_EN_BOM_CHU:
				case ACTOR_EN_SKB:
				case ACTOR_EN_ISHI:
				case ACTOR_EN_KUSA:
				case ACTOR_EN_NIW:
					return true;
				default:
					return false;
			}
		}
		
		//You can do a simple check here to see if the spawnerNetID exists and it matches the actorID, return true (or have a more sophisicated per-actor check of what they are allowed to spawn)
		return true;
	}

	void OOTServer::SendToAll(const ByteStream& payload, Player* except)
	{
		for (auto& entry : m_players)
		{
			if (entry.second.get() == except)
				continue;
			entry.second->SendPacket(payload);
		}
	}

	void OOTServer::WritePlayerEntry(ByteStream& packet, Player* player)
	{
		std::string name = player->NickName();

		packet << PackedUInt2((unsigned int)(player->NetworkID()));
		packet << PackedUInt2((unsigned int)(name.length()));
		packet << name;
		packet << PackedInt2(player->CurrentScene() ? player->CurrentScene()->SceneNum() : -1);
		packet << PackedUInt1(player->CurrentRoom() ? player->CurrentRoom()->RoomIndex() : 0);
		packet << PackedUInt1(player->Age());
	}

	void OOTServer::SendPlayerListTo(Player* target)
	{
		static const unsigned int MAX_PAYLOAD = 0xF000;

		std::vector<Player*> others;
		others.reserve(m_players.size());
		for (auto& entry : m_players)
		{
			if (entry.second.get() != target)
				others.push_back(entry.second.get());
		}

		std::size_t sent = 0;
		while (sent < others.size())
		{
			ByteStream body;
			std::size_t chunk = 0;

			while (sent + chunk < others.size())
			{
				ByteStream entryData;
				WritePlayerEntry(entryData, others[sent + chunk]);

				if (chunk != 0 && body.Length() + entryData.Length() > MAX_PAYLOAD)
					break;

				body.Write(entryData);
				chunk++;
			}

			ByteStream packet = newPacket(SERVER_PACKET_PLAYER_LIST_APPEND);
			packet << PackedInt4((int)(chunk));
			packet.Write(body);
			target->SendPacket(packet);

			sent += chunk;
		}
	}

	void OOTServer::BroadcastPlayerJoined(Player* player)
	{
		ByteStream packet = newPacket(SERVER_PACKET_PLAYER_LIST_APPEND);
		packet << PackedInt4(1);
		WritePlayerEntry(packet, player);
		SendToAll(packet, player);
	}

	void OOTServer::BroadcastPlayerLeft(Player* player)
	{
		ByteStream packet = newPacket(SERVER_PACKET_PLAYER_LIST_REMOVE);
		packet << PackedUInt2((unsigned int)(player->NetworkID()));
		SendToAll(packet, player);
	}

	void OOTServer::BroadcastPlayerListEntryUpdate(Player* player)
	{
		ByteStream packet = newPacket(SERVER_PACKET_PLAYER_LIST_UPDATE);
		WritePlayerEntry(packet, player);
		SendToAll(packet, player);
	}

	void OOTServer::ClearPartyScenes(uint32_t partyID)
	{
		if (partyID == 0)
			return;

		std::vector<Scene*> marked;

		for (auto& entry : m_scenes)
		{
			Scene* scene = entry.second.get();

			if (scene->PartyID() == partyID)
			{
				scene->MarkForDeletion();
				marked.push_back(scene);
			}
		}

		for (Scene* scene : marked)
			ClearPartyScene(scene);
	}


	void OOTServer::ClearPartyScene(Scene* scene)
	{
		if (scene == nullptr || !scene->PendingDeletion() || scene->PlayerCount() != 0)
			return;

		auto it = m_scenes.find(scene->SceneKey());
		if (it != m_scenes.end())
		{
			printf("PARTY SCENE CLEARED\n");
			scene->ResetActors();
			m_scenes.erase(it);
		}
	}


	void OOTServer::onClientConnected(ClientConnection* connection)
	{
		char ip[64] = "?";
		struct sockaddr_in6 addr;
		if (soxTcpIp(connection->Socket(), &addr))
			soxIpString((const struct sockaddr*)&addr, ip, sizeof(ip));
		std::printf("OOTServer: connection from %s (awaiting handshake)\n", ip);
	}

	bool OOTServer::onHandshake(ClientConnection* connection, ByteStream& packet, std::string* error)
	{
		 
		if (packet.BytesLeft() < 1)
			return false;

		if (packet.Read<PackedUInt1>().value() != CLIENT_PACKET_AUTH)
		{
			*error = "Invalid authentication packet";
			return false;
		}

		if (packet.BytesLeft() < ALLOWED_VERSION.length())
		{
			*error = "Invalid client version. This server requires " + ALLOWED_VERSION;
			return false;
		}

		auto version = packet.ReadString((unsigned int)ALLOWED_VERSION.length());

		if (version != ALLOWED_VERSION)
		{
			*error = "Invalid client version. This server requires " + ALLOWED_VERSION;
			return false;
		}

		if (packet.BytesLeft() < 9)
		{
			*error = "Malformed authentication packet";
			return false;
		}

		bool wasReconnect = packet.Read<PackedUInt1>().value() != 0;
		uint64_t clientGUID = packet.Read<PackedUInt8>().value();

		connection->SetClientGUID(clientGUID);

		if (wasReconnect && clientGUID != 0)
			RemovePlayerByGUID(clientGUID);

		return true;
	}


	static bool IsDayNightReloadScene(int sceneNum, int sceneVariant) {
		switch (sceneNum) {
		case 0x1B: // Market entrance (day)
		case 0x1C: // Market entrance (night)
		case 0x20: // Market (day)
		case 0x21: // Market (night)
		case 0x22: // Back alley (day)
		case 0x23: // Back alley (night)
		case 0x52: // Kakariko Village
		case 0x53: // Graveyard
			return true;

		case 0x63: // Lon Lon Ranch - normal (no race, no trapped inside the ranch)
			if (sceneVariant == 0)
				return true;
			break;

		default:
			return false;
		}
		return false;
	}


	void OOTServer::tickWorldTime()
	{
		auto now = std::chrono::steady_clock::now();
		auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTimeTick).count();
		if (elapsedMs < 1000)
			return;
		m_lastTimeTick = now;

		bool wasNight = IsNightTime(m_worldTime);
		unsigned int rate = wasNight ? m_nightSpeed
			: m_daySpeed;

		unsigned int newWorldTime = (m_worldTime + (rate * (unsigned int)(elapsedMs)) / 1000) & 0xFFFF;
		bool crossed = IsNightTime(newWorldTime) != wasNight;

		if (crossed)
		{
			std::printf("OOTServer: day/night boundary crossed (time=%#x, now %s)\n",
				newWorldTime, IsNightTime(newWorldTime) ? "night" : "day");
			for (auto& entry : m_scenes)
			{
				if (IsDayNightReloadScene(entry.second.get()->SceneNum(), entry.second.get()->OtherVariant()))
					entry.second.get()->NightDayTransition(wasNight);
			}
		}

		m_worldTime = newWorldTime;

		ByteStream packet = newPacket(SERVER_PACKET_WORLD_TIME);
		packet << PackedUInt2(m_worldTime);
		packet << PackedUInt1(crossed ? 1u : 0u);
		packet << PackedUInt2(rate);
		for (auto& entry : m_players)
			entry.second->SendPacket(packet);
	}

	void OOTServer::onPlayerAuthenticated(Player* player)
	{
		std::printf("OOTServer: player %d authenticated\n", player->NetworkID());

		ByteStream packet = newPacket(SERVER_PACKET_AUTH_RESPONSE);
		packet << PackedUInt2((unsigned int)(player->NetworkID()));
		player->SendPacket(packet);

		ByteStream timePacket = newPacket(SERVER_PACKET_WORLD_TIME);
		timePacket << PackedUInt2(m_worldTime);
		timePacket << PackedUInt1(0u);
		timePacket << PackedUInt2(IsNightTime(m_worldTime) ? m_nightSpeed
			: m_daySpeed);
		player->SendPacket(timePacket);

		if (m_skins.size())
		{
			ByteStream skinPacket = newPacket(SERVER_PACKET_POPULATE_SKINS) << PackedUInt2((unsigned int)(m_skins.size() & 0xFFFF));
			for (auto& skin : m_skins)
			{
				skinPacket << PackedUInt2((uint16_t)skin.displayName.length()) << skin.displayName;
				skinPacket << PackedUInt2((uint16_t)skin.referenceName.length()) << skin.referenceName;
				skinPacket << PackedFloat4(skin.pitch);
			}
			player->SendPacket(skinPacket);
		}

	}

	static bool IsClusterDedupActor(unsigned short actorId) {
		return actorId == 334 || actorId == 293;
	}

	void OOTServer::onIncomingData(Player* player, ByteStream& data)
	{
		auto clientPacketID = data.Read<PackedUInt1>().value();

		switch (clientPacketID)
		{
		case CLIENT_PACKET_UPDATE_PLAYER:
		{

			ByteStream props = data.Read(data.BytesLeft());
			if (props.Length() == 0)
				break;

			player->MergeCustomState(props);

			Scene* scene = player->CurrentScene();
			if (!scene)
				break;

			ByteStream packet = newPacket(SERVER_PACKET_ACTOR_PROPERTIES);
			packet << PackedUInt2((unsigned int)(player->NetworkID()));
			packet.Write(props);
			scene->SendToAll(packet, player);
		}
		break;

		case CLIENT_PACKET_SET_ROOM_SCENE:
		{
			int sceneNum = (int)(data.Read<PackedUInt2>().value());
			int isFuture = (int)(data.Read<PackedUInt1>().value());
			int otherVariant = (int)(data.Read<PackedUInt2>().value());
			int roomIndex = (int)(data.Read<PackedUInt1>().value());
			int entranceID = data.Read<PackedInt4>().value();
			uint16_t mapIndex = data.Read<PackedUInt2>().value();
			bool roomTransition = data.Read<PackedUInt1>().value() == 1;

			uint32_t sceneFlags = 0U;
			uint32_t clearFlags = 0U;

			
			if (!roomTransition) {
				sceneFlags = data.Read<PackedUInt4>().value();
				clearFlags = data.Read<PackedUInt4>().value();
				player->MergeCustomState(data.Read(data.BytesLeft()));
			}

			player->SetEntranceID(entranceID);

			std::printf("[trace] p%d SET_ROOM_SCENE scene=%d room=%d roomTransition=%d otherVariant=%d (was: scene=%s room=%d)\n",
				player->NetworkID(), sceneNum, roomIndex, roomTransition, otherVariant,
				player->CurrentScene() ? "set" : "none",
				player->CurrentRoom() ? player->CurrentRoom()->RoomIndex() : -1);

			auto oldScene = player->CurrentScene();
			Scene* scene = GetOrCreateScene(sceneNum, isFuture, otherVariant, mapIndex, player->PartyHash());
			if (scene == nullptr)
				break;

			bool sameRoomReentry = player->CurrentScene() == scene &&
				player->CurrentRoom() && player->CurrentRoom()->RoomIndex() == roomIndex;
			bool reentry = oldScene == scene && (!roomTransition);

			bool wasRoomLeader = false;
			if (reentry)
			{
				Room* oldRoom = player->CurrentRoom();
				wasRoomLeader = oldRoom && oldRoom->RoomIndex() == roomIndex &&
					oldRoom->Leader() == player;
			}

			if (!roomTransition)
			{
				std::printf("OOTServer: player %d reloaded scene %d (roomTransition=%d sameRoom=%d wasLeader=%d) -- full re-entry\n",
					player->NetworkID(), sceneNum, roomTransition, (int)sameRoomReentry, (int)wasRoomLeader);

				bool isRefresh = scene == player->CurrentScene();
				if (player->CurrentScene())
				{
					player->CurrentScene()->RemovePlayer(player, isRefresh);

					if (!isRefresh)
						ClearPartyScene(oldScene);
				}

				scene->AddPlayer(player, isRefresh);
			}

			scene->SetPlayerRoom(player, roomIndex);
			BroadcastPlayerListEntryUpdate(player);

		}
		break;

		case CLIENT_PACKET_REQUEST_ACTOR_SPAWN:
		case CLIENT_PACKET_REQUEST_ACTOR_SPAWN_AS_CHILD:
		{
			if (data.BytesLeft() < 5)
				break;

			uint32_t sceneKey = (int)(data.Read<PackedUInt4>().value());
			int roomIndex = (int)(data.Read<PackedInt1>().value());

			Scene* scene = player->CurrentScene();
			if (!scene || scene->ClientSceneKey() != sceneKey)
				break;

			bool roomExisted = scene->GetRoom(roomIndex) != nullptr;
			Room* room = scene->GetOrCreateRoom(roomIndex);

			if (!roomExisted && room->PlayerCount() == 0)
			{
				room->SetCreator(player);
				std::printf("[trace] p%d created room %d via spawn prefetch (no leader yet)\n",
					player->NetworkID(), roomIndex);
			}

			AbstractActor* parent = nullptr;

			int parentID = 0;
			if (clientPacketID == CLIENT_PACKET_REQUEST_ACTOR_SPAWN_AS_CHILD)
			{
				parentID = (int)(data.Read<PackedUInt2>().value());
				parentID = room->GetWorldActor(parentID) != nullptr ? parentID : 0;

			}
			int actorID = (int)(data.Read<PackedUInt2>().value());
			int params = data.Read<PackedInt2>().value();
			float x = data.Read<PackedFloat4>().value();
			float y = data.Read<PackedFloat4>().value();
			float z = data.Read<PackedFloat4>().value();
			short rotX = (short)(data.Read<PackedInt2>().value());
			short rotY = (short)(data.Read<PackedInt2>().value());
			short rotZ = (short)(data.Read<PackedInt2>().value());

			int spawnerNetID = (int)(data.Read<PackedUInt2>().value());

			bool requesterOwns = false;
			if (data.BytesLeft() >= 1)
				requesterOwns = data.Read<PackedUInt1>().value() == 1;

			bool authorized;
			if (requesterOwns)
				authorized = true;
			else if (spawnerNetID == 0)

				authorized = room->Creator() == player || scene->CanOverrideSpawnAuthorize(actorID, params);
			else
			{
				WorldActor* spawner = room->GetWorldActor(spawnerNetID);
				authorized = spawner != nullptr && spawner->Owner() == player;
			}

			std::printf("[trace] p%d SPAWN_REQ actorID=%i room=%d spawner=%d own=%d -> %s\n",
				player->NetworkID(), actorID, roomIndex , spawnerNetID, requesterOwns ? 1 : 0,
				authorized ? "ACCEPT" : "REJECT");
			if (!authorized)
			{
				std::printf("[trace] spawn REJECTED p%d actor %d (room %d creator is p%d, leader p%d)\n",
					player->NetworkID(), actorID, roomIndex,
					room->Creator() ? room->Creator()->NetworkID() : 0,
					room->Leader() ? room->Leader()->NetworkID() : 0);
				break;
			}

			WorldActor* spawned = room->SpawnActor(actorID, params, x, y, z, rotX, rotY, rotZ, x, y, z, rotX, rotY, rotZ, parentID);
			if (spawned != nullptr && requesterOwns && spawned->Owner() != player)
			{
				spawned->ReassignOwner(player);
			}
		}
		break;

		case CLIENT_PACKET_ACTOR_SPAWN:
		case CLIENT_PACKET_ACTOR_SPAWN_AS_CHILD:
		{
			if (data.BytesLeft() < 5)
				break;

			uint32_t sceneKey = data.Read<PackedUInt4>().value();
			int roomIndex = (int)(data.Read<PackedInt1>().value());

			Scene* scene = player->CurrentScene();
			if (!scene || scene->ClientSceneKey() != sceneKey)
				break;
			

			bool roomExisted = scene->GetRoom(roomIndex) != nullptr;
			Room* room = scene->GetOrCreateRoom(roomIndex);
			if (!roomExisted && room->PlayerCount() == 0)
			{
				room->SetCreator(player);
				std::printf("[trace] p%d created room %d via spawn (no leader yet)\n",
					player->NetworkID(), roomIndex);
			}

			int parentID = 0;
			if (clientPacketID == CLIENT_PACKET_ACTOR_SPAWN_AS_CHILD)
			{
				parentID = (int)(data.Read<PackedUInt2>().value());
				parentID = room->GetWorldActor(parentID) != nullptr ? parentID : 0;

				
				auto parentLocalID = data.Read<PackedUInt4>().value();
				printf("PARENT ID: %i:%u\n", parentID, parentLocalID);
				if (parentID == 0 && parentLocalID) {
					auto parent = scene->FindActorByLocalId(player, parentLocalID);

					if (parent != nullptr)
						parentID = parent->NetworkID();
					printf("PARENT: %p:%i\n", parent, parentID);
				}
			}

			int actorID = (int)(data.Read<PackedUInt2>().value());
			unsigned int localID = data.Read<PackedUInt4>().value();
			int params = data.Read<PackedInt2>().value();
			float x = data.Read<PackedFloat4>().value();
			float y = data.Read<PackedFloat4>().value();
			float z = data.Read<PackedFloat4>().value();
			short rotX = (short)(data.Read<PackedInt2>().value());
			short rotY = (short)(data.Read<PackedInt2>().value());
			short rotZ = (short)(data.Read<PackedInt2>().value());
			int spawnerNetID = (int)(data.Read<PackedUInt2>().value());
			 
			if (IsClusterDedupActor(actorID))
			{
				if (WorldActor* existing = room->FindActor(actorID, params, x, z))
				{
					printf("DEDUP ABORTED\n");
					ByteStream reply = newPacket(SERVER_PACKET_ASSIGN_ACTOR_ID);
					reply << PackedUInt4(sceneKey);
					reply << PackedUInt1(roomIndex);
					reply << PackedUInt4(localID);
					reply << PackedUInt2(0U);
					player->SendPacket(reply);
					break;
				}

			}

			WorldActor* spawned = CanSpawnActor(player, spawnerNetID, actorID, parentID, params) ? room->SpawnActor(actorID, params, x, y, z,
				rotX, rotY, rotZ, x, y, z, rotX, rotY, rotZ, parentID,
				player, player) : nullptr;

			if (spawned == nullptr)
			{
				std::printf("[trace] p%d SPAWN localID=%u room=%d -> no netID (dedup/cap)\n",
					player->NetworkID(), localID, roomIndex);
				ByteStream reply = newPacket(SERVER_PACKET_ASSIGN_ACTOR_ID);
				reply << PackedUInt4(sceneKey);
				reply << PackedUInt1(roomIndex);
				reply << PackedUInt4(localID);
				reply << PackedUInt2(0U);
				player->SendPacket(reply);
				break;
			}
			spawned->SetLocalID(localID);

			std::printf("[trace] p%d SPAWN actorID=%i localID=%u -> netID=%d room=%d\n",
				player->NetworkID(), actorID, localID, spawned->NetworkID(), roomIndex);

			ByteStream reply = newPacket(SERVER_PACKET_ASSIGN_ACTOR_ID);
			reply << PackedUInt4(sceneKey);
			reply << PackedUInt1(roomIndex);
			reply << PackedUInt4(localID);
			reply << PackedUInt2((unsigned int)(spawned->NetworkID()));
			player->SendPacket(reply);
		}
		break;

		case CLIENT_PACKET_ACTOR_PROPERTIES:
		{
			if (data.BytesLeft() < 2)
			{
				std::printf("OOTServer: malformed ACTOR_PROPERTIES from player %d\n", player->NetworkID());
				break;
			}

			int networkID = (int)(data.Read<PackedUInt2>().value());

			AbstractActor* actor = m_actors.Get(networkID);

			if (!actor || actor->Type() != NetActorType::WORLD_ACTOR)
				break;

			WorldActor* worldActor = static_cast<WorldActor*>(actor);

			Scene* actorScene = worldActor->OwningScene();
			if (!actorScene || worldActor->Owner() != player)
				break;

			ByteStream props = data.Read(data.BytesLeft());
			if (props.Length() == 0)
				break;

			const bool wasSceneScoped = worldActor->IsSceneScoped();
			worldActor->MergeCustomState(props);
			const bool nowSceneScoped = worldActor->IsSceneScoped();

			if (!wasSceneScoped && nowSceneScoped)
			{
				std::printf("[trace] actor %d went scene-scoped (left room)\n", networkID);
				actorScene->MakeActorSceneScoped(worldActor);
			}
			else if (wasSceneScoped && !nowSceneScoped)
			{
				actorScene->MakeActorRoomScoped(worldActor);
			}

			ByteStream packet = newPacket(SERVER_PACKET_ACTOR_PROPERTIES);
			packet << PackedUInt2((unsigned int)(networkID));
			packet.Write(props);
			worldActor->Broadcast(packet, player);
		}
		break;

		case CLIENT_PACKET_ACTOR_DIED:
		{
			if (data.BytesLeft() < 2)
			{
				std::printf("OOTServer: malformed ACTOR_DIED from player %d\n", player->NetworkID());
				break;
			}

			int networkID = (int)(data.Read<PackedUInt2>().value());

			printf("KILLED ACTOR: %i\n", networkID);
			AbstractActor* actor = m_actors.Get(networkID);
			if (!actor || actor->Type() != NetActorType::WORLD_ACTOR)
				break;
			WorldActor* worldActor = static_cast<WorldActor*>(actor);

			Scene* actorScene = worldActor->OwningScene();
			if (!actorScene || worldActor->Owner() != player)
				break;

			actorScene->DestroyActor(&m_actors, worldActor);
		}
		break;

		case CLIENT_PACKET_ACTOR_TRIGGER_TO_PUPPETS:
		case CLIENT_PACKET_ACTOR_TRIGGER_TO_LEADER:
		{
			if (data.BytesLeft() < 2)
			{
				std::printf("OOTServer: malformed ACTOR_TRIGGER from player %d\n", player->NetworkID());
				break;
			}

			int networkID = (int)(data.Read<PackedUInt2>().value());

			AbstractActor* actor = m_actors.Get(networkID);
			if (!actor || actor->Type() != NetActorType::WORLD_ACTOR)
				break;
			WorldActor* worldActor = static_cast<WorldActor*>(actor);

			ByteStream content = data.Read(data.BytesLeft());
			if (clientPacketID == CLIENT_PACKET_ACTOR_TRIGGER_TO_PUPPETS)
			{
				if (worldActor->Owner() != player)
					break;

				ByteStream packet = newPacket(SERVER_PACKET_ACTOR_TRIGGER);
				packet << PackedUInt2((unsigned int)(networkID));
				packet << PackedUInt1(0);
				packet.Write(content);

				worldActor->Broadcast(packet, player);
			}
			else {
				if (worldActor->Owner() == nullptr || worldActor->Owner() == player)
					break;

				printf("RECEIVED CLIENT_PACKET_ACTOR_TRIGGER_TO_LEADER\n");

				ByteStream packet = newPacket(SERVER_PACKET_ACTOR_TRIGGER);
				packet << PackedUInt2((unsigned int)(networkID));
				packet << PackedUInt1(1);
				packet.Write(content);

				worldActor->Owner()->SendPacket(packet);

			}
		}
		break;

		case CLIENT_PACKET_UPDATE_APPEARANCE:
		{
			ByteStream blob = data.Read(data.BytesLeft());

			auto oldNickName = player->NickName();
			auto oldAge = player->Age();
			player->SetAppearance(blob);

			if (player->InitedPlayerList() && (oldNickName != player->NickName() || oldAge != player->Age()))
				BroadcastPlayerListEntryUpdate(player);

			if (oldAge != player->Age())
			{
				if (auto party = player->GetParty())
					party->OnMemberAgeChanged(player);
			}
			
			Scene* scene = player->CurrentScene();
			if (!scene)
				break;

			ByteStream packet = newPacket(SERVER_PACKET_UPDATE_APPEARANCE);
			packet << PackedUInt2((unsigned int)(player->NetworkID()));
			packet.Write(blob);
			scene->SendToAll(packet, player);
		}
		break;

		case CLIENT_PACKET_CLAIM_ACTOR:
		{
			static constexpr uint8_t CLAIM_REASON_COOLDOWN = 0;
			static constexpr uint8_t CLAIM_REASON_NOW = 1;
			static const time_t CLAIM_COOLDOWN_SECONDS = 2;

			if (data.BytesLeft() < 2)
			{
				std::printf("OOTServer: malformed CLAIM_ACTOR from player %d\n", player->NetworkID());
				break;
			}

			int networkID = (int)(data.Read<PackedUInt2>().value());

			AbstractActor* actor = m_actors.Get(networkID);
			if (!actor || actor->Type() != NetActorType::WORLD_ACTOR)
				break;
			WorldActor* worldActor = static_cast<WorldActor*>(actor);

			Room* room = worldActor->OwningRoom();

			if (room && worldActor->Owner() == nullptr && room->Leader() != nullptr)
			{
				std::printf("OOTServer: actor %d had no owner (bug?) -- adopting room leader\n", networkID);
				worldActor->ReassignOwner(room->Leader());

			}

			unsigned int reason = CLAIM_REASON_NOW;
			if (data.BytesLeft() >= 1)
				reason = data.Read<PackedUInt1>().value();


			bool allow = room ? (player->CurrentRoom() == room)
				: (player->CurrentScene() == worldActor->OwningScene());
			if (allow && reason == CLAIM_REASON_COOLDOWN &&
				time(nullptr) - worldActor->LastOwnerChange() < CLAIM_COOLDOWN_SECONDS)
			{
				allow = false;
			}

			if (allow && worldActor->IsLocked() && worldActor->LockHolder() != player)
				allow = false;

			if (allow)
			{
				worldActor->ClearDeclinedAssignment(player->Guid());
				worldActor->ReassignOwner(player);
			}
			else if (!allow)
			{
				//Not allowed. Set this players leader back and send the full state of the actor
				ByteStream packet = newPacket(SERVER_PACKET_SET_ACTOR_LEADER);
				packet << PackedUInt2((unsigned int)(networkID));
				packet << PackedUInt2(worldActor->Owner()
					? (unsigned int)(worldActor->Owner()->NetworkID())
					: 0u);
				player->SendPacket(packet);

				ByteStream props = newPacket(SERVER_PACKET_ACTOR_PROPERTIES);
				props << PackedUInt2((unsigned int)(networkID));
				props.Write(worldActor->FullPropertiesAndStandard());
				player->SendPacket(props);

			}
		}
		break;

		case CLIENT_PACKET_SCENE_FLAG:
		{
			if (data.BytesLeft() < 4 + 5)
			{
				std::printf("OOTServer: malformed SCENE_FLAG from player %d\n", player->NetworkID());
				break;
			}

			uint32_t sceneKey = data.Read<PackedUInt4>().value();
			int roomIndex = (int)(data.Read<PackedInt1>().value());

			unsigned int flagType = data.Read<PackedUInt1>().value();
			unsigned int flag = data.Read<PackedUInt2>().value();
			unsigned int options = data.Read<PackedUInt1>().value();


			unsigned int setFlag = (options & SCENE_FLAG_OPT_SET) != 0;
			unsigned int lockedDoor = (options & SCENE_FLAG_OPT_LOCKED_DOOR) != 0;



			Scene* scene = player->CurrentScene();
			if (!scene)
				break;

			constexpr unsigned int kFlagTypeSceneSwitch = 1;
			constexpr unsigned int kFlagTypeSceneClear = 3;
			constexpr unsigned int kFlagTypeINFSwitch = 7;
			constexpr unsigned int kFlagTypeEventINFSwitch = 5;



			if (lockedDoor)
			{
				if (scene->PartyID() == 0)
				{
					scene->AppendLockedDoorMask(flag);
					return;
				}
				
			}

			if (flagType == kFlagTypeSceneSwitch && flag >= 0x20)
			{
				bool roomScoped = flag >= 0x38;

				//Room temps
				if (roomScoped) {
					if (Room* room = player->CurrentRoom())
					{
						if (room->CanSyncTempFlag(flag - 0x20))
						{
							room->SetTempFlag((int)(flag), setFlag != 0);
							ByteStream packet = newPacket(SERVER_PACKET_SCENE_FLAG);
							packet << PackedUInt1(flagType);
							packet << PackedUInt2(flag);
							packet << PackedUInt1(setFlag);
							room->SendToAll(packet, player);
						}
					}
				}
				//Scene temps
				else {
					Scene* scene = player->CurrentScene();
					if (scene && scene->CanSyncTempFlag(flag - 0x20)) {
						scene->SetTempFlag(flag, setFlag != 0);
						ByteStream packet = newPacket(SERVER_PACKET_SCENE_FLAG);
						packet << PackedUInt1(flagType);
						packet << PackedUInt2(flag);
						packet << PackedUInt1(setFlag);
						scene->SendToAll(packet, player);
					}
				}
				break;
			}
			else if (flagType == kFlagTypeSceneSwitch)
			{
				if (scene->CanSyncSceneFlag(flag))
				{
					bool changed = setFlag ? scene->SetSceneFlag(flag)
						: scene->UnsetSceneFlag(flag);
				}
				else break;
			}

			else if (flagType == kFlagTypeINFSwitch)
			{
				bool persistant = false;
				if (!scene->CanSyncINFFlag(flag, &persistant))
					break;

				else if (persistant)
					scene->SetINFFlag(flag, setFlag);
			}
			else if (flagType == kFlagTypeEventINFSwitch)
			{
				bool persistant = false;
				if (!scene->CanSyncEventINFFlag(flag, &persistant))
					break;
				else if (persistant) {
					scene->SetEventINFFlag(flag, setFlag);
				}
			}
			else if (flagType == kFlagTypeSceneClear)
			{
				bool changed = setFlag ? scene->SetSceneClearFlag(flag)
					: scene->UnsetSceneClearFlag(flag);
			}
			else break;

			std::printf("FORWARDING FLAG: %i:%i\n", flagType, flag);
			ByteStream packet = newPacket(SERVER_PACKET_SCENE_FLAG);
			packet << PackedUInt1(flagType);
			packet << PackedUInt2(flag);
			packet << PackedUInt1(setFlag);
			scene->SendToAll(packet, player);
		}
		break;

		case CLIENT_PACKET_SETUP_ACTORS_DONE:
		{
			std::printf("[trace] CLIENT_PACKET_SETUP_ACTORS_DONE (%i)\n", player->NetworkID());

			if (data.BytesLeft() < 5)
				break;

			uint32_t sceneKey = data.Read<PackedUInt4>().value();
			int roomIndex = (int)(data.Read<PackedUInt1>().value());

			Scene* scene = GetScene(MakeServerSceneKey(sceneKey, player->PartyHash()));
			if (!scene)
				break;

		}
		break;

		case CLIENT_PACKET_SCENE_TRIGGER:
		{
			if (data.BytesLeft() < 4)
			{
				std::printf("OOTServer: malformed SCENE_TRIGGER from player %d\n", player->NetworkID());
				break;
			}

			Scene* scene = player->CurrentScene();
			if (!scene)
				break;

			auto sceneKey = data.Read<PackedUInt4>().value();
			bool roomOnly = data.Read<PackedUInt1>().value();
			if (scene->ClientSceneKey() == sceneKey)
			{
				ByteStream content = data.Read(data.BytesLeft());
				Room* room = player->CurrentRoom();

				if (roomOnly && room)
				{
					room->SendToAll(newPacket(SERVER_PACKET_SCENE_TRIGGER) << PackedUInt4(sceneKey) << PackedUInt2(player->NetworkID()) << content, player);
				}
				else scene->SendToAll(newPacket(SERVER_PACKET_SCENE_TRIGGER) << PackedUInt4(sceneKey) << PackedUInt2(player->NetworkID()) << content, player);
			}
		}
		break;

		case CLIENT_PACKET_RELINQUISH_ACTOR_LEADER:
		{
			if (data.BytesLeft() < 2)
			{
				std::printf("OOTServer: malformed RELINQUISH_ACTOR_LEADER from player %d\n", player->NetworkID());
				break;
			}

			unsigned int count = data.Read<PackedUInt2>().value();
			if (data.BytesLeft() < (size_t)(count) * 2)
			{
				std::printf("OOTServer: truncated RELINQUISH_ACTOR_LEADER from player %d (count %u)\n",
					player->NetworkID(), count);
				break;
			}

			for (unsigned int i = 0; i < count; i++)
			{
				int networkID = (int)(data.Read<PackedUInt2>().value());

				AbstractActor* actor = m_actors.Get(networkID);
				if (!actor || actor->Type() != NetActorType::WORLD_ACTOR)
					continue;
				WorldActor* worldActor = static_cast<WorldActor*>(actor);

				if (worldActor->Owner() != player)
					continue;

				Room* room = worldActor->OwningRoom();
				Player* newOwner = nullptr;

				if (room)
				{
					newOwner = room->Leader();
					if (newOwner == player)
						newOwner = room->AnyOtherEligiblePlayer(player, worldActor);
				}
				else if (Scene* scene = worldActor->OwningScene())
				{
					newOwner = scene->ClosestOtherEligiblePlayer(player, worldActor);
				}

				if (newOwner == nullptr)
				{
					worldActor->SetAwaitingLeader(true);
					worldActor->ReassignOwner(nullptr);
					continue;
				}

				if (worldActor->Owner() != newOwner)
				{
					worldActor->ReassignOwner(newOwner);

				}
			}
		} break;

		case CLIENT_PACKET_SET_ACTOR_LOCKED:
		{
			if (data.BytesLeft() < 3) {
				std::printf("OOTServer: malformed SET_ACTOR_LOCKED from player %d\n", player->NetworkID());
				break;
			}
			int  networkID = (int)(data.Read<PackedUInt2>().value());
			bool locked = data.Read<PackedUInt1>().value() != 0;

			AbstractActor* actor = m_actors.Get(networkID);
			if (!actor || actor->Type() != NetActorType::WORLD_ACTOR)
				break;
			WorldActor* worldActor = static_cast<WorldActor*>(actor);
			Room* room = worldActor->OwningRoom();
			if (!room)
				break;

			if (locked) {
				if (worldActor->Owner() != player)
					break;
			}
			else {
				if (worldActor->LockHolder() != player)
					break;
			}

			if (worldActor->IsLocked() == locked)
				break;

			worldActor->SetLocked(locked, player);

			ByteStream packet = newPacket(SERVER_PACKET_SET_ACTOR_LOCKED);
			packet << PackedUInt2((unsigned int)(networkID));
			packet << PackedUInt1(locked ? 1u : 0u);
			room->SendToAll(packet);
		} break;

		case CLIENT_PACKET_ACTOR_SOUND:
		{
			Room* room = player->CurrentRoom();
			if (!room)
				break;

			ByteStream body = data.Read(data.BytesLeft());
			if (body.Length() == 0)
				break;

			ByteStream packet = newPacket(SERVER_PACKET_ACTOR_SOUND);
			packet.Write(body);
			room->SendToAll(packet, player);
		}
		break;

		case CLIENT_PACKET_OCARINA_SFX:
		{
			Room* room = player->CurrentRoom();
			if (!room)
				break;

			ByteStream body = data.Read(data.BytesLeft());
			if (body.Length() == 0)
				break;

			ByteStream packet = newPacket(SERVER_PACKET_OCARINA_SFX);
			packet.Write(body);
			room->SendToAll(packet, player);
		}
		break;

		case CLIENT_PACKET_SET_PAUSE_STATE:
		{
			if (data.BytesLeft() < 1)
				break;

			bool paused = data.Read<PackedUInt1>().value() == 1;
			player->SetPaused(paused);

			if (!paused)
			{
				if (Room* room = player->CurrentRoom())
				{
					room->OnPlayerUnpaused(player);
				}

				if (Scene* scene = player->CurrentScene())
					scene->AdoptOwnerlessActors(player);

			}
		}
		break;

		case CLIENT_PACKET_DECLINE_ACTOR_LEADER:
		{
			if (data.BytesLeft() < 2)
				break;

			int networkID = (int)(data.Read<PackedUInt2>().value());
			AbstractActor* actor = m_actors.Get(networkID);
			if (!actor || actor->Type() != NetActorType::WORLD_ACTOR)
				break;

			WorldActor* worldActor = static_cast<WorldActor*>(actor);

			std::printf("[trace] player %d declined actor %d\n", player->NetworkID(), networkID);

			if (worldActor->Owner() != player)
				break;

			worldActor->RecordDeclinedAssignment(player->Guid());

			Room* room = worldActor->OwningRoom();
			Player* newOwner = nullptr;

			if (room)
				newOwner = room->AnyOtherEligiblePlayer(player, worldActor);
			else if (Scene* actorScene = worldActor->OwningScene())
				newOwner = actorScene->ClosestOtherEligiblePlayer(player, worldActor);

			if (newOwner == nullptr)
				worldActor->SetAwaitingLeader(true);

			worldActor->ReassignOwner(newOwner);
		}
		break;

		case CLIENT_PACKET_INIT_PLAYER_LIST:
		{
			if (player->MarkInitPlayerList())
			{
				SendPlayerListTo(player);
				BroadcastPlayerJoined(player);
			}
		}
		break;

		case CLIENT_PACKET_PARTY_CREATE:
		{
			if (player->PartyGetID() != 0)
				break;

			auto settingsJSON = data.ReadString(data.BytesLeft());

			SetPlayerParty(player, Party::Create());

			auto& party = player->GetParty();
			if (party)
			{
				party->SetSettings(settingsJSON);
				auto admin = party->GetAdmin();

				player->SendPacket(newPacket(SERVER_PACKET_PARTY_JOIN)
					<< PackedUInt4(player->PartyGetID()) << PackedUInt2(admin ? admin->NetworkID() : 0) << party->Settings());
			}
		}
		break;

		case CLIENT_PACKET_PARTY_INVITE:
		{
			if (player->PartyGetID() == 0)
				break;

			if (data.BytesLeft() < 2)
				break;

			auto otherPlayerNetworkID = data.Read<PackedUInt2>().value();

			auto it = m_players.find(otherPlayerNetworkID);
			if (it == m_players.end())
				break;

			Player* otherPlayer = it->second.get();

			if (otherPlayer == player || otherPlayer->PartyGetID() == player->PartyGetID())
				break;


			auto party = player->GetParty();
			if (party == nullptr)
				break;

			party->AddInvitedPlayer(otherPlayer->Guid());
			otherPlayer->SendPacket(newPacket(SERVER_PACKET_PARTY_INVITE) << PackedUInt2((unsigned int)(player->NetworkID())) << PackedUInt4(party->ID()));
		}
		break;

		case CLIENT_PACKET_PARTY_JOIN:
		{
			if (data.BytesLeft() < 6)
				break;

			auto otherPlayerNetworkID = data.Read<PackedUInt2>().value();
			auto partyID = data.Read<PackedUInt4>().value();

			if (partyID == 0 || player->PartyGetID() == partyID)
				break;


			auto party = Party::FindByID(partyID);

			if (party == nullptr)
				break;

			if (!party->HasInvitedPlayer(player->Guid()))
				break;

			auto it = m_players.find(otherPlayerNetworkID);

			if (it == m_players.end() || it->second->PartyGetID() != partyID)
				break;

			party->RemoveInvitedPlayer(player->Guid());

			SetPlayerParty(player, party);

			auto admin = party->GetAdmin();
			player->SendPacket(newPacket(SERVER_PACKET_PARTY_JOIN) << PackedUInt4(partyID) << PackedUInt2(admin ? admin->NetworkID() : 0) << party->Settings());


			for (Player* member : party->Members())
			{
				if (member != player)
				{
					member->SendPacket(newPacket(SERVER_PACKET_PARTY_MEMBER_ADD)
						<< PackedUInt2((unsigned int)(player->NetworkID())));

					player->SendPacket(newPacket(SERVER_PACKET_PARTY_MEMBER_ADD)
						<< PackedUInt2((unsigned int)(member->NetworkID())));
				}
			}

			// Story Co-op: catch the joiner up on the team save (their own upload is merged in afterwards)
			if (party->StoryCoop() && party->StorySeeded())
				player->SendPacket(MakeStoryStatePacket(party->Story(), STORY_FLAG_CATCH_UP));

			party->OnMemberAdded(player);
		}
		break;

		case CLIENT_PACKET_PARTY_DECLINE:
		{
			if (data.BytesLeft() < 6)
				break;

			auto otherPlayerNetworkID = data.Read<PackedUInt2>().value();
			auto partyID = data.Read<PackedUInt4>().value();


			if (partyID == 0 || player->PartyGetID() == partyID)
				break;

			auto party = Party::FindByID(partyID);

			if (party == nullptr)
				break;

			party->RemoveInvitedPlayer(player->Guid());
		}
		break;

		case CLIENT_PACKET_PARTY_LEAVE:
		{
			auto party = player->GetParty();

			if (party == nullptr)
				break;

			std::vector<Player*> formerMembers = party->Members();

			SetPlayerParty(player, nullptr);

			auto admin = party->GetAdmin();

			player->SendPacket(newPacket(SERVER_PACKET_PARTY_JOIN)
				<< PackedUInt4(0u) << PackedUInt2(0u));

			for (Player* member : formerMembers)
			{
				if (member != player)
					member->SendPacket(newPacket(SERVER_PACKET_PARTY_MEMBER_REMOVE) << PackedUInt2((unsigned int)(player->NetworkID())) << PackedUInt2(admin ? admin->NetworkID() : 0));
			}
		}
		break;

		case CLIENT_PACKET_PARTY_ENABLE_SCENES: {
			if (data.BytesLeft() < 1)
				break;

			bool enabled = data.Read<PackedUInt1>().value() != 0;

			//player->SetPartyScenes(enabled);
		}
		break;

		case CLIENT_PACKET_TELEPORT_TO_PARTY_MEMBER: {
			if (data.BytesLeft() < 2)
				break;

			auto memberID = data.Read<PackedUInt2>().value();
			auto partyID = player->PartyGetID();
			if (partyID == 0)
				break;

			auto it = m_players.find(memberID);


			if (it == m_players.end() || it->second->PartyGetID() != partyID)
				break;

			auto& other = it->second;

			if (other->Age() != player->Age())
			{
				std::string error = "Unable to teleport player: Age mismatch";
				player->SendPacket(newPacket(SERVER_PACKET_SET_PLAYERLIST_STATUS) << PackedUInt1(255) << PackedUInt1(0) << PackedUInt1(0) << error);
			} else if (other->CurrentScene() && other->CurrentRoom())
			{

				auto sceneNum = other->CurrentScene()->SceneNum();
				auto roomNum = other->CurrentRoom()->RoomIndex();

				player->TeleportTo(other->EntranceID(), sceneNum, roomNum, other->X(), other->Y(), other->Z());
			}
			break;

		}

		case CLIENT_PACKET_KEEP_ALIVE:
			break;

		case CLIENT_PACKET_ADD_STATIC_ACTOR:
		{
			printf("RECEIVED a\n");
			if (data.BytesLeft() < 5)
				break;

			uint32_t sceneKey = (int)(data.Read<PackedUInt4>().value());
			int roomIndex = (int)(data.Read<PackedInt1>().value());

			Scene* scene = player->CurrentScene();
			printf("RECEIVED b\n");
			if (!scene || scene->ClientSceneKey() != sceneKey)
				break;
			printf("RECEIVED c\n");
			auto room = scene->GetRoom(roomIndex);
			if (!room)
				break;

			int networkID = data.Read<PackedUInt2>().value();

			if (networkID > 0)
			{
				printf("RECEIVED d\n");
				auto actorID = (int16_t)data.Read<PackedUInt2>().value();
				auto x = data.Read<PackedFloat4>().value();
				auto y = data.Read<PackedFloat4>().value();
				auto z = data.Read<PackedFloat4>().value();
				auto rotX = data.Read<PackedInt2>().value();
				auto rotY = data.Read<PackedInt2>().value();
				auto rotZ = data.Read<PackedInt2>().value();
				int16_t params = data.Read<PackedInt2>().value();

				//Special handling for warp gates and heart pieces...
				bool heartOrWarp = actorID == ActorID::ACTOR_DOOR_WARP1 || actorID == ActorID::ACTOR_ITEM_B_HEART;
				if (heartOrWarp)
				{
					if (!scene->IsBoss())
						break;

					if (scene->SceneNum() == SCENE_DODONGOS_CAVERN_BOSS || scene->SceneNum() == SCENE_FIRE_TEMPLE_BOSS || scene->SceneNum() == SCENE_WATER_TEMPLE_BOSS || scene->SceneNum() == SCENE_SPIRIT_TEMPLE_BOSS || scene->SceneNum() == SCENE_JABU_JABU_BOSS)
						break;

					auto& actors = room->StaticActors();
					bool found = false;
					for (auto& existing : actors)
					{
						if (existing.actorID == actorID) {
							found = true;
							break;
						}
					}

					if (found)
						break;
				}

				room->AddStaticActor({ actorID, params, x, y, z, rotX, rotY, rotZ });

			}
		}
		break;

		case CLIENT_PACKET_CHEST_OPENED:
		{
			uint32_t sceneIndex = data.Read<PackedUInt4>().value();
			int roomIndex = (int)(data.Read<PackedInt1>().value());
			uint16_t mapIndex = data.Read<PackedUInt2>().value();
			unsigned int flag = data.Read<PackedUInt1>().value();
			unsigned int modId = data.Read<PackedUInt2>().value();
			unsigned int getItemId = data.Read<PackedUInt2>().value();

			ByteStream packet = newPacket(SERVER_PACKET_CHEST_OPENED);
			packet << PackedUInt2(player->NetworkID());
			packet << PackedUInt4(sceneIndex);
			packet << PackedInt1(roomIndex);
			packet << PackedUInt2(mapIndex);
			packet << PackedUInt1(flag);
			packet << PackedUInt2(modId);
			packet << PackedUInt2(getItemId);

			if (auto& party = player->GetParty())
				party->SendToAll(packet, player);
		}
		break;

		case CLIENT_PACKET_CHAT_MESSAGE:
		{
			auto text = data.ReadString(data.BytesLeft());

			Scene* scene = player->CurrentScene();
			if (scene == nullptr)
				break;

			ByteStream packet = newPacket(SERVER_PACKET_CHAT_MESSAGE);
			packet << PackedUInt2(player->NetworkID()) << text;

			scene->SendToAll(packet, nullptr);
		}
		break;

		case CLIENT_PACKET_UPDATE_PARTY_SETTINGS:
		{
			auto& party = player->GetParty();
			if (!party)
				break;

			bool accepted = party->SetSettingsFromMember(data.BytesLeft() ? data.ReadString(data.BytesLeft()) : std::string(), party->GetAdmin() == player);
			party->SendToAll(newPacket(SERVER_PACKET_UPDATE_PARTY_SETTINGS) << party->Settings(), player);

			// a non-admin tried to change the Story Co-op keys: tell the sender what the party really uses
			if (!accepted)
				player->SendPacket(newPacket(SERVER_PACKET_UPDATE_PARTY_SETTINGS) << party->Settings());
		}
		break;

		case CLIENT_PACKET_UPDATE_DUNGEON_KEYS:
		{
			auto mapIndex = (uint8_t)data.Read<PackedUInt1>().value();
			auto count = (int)data.Read<PackedUInt1>().value();

			if (auto party = player->GetParty())
			{
				party->SetDungeonKeys(mapIndex, count);
				party->SendToAll(newPacket(SERVER_PACKET_UPDATE_DUNGEON_KEYS) << PackedUInt1(mapIndex) << PackedUInt1(party->DungeonKeys(mapIndex)), player);
			}
		}
		break;

		case CLIENT_PACKET_STORY_PROGRESS:
		{
			auto party = player->GetParty();
			if (!party || !party->StoryCoop() || data.BytesLeft() < 1)
				break;

			auto flags = (uint8_t)data.Read<PackedUInt1>().value();

			StoryProgress incoming;
			if (!incoming.Deserialize(data))
				break;

			if (party->MergeStory(incoming))
				party->SendToAll(MakeStoryStatePacket(party->Story(), 0), player);

			// an uploading member may lack things the team already has: catch them up
			if (flags & STORY_FLAG_SEED)
			{
				StoryProgress theirs = incoming;
				if (theirs.MergeFrom(party->Story()))
					player->SendPacket(MakeStoryStatePacket(party->Story(), STORY_FLAG_CATCH_UP));
			}
		}
		break;

		case CLIENT_PACKET_STORY_TIME_VOTE:
		{
			auto party = player->GetParty();
			if (!party || !party->StoryCoop() || data.BytesLeft() < 1)
				break;

			auto action = data.Read<PackedUInt1>().value();
			party->HandleVote(player, action != 0);
		}
		break;

		case CLIENT_PACKET_STORY_SHARE_ITEM:
		{
			auto party = player->GetParty();
			if (!party || !party->StoryCoop() || !party->ShareConsumables() || data.BytesLeft() < 4)
				break;

			auto modIndex = data.Read<PackedUInt2>().value();
			auto getItemId = data.Read<PackedUInt2>().value();

			// relayed only, never stored: consumables stay per player
			party->SendToAll(newPacket(SERVER_PACKET_STORY_SHARE_ITEM)
				<< PackedUInt2((unsigned int)(player->NetworkID())) << PackedUInt2(modIndex) << PackedUInt2(getItemId), player);
		}
		break;

		default:
			std::printf("OOTServer: unknown packet id %u from player %d\n",
				clientPacketID, player->NetworkID());
			break;
		}
	}

	void OOTServer::SetPlayerParty(Player* player, std::shared_ptr<Party> party)
	{
		if (player == nullptr || player->GetParty() == party)
			return;

		auto old = player->GetParty();

		if (old != nullptr)
		{
			old->RemoveMember(player);
			old->OnMemberRemoved(player);
		}

		player->SetParty(party);

		if (party != nullptr)
			party->AddMember(player);

		if (old != nullptr && old->MemberCount() == 0)
			ClearPartyScenes(old->ID());
	}

	void OOTServer::onClientDisconnected(ClientConnection* connection)
	{
		(void)connection;
		std::printf("OOTServer: connection dropped before handshake\n");
	}

	void OOTServer::onPlayerDisconnected(Player* player)
	{
		std::printf("OOTServer DISCONNECT: player %d %s\n", player->NetworkID(),
			player->IsBot() ? "(bot) removed" : "disconnected");

		if (Scene* scene = player->CurrentScene())
			scene->RemovePlayer(player);

		if (auto party = player->GetParty())
		{
			std::vector<Player*> formerMembers = party->Members();

			party->RemoveMember(player);
			party->OnMemberRemoved(player);
			player->SetParty(nullptr);

			auto admin = party->GetAdmin();
			for (Player* member : formerMembers)
			{
				if (member != player)
					member->SendPacket(newPacket(SERVER_PACKET_PARTY_MEMBER_REMOVE)
						<< PackedUInt2((unsigned int)(player->NetworkID())) << PackedUInt2(admin ? admin->NetworkID() : 0));
			}

			if (party->MemberCount() == 0)
				ClearPartyScenes(party->ID());
		}

		BroadcastPlayerLeft(player);
	}
}
