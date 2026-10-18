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

struct AutoTestAccess;

class ZeldaOnlineClient : public ZNetworking {
    friend struct AutoTestAccess;

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
    // A team snapshot only belongs to the party and the loaded save it was received for
    uint32_t m_storySaveSerial = 0U;         // bumped every time a save is loaded or exited
    uint32_t m_storyPendingParty = 0U;
    uint32_t m_storyPendingSerial = 0U;

    // Story Co-op dungeon progress: permanent switch and clear flags of dungeon and boss scenes. The netcode empties the
    // saved copy while a dungeon is loaded (the server instance owns the live flags), so they are kept here between visits
    // and re-applied whenever the scene is loaded, including after the server instance has reset.
    uint32_t m_storyDungeonSwch[STORY_SCENE_COUNT] = {};
    uint32_t m_storyDungeonClear[STORY_SCENE_COUNT] = {};

    // Story Co-op last writer wins state (Epona's parking spot, scarecrow songs, Water Temple level, Forest Temple
    // elevator). Index = STORY_LWW_* slot id. `data` is what this client last published or adopted at `stamp`.
    struct StoryLwwLocal {
        bool has = false;
        uint32_t stamp = 0;
        std::vector<uint8_t> data;
    };
    StoryLwwLocal m_storyLww[STORY_LWW_MAX_SLOTS];
    int m_storyPoeSeen = -1; // Big Poe points as last seen (-1 until read); a rise is sent to the server as a delta

    // Story Co-op time travel: the pedestal vote
    bool m_storyHold = false;                // waiting at the Master Sword pedestal for the other members
    int m_storyApprovedFrames = 0;           // the server said everybody voted, the pedestal may run its native sequence
    bool m_storyAllowAgeChange = false;      // lets MoveToEra through the SwitchAge lock
    int m_storyMoveEra = -1;                 // era the server wants this player in (0 adult, 1 child), -1 none
    int m_storyVoted = 0;
    int m_storyTotal = 0;
    std::vector<std::string> m_storyMissing;

    // Story Co-op cutscenes watched together: a story cutscene that starts here is started for the members in the same
    // scene too (a scripted cutscene by its resource path, a scene cutscene by the transition that loads it)
    std::string m_storyCsPath;               // keeps the path alive while csCtx.segment points at it
    bool m_storyCsWasActive = false;         // csCtx was running last frame
    bool m_storyCsTransitionSeen = false;    // the current transition was already looked at
    int m_storyCsRemoteFrames = 0;           // a cutscene was just started for a member; do not announce it again
    int m_storyCsSent = 0;
    int m_storyCsReceived = 0;
    int m_storyCsStarted = 0;
    bool m_storyCsHasPending = false;
    int m_storyCsPendingWait = 0;
    uint8_t m_storyCsPendingKind = 0;
    uint16_t m_storyCsPendingEntrance = 0;
    uint16_t m_storyCsPendingIndex = 0;
    std::string m_storyCsPendingPath;

    // Story Co-op key dialogue: the text boxes of story NPCs are shown to the members in the same scene
    bool m_storyDlgSpeaking = false;         // this player has a key conversation open
    uint16_t m_storyDlgSentText = 0;
    int m_storyDlgBeat = 0;
    bool m_storyDlgFollowing = false;        // another member's key conversation is on screen here
    unsigned int m_storyDlgSpeaker = 0;
    uint16_t m_storyDlgShownText = 0;
    int m_storyDlgQuiet = 0;                 // frames since the speaker last said anything
    bool m_storyDlgFroze = false;            // this client set the cutscene flag on the player
    bool m_storyDlgHasPending = false;
    uint8_t m_storyDlgPendingKind = 0;
    uint16_t m_storyDlgPendingText = 0;
    unsigned int m_storyDlgPendingSender = 0;
    int m_storyDlgSent = 0;
    int m_storyDlgReceived = 0;
    int m_storyDlgShown = 0;
    int m_storyDlgTestActor = -1;            // auto-test only: the NPC a text box without a talk actor belongs to


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
    void StoryOnSaveChanged(bool loaded);
    void StoryMarkDirty();
  public:
    void StoryResetEquipLost();
  private:
    void StoryRememberDungeonFlags(int sceneNum, uint32_t swch, uint32_t clear);
    void StoryApplyDungeonFlags(PlayState* play);
    void StoryOnDungeonFlag(int sceneNum, int flagType, int flag, bool set);
    void StoryLwwPublish(uint8_t id, const std::vector<uint8_t>& data, bool seed = false);
    void StoryLwwAdopt(const StoryLwwSlot& slot);
    void StoryTrackLocalState();
    void StoryTrackEquipment();
    void StoryRegisterSave();
    void StoryResetLocalState();
    void OnStoryDungeonClear(ByteStream& packet);
    StoryProgress StoryCapture();
    bool StoryApply(const StoryProgress& team);
    bool StoryCanApply();
    void StorySendProgress(const StoryProgress& progress, bool seed);
    void StoryUpdate();
    void StoryOnItemReceive(const GetItemEntry& entry);
    void OnStoryState(ByteStream& packet);
    void OnStoryShareItem(ByteStream& packet);

    void StorySendVote(bool vote);
    void StoryOnPedestalUse(Actor* pedestal, bool* should);
    void StoryOnSwitchAge(bool* should);
    void StoryOnSceneInit();
    void StoryReleaseHold(bool restorePlayer);
    void StoryMoveToEra();
    std::string StoryHoldText() const;
    void OnStoryVoteStatus(ByteStream& packet);
    void OnStoryTravel(ByteStream& packet);
    void OnStorySetEra(ByteStream& packet);
    void StoryWatchCutscenes();
    void StoryStartRemoteCutscene();
    void StoryWatchDialogue();
    void StoryReleaseDialogue();
    void OnStoryDialogue(ByteStream& packet);
    void OnStoryCutscene(ByteStream& packet);


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
