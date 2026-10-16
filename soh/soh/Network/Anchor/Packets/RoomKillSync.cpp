#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include <limits>

extern "C" {
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace {
struct RoomKillTarget {
    std::string actorKey;
    s16 actorId = -1;
    bool hasApproxPos = false;
    Vec3f approxPos = { 0.0f, 0.0f, 0.0f };
};

RoomKillTarget BuildTargetFromActorKey(const std::string& actorKey) {
    RoomKillTarget target;
    target.actorKey = actorKey;

    // Key format embeds actorId + home.pos as ints (category is NOT part of
    // the key — see GetActorKey's comment in ActorStateUpdate.cpp):
    // scene_actorId_room_params_homeX_homeY_homeZ[_homeRotX_homeRotY_homeRotZ]
    int sceneNum = 0;
    int actorId = 0;
    int roomNum = 0;
    int params = 0;
    int homeX = 0;
    int homeY = 0;
    int homeZ = 0;
    int parsed = sscanf(actorKey.c_str(), "%d_%d_%d_%d_%d_%d_%d",
                        &sceneNum, &actorId, &roomNum, &params, &homeX, &homeY, &homeZ);
    if (parsed == 7) {
        target.actorId = (s16)actorId;
        target.hasApproxPos = true;
        target.approxPos.x = (f32)homeX;
        target.approxPos.y = (f32)homeY;
        target.approxPos.z = (f32)homeZ;
    }

    return target;
}

Actor* FindClosestActorOfTypeInRoom(s16 sceneNum, s8 roomNum, s16 actorId, const Vec3f& approxPos, f32 radius) {
    Actor* best = nullptr;
    f32 bestDistSq = std::numeric_limits<f32>::max();
    f32 maxDistSq = radius * radius;
    Vec3f approxPosMutable = approxPos;

    for (int cat : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
        Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            if (actor->id != actorId || actor->room != roomNum || actor->update == nullptr) {
                actor = actor->next;
                continue;
            }

            // Keep fallback bounded to the same scene and a sane radius.
            if (sceneNum != gPlayState->sceneNum) {
                actor = actor->next;
                continue;
            }

            f32 distSq = Math3D_Vec3fDistSq(&actor->home.pos, &approxPosMutable);
            if (distSq <= maxDistSq && distSq < bestDistSq) {
                best = actor;
                bestDistSq = distSq;
            }

            actor = actor->next;
        }
    }

    return best;
}
} // namespace

/**
 * ROOM_KILL_SYNC
 *
 * Bidirectional packet sent by whichever side had been killing enemies alone
 * in a room the moment the other player enters that same room.
 *
 * Problem it solves:
 *   Because enemies respawn on every room entry, a player that cleared a room
 *   solo will have dead enemies while the arriving player sees them alive.
 *   This packet bridges that gap by immediately killing the relevant actors on
 *   the receiver side right after the shared room is established.
 *
 * Flow (client clears room before host arrives):
 *   1. Client kills enemy while host is elsewhere → actorKey recorded in
 *      pendingRoomKills["sceneNum_roomNum"].
 *   2. OnGameFrameUpdate detects IsOwnerInSameRoom() transition false → true.
 *   3. Client sends ROOM_KILL_SYNC with the pending kill list.
 *   4. Host finds matching living actors and calls Actor_Kill() on each.
 *   5. Host's OnActorKill → SendPacket_ActorKilled propagates back to client
 *      (no-op because the actor is already dead there).
 *
 * Flow (host clears room before client arrives):
 *   Symmetric: authority records kills in pendingRoomKills, detects
 *   IsAnyClientInSameRoom() transition false → true, sends to client.
 */

void Anchor::SendPacket_RoomKillSync() {
    if (!IsSaveLoaded() || !gPlayState) return;

    std::string roomKey = std::to_string(gPlayState->sceneNum) + "_" +
                          std::to_string((s8)gPlayState->roomCtx.curRoom.num);

    auto it = pendingRoomKills.find(roomKey);
    if (it == pendingRoomKills.end() || it->second.empty()) return;

    nlohmann::json payload;
    payload["type"]     = ROOM_KILL_SYNC;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["roomNum"]  = (s8)gPlayState->roomCtx.curRoom.num;
    payload["kills"]    = nlohmann::json::array();

    for (const auto& key : it->second) {
        RoomKillTarget target = BuildTargetFromActorKey(key);

        nlohmann::json killEntry;
        killEntry["actorKey"] = key;
        killEntry["actorId"] = target.actorId;
        if (target.hasApproxPos) {
            killEntry["approxPos"]["x"] = target.approxPos.x;
            killEntry["approxPos"]["y"] = target.approxPos.y;
            killEntry["approxPos"]["z"] = target.approxPos.z;
        }

        payload["kills"].push_back(killEntry);
    }

    SPDLOG_INFO("[Anchor:EnemySync] {}: ROOM_KILL_SYNC send | scene=0x{:02x} room={} kills={}",
                IsEnemyAuthority() ? "HOST" : "CLIENT", gPlayState->sceneNum,
                (s8)gPlayState->roomCtx.curRoom.num, it->second.size());

    // Keep client-side pending entries for reliability retries until authority
    // confirms via ACTOR_KILLED. Host keeps previous behavior.
    if (IsEnemyAuthority()) {
        pendingRoomKills.erase(it);
    }

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_RoomKillSync(nlohmann::json payload) {
    if (!IsSaveLoaded() || !gPlayState) return;
    if (!payload.contains("kills") || !payload.contains("sceneNum")) return;

    s16 sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
    if (sceneNum != gPlayState->sceneNum) return;

    s8 roomNum = payload.value("roomNum", (s8)-1);
    size_t receivedKills = payload["kills"].size();
    size_t appliedKills = 0;

    SPDLOG_INFO("[Anchor:EnemySync] {}: ROOM_KILL_SYNC recv | scene=0x{:02x} room={} kills={}",
                IsEnemyAuthority() ? "HOST" : "CLIENT", sceneNum, roomNum, receivedKills);

    for (const auto& killEntry : payload["kills"]) {
        RoomKillTarget target;

        // Backward-compat: accept both legacy string entries and object entries.
        if (killEntry.is_string()) {
            target = BuildTargetFromActorKey(killEntry.get<std::string>());
        } else if (killEntry.is_object() && killEntry.contains("actorKey")) {
            target.actorKey = killEntry["actorKey"].get<std::string>();
            target.actorId = killEntry.value("actorId", (s16)-1);
            if (killEntry.contains("approxPos")) {
                const auto& approxPos = killEntry["approxPos"];
                target.approxPos.x = approxPos.value("x", 0.0f);
                target.approxPos.y = approxPos.value("y", 0.0f);
                target.approxPos.z = approxPos.value("z", 0.0f);
                target.hasApproxPos = true;
            }
        } else {
            continue;
        }

        std::string actorKey = target.actorKey;
        bool found = false;
        bool usedFallback = false;
        Actor* matchedActor = nullptr;

        for (int cat : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
            Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
            while (actor != nullptr) {
                Actor* next = actor->next; // cache before potential invalidation
                if (GetActorKey(actor, sceneNum) == actorKey)
                {
                    matchedActor = actor;
                    found = true;
                    break; // move on to next actorKey in the list
                }
                actor = next;
            }

            if (found) {
                break;
            }
        }

        // Pragmatic overkill fallback:
        // if the exact key is gone/drifted but we know the actor type and spawn area,
        // take the nearest matching actor in the same room.
        if (!found && target.actorId >= 0 && target.hasApproxPos) {
            matchedActor = FindClosestActorOfTypeInRoom(sceneNum, roomNum, target.actorId, target.approxPos, 16.0f);
            if (matchedActor != nullptr) {
                found = true;
                usedFallback = true;
            }
        }

        if (found && matchedActor != nullptr) {
            // Kill regardless of current health. Some enemies can sit at
            // health==0 for a while before calling Actor_Kill themselves.
            // ROOM_KILL_SYNC is an authoritative "remove now" signal.
            matchedActor->colChkInfo.health = 0;
            if (gPlayState->csCtx.state != CS_STATE_IDLE && gSaveContext.cutsceneIndex < 0xFFF0 &&
                gPlayState->sceneNum != SCENE_FOREST_TEMPLE_BOSS && matchedActor->category == ACTORCAT_BOSS) {
                ReleaseOrphanedCutscene("boss killed mid-cs");
            }
            ApplyNativeEnemyDeathSideEffects(matchedActor);
            applyingRemoteKill = true;
            Actor_Kill(matchedActor);
            applyingRemoteKill = false;
            appliedKills++;
            if (usedFallback) {
                SPDLOG_INFO("[Anchor:EnemySync] {}: ROOM_KILL_SYNC fallback matched | actorKey={} | actorId={} | room={}",
                            IsEnemyAuthority() ? "HOST" : "CLIENT", actorKey, (int)target.actorId, roomNum);
            }
        }

        if (!found) {
            SPDLOG_WARN("[Anchor:EnemySync] {}: ROOM_KILL_SYNC actor not found/already dead | actorKey={} | scene=0x{:02x} room={}",
                        IsEnemyAuthority() ? "HOST" : "CLIENT", actorKey, sceneNum, roomNum);
        }
    }

    SPDLOG_INFO("[Anchor:EnemySync] {}: ROOM_KILL_SYNC applied | scene=0x{:02x} room={} applied={}/{}",
                IsEnemyAuthority() ? "HOST" : "CLIENT", sceneNum, roomNum, appliedKills, receivedKills);
}
