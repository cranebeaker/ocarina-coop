#ifndef NETWORK_ANCHOR_H
#define NETWORK_ANCHOR_H
#ifdef __cplusplus

#include "soh/Network/Network.h"
#include <libultraship/libultraship.h>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <queue>
#include <deque>
#include <mutex>

extern "C" {
#include "variables.h"
#include "z64.h"
}

void DummyPlayer_Init(Actor* actor, PlayState* play);
void DummyPlayer_Update(Actor* actor, PlayState* play);
void DummyPlayer_Draw(Actor* actor, PlayState* play);
void DummyPlayer_Destroy(Actor* actor, PlayState* play);

struct AnchorClient {
    uint32_t clientId;
    std::string name;
    Color_RGB8 color;
    std::string clientVersion;
    std::string teamId;
    bool online;
    bool self;
    uint32_t seed;
    bool isSaveLoaded;
    bool isGameComplete;
    s16 sceneNum;
    s8 curRoomNum;
    s32 entranceIndex;
    u16 timeIncrement; // 0 = timeless scene (dungeon/indoor), >0 = outdoor

    // Only available in PLAYER_UPDATE packets
    s32 linkAge;
    PosRot posRot;
    Vec3s jointTable[24];
    u8 movementFlags;
    Vec3s prevTransl;
    Vec3s upperLimbRot;
    s8 currentBoots;
    s8 currentShield;
    s8 currentTunic;
    u32 stateFlags1;
    u32 stateFlags2;
    u8 buttonItem0;
    s8 itemAction;
    s8 heldItemAction;
    u8 modelGroup;
    s8 invincibilityTimer;
    f32 unk_85C;
    s16 unk_862;
    s8 actionVar1;
    u8 ocarinaNote;
    f32 ocarinaModulator;
    s8 ocarinaBend;

    // Ptr to the dummy player
    Player* player;
    // > 0 while playing the throw animation on the dummy (giver animation override).
    s32 giverAnimTimer = 0;

    // Reported one-way latency in ms from the remote client's own PING/PONG
    // measurement.  Included in UPDATE_CLIENT_STATE.  UINT32_MAX = unknown (no
    // measurement yet).  Used for host-election ordering.
    uint32_t pingMs = UINT32_MAX;
};

typedef struct {
    uint32_t ownerClientId;
    u8 pvpMode;           // 0 = off, 1 = on, 2 = on with friendly fire
    u8 showLocationsMode; // 0 = none, 1 = team, 2 = all
    u8 teleportMode;      // 0 = off, 1 = team, 2 = all
    u8 syncItemsAndFlags; // 0 = off, 1 = on
    u8 syncHPAndCounts;   // 0 = per-player (HP & ammo counts separate), 1 = shared (default)
    u8 syncDayTime;       // 0 = off, 1 = on
    u8  syncEnemies;           // 0 = off (vanilla), 1 = on (custom enemy sync)
    u8  syncBGObjects;           // 0 = off (vanilla), 1 = on (BG keyframe + kill sync)
    u16 syncRadius;              // world-unit radius for pos/anim sync; 0 = unlimited
    u8  enemySyncTickRate;       // 0=5Hz(every 4th frame), 1=10Hz(every 2nd), 2=20Hz(every frame)
    u8  physicalItemExchange;    // 0 = instant sync (default), 1 = buffer until players are <100 units apart
    u8  syncCutscenes;           // 0 = off (default), 1 = sync in-scene cutscenes to nearby players
    u8  syncMinigames;           // 0 = off (default), 1 = broadcast minigame state/score/end events
    u8  syncEpona;               // 0 = off (default), 1 = spawn phantom horse under remote riders
    u8  battleRoyaleMode;        // 0 = off (default), 1 = on (PvP, kill tracking, wanted system)
    u8  brFreshStart;            // 0 = off (default), 1 = reset inventory/health/rupees on MATCH_START
} RoomState;

class Anchor : public Network {
  private:
    uint32_t spawningDummyPlayerForClientId = 0;
    bool shouldRefreshActors = false;
    bool justLoadedSave = false;
    bool isHandlingUpdateTeamState = false;
    bool isProcessingIncomingPacket = false;
    std::queue<nlohmann::json> incomingPacketQueue;
    std::mutex incomingPacketQueueMutex;
    std::queue<nlohmann::json> outgoingPacketQueue;
    std::mutex outgoingPacketQueueMutex;

    // Enemy authority: tracks last-known HP of each enemy actor so we only
    // broadcast ActorStateUpdate when health actually changes.
    // Key: actorKey string (see GetActorKey). Cleared on scene change.
    std::unordered_map<std::string, u8> trackedEnemyHealth;

    // Non-authority: tracks last-known HP so local hits can be forwarded to the owner.
    std::unordered_map<std::string, u8> trackedNonAuthEnemyHealth;

    // Non-authority: records actor health values set via incoming ActorStateUpdate packets.
    // Used to distinguish remote-driven health changes from local player hits.
    // Entry is erased after one confirmed observation in OnActorUpdate.
    std::unordered_map<std::string, u8> pendingRemoteHealthOverride;

    // Non-authority: last position received from the authority per enemy actor.
    // Used by the position-corrected update path to:
    //   1. Pre-set world.pos before the actor update so colliders register at the right spot.
    //   2. Restore world.pos after the update in case local AI moved the actor.
    //   3. Sync focus.pos (Z-targeting reticle) every frame.
    // Cleared on scene change together with the other per-scene maps.
    std::unordered_map<std::string, Vec3f> remoteEnemyPos;

    // Authority: tracks last broadcast position of each enemy so we only send
    // EnemyPositionUpdate when the enemy has actually moved.
    std::unordered_map<std::string, Vec3f> trackedEnemyPos;

    // Authority: tracks whether actor->draw was non-null on the last observed frame.
    // A change in draw state (visible ↔ hidden) triggers an immediate position packet
    // even without movement (e.g. Deku Scrub emerging from underground).
    std::unordered_map<std::string, bool> trackedEnemyDrawState;

    // Client: saves the original draw function before we suppress it so we can
    // restore it when the authority reports the enemy as visible again.
    // Keyed by actorKey. Cleared on scene change.
    std::unordered_map<std::string, ActorFunc> savedEnemyDrawFuncs;

    // Authority: last observed EnfHG::curPainting per real Phantom Ganon horse,
    // used to detect a new painting-select cycle and re-broadcast it. Cleared
    // on scene change together with the other per-scene maps.
    std::unordered_map<std::string, int16_t> gndLastCurPainting;

    // BgKeyframeSync: authority send-side tracking per background actor.
    // Cleared on scene change together with trackedEnemyPos.
    using Clock = std::chrono::steady_clock;

    struct BgKeyframe {
        Vec3f             pos        = { 0, 0, 0 };  // position at last sent keyframe
        Vec3f             vel        = { 0, 0, 0 };  // velocity at last sent keyframe (direction-reversal detection)
        Clock::time_point lastSentAt = {};            // wall-clock time of last sent packet (chrono, FPS-unabhaengig)
    };
    std::unordered_map<std::string, BgKeyframe> trackedBgActors;

    // BgKeyframeSync: client receive-side — blended toward in the per-frame hook.
    // Stores the last keyframe received from the authority per BG actor.
    struct BgKeyframeTarget {
        Vec3f pos   = { 0, 0, 0 };
        s16   rotY  = 0;  // world.rot.y from the authority keyframe
        // Wall-clock time of the last received keyframe.  Used to expire stale
        // entries when the master stops broadcasting (e.g. after an actor is
        // thrown/released) so local physics can take over.
        Clock::time_point lastReceivedAt = {};
    };
    std::unordered_map<std::string, BgKeyframeTarget> bgActorKeyframeTarget;

    // Heartbeat-Intervall in Millisekunden (2 s) – unabhaengig von der Framerate.
    static constexpr uint32_t BG_KEYFRAME_INTERVAL_MS = 2000;
    // Minimum squared position change to count as "moving" (≈ 0.5 world units per frame).
    static constexpr f32 BG_POS_CHANGE_THRESHOLD_SQ  = 0.25f;
    // Client-side blend: fraction of gap closed per frame + absolute max step.
    static constexpr f32 BG_LERP_FRACTION             = 0.15f;
    static constexpr f32 BG_LERP_MAX_STEP             = 6.0f;

    // Both sides: keyed by "sceneNum_roomNum" → set of actorKeys.  Flushed as a ROOM_KILL_SYNC
    // packet the moment the other player enters the room.
    std::map<std::string, std::set<std::string>> pendingRoomKills;

    // Per-room gameplay authority: maps roomKey("{sceneNum}_{roomNum}") to masterClientId.
    // 0 / absent = no explicit assignment; IsRoomMaster() falls back to ownerClientId.
    std::unordered_map<std::string, uint32_t> roomAuthority;

    // Both sides: set to true while HandlePacket_ItemPickup removes an item via Actor_Kill
    // so the resulting OnActorKill does not echo an ITEM_PICKUP back to the sender.
    bool isRemovingRemoteItem = false;

    // Client-side: set to true while HandlePacket_EnemyDropItem spawns an EN_ITEM00
    // so the OnActorSpawn hook does not echo the spawned actor back to the authority.
    bool isSpawningRemoteCollectible = false;

    // Both sides: set to true while HandlePacket_BoulderSpawn spawns a rolling
    // boulder so the local OnActorSpawn hook does not echo it back.
    bool isSpawningRemoteBoulder = false;

    // Both sides: set to true while applying a remote OCARINA_SONG_ACTION so the
    // OnOcarinaSongAction send-hook (fired by GameInteractor_ExecuteOnOcarinaSongAction)
    // does not re-broadcast the song we just received, which would otherwise echo
    // forever between all clients in the room.
    bool suppressOcarinaRebroadcast = false;

    // Boulder sync timing: localFrame = masterFrame + masterFrameToLocalOffset.
    // Set from ROOM_SNAPSHOT when entering a room, then used to align incoming
    // BOULDER_SPAWN triggerFrame values to local frame time.
    bool hasMasterFrameSync = false;
    int32_t masterFrameToLocalOffset = 0;

    // Incoming BOULDER_SPAWN packets that should be applied on a future local frame.
    std::vector<nlohmann::json> pendingBoulderSpawns;

    // Duplicate/replay protection per actorKey. Stores the most recent accepted
    // triggerFrame for this scene. Cleared on scene init.
    std::unordered_map<std::string, uint32_t> lastBoulderTriggerFrameByKey;

    // ── Host-Election ────────────────────────────────────────────────────────
    // When the current room owner disconnects, all clients independently elect
    // the online peer with the lowest measured ping (ties broken by lowest
    // clientId).  A 3-second grace period avoids flapping on brief reconnects.
    Clock::time_point hostOfflineSince  = {};
    bool              hostElectionArmed = false;
    static constexpr auto HOST_ELECTION_GRACE = std::chrono::milliseconds(3000);

    // ── Ping measurement (PING / PONG) ───────────────────────────────────────
    // Each client broadcasts a PING every ~5 s.  Recipients reply with PONG.
    // The original sender measures RTT and keeps an EMA of one-way latency.
    // The result is broadcast in UPDATE_CLIENT_STATE so every peer has a
    // complete global ping table for election ordering.
    uint32_t                           ownPingMs     = UINT32_MAX;
    uint32_t                           pingSeq       = 0;
    std::unordered_map<uint32_t,
        Clock::time_point>             pendingPingAt; // seq → send time
    Clock::time_point                  lastPingSentAt = {};
    static constexpr auto              PING_INTERVAL  = std::chrono::milliseconds(5000);
    // Battle Royale: last PvP attacker that hit the local player.
    // Set in HandlePacket_DamagePlayer; read by the BR death-detection hook in
    // OnGameFrameUpdate.  Reset to 0 after a PLAYER_KILLED event is sent, and
    // on every scene change to avoid stale attribution across transitions.
    uint32_t lastPvpAttackerClientId = 0;

    // Aggro radius (world units) within which NPCs turn hostile toward a "wanted" player.
    static constexpr float BR_WANTED_NPC_AGGRO_RADIUS = 400.0f;
    // Authority-side: collectibles that spawned during the current frame's actor updates.
    // Populated by OnActorSpawn(EN_ITEM00); consumed and cleared each frame.
    struct PendingCollectibleSpawn { s16 params; Vec3f pos; };
    std::vector<PendingCollectibleSpawn> recentCollectibleSpawns;

    // Physical Item Exchange: items/flags buffered until players are within proximity.
    struct PendingExchangeItem {
        u16 modId;
        u16 getItemId;
        std::string senderName;
        std::string itemName;
    };
    struct PendingExchangeFlag {
        s16 sceneNum;
        s16 flagType;
        s16 flag;
    };
    std::deque<PendingExchangeItem> physicalItemQueue;
    std::deque<PendingExchangeFlag> physicalFlagQueue;
    // How many physical-exchange items are currently in the get-item animation.
    // Used to suppress the outgoing SendPacket_GiveItem echo when Item_Give fires.
    int physicalExchangeGivePending = 0;

    // Phase 6a — Room Event channel
    // A stable, idempotent event bus for one-shot world changes (minigame starts,
    // boss phase transitions, etc.) that don't map to a save-context flag.
    //
    // Key format: "{sceneNum}_{roomNum}_{eventType}_{eventKey}"
    // Applied events are tracked per scene; the set is cleared on every scene change
    // (OnSceneInit) so events fire fresh when the same room is re-entered.
    std::unordered_set<std::string> processedRoomEvents;

    // Link-movable puzzle switch sync:
    // Tracks local per-room switch state transitions for push-block style
    // puzzles and ensures we only broadcast a solve event once per room+switch.
    // Key format:
    //   "{sceneNum}_{roomNum}_{switchFlag}"
    std::unordered_map<std::string, bool> puzzleSwitchStateByKey;
    std::unordered_set<std::string> announcedPuzzleSwitchSolveByKey;

    // BossSync infrastructure (Phase 1):
    // - dedupe/ordering per boss event stream via monotonically increasing seq
    // - lightweight room-local boss snapshot cache for late joiners
    // Key format for both maps:
    //   roomBossKey = "{sceneNum}_{roomNum}_{bossActorKey}"
    std::unordered_map<std::string, uint32_t> lastBossEventSeqByKey;
    std::unordered_map<std::string, nlohmann::json> bossSnapshotStateByKey;

    // Proximity threshold: ~100 world units ≈ 1 OoT meter.
    static constexpr float    PHYSICAL_EXCHANGE_DIST_SQ = 100.0f * 100.0f;

    // Epona Mode B exchange state machine ────────────────────────────────────
    enum class EponaExchangePhase {
        IDLE,
        WHINNEYING,    // horse is neighing (~30 frames before spawning proxy)
        PROXY_FLYING,  // En_Item00 proxy in flight toward receiver
        WAITING_DIALOG // item has been given; waiting for item-CS to end
    };
    struct EponaExchangeState {
        EponaExchangePhase  phase      = EponaExchangePhase::IDLE;
        Actor*              proxyActor = nullptr;
        Vec3f               proxyTarget = {};
        u32                 frameStart  = 0;
        PendingExchangeItem item;          // pending exchange item info
        GetItemEntry        entry;         // pre-resolved; set in StartEponaExchange()
    };
    EponaExchangeState eponaExchange;

    // Phase 6: Phantom horse actors ───────────────────────────────────────────
    // When a remote player has PLAYER_STATE1_ON_HORSE set, a phantom En_Horse_Normal
    // actor is spawned at their position so local players see a horse under the rider.
    // The phantom's update function is cleared (→ AI-silent), position is overridden
    // every frame from the received rider position.  Cleared on scene change.
    //
    // Key: clientId  Value: pointer to the phantom horse Actor in the current scene
    // (moved to public so DummyPlayer.cpp can access it directly)
    // PHANTOM_HORSE_SADDLE_HEIGHT also moved to public for the same reason.
    // (either player is in a timeless scene).  Client uses this to suppress local dayTime
    // advancement between sync packets.
    bool remoteTimeFrozen = false;
    u8 remoteNightFlag = 0;
    bool hasRemoteNightFlag = false;
    std::chrono::steady_clock::time_point lastTimeSyncRecvAt{};

    nlohmann::json PrepClientState();
    nlohmann::json PrepRoomState();
    void RegisterHooks();
    void GiveNextPhysicalExchangeItem();
    void StartEponaExchange(const PendingExchangeItem& item);
    void FlushPhysicalFlagQueue();
    void SendPacket_TriggerCutscene(u8 csState);
    bool CutsceneOwnerAlive();
    void ReleaseOrphanedCutscene(const char* reason);
    // Frame/scene of the last CS_STATE_IDLE release received from the master.
    u32 csMasterIdleFrame = 0;
    s16 csMasterIdleScene = -1;
    void HandlePacket_TriggerCutscene(nlohmann::json payload);
    void SendPacket_BgKeyframeSync(const Actor* actor);
    void HandlePacket_BgKeyframeSync(nlohmann::json payload);
    void RefreshClientActors();
    void SetDummyPlayerClientId(const Actor* actor, uint32_t clientId);

    static std::string GetActorKey(const Actor* actor, s16 sceneNum);
    // Baby Dodongo (EN_DODOJR) overwrites home.pos when it burrows, which would drift its key.
    // The key computed at init is cached here and reused by GetActorKey.
    static void CacheStableActorKey(const Actor* actor, s16 sceneNum);
    static void EraseStableActorKey(const Actor* actor);
    static void ClearStableActorKeys();
    // Replays the native death side effects (shared overlay globals / clear flag) that
    // Actor_Kill bypasses. Currently: Dodongo's Cavern Lizalfos miniboss pair (EN_ZF).
    static void ApplyNativeEnemyDeathSideEffects(Actor* actor);
    // True while a remote-originated Actor_Kill is being applied (suppresses re-queue/echo).
    bool applyingRemoteKill = false;

    void HandlePacket_Ping(nlohmann::json payload);
    void HandlePacket_Pong(nlohmann::json payload);
    void HandlePacket_ItemPickup(nlohmann::json payload);
    void HandlePacket_EnemyDropItem(nlohmann::json payload);
    void SendPacket_EnemyDropItem(s16 params, float x, float y, float z);
    void HandlePacket_TimeSync(nlohmann::json payload);
    void HandlePacket_AllClientState(nlohmann::json payload);
    void HandlePacket_ActorKilled(nlohmann::json payload);
    void HandlePacket_ActorStateUpdate(nlohmann::json payload);
    void HandlePacket_EnemyPositionUpdate(nlohmann::json payload);
    void HandlePacket_BoulderSpawn(nlohmann::json payload);
    void HandlePacket_RoomKillSync(nlohmann::json payload);
    void QueueOrApplyBoulderSpawn(nlohmann::json payload, bool allowDelay);
    void ProcessPendingBoulderSpawns();
    void HandlePacket_PlayerAttackActor(nlohmann::json payload);
    void HandlePacket_ConsumeAdultTradeItem(nlohmann::json payload);
    void HandlePacket_DamagePlayer(nlohmann::json payload);
    void HandlePacket_DisableAnchor(nlohmann::json payload);
    void HandlePacket_EntranceDiscovered(nlohmann::json payload);
    void HandlePacket_GameComplete(nlohmann::json payload);
    void HandlePacket_GiveItem(nlohmann::json payload);
    void HandlePacket_OcarinaSfx(nlohmann::json payload);
    void HandlePacket_PlayerSfx(nlohmann::json payload);
    void HandlePacket_PlayerUpdate(nlohmann::json payload);
    void HandlePacket_RequestTeamState(nlohmann::json payload);
    void HandlePacket_RequestTeleport(nlohmann::json payload);
    void HandlePacket_ServerMessage(nlohmann::json payload);
    void HandlePacket_SetCheckStatus(nlohmann::json payload);
    void HandlePacket_SetFlag(nlohmann::json payload);
    void HandlePacket_TeleportTo(nlohmann::json payload);
    void HandlePacket_UnsetFlag(nlohmann::json payload);
    void HandlePacket_UpdateBeansCount(nlohmann::json payload);
    void HandlePacket_UpdateClientState(nlohmann::json payload);
    void HandlePacket_UpdateDungeonItems(nlohmann::json payload);
    void HandlePacket_UpdateRoomState(nlohmann::json payload);
    void HandlePacket_UpdateTeamState(nlohmann::json payload);

    // Battle Royale event packet (host-authoritative match lifecycle)
    void HandlePacket_BattleRoyaleEvent(nlohmann::json payload);

    // Room authority handshake + snapshot packets
    void SendPacket_RoomJoin();
    void HandlePacket_RoomJoin(nlohmann::json payload);
    void HandlePacket_RoomMasterAssign(nlohmann::json payload);
    void HandlePacket_RoomSnapshot(nlohmann::json payload);

  public:
    // ── Host-election / Ping – public API ────────────────────────────────────
    // Called every ~5 s from OnGameFrameUpdate; broadcasts a PING to all peers.
    void SendPacket_Ping();
    // Checks whether the current room owner is still online and, if not, runs
    // a deterministic election (lowest ping, tie-break by lowest clientId).
    // Must be called both from HandlePacket_AllClientState and periodically
    // from OnGameFrameUpdate (for the grace-period expiry check).
    void ElectNewHostIfNeeded();
    // Returns the clientId of the best host candidate among all online clients
    // (including self).  Prefers lowest pingMs; breaks ties by lowest clientId.
    uint32_t PickBestHostCandidateId() const;

  private:

  public:
    // Phantom horse map: clientId → Actor* in current scene (AI-silent En_Horse_Normal).
    std::unordered_map<uint32_t, Actor*> clientPhantomHorse;
    // Distance from horse world.pos (ground) to rider's seat attachment.
    static constexpr float PHANTOM_HORSE_SADDLE_HEIGHT = 76.0f;

    uint32_t ownClientId;
    inline static const std::string clientVersion = (char*)gGitCommitHash;

    // Packet types //
    inline static const std::string ITEM_PICKUP = "ITEM_PICKUP";
    inline static const std::string ENEMY_DROP_ITEM = "ENEMY_DROP_ITEM";
    inline static const std::string TIME_SYNC = "TIME_SYNC";
    inline static const std::string ALL_CLIENT_STATE = "ALL_CLIENT_STATE";
    inline static const std::string ACTOR_KILLED = "ACTOR_KILLED";
    inline static const std::string ACTOR_STATE_UPDATE = "ACTOR_STATE_UPDATE";
    inline static const std::string ENEMY_POSITION_UPDATE = "ENEMY_POSITION_UPDATE";
    inline static const std::string BOULDER_SPAWN = "BOULDER_SPAWN";
    inline static const std::string ROOM_KILL_SYNC        = "ROOM_KILL_SYNC";
    inline static const std::string BG_KEYFRAME_SYNC      = "BG_KEYFRAME_SYNC";
    inline static const std::string TRIGGER_CUTSCENE      = "TRIGGER_CUTSCENE";
    inline static const std::string PLAYER_ATTACK_ACTOR = "PLAYER_ATTACK_ACTOR";
    inline static const std::string DAMAGE_PLAYER = "DAMAGE_PLAYER";
    inline static const std::string DISABLE_ANCHOR = "DISABLE_ANCHOR";
    inline static const std::string ENTRANCE_DISCOVERED = "ENTRANCE_DISCOVERED";
    inline static const std::string GAME_COMPLETE = "GAME_COMPLETE";
    inline static const std::string GIVE_ITEM = "GIVE_ITEM";
    inline static const std::string HANDSHAKE = "HANDSHAKE";
    inline static const std::string OCARINA_SFX = "OCARINA_SFX";
    inline static const std::string PLAYER_SFX = "PLAYER_SFX";
    inline static const std::string PLAYER_UPDATE = "PLAYER_UPDATE";
    inline static const std::string REQUEST_TEAM_STATE = "REQUEST_TEAM_STATE";
    inline static const std::string REQUEST_TELEPORT = "REQUEST_TELEPORT";
    inline static const std::string SERVER_MESSAGE = "SERVER_MESSAGE";
    inline static const std::string SET_CHECK_STATUS = "SET_CHECK_STATUS";
    inline static const std::string SET_FLAG = "SET_FLAG";
    inline static const std::string TELEPORT_TO = "TELEPORT_TO";
    inline static const std::string UNSET_FLAG = "UNSET_FLAG";
    inline static const std::string UPDATE_BEANS_COUNT = "UPDATE_BEANS_COUNT";
    inline static const std::string UPDATE_CLIENT_STATE = "UPDATE_CLIENT_STATE";
    inline static const std::string UPDATE_DUNGEON_ITEMS = "UPDATE_DUNGEON_ITEMS";
    inline static const std::string UPDATE_ROOM_STATE = "UPDATE_ROOM_STATE";
    inline static const std::string UPDATE_TEAM_STATE = "UPDATE_TEAM_STATE";
    inline static const std::string ROOM_JOIN          = "ROOM_JOIN";
    inline static const std::string ROOM_MASTER_ASSIGN = "ROOM_MASTER_ASSIGN";
    inline static const std::string ROOM_SNAPSHOT       = "ROOM_SNAPSHOT";
    inline static const std::string ROOM_EVENT          = "ROOM_EVENT";
    inline static const std::string BATTLE_ROYALE_EVENT = "BATTLE_ROYALE_EVENT";
    inline static const std::string PING                = "PING";
    inline static const std::string PONG                = "PONG";

    static Anchor* Instance;
    std::map<uint32_t, AnchorClient> clients;
    RoomState roomState;

    // ─── Battle Royale ────────────────────────────────────────────────────────
    // Kill streak per client (host authoritative; cleared on MATCH_START / PLAYER_ELIM).
    std::unordered_map<uint32_t, u8>  brKillStreak;
    // Clients currently flagged as "wanted" (killStreak >= BR_WANTED_KILL_THRESHOLD).
    std::unordered_set<uint32_t>      wantedClients;
    // Kills required to become "wanted" (5 = default).
    static constexpr u8               BR_WANTED_KILL_THRESHOLD   = 5;
    // True from MATCH_START until MATCH_END.  Guards kill/death logic so the
    // death-detection hook only fires during an active match.  Admin resets it
    // by clicking "Match starten" in the BR admin panel.
    bool                              brMatchActive = false;
    // True after the local player's own PLAYER_ELIM arrives.  Prevents double-
    // reporting (eliminated players must not send further PLAYER_KILLED events
    // or deal PvP damage).  Reset on MATCH_START.
    bool                              brEliminated  = false;
    // Alive/dead transition tracker for the kill-detection hook.  Initialized to
    // true and reset on every MATCH_START so a player who died at the end of the
    // previous match does not trigger a spurious PLAYER_KILLED on rematch.
    bool                              brWasAlive    = true;
    // Deadline until which PvP damage is suppressed after MATCH_START.
    // Gives all players time to orient themselves before the fight begins.
    // Zero-initialised → no protection active (before first match).
    Clock::time_point                 brStartProtectionUntil = {};
    // ─────────────────────────────────────────────────────────────────────────

    void Enable();
    void Disable();
    void OnIncomingJson(nlohmann::json payload);
    void OnConnected();
    void OnDisconnected();
    void ProcessOutgoingPackets();
    void DrawMenu();
    void ProcessIncomingPacketQueue();
    void SendJsonToRemote(nlohmann::json packet);
    bool IsSaveLoaded();
    bool CanTeleportTo(uint32_t clientId);
    bool IsEnemyAuthority();
    uint32_t GetDummyPlayerClientId(const Actor* actor);

    void SendPacket_ItemPickup(const Actor* actor);
    void SendPacket_TimeSync();
    void SendPacket_ClearTeamState(std::string teamId);
    void SendPacket_ActorKilled(const Actor* actor);
    void SendPacket_ActorStateUpdate(const Actor* actor);
    void SendPacket_EnemyPositionUpdate(const Actor* actor);
    void SendPacket_BoulderSpawn(const Actor* actor);
    void SendPacket_RoomKillSync();
    bool IsAnyClientInSameRoom() const;
    bool IsOwnerInSameRoom() const;
    // Room-master: per-room gameplay authority helpers.
    static std::string BuildRoomKey(s16 sceneNum, s8 roomNum);
    std::string GetCurrentRoomKey();
    bool IsHostAuthority();
    bool IsRoomMaster();
    bool IsActorInsideSyncRadius(const Actor* actor);
    bool ShouldActorBeNetworkDriven(const Actor* actor);
    void SendPacket_RoomSnapshot(uint32_t targetClientId);
    // Phase 6a — generic room event broadcast / receive
    // streaming=true: skips dedup on receive side (use for high-frequency score updates)
    void SendPacket_RoomEvent(const std::string& eventType, const std::string& eventKey,
                              const nlohmann::json& eventData = {}, bool streaming = false);
    void HandlePacket_RoomEvent(nlohmann::json payload);
    void SendPacket_PlayerAttackActor(const Actor* actor, u8 damage);
    void SendPacket_DamagePlayer(u32 clientId, u8 damageEffect, u8 damage);
    void SendPacket_BattleRoyaleEvent(const std::string& eventType, nlohmann::json data = {});
    void SendPacket_EntranceDiscovered(u16 entranceIndex);
    void SendPacket_GameComplete();
    void SendPacket_GiveItem(u16 modId, s16 getItemId);
    void SendPacket_Handshake();
    void SendPacket_OcarinaSfx(uint8_t note, float modulator, int8_t bend);
    void SendPacket_PlayerSfx(u16 sfxId);
    void SendPacket_PlayerUpdate();
    void SendPacket_RequestTeamState();
    void SendPacket_RequestTeleport(u32 clientId);
    void SendPacket_SetCheckStatus(RandomizerCheck rc);
    void SendPacket_SetFlag(s16 sceneNum, s16 flagType, s16 flag);
    void SendPacket_TeleportTo(u32 clientId);
    void SendPacket_UnsetFlag(s16 sceneNum, s16 flagType, s16 flag);
    void SendPacket_UpdateBeansCount();
    void SendPacket_UpdateClientState();
    void SendPacket_UpdateDungeonItems();
    void SendPacket_UpdateRoomState();
    void SendPacket_UpdateTeamState();
};

typedef enum {
    // Starting at 5 to continue from the last value in the PlayerDamageResponseType enum
    DUMMY_PLAYER_HIT_RESPONSE_STUN = 5,
    DUMMY_PLAYER_HIT_RESPONSE_FIRE,
    DUMMY_PLAYER_HIT_RESPONSE_NORMAL,
} DummyPlayerDamageResponseType;

class AnchorRoomWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override{};
    void DrawElement() override;
    void Draw() override;
    void UpdateElement() override{};
};

// Re-syncs ObjSwitch actors (eye/floor/crystal) after a remote switch flag change.
void Anchor_ApplyRemoteSwitchFlagToObjSwitch(s16 sceneNum, s16 flag, bool isOn);

#endif // __cplusplus
#endif // NETWORK_ANCHOR_H
