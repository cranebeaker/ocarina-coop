#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/cvar_prefixes.h"

extern "C" {
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

/**
 * ACTOR_KILLED
 *
 * Sent by the enemy authority (room owner) when an enemy or boss actor is killed.
 *
 * Forces all non-authority clients to kill the matching local actor, ensuring that
 * enemy deaths are consistent across all clients.
 *
 * This is the authoritative kill signal: if the owner killed it, it's dead everywhere.
 * This also covers the case where a non-owner player attacks an enemy — if the owner
 * independently kills that enemy (or the kill propagates through ACTOR_STATE_UPDATE
 * reducing HP to 0 and triggering the enemy's own death logic), all clients will
 * receive this packet and kill their local copy.
 */

extern "C" {
extern s16 D_80B4A1B0;
extern s16 D_80B4A1B4;
void func_800F5B58(void);
}
#include "overlays/actors/ovl_En_Zf/z_en_zf.h"
#include "overlays/actors/ovl_En_Po_Sisters/z_en_po_sisters.h"
extern "C" {
#include "overlays/actors/ovl_En_Dh/z_en_dh.h"
#include "overlays/actors/ovl_En_Ik/z_en_ik.h"
#include "overlays/actors/ovl_En_Rd/z_en_rd.h"
#include "overlays/actors/ovl_En_Wf/z_en_wf.h"
}

void Anchor::ApplyNativeEnemyDeathSideEffects(Actor* actor) {
    if (actor == nullptr || !gPlayState) return;
    // Forest Temple Stalfos room: BgMoriBigst counts its EnTest children in home.rot.z and only
    // EnTest decrements it (z_en_test.c: parent->home.rot.z--). Mirror that for remote kills.
    if (actor->id == ACTOR_EN_TEST && actor->parent != nullptr && actor->parent->id == ACTOR_BG_MORI_BIGST &&
        actor->parent->home.rot.z > 0) {
        actor->parent->home.rot.z--;
    }
    // Forest Temple Meg (real sister, unk_194 == 0 && unk_195 == 0): native death unsets switch 0x1B
    // (z_en_po_sisters.c). 0x1B is excluded from flag sync, so apply it locally.
    if (actor->id == ACTOR_EN_PO_SISTERS && gPlayState->sceneNum == SCENE_FOREST_TEMPLE) {
        EnPoSisters* po = (EnPoSisters*)actor;
        if (po->unk_194 == 0 && po->unk_195 == 0) {
            Flags_UnsetSwitch(gPlayState, 0x1B);
        }
    }
    // Dead Hand: its En_Dha hands self-kill when parent->params == ENDH_DEATH. A remote kill skips
    // the native death, so the hands would keep a dangling parent pointer. Kill them here.
    if (actor->id == ACTOR_EN_DH) {
        Actor* hand = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head;
        while (hand != nullptr) {
            Actor* nextHand = hand->next;
            if (hand->id == ACTOR_EN_DHA && hand->parent == actor) {
                Actor_Kill(hand);
            }
            hand = nextHand;
        }
        actor->params = ENDH_DEATH;
        return;
    }
    // Death-end switch flags that the native routines set after the death animation (skipped by
    // remote/overkill kills). Same guards as the actors themselves.
    if (!CVarGetInteger(CVAR_ENHANCEMENT("RandomizedEnemies"), 0) &&
        !CVarGetInteger(CVAR_REMOTE_CROWD_CONTROL("Enabled"), 0)) {
        if (actor->id == ACTOR_EN_IK) {
            EnIk* ik = (EnIk*)actor;
            if (actor->params != 0 && ik->switchFlags != 0xFF) Flags_SetSwitch(gPlayState, ik->switchFlags);
        } else if (actor->id == ACTOR_EN_RD) {
            s16 f = ((EnRd*)actor)->rdFlags & 0x7F;
            if (!Flags_GetSwitch(gPlayState, f)) Flags_SetSwitch(gPlayState, f);
        } else if (actor->id == ACTOR_EN_WF) {
            EnWf* wf = (EnWf*)actor;
            if (wf->switchFlag != 0xFF) Flags_SetSwitch(gPlayState, wf->switchFlag);
        }
    }
    // Only live (not already dying/killed) miniboss Lizalfos. EnZf_Die moves the actor to
    // ACTORCAT_PROP, and Actor_Kill nulls update, so both guard against double application.
    if (actor->id != ACTOR_EN_ZF || actor->params < ENZF_TYPE_LIZALFOS_MINIBOSS_A) return;
    if (actor->update == nullptr || actor->category != ACTORCAT_ENEMY) return;

    // Mirrors EnZf_SetupDie + EnZf_Die (z_en_zf.c) without touching unk_3F4 (cosmetic).
    const s16 before = D_80B4A1B4;
    if (before != -1) {
        Actor* partner = (actor->prev != nullptr) ? actor->prev : actor->next;
        if (partner != nullptr && partner->colChkInfo.health < 3) {
            partner->colChkInfo.health = 3;
        }
    }
    D_80B4A1B0 = 0;
    if (before == -1) {
        Flags_SetSwitch(gPlayState, ((EnZf*)actor)->clearFlag);
        func_800F5B58();
    } else {
        D_80B4A1B4 = -1;
    }
    SPDLOG_INFO("[Anchor:EnemySync] Lizalfos miniboss death side effects | params={} | D_80B4A1B4 {} -> {}",
                (int)actor->params, (int)before, (int)D_80B4A1B4);
}

void Anchor::SendPacket_ActorKilled(const Actor* actor) {
    if (!IsSaveLoaded()) return;

    std::string actorKey = GetActorKey(actor, gPlayState->sceneNum);
    SPDLOG_INFO("[Anchor:EnemySync] HOST: ACTOR_KILLED send | actorKey={} | scene=0x{:02x}", actorKey, gPlayState->sceneNum);

    nlohmann::json payload;
    payload["type"] = ACTOR_KILLED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["actorKey"] = actorKey;
    payload["actorId"] = actor->id;
    payload["actorCategory"] = actor->category;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ActorKilled(nlohmann::json payload) {
    if (!IsSaveLoaded() || IsEnemyAuthority()) return;
    if (!payload.contains("actorKey")) return;

    s16 sceneNum = payload.value("sceneNum", (s16)SCENE_ID_MAX);
    if (sceneNum != gPlayState->sceneNum) return;

    std::string actorKey = payload["actorKey"].get<std::string>();
    int receivedCategory = payload.value("actorCategory", (int)ACTORCAT_ENEMY);
    SPDLOG_INFO("[Anchor:ActorSync] CLIENT: ACTOR_KILLED recv | actorKey={} | category={} | scene=0x{:02x}",
                actorKey, receivedCategory, sceneNum);

    // Confirmation path for ROOM_KILL_SYNC retries:
    // when authority confirms a kill, drop this key from all pending room sets.
    for (auto it = pendingRoomKills.begin(); it != pendingRoomKills.end();) {
        it->second.erase(actorKey);
        if (it->second.empty()) {
            it = pendingRoomKills.erase(it);
        } else {
            ++it;
        }
    }

    // Determine behaviour (collectible/clear-flag handling) from the reported category.
    bool isBgKill     = (receivedCategory == ACTORCAT_BG || receivedCategory == ACTORCAT_PROP);
    bool isEnemyKill  = (receivedCategory == ACTORCAT_ENEMY || receivedCategory == ACTORCAT_BOSS);

    // ALWAYS scan every category when looking for the actor to kill — do not
    // restrict by receivedCategory. Some actors (e.g. En_Test/Stalfos) change
    // category at runtime via Actor_ChangeCategory (ENEMY <-> PROP while
    // "downed"); the sender's category at kill-time can therefore differ from
    // whatever category the receiver's still-living local copy currently sits
    // in. Since GetActorKey no longer encodes category, scanning all four
    // lists is cheap and guarantees we still find the matching actor.
    std::vector<int> catsToScan = { ACTORCAT_ENEMY, ACTORCAT_BOSS, ACTORCAT_BG, ACTORCAT_PROP };

    for (int cat : catsToScan) {
        Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
        while (actor != nullptr) {
            Actor* next = actor->next; // cache before potential Actor_Kill invalidates pointers
            if (GetActorKey(actor, sceneNum) == actorKey) {
                if (isEnemyKill) {
                    // Always zero health before kill so the actor leaves the correct
                    // drop state, even if EnemyPositionUpdate already set it to 0.
                    actor->colChkInfo.health = 0;
                }
                // Boss already running its forced native death sequence (BOSS_DEFEAT_START):
                // do not cut it short. The sequence spawns the heart container / warp at its end.
                // A frame deadline in HookHandlers hard-kills it if it never finishes.
                if (actor->category == ACTORCAT_BOSS && bossDefeatStartedKeys.count(actorKey) &&
                    actor->update != nullptr) {
                    const s32 bossRoom = (actor->room >= 0) ? actor->room
                                                            : (s32)gPlayState->roomCtx.curRoom.num;
                    Flags_SetTempClear(gPlayState, bossRoom);
                    Flags_SetClear(gPlayState, bossRoom);
                    deferredBossKills[actorKey] = gPlayState->state.frames + 2400;
                    SPDLOG_INFO("[Anchor:ActorSync] CLIENT: ACTOR_KILLED deferred (boss death sequence running) | actorKey={}",
                                actorKey);
                    return;
                }
                if (gPlayState->csCtx.state != CS_STATE_IDLE && gSaveContext.cutsceneIndex < 0xFFF0 &&
                    gPlayState->sceneNum != SCENE_FOREST_TEMPLE_BOSS && actor->category == ACTORCAT_BOSS) {
                    ReleaseOrphanedCutscene("boss killed mid-cs");
                }
                ApplyNativeEnemyDeathSideEffects(actor);
                {
                    applyingRemoteKill = true;
                    Actor_Kill(actor);
                    applyingRemoteKill = false;
                }

                // For boss actors: the game-native death sequence (which runs on the
                // master) normally calls Flags_SetTempClear / Flags_SetClear for the
                // room so that DoorWarp1_AwaitClearFlag (the blue exit portal) can
                // advance.  On non-master clients Actor_Kill is called externally via
                // this packet, so the boss never runs its own clear-flag code.
                // Set both flags here to ensure the blue warp appears for everyone.
                if (isEnemyKill && actor->category == ACTORCAT_BOSS && gPlayState) {
                    const s32 bossRoom = (actor->room >= 0) ? actor->room
                                                            : (s32)gPlayState->roomCtx.curRoom.num;
                    Flags_SetTempClear(gPlayState, bossRoom);
                    Flags_SetClear(gPlayState, bossRoom);
                }
                // Clean up client-side BG tracking entries for dead actors.
                if (isBgKill) {
                    bgActorKeyframeTarget.erase(actorKey);
                }
                SPDLOG_INFO("[Anchor:ActorSync] CLIENT: ACTOR_KILLED applied | actorKey={} | category={}", actorKey, cat);
                return;
            }
            actor = next;
        }
    }

    SPDLOG_WARN("[Anchor:ActorSync] CLIENT: ACTOR_KILLED actor not found | actorKey={} | category={} | scene=0x{:02x}",
                actorKey, receivedCategory, sceneNum);
}
