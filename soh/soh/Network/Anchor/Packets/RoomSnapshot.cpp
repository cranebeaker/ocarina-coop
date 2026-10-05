#include "soh/Network/Anchor/Anchor.h"
#include "soh/Network/Anchor/BossSync/BossSyncDispatch.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace {
bool IsRollingBoulderActor(s16 actorId) {
    return actorId == ACTOR_EN_BW || actorId == ACTOR_EN_GOROIWA;
}

bool IsBossScene(s16 sceneNum) {
    switch (sceneNum) {
        case SCENE_DEKU_TREE_BOSS:
        case SCENE_DODONGOS_CAVERN_BOSS:
        case SCENE_JABU_JABU_BOSS:
        case SCENE_FOREST_TEMPLE_BOSS:
        case SCENE_FIRE_TEMPLE_BOSS:
        case SCENE_WATER_TEMPLE_BOSS:
        case SCENE_SPIRIT_TEMPLE_BOSS:
        case SCENE_SHADOW_TEMPLE_BOSS:
        case SCENE_GANONDORF_BOSS:
        case SCENE_GANON_BOSS:
            return true;
        default:
            return false;
    }
}

bool IsLinkPuzzleSwitchActor(const Actor* actor) {
    if (actor == nullptr) {
        return false;
    }

    return actor->id == ACTOR_OBJ_OSHIHIKI || actor->id == ACTOR_OBJ_MAKEOSHIHIKI;
}

bool TryExtractLinkPuzzleSwitchFlag(const Actor* actor, s16* outSwitchFlag) {
    if (!IsLinkPuzzleSwitchActor(actor) || outSwitchFlag == nullptr) {
        return false;
    }

    const s16 switchFlag = (actor->params >> 8) & 0x3F;
    if (switchFlag < 0 || switchFlag > 0x3F) {
        return false;
    }

    *outSwitchFlag = switchFlag;
    return true;
}
}

/**
 * ROOM_SNAPSHOT
 *
 * One-time packet sent by the room master to a client that just entered the
 * same scene/room.  Synchronises all enemy state so the late joiner starts with
 * correct HP, positions, draw state, and alive/dead status instead of freshly-
 * spawned ROM defaults.
 *
 * Additionally carries a "brMatchState" block so a client joining mid-match
 * immediately knows the BR match is active, which players are eliminated, which
 * are wanted, and how much start-protection time remains.  Without this a late
 * joiner would see stale brMatchActive=false and could deal/receive PvP damage
 * during the start-protection window.
 *
 * Fields per enemy entry:
 *   key         – composite actorKey (scene+cat+id+room+params+homePos+homeRot)
 *   hp          – current health
 *   posX/Y/Z    – world position
 *   rotY        – world Y rotation
 *   shapeRotY   – shape (visual) Y rotation
 *   alive       – false if Actor_Kill has already been called (update == nullptr)
 *   drawEnabled – false if the enemy is currently hidden (draw == nullptr),
 *                 e.g. a Deku Scrub underground
 *
 * Flow:
 *   1. New client sends ROOM_JOIN.
 *   2. Host broadcasts ROOM_MASTER_ASSIGN (with joiningClientId).
 *   3. Room master receives it, detects joiningClientId != self, calls
 *      SendPacket_RoomSnapshot(joiningClientId).
 *   4. All peers receive the broadcast; only targetClientId applies it.
 *   5. Dead actors are killed in a second pass to keep linked-list traversal safe.
 *   6. Hidden actors' draw functions are suppressed via savedEnemyDrawFuncs so
 *      they can be cleanly restored when the authority un-hides them.
 */

// ─── Send ─────────────────────────────────────────────────────────────────────

void Anchor::SendPacket_RoomSnapshot(uint32_t targetClientId) {
    if (!IsSaveLoaded() || !gPlayState) return;
    if (!IsRoomMaster()) return;

    const s16 sceneNum = gPlayState->sceneNum;
    const s8  curRoom  = (s8)gPlayState->roomCtx.curRoom.num;

    nlohmann::json enemies = nlohmann::json::array();

    for (int cat : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
        Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            // Include actors that belong to the current room.
            // Boss actors in boss scenes have room == -1 (global actor), so they
            // would be excluded by a strict curRoom check and never appear in the
            // snapshot. Include all global ACTORCAT_BOSS actors unconditionally.
            const bool inRoom = (actor->room == curRoom) ||
                                (cat == ACTORCAT_BOSS && actor->room == -1);
            if (inRoom) {
                nlohmann::json e;
                e["key"]         = GetActorKey(actor, sceneNum);
                e["hp"]          = (int)actor->colChkInfo.health;
                e["posX"]        = actor->world.pos.x;
                e["posY"]        = actor->world.pos.y;
                e["posZ"]        = actor->world.pos.z;
                e["rotY"]        = (int)actor->world.rot.y;
                e["shapeRotY"]   = (int)actor->shape.rot.y;
                // update == nullptr is the standard "dead" marker set by Actor_Kill.
                e["alive"]       = (actor->update != nullptr);
                // draw == nullptr means the enemy is hidden (e.g. Deku Scrub underground).
                e["drawEnabled"] = (actor->draw != nullptr);
                enemies.push_back(e);
            }
            actor = actor->next;
        }
    }

    nlohmann::json payload;
    payload["type"]           = ROOM_SNAPSHOT;
    payload["roomKey"]        = GetCurrentRoomKey();
    payload["targetClientId"] = targetClientId;
    payload["masterFrameNow"] = (uint32_t)gPlayState->state.frames;
    payload["enemies"]        = enemies;

    // ── BG actor snapshot ────────────────────────────────────────────────────
    // Include the LIVE positions of every BG/PROP actor that has moved from its
    // spawn position (i.e. is already being tracked by the authority-side
    // BgKeyframeSync scan in trackedBgActors).  Static objects that have never
    // moved aren't in trackedBgActors and don't need to be sent — the client's
    // locally-simulated spawn position will already be correct.
    nlohmann::json bgObjects = nlohmann::json::array();
    for (int cat : { ACTORCAT_BG, ACTORCAT_PROP }) {
        Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            // Only actors in the current room that are alive AND known to be moving.
            if (actor->room == curRoom && actor->update != nullptr) {
                const std::string key = GetActorKey(actor, sceneNum);
                if (trackedBgActors.count(key)) {
                    nlohmann::json b;
                    b["key"]  = key;
                    b["posX"] = actor->world.pos.x;
                    b["posY"] = actor->world.pos.y;
                    b["posZ"] = actor->world.pos.z;
                    b["rotY"] = (int)actor->world.rot.y;
                    bgObjects.push_back(b);
                }
            }
            actor = actor->next;
        }
    }
    payload["bgObjects"] = bgObjects;

    // ── Rolling boulder snapshot ────────────────────────────────────────────
    // One-shot room-enter alignment: include currently active rolling boulders
    // so late joiners immediately see the same hazard positions as the master.
    nlohmann::json boulders = nlohmann::json::array();
    for (int cat : { ACTORCAT_PROP, ACTORCAT_ENEMY }) {
        Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            if ((actor->room == curRoom || actor->room == -1) && actor->update != nullptr &&
                IsRollingBoulderActor(actor->id)) {
                nlohmann::json b;
                b["sceneNum"] = sceneNum;
                b["roomNum"] = curRoom;
                b["actorKey"] = GetActorKey(actor, sceneNum);
                b["actorId"] = (int)actor->id;
                b["params"] = (int)actor->params;
                b["posX"] = actor->world.pos.x;
                b["posY"] = actor->world.pos.y;
                b["posZ"] = actor->world.pos.z;
                b["rotX"] = (int)actor->world.rot.x;
                b["rotY"] = (int)actor->world.rot.y;
                b["rotZ"] = (int)actor->world.rot.z;
                // Snapshot aligns to "now" on master timeline.
                b["triggerFrame"] = (uint32_t)gPlayState->state.frames;
                boulders.push_back(b);
            }
            actor = actor->next;
        }
    }
    payload["boulders"] = boulders;

    // ── Puzzle switch snapshot (push-block style) ─────────────────────────
    // Late-join safety: include currently solved push-block switch flags so
    // joiners apply solved puzzle state immediately without waiting for a
    // transition event that may have happened before they joined.
    nlohmann::json puzzleSwitches = nlohmann::json::array();
    std::unordered_set<int> seenPuzzleSwitches;
    for (int cat : { ACTORCAT_BG, ACTORCAT_PROP }) {
        Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            if (actor->room != curRoom && actor->room != -1) {
                actor = actor->next;
                continue;
            }

            s16 switchFlag = -1;
            if (!TryExtractLinkPuzzleSwitchFlag(actor, &switchFlag)) {
                actor = actor->next;
                continue;
            }

            if (!Flags_GetSwitch(gPlayState, switchFlag)) {
                actor = actor->next;
                continue;
            }

            if (!seenPuzzleSwitches.insert((int)switchFlag).second) {
                actor = actor->next;
                continue;
            }

            nlohmann::json p;
            p["switchFlag"] = (int)switchFlag;
            p["actorId"] = (int)actor->id;
            p["actorKey"] = GetActorKey(actor, sceneNum);
            puzzleSwitches.push_back(p);
            actor = actor->next;
        }
    }
    payload["puzzleSwitches"] = puzzleSwitches;

    // ── Boss sync snapshot (Phase 1 infrastructure) ───────────────────────
    // Cache contains last known room-local boss events/state by
    // roomBossKey="{scene}_{room}_{bossActorKey}". Send only entries for the
    // current room so a late joiner can continue from the same boss phase.
    nlohmann::json bossStates = nlohmann::json::array();
    const std::string roomPrefix = BuildRoomKey(sceneNum, curRoom) + "_";
    std::unordered_set<std::string> seenBossKeys;
    for (const auto& [roomBossKey, state] : bossSnapshotStateByKey) {
        if (roomBossKey.rfind(roomPrefix, 0) == 0) {
            nlohmann::json entry = state;
            entry["roomBossKey"] = roomBossKey;
            bossStates.push_back(entry);
            seenBossKeys.insert(state.value("bossActorKey", std::string("")));
        }
    }

    // Fallback: if an active boss is present but no explicit event cache exists
    // yet, emit a baseline boss snapshot entry so late joiners still skip intro
    // and enter combat immediately.
    Actor* boss = gPlayState->actorCtx.actorLists[ACTORCAT_BOSS].head;
    while (boss != nullptr) {
        if (boss->update != nullptr && (boss->room == curRoom || boss->room == -1)) {
            const std::string bossActorKey = GetActorKey(boss, sceneNum);
            if (!bossActorKey.empty() && !seenBossKeys.count(bossActorKey)) {
                nlohmann::json entry;
                entry["roomBossKey"] = roomPrefix + bossActorKey;
                entry["bossActorKey"] = bossActorKey;
                entry["bossActorId"] = (int)boss->id;
                entry["sceneNum"] = sceneNum;
                entry["roomNum"] = curRoom;
                entry["lastEventType"] = "BOSS_STAGE_ENTER";
                entry["lastSeq"] = (uint32_t)gPlayState->state.frames;
                entry["masterFrame"] = (uint32_t)gPlayState->state.frames;
                entry["lateJoinCanSkipIntro"] = true;
                entry["hp"] = (int)boss->colChkInfo.health;
                bossStates.push_back(entry);
            }
        }
        boss = boss->next;
    }
    payload["bossStates"] = bossStates;

    // ── Battle Royale match state snapshot ───────────────────────────────────
    // A late joiner must know the current BR state immediately.  We encode:
    //   matchActive              – true if a match is in progress
    //   killStreaks              – object of { clientId -> streak }.  0 = just respawned.
    //   wantedClients            – array of currently-wanted client IDs
    //   startProtectionRemainingMs – milliseconds of start-protection left (0 = none)
    //
    // The receiver reconstructs the exact same flags (brMatchActive,
    // brKillStreak, wantedClients, brStartProtectionUntil) that
    // it would have built up by processing individual BATTLE_ROYALE_EVENT packets.
    // brEliminated is NOT included — it is a transient flag (true only between
    // local PLAYER_KILLED send and PLAYER_ELIM receive) and is always false for
    // a late joiner.
    if (roomState.battleRoyaleMode) {
        nlohmann::json brState;
        brState["matchActive"] = brMatchActive;

        nlohmann::json killStreaksJson;
        for (const auto& [cid, streak] : brKillStreak) {
            killStreaksJson[std::to_string(cid)] = (int)streak;
        }
        brState["killStreaks"] = killStreaksJson;

        nlohmann::json wantedJson = nlohmann::json::array();
        for (uint32_t cid : wantedClients) {
            wantedJson.push_back(cid);
        }
        brState["wantedClients"] = wantedJson;

        // Convert the absolute deadline into a relative "milliseconds remaining"
        // value so the receiver can recreate the same deadline on their clock.
        const auto now   = Clock::now();
        const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               brStartProtectionUntil - now).count();
        brState["startProtectionRemainingMs"] = (ms > 0) ? ms : 0;

        payload["brMatchState"] = brState;
    }

    SPDLOG_INFO("[Anchor] RoomSnapshot: sending {} enemy + {} BG + {} boulder + {} puzzle + {} boss state(s) to client {}",
                enemies.size(), bgObjects.size(), boulders.size(), puzzleSwitches.size(), bossStates.size(), targetClientId);
    SendJsonToRemote(payload);
}

// ─── Handle ───────────────────────────────────────────────────────────────────

void Anchor::HandlePacket_RoomSnapshot(nlohmann::json payload) {
    // Only the designated target client applies this snapshot; all others drop it.
    const uint32_t targetClientId = payload.value("targetClientId", 0u);
    if (targetClientId != ownClientId) return;

    if (!IsSaveLoaded() || !gPlayState) return;

    // If we already moved on to a different room, the snapshot is stale — drop it.
    const std::string roomKey = payload.value("roomKey", "");
    if (roomKey.empty() || roomKey != GetCurrentRoomKey()) return;

    if (payload.contains("masterFrameNow")) {
        uint32_t masterFrameNow = payload.value("masterFrameNow", (uint32_t)0);
        masterFrameToLocalOffset = (int32_t)gPlayState->state.frames - (int32_t)masterFrameNow;
        hasMasterFrameSync = true;
    }

    if (!payload.contains("enemies")) return;

    const auto& enemyArray = payload["enemies"];
    SPDLOG_INFO("[Anchor] RoomSnapshot: applying {} enemy state(s) for room {}", enemyArray.size(), roomKey);

    // Collect actors to kill in a separate list so the linked-list traversal
    // is not invalidated by Actor_Kill mid-loop.
    std::vector<Actor*> toKill;

    for (const auto& e : enemyArray) {
        const std::string key         = e.value("key", "");
        const u8          hp          = (u8)e.value("hp", 0);
        const float       posX        = e.value("posX", 0.0f);
        const float       posY        = e.value("posY", 0.0f);
        const float       posZ        = e.value("posZ", 0.0f);
        const s16         rotY        = (s16)e.value("rotY", 0);
        const s16         shapeRotY   = (s16)e.value("shapeRotY", (int)rotY);
        const bool        alive       = e.value("alive", true);
        const bool        drawEnabled = e.value("drawEnabled", true); // backward-compat default = visible

        if (key.empty()) continue;

        for (int cat : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
            Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
            while (actor != nullptr) {
                if (GetActorKey(actor, gPlayState->sceneNum) == key) {
                    if (!alive) {
                        // Schedule for killing — only if not already dead.
                        if (actor->update != nullptr) {
                            toKill.push_back(actor);
                        }
                    } else {
                        // Apply HP, position, and rotation overrides.
                        actor->colChkInfo.health = hp;
                        actor->world.pos.x       = posX;
                        actor->world.pos.y       = posY;
                        actor->world.pos.z       = posZ;
                        actor->world.rot.y       = rotY;
                        actor->shape.rot.y       = shapeRotY;

                        // Draw-state override (Phase 4): suppress rendering for
                        // hidden enemies (e.g. Deku Scrubs underground).
                        // Save the original draw function so it can be restored
                        // by EnemyPositionUpdate when the enemy reappears.
                        // Bosses never use this override, because a false hide state
                        // would make them disappear from the room for everyone.
                        if (actor->category != ACTORCAT_BOSS) {
                            if (!drawEnabled) {
                                if (actor->draw != nullptr) {
                                    savedEnemyDrawFuncs[key] = actor->draw;
                                    actor->draw = nullptr;
                                }
                            } else {
                                auto savedIt = savedEnemyDrawFuncs.find(key);
                                if (savedIt != savedEnemyDrawFuncs.end()) {
                                    actor->draw = savedIt->second;
                                    savedEnemyDrawFuncs.erase(savedIt);
                                }
                            }
                        }

                        // Mark as remote-driven so the non-authority's OnActorUpdate
                        // guard does not echo this HP change back to the master.
                        pendingRemoteHealthOverride[key] = hp;
                    }
                    // actorKey is unique per room — stop scanning this category.
                    break;
                }
                actor = actor->next;
            }
        }
    }

    // Kill collected dead actors after the traversal is complete.
    for (Actor* actor : toKill) {
        Actor_Kill(actor);
    }

    // ── Apply BG actor positions ─────────────────────────────────────────────
    // Populate bgActorKeyframeTarget so the per-frame blend hook immediately
    // starts driving tracked BG actors to the correct snapshot position.
    // Without this, a newly-joined client would see platforms at their spawn
    // position and then "snap" to the real position on the next keyframe packet.
    if (payload.contains("bgObjects") && roomState.syncBGObjects) {
        const auto& bgArray = payload["bgObjects"];
        SPDLOG_INFO("[Anchor] RoomSnapshot: applying {} BG object position(s)", bgArray.size());
        for (const auto& b : bgArray) {
            const std::string key = b.value("key", "");
            if (key.empty()) continue;
            BgKeyframeTarget target;
            target.pos.x = b.value("posX", 0.0f);
            target.pos.y = b.value("posY", 0.0f);
            target.pos.z = b.value("posZ", 0.0f);
            target.rotY  = (s16)b.value("rotY", 0);
            bgActorKeyframeTarget[key] = target;
        }
    }

    if (payload.contains("boulders") && roomState.syncEnemies) {
        const auto& boulderArray = payload["boulders"];
        SPDLOG_INFO("[Anchor] RoomSnapshot: applying {} rolling boulder state(s)", boulderArray.size());
        for (const auto& b : boulderArray) {
            QueueOrApplyBoulderSpawn(b, true);
        }
    }

    if (payload.contains("puzzleSwitches") && roomState.syncBGObjects) {
        const auto& puzzleArray = payload["puzzleSwitches"];
        SPDLOG_INFO("[Anchor] RoomSnapshot: applying {} puzzle switch state(s)", puzzleArray.size());
        for (const auto& p : puzzleArray) {
            const s16 switchFlag = (s16)p.value("switchFlag", -1);
            if (switchFlag < 0 || switchFlag > 0x3F) {
                continue;
            }

            if (!Flags_GetSwitch(gPlayState, switchFlag)) {
                Flags_SetSwitch(gPlayState, switchFlag);
            }
        }
    }

    if (payload.contains("bossStates") && payload["bossStates"].is_array()) {
        const auto& bossStateArray = payload["bossStates"];
        SPDLOG_INFO("[Anchor] RoomSnapshot: applying {} boss state(s)", bossStateArray.size());
        bool anyLateJoinSkip = false;
        for (const auto& state : bossStateArray) {
            if (!state.is_object()) continue;

            std::string roomBossKey = state.value("roomBossKey", std::string(""));
            std::string bossActorKey = state.value("bossActorKey", std::string(""));
            if (roomBossKey.empty() && !bossActorKey.empty()) {
                roomBossKey = GetCurrentRoomKey() + "_" + bossActorKey;
            }
            if (roomBossKey.empty()) continue;

            bossSnapshotStateByKey[roomBossKey] = state;
            uint32_t seq = state.value("lastSeq", (uint32_t)0);
            if (seq != 0) {
                uint32_t& lastSeq = lastBossEventSeqByKey[roomBossKey];
                if (seq > lastSeq) {
                    lastSeq = seq;
                }
            }

            if (state.value("lateJoinCanSkipIntro", true)) {
                anyLateJoinSkip = true;
            }

            // Apply snapshot HP to the live boss actor so the late-joiner sees
            // the correct health immediately — not the freshly-spawned ROM default.
            const s16 bossActorId = (s16)state.value("bossActorId", (int)-1);
            const s16 stateSceneNum = (s16)state.value("sceneNum", (int)gPlayState->sceneNum);
            if (bossActorId >= 0) {
                AnchorBossSync::ApplyBossSnapshot(gPlayState, stateSceneNum, bossActorId, state);
            }
        }

        // Late-joiner QoL: if a boss fight is already in progress, skip any
        // currently running intro/intermission cutscene so the player can
        // immediately participate in combat.
        // EXCEPTION: Forest Temple Boss (Phantom Ganon). Its intro is not a
        // standard cutscene-command sequence — it's driven directly by the
        // BossGanondrof/EnfHG actors' own camera fields (cameraEye/cameraAt/etc.,
        // painting-select, decoy-rider spawn). Force-terminating csCtx mid-way
        // via func_8006450C leaves that actor-driven camera state inconsistent
        // and crashes the client on the very next frame. Skip the forced-skip
        // for this scene; the fight is short enough that a late joiner simply
        // watches the remainder of the intro play out normally.
        if (anyLateJoinSkip && IsBossScene(gPlayState->sceneNum) &&
            gPlayState->sceneNum != SCENE_FOREST_TEMPLE_BOSS &&
            gPlayState->csCtx.state != CS_STATE_IDLE) {
            func_8006450C(gPlayState, &gPlayState->csCtx);
        }
    }


    // ── Restore Battle Royale match state ────────────────────────────────────
    // Rebuild brMatchActive, brKillStreak, wantedClients, brEliminated and
    // brStartProtectionUntil from the snapshot so the late joiner behaves
    // identically to clients that processed each BATTLE_ROYALE_EVENT in order.
    if (payload.contains("brMatchState") && roomState.battleRoyaleMode) {
        const auto& brs = payload["brMatchState"];

        brMatchActive = brs.value("matchActive", false);

        brKillStreak.clear();
        if (brs.contains("killStreaks") && brs["killStreaks"].is_object()) {
            for (auto& [cidStr, streakVal] : brs["killStreaks"].items()) {
                try {
                    const uint32_t cid    = (uint32_t)std::stoul(cidStr);
                    const u8       streak = (u8)streakVal.get<int>();
                    brKillStreak[cid]     = streak;
                } catch (...) {
                    // Malformed entry — skip silently.
                }
            }
        }

        wantedClients.clear();
        if (brs.contains("wantedClients") && brs["wantedClients"].is_array()) {
            for (const auto& cid : brs["wantedClients"]) {
                wantedClients.insert(cid.get<uint32_t>());
            }
        }

        // Reconstruct the start-protection deadline on the local clock.
        const int64_t remainMs = brs.value("startProtectionRemainingMs", (int64_t)0);
        if (remainMs > 0) {
            brStartProtectionUntil = Clock::now() + std::chrono::milliseconds(remainMs);
        } else {
            brStartProtectionUntil = {};
        }

        // brEliminated ist ein transienter Flag (nur true zwischen PLAYER_KILLED-Senden
        // und PLAYER_ELIM-Empfangen). Ein Late Joiner befindet sich nie in diesem
        // Zwischenzustand, daher immer false setzen.
        // Der alte 0xFF-Sentinel wurde entfernt; brKillStreak=0 bedeutet nur Respawn,
        // nicht permanente Eliminierung.
        brEliminated = false;

        SPDLOG_INFO("[Anchor] RoomSnapshot: BR state restored — matchActive={} eliminated={} "
                    "wantedCount={} protectionMs={}",
                    brMatchActive, brEliminated, wantedClients.size(), remainMs);
    }
}
