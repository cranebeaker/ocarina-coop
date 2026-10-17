#ifndef ZELDAONLINEH
#define ZELDAONLINEH

#define ACTOR_FLAG_ZO_USER1 (1 << 31)
#define ACTOR_FLAG_ZO_USER2 (1 << 30)
#define ACTOR_FLAG_ZO_USER3 (1 << 29)

#define ZO_HORSE_PUPPET 0x4000

#ifdef __cplusplus
#include <unordered_set>
#include <unordered_map>
#include "ZNetworking.hpp"
#include "ByteStream.hpp"
#include "BytePacking.hpp"
#include "PacketTypes.hpp"
#include "AbstractActorController.hpp"
#include "ResourceDownloader.hpp"
#include <ctime>
#include "Guid.hpp"
#include "PartySettings.hpp"
#include "StoryProgress.hpp"

struct GetItemEntry;




namespace ZeldaOnline {

    
std::vector<std::string> MissingArchives(const std::vector<std::string>& archiveNames);

class ZeldaOnlineClient : public ZNetworking {
  private:
    static const unsigned int FRAME_HEADER_SIZE = 2;
    static const unsigned int MAX_FRAME_PAYLOAD = 0xFFFF;

    enum RequestRoomSceneChange  {
        RequestRoomSceneChangeNone,
        RequestRoomSceneChangeRoom,
        RequestRoomSceneChangeScene
    } ;
    std::string m_host;
    int m_port = 21050;
    std::string m_nickName = "Player";

    uint64_t m_clientGUID = NewGuid();
    ByteStream recvBuffer;

    std::unordered_map<int, AbstractActorController*> m_networkedActors;
    ByteStream outgoingBuffer;
    int m_myNetworkID = 0;

    bool m_hasWorldTime = false;
    u16 m_serverTimeRate = 200;
    u16 m_serverDayTime = 0;
    u8 m_boundaryCueOnLoad = 0;
    bool m_sceneLoadedAtNight = false;
    bool m_reloadPending = false;
    u8 m_wasSimulationPaused = 0;

    int m_lastScene = -1;
    int m_lastRoom = -1;
    int m_removeNametagTimer = 0;
    int m_chatVisibleTimer = 0;
    int m_clearPlayerListStatusTimer = 0;

    u32 m_nextLocalID = 1;

    ByteStream m_lastAppearanceBlob;


    bool m_applyingRemoteSpawn = false;
    uint32_t m_partyID = 0U;
    PartySettings m_partySettings;

    int m_partySize = 1;
    int m_maxNeighbours = 2;
    float m_neighbourChancePerPlayer = 0.25f;
    f32 m_healthWeightPerPlayer = 0.25f;
    bool m_sendFullPlayerProps = true;
    ByteStream m_lastPlayerProps;
    bool m_hooksEnabled = false;
    std::unordered_set<int> m_blockedParentSpawners;
    bool m_didDisconnect = false;
    bool m_autoReconnect = true;
    bool m_wasConnected = false;

    uint64_t m_bytesSent = 0;
    uint64_t m_bytesReceived = 0;
    time_t m_lastReportTime = 0;
    int m_showConnectionStatusTimer = 0;


    std::string m_skinRef = "";
    std::string m_skinName = "";
    
    uint32_t m_sceneLockedDoorFlags = 0U;  
    uint32_t m_savedSwch = 0U;              //We save the current scenes flags in OnSceneInit so we can reapply the locked doors
    int m_blockSceneSetupActors = -1;
    int m_keepAliveTimer = 20 * 3;
    bool m_skipItemGive = false;
    ResourceDownloader m_downloader;
    std::string m_fileServerUrl;

    std::unordered_map<unsigned int, uint64_t> m_dungeonSessions;
    std::unordered_map<unsigned int, uint32_t> m_savedChests;
    RequestRoomSceneChange m_roomSceneChange = RequestRoomSceneChange::RequestRoomSceneChangeNone;

    // Story Co-op (see ZeldaOnlineStoryCoop.cpp)
    bool m_storyWasActive = false;
    uint32_t m_storyPartyID = 0U;
    bool m_storySeedPending = false;
    bool m_storyDirty = false;
    bool m_storyHasPending = false;
    int m_storyFrame = 0;
    int m_storySendCooldown = 0;
    int m_storyNotifyCooldown = 0;
    StoryProgress m_storyLastSent;
    StoryProgress m_storyPending;

    void ReportBandwidth();
    bool NextPacket(ByteStream& out);
    void SendPacket_PlayerUpdate();
    void DrawOverlay(GraphicsContext* gfxCtx);

    void UpdateAppearance();
    void RegisterNetworkingHook(bool enabled);
    void RegisterHooks(bool enabled);
    void OnActorKill(Actor* actor);
    void OnGameFrameUpdate();
    void InitPuppetPlayer(Actor* actor);
    void OnSceneInited(int sceneNum);
    void NetworkHook();

    bool StoryCoopActive() const;
    bool StoryShareConsumables() const;
    void StoryRefreshState();
    void StoryMarkDirty();
    StoryProgress StoryCapture();
    bool StoryApply(const StoryProgress& team);
    bool StoryCanApply();
    void StorySendProgress(const StoryProgress& progress, bool seed);
    void StoryUpdate();
    void StoryOnItemReceive(const GetItemEntry& entry);
    void OnStoryState(ByteStream& packet);
    void OnStoryShareItem(ByteStream& packet);

  public:
    static ZeldaOnlineClient* Instance;

    ZeldaOnlineClient();

    void Enable();
    void Disable() {
        ZNetworking::Disable();
    }

    std::string BuildPartySettingsJSON(const PartySettings& settings);
    bool ParsePartySettingsJSON(const std::string& partySettings, PartySettings& out);

    void SetPartySettings(const PartySettings& settings);
    void LoadPartySettings();
    static bool SplitSkinRef(const std::string& ref, std::string& resourceName,
                             std::vector<std::string>& archiveNames) {
        size_t bar = ref.find('|');

        resourceName = (bar == std::string::npos) ? ref : ref.substr(0, bar);
        if (resourceName.empty()) {
            return true;
        }

        if (resourceName == "link") {
            resourceName = "";
            return true;
        }

        if (bar == std::string::npos) {
            archiveNames.push_back(resourceName + ".o2r");
            return true;
        }

        std::string archives = ref.substr(bar + 1);
        size_t start = 0;

        while (start <= archives.size()) {
            size_t comma = archives.find(',', start);
            std::string one = archives.substr(start, comma - start);
            if (!one.empty()) {
                archiveNames.push_back(one);
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }

        return !archiveNames.empty();
    }

    static bool IsNightTime(u16 dayTime) {
        return dayTime > 0xC000 || dayTime < 0x4555;
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

            case 0x63:  // Lon Lon Ranch - normal (no race, no trapped inside the ranch)
                if (sceneVariant == 0)
                    return true;
                break;

            default:
                return false;
        }
        return false;
    }

   static bool ShouldFreezeTime(int sceneNum, int sceneVariant) {
        switch (sceneNum) {
            case SCENE_LON_LON_RANCH:
                return sceneVariant != 0;

            default:
                return false;
        }
    }


    int MyNetworkID() const {
        return m_myNetworkID;
    }
    void OnIncomingData(const char* payload, int length) override;

    void ProcessOutgoingPackets() override;

    void Connect();
    void Disconnect();

    void OnConnected() override;
    void OnDisconnected() override;
    void OnConnectionClosedBeforeConnect() override;

    virtual void OnIncomingPacket(ByteStream& packet);

    void RemoveNetworkedActor(int networkID, AbstractActorController* expected) {
        auto it = m_networkedActors.find(networkID);
        if (it != m_networkedActors.end() && it->second == expected)
            m_networkedActors.erase(it);
    }

    AbstractActorController* GetNetworkController(int networkID) {
        auto it = m_networkedActors.find(networkID);
        return it == m_networkedActors.end() ? nullptr : it->second;
    }

    std::unordered_map<int, AbstractActorController*>& NetworkedActors() {
        return m_networkedActors;
    }

    void RequestSkinDownload(const std::vector<std::string>& archiveNames, const std::string& skinName);

    bool WritePacket(const ByteStream& data);
    void SendSceneTrigger(const std::string& name, const ByteStream& payload);
    void SendRoomTrigger(const std::string& name, const ByteStream& payload);
    void SendHitTrigger(ActorID actor, f32 posX, f32 posY, f32 posZ);
    Actor* RequestSpawnActor(s16 actorId, f32 posX, f32 posY, f32 posZ, s16 rotX, s16 rotY, s16 rotZ, s16 params);
    Actor* RequestSpawnActorAsChild(AbstractActorController* parent, s16 actorId, f32 posX, f32 posY, f32 posZ,
                                    s16 rotX, s16 rotY, s16 rotZ, s16 params);

    Actor* SpawnWarpOrHeart(s16 actorId, f32 posX, f32 posY, f32 posZ, s16 rotX, s16 rotY, s16 rotZ, s16 params);
    Actor* SpawnActor(s16 actorId, f32 posX, f32 posY, f32 posZ, s16 rotX, s16 rotY, s16 rotZ, s16 params, bool neighbourSpawn = false);
    Actor* SpawnActorAsChild(Actor* parent, s16 actorId, f32 posX, f32 posY, f32 posZ, s16 rotX, s16 rotY, s16 rotZ,
                             s16 params);
    bool RequestRoomSceneChange(bool roomTransition = false);
    Actor* FindExistingActor(s16 actorId, s16 params, int category, f32 homeX, f32 homeZ);


    bool IsLocalPlayerClosestTo(const Vec3f& pos, f32 localDistSq) {
        for (auto& entry : m_networkedActors) {
            AbstractActorController* c = entry.second;
            if (!c->IsPlayer())
                continue;
            Vec3f d = { c->GetActor()->world.pos.x - pos.x, 0.0f, c->GetActor()->world.pos.z - pos.z };
            if (SQ(d.x) + SQ(d.z) < localDistSq)
                return false;
        }
        return true;
    }

    void RegisterActorHooks(bool enabled);
    void OnSceneTrigger(AbstractActorController* sendingPlayer, const std::string& name, ByteStream& data);
    void SendActorDied(int networkID);

    void TransmitActorSound(int networkID, u16 sfxId);


    void SendDeclineActorLeader(int networkID);

    void DetachAndKill(AbstractActorController* controller);
    const std::string& LocalSkinName() const {
        return m_skinName;
    }

    
    //TO be used with enemies spawning neighbours
    int RollNeighbourCount(f32 weight = 1.0f) const;
    float RollHealthMultiplier(f32 weight, f32 settingWeight) const; 
    float RollEnemyHealthMultiplier(f32 weight = 1.0f) const;
    float RollBossHealthMultiplier(f32 weight = 1.0f) const;
};
}

#endif

#ifdef __cplusplus
extern "C" {
#endif

Actor* ZeldaOnlineClient_SpawnActor(s16 actorId, f32 posX, f32 posY, f32 posZ, s16 rotX, s16 rotY, s16 rotZ,
                                    s16 params);

Actor* ZeldaOnlineClient_SpawnActorAsChild(Actor* parent, s16 actorId, f32 posX, f32 posY, f32 posZ, s16 rotX, s16 rotY,
                                           s16 rotZ, s16 params);

int ZeldaOnlineClient_RequestRoomSceneChange(int freshLoad);
const char* ZeldaOnline_LocalSkinName();
extern u8 gZeldaOnlineEngineCleanup;

#ifdef __cplusplus
}
#endif

#endif
