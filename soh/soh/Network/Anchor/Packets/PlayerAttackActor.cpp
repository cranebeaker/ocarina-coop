#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>

extern "C" {
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
#include "src/overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
#include "src/overlays/actors/ovl_Boss_Ganondrof/z_boss_ganondrof.h"
#include "src/overlays/actors/ovl_Boss_Ganon/z_boss_ganon.h"
#include "src/overlays/actors/ovl_Boss_Sst/z_boss_sst.h"
#include "src/overlays/actors/ovl_Boss_Tw/z_boss_tw.h"
#include "src/overlays/actors/ovl_En_Dh/z_en_dh.h"
void EnDh_SetupDeath(EnDh* dh);
}
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"

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
                    // Bosses: a client-dealt killing blow never runs the hit callback on the
                    // master, so start the native death sequence instead of hard-killing.
                    // The OnBossDefeat hook makes the master announce BOSS_DEFEAT_START.
                    if (actor->category == ACTORCAT_BOSS) {
                        s32 started = 0;
                        switch (actor->id) {
                            case ACTOR_BOSS_GOMA:
                                started = BossGoma_NetworkForceDefeat((BossGoma*)actor, gPlayState);
                                break;
                            case ACTOR_BOSS_GANONDROF:
                                started = BossGanondrof_NetworkForceDefeat((BossGanondrof*)actor, gPlayState);
                                break;
                            case ACTOR_BOSS_GANON:
                                started = BossGanon_NetworkForceDefeat((BossGanon*)actor, gPlayState);
                                break;
                            case ACTOR_BOSS_GANON2:
                                // Ganon's death (and the phase-1 knockdown) only start from his own
                                // collision callback; never hard-kill him. Leave 1 HP so the next
                                // native hit does the real transition.
                                actor->colChkInfo.health = 1;
                                return;
                            case ACTOR_BOSS_SST:
                                started = BossSst_NetworkForceDefeat((BossSst*)actor, gPlayState);
                                break;
                            case ACTOR_BOSS_TW:
                                started = BossTw_NetworkForceDefeat((BossTw*)actor, gPlayState);
                                break;
                            default:
                                break;
                        }
                        if (started) {
                            GameInteractor_ExecuteOnBossDefeat(actor);
                            return;
                        }
                    }
                    // Dead Hand: run its native death (sets ENDH_DEATH so En_Dha hands self-kill);
                    // it calls Actor_Kill itself, which triggers ACTOR_KILLED.
                    if (actor->id == ACTOR_EN_DH) {
                        if (actor->params != ENDH_DEATH) EnDh_SetupDeath((EnDh*)actor);
                        return;
                    }
                    // Nabooru Iron Knuckle (params 0): its native health<=10 branch handles the defeat
                    // cutscene on the authority; do not hard-kill it.
                    if (actor->id == ACTOR_EN_IK && actor->params == 0) return;
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
