#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/BossSync/BossSyncDispatch.h"
#include "soh/Notification/Notification.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_En_Ru1/z_en_ru1.h"
#include "src/overlays/actors/ovl_Bg_Ydan_Sp/z_bg_ydan_sp.h"
#include "src/overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
#include "src/overlays/actors/ovl_Boss_Ganondrof/z_boss_ganondrof.h"
#include "src/overlays/actors/ovl_Boss_Ganon/z_boss_ganon.h"
#include "src/overlays/actors/ovl_Boss_Sst/z_boss_sst.h"
#include "src/overlays/actors/ovl_Boss_Tw/z_boss_tw.h"
#include "src/overlays/actors/ovl_Bg_Spot02_Objects/z_bg_spot02_objects.h"
#include "src/overlays/actors/ovl_Obj_Timeblock/z_obj_timeblock.h"
#include "src/overlays/actors/ovl_Bg_Ddan_Kd/z_bg_ddan_kd.h"
#include "src/overlays/actors/ovl_Bg_Dodoago/z_bg_dodoago.h"
extern PlayState* gPlayState;
}

namespace {
bool IsBossSyncEventType(const std::string& eventType) {
    return eventType.rfind("BOSS_", 0) == 0;
}

// Boss adapters build their actor keys with an extra "category" field (11 fields), whereas
// Anchor::GetActorKey has 10 fields and excludes category. Accept both formats.
bool BossKeyMatches(const Actor* actor, s16 sceneNum, const std::string& key) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%d_%d_%d_%d_%d_%d_%d_%d_%d_%d", (int)sceneNum, (int)actor->id, (int)actor->room,
             (int)actor->params, (int)actor->home.pos.x, (int)actor->home.pos.y, (int)actor->home.pos.z,
             (int)actor->home.rot.x, (int)actor->home.rot.y, (int)actor->home.rot.z);
    if (key == buf) return true;
    snprintf(buf, sizeof(buf), "%d_%d_%d_%d_%d_%d_%d_%d_%d_%d_%d", (int)sceneNum, (int)actor->category,
             (int)actor->id, (int)actor->room, (int)actor->params, (int)actor->home.pos.x,
             (int)actor->home.pos.y, (int)actor->home.pos.z, (int)actor->home.rot.x, (int)actor->home.rot.y,
             (int)actor->home.rot.z);
    return key == buf;
}
}

/**
 * ROOM_EVENT
 *
 * Phase 6a — Generic idempotent room-event channel.
 *
 * Purpose:
 *   Broadcasts one-shot in-room events that don't map to a save-context flag
 *   (minigame starts, boss phase transitions, score updates, etc.).
 *
 * Idempotency:
 *   Each event is identified by the triple
 *       sceneNum + "_" + roomNum + "_" + eventType + "_" + eventKey
 *   which is stored in `processedRoomEvents` on the receiving client.
 *   Duplicate deliveries (retransmits, late joiners) are silently dropped.
 *   The set is cleared on every OnSceneInit() so events are fresh each room visit.
 *
 * Event types:
 *   "MINIGAME_START"   — a minigame has begun; eventData: { "minigameId": int }
 *   "MINIGAME_SCORE"   — current live score; eventData: { "minigameId": int, "score": int }
 *   "MINIGAME_END"     — minigame finished; eventData: { "minigameId": int, "finalScore": int }
 *   (further types added per Phase 6 special actors)
 *
 * Authority:
 *   Any client may send ROOM_EVENT, but in practice the Room Master is the
 *   authoritative sender for score/state changes.  Clients that receive an
 *   event echo apply it exactly once.
 *
 * Payload structure:
 *   {
 *     "type":      "ROOM_EVENT",
 *     "clientId":  <sender>,          // auto-injected by SendJsonToRemote
 *     "sceneNum":  <int>,
 *     "roomNum":   <int>,
 *     "eventType": <string>,          // "MINIGAME_START", "MINIGAME_SCORE", ...
 *     "eventKey":  <string>,          // stable per-instance key within room
 *     "eventData": { ... }            // optional, type-specific payload
 *   }
 */

// ─────────────────────────────────────────────────────────────────────────────
// Send
// ─────────────────────────────────────────────────────────────────────────────

void Anchor::SendPacket_RoomEvent(const std::string& eventType,
                                  const std::string& eventKey,
                                  const nlohmann::json& eventData,
                                  bool streaming) {
    if (!IsSaveLoaded() || !gPlayState) return;

    const bool isBossEvent = IsBossSyncEventType(eventType);
    if (isBossEvent && !IsRoomMaster()) {
        return;
    }

    nlohmann::json payload;
    payload["type"]      = ROOM_EVENT;
    payload["sceneNum"]  = gPlayState->sceneNum;
    payload["roomNum"]   = gPlayState->roomCtx.curRoom.num;
    payload["eventType"] = eventType;
    payload["eventKey"]  = eventKey;
    if (!eventData.empty()) {
        payload["eventData"] = eventData;
    }
    if (isBossEvent) {
        // Boss events are ordered via monotonic sequence. If caller did not
        // supply one, use current frame as stable local fallback.
        payload["seq"] = eventData.value("seq", (uint32_t)gPlayState->state.frames);
        payload["masterFrame"] = eventData.value("masterFrame", (uint32_t)gPlayState->state.frames);
    }
    if (streaming) {
        // streaming=true: receiver skips idempotency check so every update is applied.
        // Also sets quiet=true to suppress debug spam for high-frequency events.
        payload["streaming"] = true;
        payload["quiet"]     = true;
    }

    SPDLOG_INFO("[Anchor:EventSync] SEND {} | key={} | scene=0x{:02x} room={}",
                eventType, eventKey, (int)gPlayState->sceneNum,
                (int)gPlayState->roomCtx.curRoom.num);

    SendJsonToRemote(payload);
}

// ─────────────────────────────────────────────────────────────────────────────
// Handle
// ─────────────────────────────────────────────────────────────────────────────

void Anchor::HandlePacket_RoomEvent(nlohmann::json payload) {
    if (!IsSaveLoaded() || !gPlayState) return;

    const s16         packetScene   = payload.value("sceneNum",  (int)-1);
    const int         packetRoom    = payload.value("roomNum",   -1);
    const std::string eventType     = payload.value("eventType", std::string(""));
    const std::string eventKey      = payload.value("eventKey",  std::string(""));
    const nlohmann::json eventData  = payload.value("eventData", nlohmann::json{});

    const bool        isStreaming   = payload.value("streaming", false);
    const bool        isBossEvent   = IsBossSyncEventType(eventType);

    if (packetScene < 0 || packetRoom < 0 || eventType.empty() || eventKey.empty()) {
        SPDLOG_WARN("[Anchor:EventSync] RECV malformed ROOM_EVENT — ignored");
        return;
    }

    // Only process events for the current scene.
    if (packetScene != gPlayState->sceneNum) return;

    if (isBossEvent && !roomState.syncEnemies) return;

    if (isBossEvent && payload.contains("clientId")) {
        const uint32_t senderClientId = payload.value("clientId", 0u);
        const std::string roomKey = BuildRoomKey(packetScene, (s8)packetRoom);
        uint32_t roomMasterId = roomState.ownerClientId;
        auto roomIt = roomAuthority.find(roomKey);
        if (roomIt != roomAuthority.end() && roomIt->second != 0) {
            roomMasterId = roomIt->second;
        }
        if (senderClientId != 0 && roomMasterId != 0 && senderClientId != roomMasterId) {
            SPDLOG_WARN("[Anchor:BossSync] Ignore boss event from non-master sender={} expectedMaster={} type={}",
                        senderClientId, roomMasterId, eventType);
            return;
        }
    }

    // Idempotency check (skipped for streaming events like MINIGAME_SCORE).
    const std::string dedupKey = std::to_string(packetScene) + "_" +
                                 std::to_string(packetRoom)  + "_" +
                                 eventType                   + "_" +
                                 eventKey;
    if (!isStreaming && !isBossEvent) {
        if (processedRoomEvents.count(dedupKey)) {
            // Duplicate delivery — silently drop.
            return;
        }
        processedRoomEvents.insert(dedupKey);
    }

    if (!isStreaming) {
        SPDLOG_INFO("[Anchor:EventSync] RECV {} | key={} | scene=0x{:02x} room={}",
                    eventType, eventKey, (int)packetScene, packetRoom);
    }

    bool adapterHandled = false;

    if (isBossEvent) {
        std::string bossActorKey = eventData.value("bossActorKey", std::string(""));
        if (bossActorKey.empty()) {
            bossActorKey = eventKey;
        }

        uint32_t seq = payload.value("seq", eventData.value("seq", (uint32_t)0));
        uint32_t masterFrame = payload.value("masterFrame", eventData.value("masterFrame", (uint32_t)0));

        const std::string roomBossKey = BuildRoomKey(packetScene, (s8)packetRoom) + "_" + bossActorKey;
        // Sequence dedup: seq==0 (frame-0 edge case) counts as seq=1 to prevent bypass.
        // Any event at frame 0 gets treated as strictly ordered sequence 1.
        const uint32_t effectiveSeq = (seq == 0) ? 1u : seq;
        {
            uint32_t& lastSeq = lastBossEventSeqByKey[roomBossKey];
            if (effectiveSeq <= lastSeq) {
                return;
            }
            lastSeq = effectiveSeq;
        }

        nlohmann::json& state = bossSnapshotStateByKey[roomBossKey];
        state["bossActorKey"] = bossActorKey;
        state["sceneNum"] = packetScene;
        state["roomNum"] = packetRoom;
        state["lastEventType"] = eventType;
        state["lastSeq"] = seq;
        state["masterFrame"] = masterFrame;
        if (eventData.contains("phaseId"))       state["phaseId"]       = eventData["phaseId"];
        if (eventData.contains("subState"))      state["subState"]      = eventData["subState"];
        if (eventData.contains("invuln"))        state["invuln"]        = eventData["invuln"];
        if (eventData.contains("weakpointMask")) state["weakpointMask"] = eventData["weakpointMask"];
        // hp and bossActorId are critical for master-migration: if this client
        // later becomes master, it must be able to send a correct bossStates
        // snapshot to late joiners. Without hp, ApplyBossSnapshot uses default=1.
        if (eventData.contains("hp"))            state["hp"]            = eventData["hp"];
        if (eventData.contains("bossActorId"))   state["bossActorId"]   = eventData["bossActorId"];
        state["lateJoinCanSkipIntro"] = eventData.value("lateJoinCanSkipIntro", true);

        const s16 bossActorId = (s16)eventData.value("bossActorId", (int)-1);
        if (bossActorId >= 0) {
            adapterHandled = AnchorBossSync::ApplyBossEvent(gPlayState, packetScene, bossActorId, payload);
        }
    }

    // ── Dispatch per event type ──────────────────────────────────────────────

    if (eventType == "MINIGAME_START") {
        // All clients in the same room enter the same minigame state so their
        // HUD score counter and end-of-round dialog show consistent results.
        const u16 minigameId = (u16)eventData.value("minigameId", 0);
        SPDLOG_INFO("[Anchor:EventSync] MINIGAME_START | minigameId={}", (int)minigameId);
        if (minigameId != 0 && roomState.syncMinigames) {
            gSaveContext.minigameState = minigameId;
            gSaveContext.minigameScore = 0;  // reset score for a clean start
            // Kurze Notification: Spieler weiss, dass das Minispiel gestartet wurde.
            Notification::Emit({
                .prefix      = "Minispiel",
                .prefixColor = ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                .message     = "gestartet!",
                .remainingTime = 3.0f,
            });
        }

    } else if (eventType == "MINIGAME_SCORE") {
        // Streaming live score — applied directly so every participant sees the
        // same running total.  The authority already owns the value; clients receive
        // and mirror it so the end-of-round NPC dialog matches for everyone.
        const u16 score = (u16)eventData.value("score", 0);
        if (roomState.syncMinigames) {
            gSaveContext.minigameScore = score;
        }

    } else if (eventType == "MINIGAME_END") {
        // Authoritative final score.  Ensures the NPC dialog ("You scored X!") shows
        // the same number for all players regardless of local counting differences.
        const u16  finalScore = (u16)eventData.value("finalScore", 0);
        const bool won        = eventData.value("won", false);
        SPDLOG_INFO("[Anchor:EventSync] MINIGAME_END | minigameId={} finalScore={} won={}",
                    eventData.value("minigameId", -1), (int)finalScore, won);
        if (roomState.syncMinigames) {
            gSaveContext.minigameScore = finalScore;
            gSaveContext.minigameState = 0;  // clear active state flag
            // Ergebnis-Notification fuer alle Nicht-Master-Clients.
            Notification::Emit({
                .prefix      = won ? "Minispiel gewonnen!" : "Minispiel beendet",
                .prefixColor = won ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                                   : ImVec4(0.55f, 0.55f, 0.55f, 1.0f),
                .message     = "Endpunktzahl: " + std::to_string((int)finalScore),
                .remainingTime = 5.0f,
            });
        }

    } else if (eventType == "MINIGAME_PROXY_STATE") {
        // Non-master clients proxy their local minigame RAM state to the room
        // master (needed for actors/minigames that don't mutate the master's
        // local state directly, e.g. diving-game counters). The master then
        // emits canonical MINIGAME_* events in HookHandlers.
        if (!roomState.syncMinigames) return;
        if (!IsRoomMaster()) return;

        const u16 minigameId = (u16)eventData.value("minigameId", 0);
        const u16 score      = (u16)eventData.value("score", 0);

        gSaveContext.minigameState = minigameId;
        gSaveContext.minigameScore = score;

    } else if (eventType == "RUTO_CARRY_STATE") {
        // Streamed carry pose for En_Ru1 inside Jabu-Jabu so remote clients do
        // not lose Ruto or desync her pickup/drop visual state.
        if (gPlayState->sceneNum != SCENE_JABU_JABU) return;

        const bool carrying = eventData.value("carrying", false);
        const f32 x = eventData.value("x", 0.0f);
        const f32 y = eventData.value("y", 0.0f);
        const f32 z = eventData.value("z", 0.0f);

        EnRu1* ruto = nullptr;
        Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head;
        while (actor != nullptr) {
            if (actor->id == ACTOR_EN_RU1) {
                ruto = (EnRu1*)actor;
                break;
            }
            actor = actor->next;
        }

        // Being carried sets room=-1 below (global, survives room transitions),
        // but a client whose local room already unloaded before that applied —
        // or who joined/transitioned after the fact — has no Ruto actor at all
        // anymore. Respawn her at the carried position so she never vanishes.
        if (!ruto && carrying) {
            const s16 params = (s16)eventData.value("params", 0);
            Actor* spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_RU1,
                                         x, y, z, 0, 0, 0, params);
            ruto = (EnRu1*)spawned;
        }
        if (!ruto) return;

        if (carrying) {
            ruto->actor.room = -1;
            ruto->actor.world.pos.x = x;
            ruto->actor.world.pos.y = y;
            ruto->actor.world.pos.z = z;
            ruto->actor.world.rot.y = (s16)eventData.value("rotY", (int)ruto->actor.world.rot.y);
            ruto->actor.shape.rot.y = ruto->actor.world.rot.y;
            ruto->actor.velocity.x = 0.0f;
            ruto->actor.velocity.y = 0.0f;
            ruto->actor.velocity.z = 0.0f;
            ruto->actor.speedXZ = 0.0f;
            ruto->actor.gravity = 0.0f;
            ruto->actor.minVelocityY = 0.0f;
            ruto->action = 31;
            ruto->drawConfig = 1;
        }

    } else if (eventType == "PUZZLE_SWITCH_SOLVED") {
        // One-shot room-local solve state for link-movable push-block puzzles.
        // Only clients inside the same room apply the switch flag.
        if (!roomState.syncBGObjects) return;
        if ((s8)packetRoom != gPlayState->roomCtx.curRoom.num) return;

        const s16 switchFlag = (s16)eventData.value("switchFlag", -1);
        if (switchFlag < 0 || switchFlag > 0x3F) return;

        if (!Flags_GetSwitch(gPlayState, switchFlag)) {
            Flags_SetSwitch(gPlayState, switchFlag);
        }

    } else if (eventType == "WEB_BURNED") {
        // One-shot: a player burned a Deku Tree spider web (BgYdanSp).
        // The eventKey is the web's actorKey.  Find the web and set its
        // destroyed switch flag.  HookHandlers already applies
        // BgYdanSp_BurnWeb(...) when this switch is set, so we avoid directly
        // calling non-exported actor internals from this packet layer.
        // This works regardless of syncItemsAndFlags.
        Actor* webActor = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head;
        while (webActor != nullptr) {
            if (webActor->id == ACTOR_BG_YDAN_SP &&
                GetActorKey(webActor, gPlayState->sceneNum) == eventKey) {
                BgYdanSp* web = (BgYdanSp*)webActor;
                const s16 switchFlag = web->isDestroyedSwitchFlag;
                if (switchFlag >= 0 && switchFlag <= 0x3F &&
                    !Flags_GetSwitch(gPlayState, switchFlag)) {
                    Flags_SetSwitch(gPlayState, switchFlag);
                }
                break;
            }
            webActor = webActor->next;
        }
    } else if (eventType == "JABU_ACTOR_SPAWN") {
        // Room-master authoritative spawn relay for Jabu-specific scripted actors
        // that may not spawn reliably on non-masters due to local cutscene/script
        // divergence (Big Octo + electrified tentacles).
        if (gPlayState->sceneNum != SCENE_JABU_JABU) return;
        if (IsRoomMaster()) return;

        s16 actorId = (s16)eventData.value("actorId", (int)-1);
        s16 params = (s16)eventData.value("params", 0);
        if (actorId != ACTOR_EN_BIGOKUTA && actorId != ACTOR_EN_BX) return;

        Vec3f pos = {
            eventData.value("x", 0.0f),
            eventData.value("y", 0.0f),
            eventData.value("z", 0.0f),
        };
        Vec3s rot = {
            (s16)eventData.value("rotX", 0),
            (s16)eventData.value("rotY", 0),
            (s16)eventData.value("rotZ", 0),
        };

        // Dedup by proximity + params to avoid duplicate spawns when local
        // scripts already created the actor naturally.
        for (int cat : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
            Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
            while (actor != nullptr) {
                if (actor->id == actorId && actor->params == params &&
                    (actor->room == packetRoom || actor->room == -1)) {
                    if (Math3D_Vec3fDistSq(&actor->world.pos, &pos) < (120.0f * 120.0f)) {
                        return;
                    }
                }
                actor = actor->next;
            }
        }

        Actor* spawned = nullptr;
        if (actorId == ACTOR_EN_BIGOKUTA) {
            // BgBdanObjects (the Octo platform, params==0) drives its own state
            // machine entirely through `this->dyna.actor.child` (checks
            // child != NULL and reads/writes child->params) — a top-level
            // Actor_Spawn never links back to the platform, so the platform
            // stays stuck forever in WaitForBigOctoToStartBattle on this client.
            Actor* platform = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head;
            while (platform != nullptr) {
                if (platform->id == ACTOR_BG_BDAN_OBJECTS && platform->params == 0 &&
                    (platform->room == packetRoom || platform->room == -1)) {
                    break;
                }
                platform = platform->next;
            }
            if (platform != nullptr) {
                spawned = Actor_SpawnAsChild(&gPlayState->actorCtx, platform, gPlayState, actorId,
                                             pos.x, pos.y, pos.z, rot.x, rot.y, rot.z, params);
            }
        }
        if (spawned == nullptr) {
            spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState, actorId,
                                  pos.x, pos.y, pos.z,
                                  rot.x, rot.y, rot.z,
                                  params);
        }
        if (spawned) {
            spawned->room = packetRoom;
        }

    } else if (eventType == "OCARINA_SONG_ACTION") {
        // Another client played an ocarina song successfully. Apply the same
        // song-action result locally on EVERY receiving client (not just the
        // master): the master needs it to drive frozen BG actors (waterfall,
        // Jabu-Jabu, temple triggers), while every other client needs it so
        // unfrozen NPCs (e.g. Darunia reacting to Saria's Song) react too —
        // otherwise only the player who physically played the song (via native
        // local engine logic) and the master ever see the reaction.
        if (!gPlayState) return;
        u16 remoteMode   = (u16)eventData.value("ocarinaMode",    0);
        u16 remoteAction = (u16)eventData.value("ocarinaAction",  0);
        u8  remoteSong   = (u8)eventData.value("lastPlayedSong",  0);
        SPDLOG_INFO("[Anchor:OcarinaSync] applying remote song | mode={} action={} song={}",
                    (int)remoteMode, (int)remoteAction, (int)remoteSong);
        gPlayState->msgCtx.ocarinaMode    = (u16)remoteMode;
        gPlayState->msgCtx.ocarinaAction  = remoteAction;
        gPlayState->msgCtx.lastPlayedSong = remoteSong;
        suppressOcarinaRebroadcast = true;
        GameInteractor_ExecuteOnOcarinaSongAction();
        suppressOcarinaRebroadcast = false;

    } else if (eventType == "OCARINA_TAG_COMPLETED") {
        // Direct replication of a successfully-completed EnOkarinaTag check
        // (e.g. Zora's River waterfall) for every peer within sync range — see
        // the send-side comment in HookHandlers.cpp for why OCARINA_SONG_ACTION
        // alone isn't enough here.
        if (!gPlayState) return;
        Vec3f pos = {
            eventData.value("x", 0.0f),
            eventData.value("y", 0.0f),
            eventData.value("z", 0.0f),
        };
        const s16 switchFlag = (s16)eventData.value("switchFlag", -1);
        const s16 type = (s16)eventData.value("type", 0);
        // Water Temple water level flags (0x1C-0x1E) are global to the temple:
        // apply regardless of distance, the level is not position-local.
        const bool isWaterTempleLevel = gPlayState->sceneNum == SCENE_WATER_TEMPLE && switchFlag >= 0x1C &&
                                        switchFlag <= 0x1E;
        if (roomState.syncRadius > 0 && !isWaterTempleLevel) {
            Player* player = GET_PLAYER(gPlayState);
            if (!player) return;
            const f32 rSq = (f32)roomState.syncRadius * (f32)roomState.syncRadius;
            if (Math3D_Vec3fDistSq(&player->actor.world.pos, &pos) > rSq) return;
        }

        if (isWaterTempleLevel) {
            // The three level flags are mutually exclusive; BgMizuWater prefers 0x1C, then 0x1D, then 0x1E,
            // so stale flags would give the wrong level when the water actor was not loaded.
            for (s16 f = 0x1C; f <= 0x1E; f++) {
                if (f != switchFlag && Flags_GetSwitch(gPlayState, f)) {
                    Flags_UnsetSwitch(gPlayState, f);
                }
            }
        }
        if (switchFlag >= 0 && !Flags_GetSwitch(gPlayState, switchFlag)) {
            Flags_SetSwitch(gPlayState, switchFlag);
        }
        if (type == 1) { // Zora's River Waterfall
            Flags_SetEventChkInf(EVENTCHKINF_OPENED_ZORAS_DOMAIN);
        } else if (type == 6 && !Flags_GetEventChkInf(EVENTCHKINF_DESTROYED_ROYAL_FAMILY_TOMB)) {
            // Royal Family's Tomb (Sun's Song): the explosion is normally driven
            // entirely by the master's own local cutscene context (csCtx), which
            // never runs on a peer who didn't trigger it — force the same
            // explosion/flag directly on the matching BG actor so every nearby
            // player sees the grave open, not just whoever played the song.
            Actor* tombActor = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head;
            while (tombActor != nullptr) {
                if (tombActor->id == ACTOR_BG_SPOT02_OBJECTS && tombActor->params == 2) {
                    BgSpot02Objects_ForceExplodeRoyalTomb((BgSpot02Objects*)tombActor, gPlayState);
                    break;
                }
                tombActor = tombActor->next;
            }
        }
        Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);

    } else if (eventType == "DDAN_KD_LOWER") {
        // Dodongo's Cavern stairs: start the lowering animation without the cutscene.
        if (!gPlayState) return;
        for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head; a != nullptr; a = a->next) {
            if (a->id == ACTOR_BG_DDAN_KD && GetActorKey(a, gPlayState->sceneNum) == eventKey) {
                BgDdanKd_ForceLower((BgDdanKd*)a, gPlayState);
                break;
            }
        }

    } else if (eventType == "DODOAGO_EYE") {
        // Dodongo head: a remote client lit an eye; mirror it locally.
        if (!gPlayState) return;
        const int eye = eventData.value("eye", -1);
        for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head; a != nullptr; a = a->next) {
            if (a->id == ACTOR_BG_DODOAGO) {
                if (BgDodoago_IsWaiting((BgDodoago*)a)) {
                    BgDodoago_ForceLightEye((BgDodoago*)a, gPlayState, eye);
                }
                break;
            }
        }

    } else if (eventType == "TIMEBLOCK_SONG_COMPLETE") {
        // See send-side comment in HookHandlers.cpp: ObjTimeblock's
        // songObserverFunc only advances for the client whose own player is
        // physically in range and attempting to play — force the same
        // completion locally for every peer within sync range.
        if (!gPlayState) return;
        Vec3f pos = {
            eventData.value("x", 0.0f),
            eventData.value("y", 0.0f),
            eventData.value("z", 0.0f),
        };
        if (roomState.syncRadius > 0) {
            Player* player = GET_PLAYER(gPlayState);
            if (!player) return;
            const f32 rSq = (f32)roomState.syncRadius * (f32)roomState.syncRadius;
            if (Math3D_Vec3fDistSq(&player->actor.world.pos, &pos) > rSq) return;
        }
        Actor* blockActor = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head;
        while (blockActor != nullptr) {
            if (blockActor->id == ACTOR_OBJ_TIMEBLOCK &&
                fabsf(blockActor->world.pos.x - pos.x) < 1.0f &&
                fabsf(blockActor->world.pos.y - pos.y) < 1.0f &&
                fabsf(blockActor->world.pos.z - pos.z) < 1.0f) {
                ObjTimeblock_ForceSongComplete((ObjTimeblock*)blockActor, gPlayState);
                break;
            }
            blockActor = blockActor->next;
        }

    } else if (eventType == "BOSS_DEATH_COMMIT") {
        // Set hp=0 so the boss's own AI triggers its native death sequence
        // (animation, sound, cutscene). This matches the master's visual.
        // We do NOT call Actor_Kill here — the definitive kill arrives later
        // via the ACTOR_KILLED packet emitted by the master's OnActorKill hook
        // once its own death sequence completes.
        // If the boss AI on this client also calls Actor_Kill (normal for OoT bosses),
        // the subsequent ACTOR_KILLED is a safe no-op on an already-dead actor.
        // CI guardrail requires explicit adapter/fallback split. For DEATH_COMMIT
        // we intentionally force fallback handling because adapters only clamp HP.
        adapterHandled = false;
        if (adapterHandled) return;
        std::string bossActorKey = eventData.value("bossActorKey", eventKey);
        if (bossActorKey.empty()) return;

        // HP only. The native death sequence is driven by BOSS_DEFEAT_START (adapter-originated
        // commits can be spurious, so no cutscene is forced here).
        for (int cat : { ACTORCAT_BOSS, ACTORCAT_ENEMY }) {
            Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
            while (actor != nullptr) {
                if (BossKeyMatches(actor, gPlayState->sceneNum, bossActorKey)) {
                    if (actor->update != nullptr && actor->colChkInfo.health != 0) {
                        actor->colChkInfo.health = 0;
                    }
                    return;
                }
                actor = actor->next;
            }
        }

    } else if (eventType == "BOSS_DEFEAT_START") {
        // Master reports the boss entered its native death sequence. Force the same sequence
        // locally (the hit callback never runs on a non-authority client).
        if (IsRoomMaster()) return;
        const std::string bossActorKey = eventData.value("bossActorKey", eventKey);
        const int defeatActorId = eventData.value("defeatActorId", -1);
        const int defeatParams = eventData.value("defeatParams", 0);
        if (bossActorKey.empty() || defeatActorId < 0) return;

        Actor* target = nullptr;
        Actor* fallback = nullptr;
        for (int cat : { ACTORCAT_BOSS, ACTORCAT_ENEMY }) {
            for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
                if (a->id != defeatActorId || a->update == nullptr) continue;
                if (BossKeyMatches(a, gPlayState->sceneNum, bossActorKey)) { target = a; break; }
                if (a->params == defeatParams && fallback == nullptr) fallback = a;
            }
            if (target) break;
        }
        if (target == nullptr) target = fallback;
        if (target == nullptr) {
            SPDLOG_WARN("[Anchor:BossSync] CLIENT: BOSS_DEFEAT_START actor not found | key={} id={}", bossActorKey,
                        defeatActorId);
            return;
        }

        s32 applied = 0;
        switch (target->id) {
            case ACTOR_BOSS_GOMA:
                applied = BossGoma_NetworkForceDefeat((BossGoma*)target, gPlayState);
                break;
            case ACTOR_BOSS_GANONDROF:
                applied = BossGanondrof_NetworkForceDefeat((BossGanondrof*)target, gPlayState);
                break;
            case ACTOR_BOSS_GANON:
                applied = BossGanon_NetworkForceDefeat((BossGanon*)target, gPlayState);
                break;
            case ACTOR_BOSS_SST:
                applied = BossSst_NetworkForceDefeat((BossSst*)target, gPlayState);
                break;
            case ACTOR_BOSS_TW:
                applied = BossTw_NetworkForceDefeat((BossTw*)target, gPlayState);
                break;
            default:
                break;
        }
        if (applied) {
            // Register under the ACTOR_KILLED key so the later kill is deferred.
            bossDefeatStartedKeys.insert(GetActorKey(target, gPlayState->sceneNum));
        }
        SPDLOG_INFO("[Anchor:BossSync] CLIENT: BOSS_DEFEAT_START | key={} id={} applied={}", bossActorKey,
                    (int)target->id, (int)applied);

    } else if (eventType == "GND_PAINTING_SELECT") {
        // Force the SAME real/fake painting choice the master already rolled —
        // ACTOR_EN_FHG is ACTORCAT_BG and is never frozen, so every client
        // would otherwise pick its own random painting independently. See
        // sPaintings/oppositeIndex in z_en_fhg.c — duplicated here since that
        // table is file-static and not exported.
        if (IsRoomMaster()) return; // master already applied its own roll natively
        const std::string hostHorseKey = eventData.value("bossActorKey", eventKey);
        const int realIdx = eventData.value("realIdx", -1);
        const int fakeIdx = eventData.value("fakeIdx", -1);
        if (realIdx < 0 || realIdx > 5) return;

        struct GndPaintingSlot { Vec3f pos; s16 yRot; };
        static const GndPaintingSlot kGndPaintings[6] = {
            { { 0.0f, 60.0f, -315.0f }, 0x0000 },   { { -260.0f, 60.0f, -145.0f }, 0x2AAA },
            { { -260.0f, 60.0f, 165.0f }, 0x5554 }, { { 0.0f, 60.0f, 315.0f }, 0x7FFE },
            { { 260.0f, 60.0f, 155.0f }, 0xAAA8 },  { { 260.0f, 60.0f, -155.0f }, 0xD552 },
        };
        constexpr f32 kBossroomCenterX = 14.0f;
        constexpr f32 kBossroomCenterY = -33.0f;
        constexpr f32 kBossroomCenterZ = -3315.0f;
        constexpr s16 kGndRealBossParam = 1;
        constexpr s16 kGndFakeBossBase = 10;

        auto applyPaintingPose = [&](Actor* horse, s16 paintingIdx) {
            const GndPaintingSlot& slot = kGndPaintings[paintingIdx];
            horse->world.pos.x = (1.3f * slot.pos.x) + (kBossroomCenterX - 4.0f);
            horse->world.pos.y = slot.pos.y + (kBossroomCenterY + 153.0f);
            horse->world.pos.z = (1.3f * slot.pos.z) - -(kBossroomCenterZ - 10.0f);
            horse->shape.rot.y = horse->world.rot.y = slot.yRot;
            *(int16_t*)((char*)horse + 0x01C2) = paintingIdx;                  // curPainting
            *(int16_t*)((char*)horse + 0x01C4) = (s16)((paintingIdx + 3) % 6); // targetPainting
        };

        Actor* realHorse = nullptr;
        Actor* node = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head;
        while (node != nullptr) {
            if (node->id == ACTOR_EN_FHG && node->params == kGndRealBossParam &&
                GetActorKey(node, gPlayState->sceneNum) == hostHorseKey) {
                realHorse = node;
                break;
            }
            node = node->next;
        }
        if (!realHorse || realHorse->update == nullptr) return;
        applyPaintingPose(realHorse, (s16)realIdx);

        if (fakeIdx >= 0 && fakeIdx <= 5) {
            // actor->child is a single pointer (last spawned child), not a
            // sibling list: the decoy rider is the real horse's child, and the
            // decoy horse is in turn the decoy rider's own child (see
            // BossGanondrof_Init).
            Actor* fakeRider = realHorse->child;
            if (fakeRider != nullptr && fakeRider->id == ACTOR_BOSS_GANONDROF &&
                fakeRider->params >= kGndFakeBossBase) {
                fakeRider->params = (s16)(fakeIdx + kGndFakeBossBase);
                Actor* fakeHorse = fakeRider->child;
                if (fakeHorse != nullptr && fakeHorse->id == ACTOR_EN_FHG && fakeHorse->update != nullptr) {
                    fakeHorse->params = (s16)(fakeIdx + kGndFakeBossBase);
                    applyPaintingPose(fakeHorse, (s16)fakeIdx);
                }
            }
        }

    } else if (eventType == "BOSS_SUBACTOR_KILL") {
        std::string targetActorKey = eventData.value("targetActorKey", std::string(""));
        if (targetActorKey.empty()) return;

        for (int cat = 0; cat < ACTORCAT_MAX; ++cat) {
            Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
            while (actor != nullptr) {
                if (BossKeyMatches(actor, gPlayState->sceneNum, targetActorKey)) {
                    // Mini-bosses (Big Octo) keep their native death animation.
                    if (actor->update != nullptr && !eventData.value("isMiniBoss", false)) {
                        Actor_Kill(actor);
                    }
                    return;
                }
                actor = actor->next;
            }
        }

    } else if (eventType == "BOSS_STAGE_ENTER" ||
               eventType == "BOSS_WEAKPOINT_HIT" ||
               eventType == "BOSS_WEAKPOINT_DESTROY" ||
               eventType == "BOSS_SUBACTOR_SPAWN" ||
               eventType == "BOSS_INVULN_SET" ||
               eventType == "BOSS_ACTION_STATE") {
        // Infrastructure-only phase: state is already cached above in
        // bossSnapshotStateByKey. Concrete boss adapters are added incrementally.
        // BOSS_ACTION_STATE is fully handled by the adapter's ApplyEvent above.
        return;

    } else if (eventType == "BOSS_CUTSCENE_GATE") {
        // Generic gate for boss intros/intermissions. If skipIntro is true,
        // late joiners and in-room clients can immediately leave the cutscene.
        const bool skipIntro = eventData.value("skipIntro", false);
        if (skipIntro && gPlayState->csCtx.state != CS_STATE_IDLE) {
            func_8006450C(gPlayState, &gPlayState->csCtx);
        }
        return;

    } else {
        // Unknown or future event type — warn but don't crash.
        SPDLOG_WARN("[Anchor:EventSync] Unknown eventType '{}' — ignored", eventType);
    }
}
