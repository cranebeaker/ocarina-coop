#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

/**
 * ENEMY_POSITION_UPDATE
 *
 * High-frequency packet sent by the room owner (enemy authority) when an enemy
 * has moved more than a small threshold.  Non-authority clients are frozen via
 * ShouldActorUpdate = false, so position must be driven entirely by these packets.
 *
 * Throttle policy (authority side, enforced in HookHandlers.cpp):
 *   Send if squared-distance from last sent position > 4.0  (≈2 world units)
 *   OR if health has changed since last ActorStateUpdate.
 */

// ─── Room-check helpers ───────────────────────────────────────────────────────

/**
 * Returns true if at least one connected, save-loaded client is in the same
 * scene AND room as the local player.  Used by the authority to decide whether
 * any position sync is needed at all.
 */
bool Anchor::IsAnyClientInSameRoom() const {
    if (!gPlayState) return false;
    s16 myScene = gPlayState->sceneNum;
    s8  myRoom  = (s8)gPlayState->roomCtx.curRoom.num;
    for (auto& [id, client] : clients) {
        if (client.self || !client.online || !client.isSaveLoaded) continue;
        if (client.sceneNum == myScene && client.curRoomNum == myRoom) {
            SPDLOG_DEBUG("[Anchor:EnemySync] HOST: IsAnyClientInSameRoom=true | scene=0x{:02x} room={}", myScene, myRoom);
            return true;
        }
    }
    SPDLOG_DEBUG("[Anchor:EnemySync] HOST: IsAnyClientInSameRoom=false | scene=0x{:02x} room={}", myScene, myRoom);
    return false;
}

/**
 * Returns true if the room owner (enemy authority) is in the same scene AND
 * room as the local player.  Used by non-authority clients to decide whether
 * to freeze local enemy AI.
 */
bool Anchor::IsOwnerInSameRoom() const {
    if (!gPlayState) return false;
    s16 myScene = gPlayState->sceneNum;
    s8  myRoom  = (s8)gPlayState->roomCtx.curRoom.num;

    // Use the explicitly assigned room master if available; fall back to global owner.
    const std::string key = BuildRoomKey(myScene, myRoom);
    uint32_t masterClientId = roomState.ownerClientId;
    auto it = roomAuthority.find(key);
    if (it != roomAuthority.end() && it->second != 0) {
        masterClientId = it->second;
    }

    for (auto& [id, client] : clients) {
        if (id != masterClientId) continue;
        bool masterHere = client.online && client.isSaveLoaded && client.sceneNum == myScene && client.curRoomNum == myRoom;
        if (masterHere) {
            SPDLOG_DEBUG("[Anchor:EnemySync] CLIENT: IsOwnerInSameRoom=true | scene=0x{:02x} room={} | masterScene=0x{:02x} masterRoom={}",
                         myScene, myRoom, client.sceneNum, client.curRoomNum);
        } else {
            SPDLOG_DEBUG("[Anchor:EnemySync] CLIENT: IsOwnerInSameRoom=false | scene=0x{:02x} room={} | masterScene=0x{:02x} masterRoom={}",
                         myScene, myRoom, client.sceneNum, client.curRoomNum);
        }
        return masterHere;
    }
    SPDLOG_DEBUG("[Anchor:EnemySync] CLIENT: IsOwnerInSameRoom=false | no master found");
    return false;
}

// ─── Send ─────────────────────────────────────────────────────────────────────

void Anchor::SendPacket_EnemyPositionUpdate(const Actor* actor) {
    if (!IsSaveLoaded()) return;

    nlohmann::json payload;
    payload["type"]        = ENEMY_POSITION_UPDATE;
    payload["quiet"]       = true; // suppress per-packet debug spam
    payload["sceneNum"]    = gPlayState->sceneNum;
    payload["actorKey"]    = GetActorKey(actor, gPlayState->sceneNum);
    payload["posX"]        = actor->world.pos.x;
    payload["posY"]        = actor->world.pos.y;
    payload["posZ"]        = actor->world.pos.z;
    payload["rotY"]        = (int)actor->world.rot.y;
    payload["shapeRotY"]   = (int)actor->shape.rot.y;
    payload["health"]      = actor->colChkInfo.health;
    // Draw state: false means actor->draw == nullptr (enemy is hidden, e.g. Deku Scrub underground).
    // The client uses this to suppress rendering without touching AI-controlled function pointers.
    payload["drawEnabled"] = (actor->draw != nullptr);

    SendJsonToRemote(payload);
}

// ─── Handle ───────────────────────────────────────────────────────────────────

void Anchor::HandlePacket_EnemyPositionUpdate(nlohmann::json payload) {
    if (!IsSaveLoaded() || IsEnemyAuthority()) return;
    if (!payload.contains("actorKey")) return;

    s16 sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
    if (sceneNum != gPlayState->sceneNum) return;

    std::string actorKey  = payload["actorKey"].get<std::string>();
    float posX            = payload.value("posX", 0.0f);
    float posY            = payload.value("posY", 0.0f);
    float posZ            = payload.value("posZ", 0.0f);
    s16   rotY            = (s16)payload.value("rotY", 0);
    s16   shapeRotY       = (s16)payload.value("shapeRotY", (int)rotY);
    u8    health          = payload.value("health", (u8)1);
    // Backward-compatible: old servers don't send drawEnabled; treat as visible.
    bool  drawEnabled     = payload.value("drawEnabled", true);

    for (int cat : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
        Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            if (GetActorKey(actor, sceneNum) == actorKey) {
                actor->world.pos.x = posX;
                actor->world.pos.y = posY;
                actor->world.pos.z = posZ;
                actor->world.rot.y = rotY;
                // Keep visual and physical rotation aligned.
                actor->shape.rot.y = shapeRotY;
                // Store the authoritative position for the position-corrected update path.
                // OnBeforeActorUpdate and OnActorUpdate use this to pin world.pos every frame
                // so colliders register at the correct spot and Z-targeting stays accurate.
                remoteEnemyPos[actorKey] = actor->world.pos;

                if (health != actor->colChkInfo.health) {
                    actor->colChkInfo.health = health;
                    pendingRemoteHealthOverride[actorKey] = health;
                }

                // ── Draw-state override (Phase 4) ────────────────────────────
                // When the room master hides an enemy (e.g. Deku Scrub going underground),
                // suppress rendering on the client by nulling the draw pointer.
                // Restore the saved function when the enemy reappears.
                // Bosses never use this override: a false hide state would make the
                // boss invisible and untargetable for the whole room.
                if (actor->category != ACTORCAT_BOSS) {
                    if (!drawEnabled) {
                        // Save original draw function before we hide it (once only).
                        if (actor->draw != nullptr) {
                            savedEnemyDrawFuncs[actorKey] = actor->draw;
                            actor->draw = nullptr;
                        }
                    } else {
                        // Restore draw function if we previously suppressed it.
                        auto savedIt = savedEnemyDrawFuncs.find(actorKey);
                        if (savedIt != savedEnemyDrawFuncs.end()) {
                            actor->draw = savedIt->second;
                            savedEnemyDrawFuncs.erase(savedIt);
                        }
                    }
                }

                return;
            }
            actor = actor->next;
        }
    }
}
