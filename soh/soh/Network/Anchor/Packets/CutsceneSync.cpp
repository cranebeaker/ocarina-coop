#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "z64cutscene.h"
#include "z64player.h"
#include "macros.h"
extern PlayState* gPlayState;
}

/**
 * TRIGGER_CUTSCENE
 *
 * Sent by whichever client first transitions from CS_STATE_IDLE to a running
 * cutscene state (SKIPPABLE_INIT or UNSKIPPABLE_INIT).
 *
 * Conditions checked before sending (all must be true):
 *   1. roomState.syncCutscenes == 1
 *   2. roomState.syncEnemies   == 1  (shares radius / room infrastructure)
 *   3. At least one connected client is in the same scene + room
 *   4. That client is within the Enemy Sync Radius of the local player
 *
 * On receive: if the local engine is still in CS_STATE_IDLE and the scene
 * numbers match, trigger the matching cutscene state.
 */

/**
 * True while an actor that can legitimately drive/end an actor-driven cutscene
 * is still alive (update != NULL; Actor_Kill nulls update).
 */
bool Anchor::CutsceneOwnerAlive() {
    if (!gPlayState) return false;
    Player* player = GET_PLAYER(gPlayState);
    if (player != NULL && player->csActor != NULL && player->csActor->update != NULL) return true;

    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != NULL; a = a->next) {
            if (a->update == NULL) continue;
            switch (a->id) {
                case ACTOR_BOSS_GOMA:
                case ACTOR_BOSS_DODONGO:
                case ACTOR_BOSS_FD:
                case ACTOR_BOSS_FD2:
                case ACTOR_BOSS_GANON:
                case ACTOR_BOSS_GANON2:
                case ACTOR_BOSS_GANONDROF:
                case ACTOR_BOSS_MO:
                case ACTOR_BOSS_SST:
                case ACTOR_BOSS_TW:
                case ACTOR_BOSS_VA:
                case ACTOR_EN_CLEAR_TAG:
                case ACTOR_EN_FHG:
                case ACTOR_FISHING:
                case ACTOR_BG_TREEMOUTH:
                    return true;
                default:
                    break;
            }
        }
    }
    return false;
}

/**
 * Releases an actor-driven cutscene whose owning actor is gone (e.g. killed by
 * ACTOR_KILLED mid-cutscene): csCtx, sub-cameras, HUD/letterbox and the player.
 * Never touches scripted cutscenes (cutsceneIndex >= 0xFFF0) or the forest boss.
 */
void Anchor::ReleaseOrphanedCutscene(const char* reason) {
    if (!IsSaveLoaded() || !gPlayState) return;
    if (gSaveContext.cutsceneIndex >= 0xFFF0) return;
    if (gPlayState->sceneNum == SCENE_FOREST_TEMPLE_BOSS) return;

    u8 prevState = (u8)gPlayState->csCtx.state;

    // Camera: owner (and thus its sub-camera id) is gone; return to the main camera.
    if (gPlayState->activeCamera != MAIN_CAM) {
        Play_ClearAllSubCameras(gPlayState);
        Play_ChangeCameraStatus(gPlayState, MAIN_CAM, CAM_STAT_ACTIVE);
    }

    // Player: vanilla release path.
    Player* player = GET_PLAYER(gPlayState);
    if (player != NULL && player->csAction != 0 && player->csAction != 7) {
        Player_SetCsActionWithHaltedActors(gPlayState, NULL, 7);
    }

    // csCtx (mirrors func_80068DC0 / func_80068D84 end states).
    func_8006450C(gPlayState, &gPlayState->csCtx);
    gPlayState->csCtx.linkAction = NULL;
    for (int i = 0; i < 10; i++) {
        gPlayState->csCtx.npcActions[i] = NULL;
    }
    Audio_SetCutsceneFlag(0);
    ShrinkWindow_SetVal(0);
    Interface_ChangeAlpha(50);

    SPDLOG_WARN("[Anchor:CutsceneSync] released orphaned cutscene ({}): prevState={}", reason, prevState);
}

void Anchor::SendPacket_TriggerCutscene(u8 csState) {
    if (!IsSaveLoaded() || !gPlayState) return;

    nlohmann::json payload;
    payload["type"]     = TRIGGER_CUTSCENE;
    payload["csState"]  = csState;
    payload["sceneNum"] = (s16)gPlayState->sceneNum;

    SendJsonToRemote(payload);
}


void Anchor::HandlePacket_TriggerCutscene(nlohmann::json payload) {
    if (!IsSaveLoaded() || !gPlayState) return;
    // IDLE release is idempotent and only acts on orphaned cutscenes, so it is
    // accepted regardless of syncCutscenes/syncEnemies.

    // Sanity: sender must be in the same scene.
    s16 senderScene = payload.value("sceneNum", (s16)-1);
    if (senderScene != (s16)gPlayState->sceneNum) return;

    u8 csState = payload.value("csState", (u8)CS_STATE_IDLE);

    if (csState == CS_STATE_IDLE) {
        // Master's cutscene ended — release any client that is still stuck in a
        // non-IDLE CS state (e.g. a locally-triggered CS whose ending actor is
        // frozen by enemy sync, or any leftover state from a previous session).
        // func_8006450C cleanly resets csCtx (state → IDLE, unk_0C → 0).
        // Latch the time so the watchdog can release a cutscene that starts just after.
        csMasterIdleFrame = gPlayState->state.frames;
        csMasterIdleScene = (s16)gPlayState->sceneNum;
        if (gPlayState->csCtx.state != CS_STATE_IDLE && gSaveContext.cutsceneIndex < 0xFFF0 &&
            !CutsceneOwnerAlive()) {
            ReleaseOrphanedCutscene("master idle");
        }
    }
    // INTENTIONALLY no func_80064520 / func_80064534 calls for non-IDLE states.
    //
    // Root cause of the freeze+letterbox bug:
    //   Calling func_80064520/func_80064534 sets csCtx.state = SKIPPABLE_INIT but
    //   does NOT set csCtx.scriptList[0].script — that is done by the actor that
    //   normally triggers the cutscene.  Without a valid script pointer the CS
    //   subsystem enters an indeterminate state: letterbox appears, the player
    //   is frozen, but no commands ever execute.  The state never reaches IDLE
    //   on its own, causing a permanent softlock until func_8006450C is called.
    //
    // Correct behaviour:
    //   Actor-driven cutscenes run on ALL clients because the boss/trigger actor
    //   is NOT frozen by ShouldActorUpdate (ACTORCAT_BOSS runs normally on every
    //   client).  The actor sets csCtx.scriptList[0] and calls func_80064520
    //   itself — so the CS plays naturally without network intervention.
    //   Non-master clients that do NOT have the CS triggered locally (different
    //   local state) simply play freely while the master watches the cutscene.
    //   The CS_STATE_IDLE release above ensures they are un-stuck if needed.
}
