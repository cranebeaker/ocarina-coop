#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

/**
 * PLAYER_ATTACK_ACTOR
 *
 * Sent by a non-authority client when the local player damages an enemy.
 *
 * Timing note (important for correctness):
 *   ProcessIncomingPacketQueue() is called from OnGameFrameUpdate in game.c,
 *   which fires AFTER Play_Update() (CollisionCheck + Actor_UpdateAll) for
 *   the current frame.  This means the handler runs AFTER the actor loop.
 *
 *   We therefore set actor->colChkInfo.damage and let the NEXT frame's
 *   Actor_UpdateAll call the enemy's own update() → Actor_ApplyDamage().
 *   This gives the host the full natural hit response: hit flash, stagger,
 *   damage sounds, and the normal death sequence (Actor_Kill → OnActorKill
 *   → SendPacket_ActorKilled).
 *
 * Flow:
 *   Client:  OnBeforeActorUpdate detects colChkInfo.damage > 0
 *            → zeroes local damage (no local HP change)
 *            → sends PLAYER_ATTACK_ACTOR(actorKey, damage)
 *   Host:    HandlePacket_PlayerAttackActor sets colChkInfo.damage
 *            → next frame: actor->update() calls Actor_ApplyDamage
 *            → death / OnActorKill / SendPacket_ActorKilled as normal
 */

void Anchor::SendPacket_PlayerAttackActor(const Actor* actor, u8 damage) {
    if (!IsSaveLoaded()) return;

    nlohmann::json payload;
    payload["type"]       = PLAYER_ATTACK_ACTOR;
    payload["quiet"]      = false; // enable logging for debug tracking
    payload["sceneNum"]   = gPlayState->sceneNum;
    payload["actorKey"]   = GetActorKey(actor, gPlayState->sceneNum);
    payload["damage"]     = damage;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_PlayerAttackActor(nlohmann::json payload) {
    // Only the enemy authority processes incoming attack packets.
    if (!IsSaveLoaded() || !IsEnemyAuthority()) return;
    if (!payload.contains("actorKey") || !payload.contains("damage")) return;

    s16 sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
    if (sceneNum != gPlayState->sceneNum) return;

    std::string actorKey = payload["actorKey"].get<std::string>();
    u8 damage            = payload["damage"].get<u8>();

    // IMPORTANT: We cannot rely on queuing colChkInfo.damage and waiting for the
    // enemy's own update() to call Actor_ApplyDamage().  Most enemies gate damage
    // processing on the acHit pointer (set by CollisionCheck_ApplyDamage when an
    // actual AT/OC collision occurs).  Since no real collision happens on the host
    // machine for a client-side hit, acHit is never set, so queued damage is silently
    // ignored.  We therefore apply damage directly here.
    for (int cat : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
        Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            if (actor->colChkInfo.health > 0 &&
                GetActorKey(actor, gPlayState->sceneNum) == actorKey)
            {
                // Apply damage directly — acHit is never set for remote hits
                // (no real collision on the host), so the enemy's own update()
                // would silently ignore queued damage.
                // Do NOT call Actor_Kill here even when health reaches 0: the actor's
                // own update() will detect health == 0 on the very next frame and
                // transition into its native death action (falling animation, sounds,
                // etc.).  When that sequence completes the actor calls Actor_Kill
                // itself, firing OnActorKill → SendPacket_ActorKilled to all clients.
                SPDLOG_INFO("[Anchor:EnemySync] HOST: Damage packet received | actorKey={} | damage={} | preHealth={} | pos=({:.1f},{:.1f},{:.1f})", 
                            actorKey, (int)damage, (int)actor->colChkInfo.health, actor->world.pos.x, actor->world.pos.y, actor->world.pos.z);
                actor->colChkInfo.damage = damage;
                Actor_ApplyDamage(actor);
                actor->colChkInfo.damage = 0;
                SPDLOG_INFO("[Anchor:EnemySync] HOST: Damage applied | actorKey={} | postHealth={}", 
                            actorKey, (int)actor->colChkInfo.health);

                // Overkill safeguard: if remote damage already brought HP to zero,
                // finalize death immediately on authority to avoid race conditions
                // where client-side death advances faster than host actor teardown.
                if (actor->colChkInfo.health == 0) {
                    SPDLOG_INFO("[Anchor:EnemySync] HOST: Overkill finalize | actorKey={} | pos=({:.1f},{:.1f},{:.1f})",
                                actorKey, actor->world.pos.x, actor->world.pos.y, actor->world.pos.z);
                    ApplyNativeEnemyDeathSideEffects(actor);
                    Actor_Kill(actor);
                }
                return;
            }
            actor = actor->next;
        }
    }
    SPDLOG_WARN("[Anchor:EnemySync] HOST: Could not find actor for damage packet | actorKey={}", actorKey);
}
