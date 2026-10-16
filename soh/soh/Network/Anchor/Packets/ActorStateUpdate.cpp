#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

/**
 * ACTOR_STATE_UPDATE
 *
 * Sent by the enemy authority (room owner) to sync enemy HP to all other clients.
 *
 * Fires when an enemy or boss actor's health changes on the authority client.
 * Non-authority clients update the matching local actor's HP to maintain consistency.
 *
 * Actor identification uses a composite key:
 *   sceneNum + actorId + room + params + home.pos (spawn position)
 * This uniquely identifies any actor instance across all clients since all clients
 * start with the same actor spawn data from the ROM.
 *
 * IMPORTANT: category is intentionally NOT part of this key. Several actors
 * (e.g. En_Test/Stalfos via Actor_ChangeCategory, toggling ACTORCAT_ENEMY <->
 * ACTORCAT_PROP while "downed" before reviving or dying for good) change
 * category at runtime. If category were part of the key, the host's key for
 * such an actor would stop matching the client's key the moment their local
 * category observations diverge (which they always do, since non-authority
 * clients never run the category-changing logic themselves) — e.g. ACTOR_KILLED
 * for the permanent death (sent while category==PROP on the host) would never
 * match the client's still-ACTORCAT_ENEMY local copy, leaving it alive forever.
 */

static std::unordered_map<const Actor*, std::string> sStableActorKeys;

static std::string BuildActorKey(const Actor* actor, s16 sceneNum) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%d_%d_%d_%d_%d_%d_%d_%d_%d_%d",
        (int)sceneNum, (int)actor->id, (int)actor->room, (int)actor->params,
        (int)actor->home.pos.x, (int)actor->home.pos.y, (int)actor->home.pos.z,
        (int)actor->home.rot.x, (int)actor->home.rot.y, (int)actor->home.rot.z);
    return std::string(buf);
}

void Anchor::CacheStableActorKey(const Actor* actor, s16 sceneNum) {
    if (actor == nullptr || actor->id != ACTOR_EN_DODOJR) return;
    sStableActorKeys[actor] = BuildActorKey(actor, sceneNum);
}

void Anchor::EraseStableActorKey(const Actor* actor) {
    sStableActorKeys.erase(actor);
}

void Anchor::ClearStableActorKeys() {
    sStableActorKeys.clear();
}

std::string Anchor::GetActorKey(const Actor* actor, s16 sceneNum) {
    if (actor->id == ACTOR_EN_DODOJR && !sStableActorKeys.empty()) {
        auto it = sStableActorKeys.find(actor);
        if (it != sStableActorKeys.end()) return it->second;
    }
    // Keese mutate params at runtime (ignite/extinguish at torches, Init clears 0x8000),
    // independently per client. Keep only the perch bit so the key stays stable.
    int keyParams = (int)actor->params;
    if (actor->id == ACTOR_EN_FIREFLY) keyParams = (int)(actor->params & 1);
    char buf[128];
    snprintf(buf, sizeof(buf), "%d_%d_%d_%d_%d_%d_%d_%d_%d_%d",
        (int)sceneNum,
        (int)actor->id,
        (int)actor->room,
        keyParams,
        (int)actor->home.pos.x,
        (int)actor->home.pos.y,
        (int)actor->home.pos.z,
        (int)actor->home.rot.x,
        (int)actor->home.rot.y,
        (int)actor->home.rot.z);
    return std::string(buf);
}

bool Anchor::IsEnemyAuthority() {
    return IsRoomMaster();
}

void Anchor::SendPacket_ActorStateUpdate(const Actor* actor) {
    if (!IsSaveLoaded()) return;

    nlohmann::json payload;
    payload["type"] = ACTOR_STATE_UPDATE;
    payload["quiet"] = true; // High-frequency packet, don't spam debug log
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["actorKey"] = GetActorKey(actor, gPlayState->sceneNum);
    payload["health"] = actor->colChkInfo.health;
    // Position and rotation so non-authority clients track enemy location
    payload["posX"] = actor->world.pos.x;
    payload["posY"] = actor->world.pos.y;
    payload["posZ"] = actor->world.pos.z;
    payload["rotY"] = (int)actor->world.rot.y;
    payload["shapeRotY"] = (int)actor->shape.rot.y;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ActorStateUpdate(nlohmann::json payload) {
    if (!IsSaveLoaded() || IsEnemyAuthority()) return;
    if (!payload.contains("actorKey") || !payload.contains("health")) return;

    s16 sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
    if (sceneNum != gPlayState->sceneNum) return;

    std::string actorKey = payload["actorKey"].get<std::string>();
    u8 health = payload["health"].get<u8>();
    float posX = payload.value("posX", 0.0f);
    float posY = payload.value("posY", 0.0f);
    float posZ = payload.value("posZ", 0.0f);
    s16   rotY = (s16)payload.value("rotY", 0);
    s16 shapeRotY = (s16)payload.value("shapeRotY", (int)rotY);
    bool hasPos = payload.contains("posX");

    // Find matching actor in the local scene and update its HP
    for (int cat : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
        Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            if (GetActorKey(actor, sceneNum) == actorKey) {
                // For bosses, apply HP only when it decreases (min-merge): local
                // hits already reduced HP locally; restoring to a higher authority
                // value would create an unkillable loop when multi-part boss phases
                // haven't synced yet.  Always apply HP=0 so the death transition
                // propagates correctly.
                if (cat == ACTORCAT_BOSS) {
                    if (health < actor->colChkInfo.health || health == 0) {
                        actor->colChkInfo.health = health;
                        pendingRemoteHealthOverride[actorKey] = health;
                    }
                } else {
                    actor->colChkInfo.health = health;
                    pendingRemoteHealthOverride[actorKey] = health;
                }
                // Override position so both clients see the enemy in the same place
                if (hasPos) {
                    actor->world.pos.x = posX;
                    actor->world.pos.y = posY;
                    actor->world.pos.z = posZ;
                    actor->world.rot.y = rotY;
                    actor->shape.rot.y = shapeRotY;
                }
                return;
            }
            actor = actor->next;
        }
    }
}
