#include "Anchor.h"
#include "soh/Enhancements/nametag.h"

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_En_Horse/z_en_horse.h"
extern PlayState* gPlayState;

void Player_UseItem(PlayState* play, Player* player, s32 item);
void Player_Draw(Actor* actor, PlayState* play);
}

static DamageTable DummyPlayerDamageTable = {
    /* Deku nut      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Deku stick    */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Slingshot     */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Explosive     */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Boomerang     */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Normal arrow  */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Hammer swing  */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Hookshot      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Kokiri sword  */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master sword  */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant's Knife */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Fire arrow    */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice arrow     */ DMG_ENTRY(4, PLAYER_HIT_RESPONSE_ICE_TRAP),
    /* Light arrow   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_ELECTRIC_SHOCK),
    /* Unk arrow 1   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Unk arrow 2   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Unk arrow 3   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Fire magic    */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice magic     */ DMG_ENTRY(3, PLAYER_HIT_RESPONSE_ICE_TRAP),
    /* Light magic   */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_ELECTRIC_SHOCK),
    /* Shield        */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Mirror Ray    */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Kokiri spin   */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant spin    */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master spin   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Kokiri jump   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant jump    */ DMG_ENTRY(8, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master jump   */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Unknown 1     */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Unblockable   */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Hammer jump   */ DMG_ENTRY(4, PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Unknown 2     */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
};

// Minimal update stub for phantom Epona actors.
// Advances ONLY the Skin skelAnime so bone matrices stay valid — the Skin draw
// function calls Skin_GetOrientations which reads these matrices, and leaving
// them stale causes Skin_UpdateVertices to SIGSEGV after ~30 s.
//
// Intentionally does NOT run the full EnHorse_Update logic so the phantom:
//   - Never reads DREG(53) (Epona song signal → would "activate" the phantom,
//     steal the song response, and prevent the client from calling their own Epona)
//   - Never calls EnHorse_MountDismount (would let local Link mount the phantom)
//   - Never responds to player proximity or aggro
//   - Never modifies world position (position is driven by DummyPlayer_Update)
static void PhantomEpona_Update(Actor* thisx, PlayState* play) {
    EnHorse* horse = (EnHorse*)thisx;
    SkelAnime_Update(&horse->skin.skelAnime);
}

void DummyPlayer_Init(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    // Hack to account for usage of gSaveContext in Player_Init
    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = client.linkAge;

    // #region modeled after EnTorch2_Init and Player_Init
    actor->room = -1;
    player->itemAction = player->heldItemAction = -1;
    player->heldItemId = ITEM_NONE;
    Player_UseItem(play, player, ITEM_NONE);
    Player_SetModelGroup(player, Player_ActionToModelGroup(player, player->heldItemAction));
    play->playerInit(player, play, gPlayerSkelHeaders[client.linkAge]);

    play->func_11D54(player, play);
    // #endregion

    player->cylinder.base.acFlags = AC_ON | AC_TYPE_PLAYER;
    player->cylinder.base.ocFlags2 = OC2_TYPE_1;
    player->cylinder.info.bumperFlags = BUMP_ON | BUMP_HOOKABLE | BUMP_NO_HITMARK;
    player->actor.flags |= ACTOR_FLAG_HOOKSHOT_PULLS_PLAYER;
    player->cylinder.dim.radius = 30;
    player->actor.colChkInfo.damageTable = &DummyPlayerDamageTable;

    gSaveContext.linkAge = originalAge;

    bool isGlobalRoom = (std::string("soh-global") == CVarGetString(CVAR_REMOTE_ANCHOR("RoomId"), ""));

    if (!isGlobalRoom) {
        NameTag_RegisterForActorWithOptions(actor, client.name.c_str(), {});
    }
}

void Math_Vec3s_Copy(Vec3s* dest, Vec3s* src) {
    dest->x = src->x;
    dest->y = src->y;
    dest->z = src->z;
}

// Update the actor with new data from the client
void DummyPlayer_Update(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    if (client.sceneNum != gPlayState->sceneNum || !client.online || !client.isSaveLoaded) {
        actor->world.pos.x = -9999.0f;
        actor->world.pos.y = -9999.0f;
        actor->world.pos.z = -9999.0f;
        actor->shape.shadowAlpha = 0;
        return;
    }

    actor->shape.shadowAlpha = 255;
    Math_Vec3s_Copy(&player->upperLimbRot, &client.upperLimbRot);
    Math_Vec3s_Copy(&actor->shape.rot, &client.posRot.rot);
    Math_Vec3f_Copy(&actor->world.pos, &client.posRot.pos);
    player->currentBoots = client.currentBoots;
    player->currentShield = client.currentShield;
    player->currentTunic = client.currentTunic;
    player->stateFlags1 = client.stateFlags1;
    player->stateFlags2 = client.stateFlags2;
    player->itemAction = client.itemAction;
    player->heldItemAction = client.heldItemAction;
    player->invincibilityTimer = client.invincibilityTimer;
    player->unk_862 = client.unk_862;
    player->unk_85C = client.unk_85C;
    player->av1.actionVar1 = client.actionVar1;

    // Giver throw animation override: while active, advance the pre-set throw
    // animation and skip the network joint table / root motion override so the
    // dummy holds the throw pose.  Position was already restored from client.posRot.pos.
    if (client.giverAnimTimer > 0) {
        client.giverAnimTimer--;
        LinkAnimation_Update(play, &player->skelAnime);
    } else {
        player->skelAnime.jointTable = client.jointTable;
        player->skelAnime.movementFlags = client.movementFlags;
        Math_Vec3s_Copy(&player->skelAnime.prevTransl, &client.prevTransl);

        // Apply animation movement (Copied from Player_ApplyAnimMovementScaledByAge)
        Vec3f diff;
        SkelAnime_UpdateTranslation(&player->skelAnime, &diff, player->actor.shape.rot.y);

        if (player->skelAnime.movementFlags & 1) {
            if (!LINK_IS_ADULT) {
                diff.x *= 0.64f;
                diff.z *= 0.64f;
            }

            player->actor.world.pos.x += diff.x * player->actor.scale.x;
            player->actor.world.pos.z += diff.z * player->actor.scale.z;
        }

        if (player->skelAnime.movementFlags & 2) {
            if (!(player->skelAnime.movementFlags & 4)) {
                diff.y *= player->ageProperties->unk_08;
            }

            player->actor.world.pos.y += diff.y * player->actor.scale.y;
        }
    }

    if (player->modelGroup != client.modelGroup) {
        // Hack to account for usage of gSaveContext
        s32 originalAge = gSaveContext.linkAge;
        gSaveContext.linkAge = client.linkAge;
        u8 originalButtonItem0 = gSaveContext.equips.buttonItems[0];
        gSaveContext.equips.buttonItems[0] = client.buttonItem0;
        Player_SetModelGroup(player, client.modelGroup);
        gSaveContext.linkAge = originalAge;
        gSaveContext.equips.buttonItems[0] = originalButtonItem0;
    }

    // ── Phase 6: Phantom horse ────────────────────────────────────────────────
    // When the remote player has PLAYER_STATE1_ON_HORSE we need a visible horse
    // under them.  In OoT, Link's world.pos while mounted is at saddle height
    // (≈ PHANTOM_HORSE_SADDLE_HEIGHT units above ground), so the phantom horse is
    // placed at  rider.pos.y − PHANTOM_HORSE_SADDLE_HEIGHT.
    //
    // We use En_Horse_Normal (pasture horse) because it has minimal scripting.
    // Its update function is cleared → KI-silent; position is set every frame.
    // The horse is killed when the rider dismounts or their scene diverges.
    {
        const bool riderInScene = (client.sceneNum == gPlayState->sceneNum &&
                                   client.online && client.isSaveLoaded);
        const bool isOnHorse    = (player->stateFlags1 & PLAYER_STATE1_ON_HORSE) != 0;
        const bool horseSyncOn  = Anchor::Instance->roomState.syncEpona != 0;
        auto& horseMap = Anchor::Instance->clientPhantomHorse;

        if (riderInScene && isOnHorse && horseSyncOn) {
            Actor* phantom = horseMap.count(clientId) ? horseMap[clientId] : nullptr;

            if (!phantom) {
                // First frame of this client being on a horse — spawn the phantom.
                // Use ACTOR_EN_HORSE (Epona) so the correct horse skin/skeleton is
                // displayed.  EN_HORSE_NORMAL (pasture horse) uses Ingo's model
                // which looks wrong when the remote player is on Epona.
                //
                // After spawn:
                //   ENHORSE_ACT_IDLE  — stands idle, native update advances Skin
                //                       so bone matrices stay valid (nulling update
                //                       causes Skin_UpdateVertices SIGSEGV after ~30s).
                //   ENHORSE_DRAW      — must be explicit; not set by all init paths.
                //   ENHORSE_UNRIDEABLE— prevents the local Link from accidentally
                //                       mounting the phantom instead of his own Epona.
                //
                // This actor is excluded from BG keyframe tracking/blending by the
                // EN_HORSE guards in HookHandlers.cpp, so no position interference.
                Vec3f spawnPos = actor->world.pos;
                spawnPos.y -= Anchor::PHANTOM_HORSE_SADDLE_HEIGHT;
                phantom = Actor_Spawn(&gPlayState->actorCtx, gPlayState,
                                      ACTOR_EN_HORSE,
                                      spawnPos.x, spawnPos.y, spawnPos.z,
                                      0, actor->shape.rot.y, 0,
                                      /*params=*/1);
                if (phantom) {
                    EnHorse* horse = (EnHorse*)phantom;
                    horse->action         = ENHORSE_ACT_IDLE;
                    horse->stateFlags    |= ENHORSE_DRAW | ENHORSE_UNRIDEABLE;
                    horse->playerControlled = 0;
                    // Actor_Spawn leaves actor->update as the native EnHorse_Update
                    // (set by En_Horse_InitVars/EnHorse_Init). Without overriding it,
                    // the phantom runs the FULL native update every frame — including
                    // EnHorse_MountSideCheck/approach-to-player logic that reads
                    // GET_PLAYER(play), i.e. THIS client's own real, local Link. That
                    // is exactly what caused the "client gets teleported onto the
                    // host's horse / controls glued together" bug when a mounted host
                    // rides into a room where the client is also mounted: the phantom
                    // actively tried to interact with the client's real player.
                    // PhantomEpona_Update (below) replaces it with a minimal stub.
                    phantom->update = PhantomEpona_Update;
                    // Persistent across room-boundary draws (same as DummyPlayer).
                    phantom->room = -1;
                    SPDLOG_INFO("[Anchor:Horse] Spawned phantom Epona for client {} (actor={})",
                                clientId, (void*)phantom);
                }
                // Store even if null so we don’t re-attempt every frame.
                horseMap[clientId] = phantom;
            }

            if (phantom) {
                // Drive position every frame to follow the rider.
                phantom->world.pos.x = actor->world.pos.x;
                phantom->world.pos.y = actor->world.pos.y - Anchor::PHANTOM_HORSE_SADDLE_HEIGHT;
                phantom->world.pos.z = actor->world.pos.z;
                phantom->world.rot.y = actor->shape.rot.y;
                phantom->shape.rot.y = actor->shape.rot.y;
            }
        } else {
            // Rider dismounted or left scene — remove phantom.
            auto it = horseMap.find(clientId);
            if (it != horseMap.end()) {
                if (it->second) {
                    Actor_Kill(it->second);
                    SPDLOG_INFO("[Anchor:Horse] Killed phantom horse for client {}", clientId);
                }
                horseMap.erase(it);
            }
        }
    }
    // ── end phantom horse ─────────────────────────────────────────────────────

    if (Anchor::Instance->roomState.pvpMode == 0 ||
        (Anchor::Instance->roomState.pvpMode == 1 &&
         client.teamId == CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default"))) {
        actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
        return;
    }

    actor->flags &= ~ACTOR_FLAG_LOCK_ON_DISABLED;

    if (player->cylinder.base.acFlags & AC_HIT && player->invincibilityTimer == 0) {
        // BR: Wenn der lokale Spieler gerade tot/respawnend ist (brEliminated),
        // keinen Schaden senden.  Startschutz und Respawn-Schutz via brStartProtectionUntil.
        const bool senderElim   = Anchor::Instance->brEliminated;
        const bool startProtect = Anchor::Instance->roomState.battleRoyaleMode &&
                                  (std::chrono::steady_clock::now() < Anchor::Instance->brStartProtectionUntil);
        if (!senderElim && !startProtect) {
            Anchor::Instance->SendPacket_DamagePlayer(client.clientId, player->actor.colChkInfo.damageEffect,
                                                      player->actor.colChkInfo.damage);
        }
        if (player->actor.colChkInfo.damageEffect == DUMMY_PLAYER_HIT_RESPONSE_STUN) {
            Actor_SetColorFilter(&player->actor, 0, 0xFF, 0, 24);
        } else {
            player->invincibilityTimer = 20;
        }
    }

    Collider_UpdateCylinder(&player->actor, &player->cylinder);

    if (!(player->stateFlags2 & PLAYER_STATE2_FROZEN)) {
        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_HANGING_OFF_LEDGE |
                                     PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_ON_HORSE))) {
            CollisionCheck_SetOC(play, &play->colChkCtx, &player->cylinder.base);
        }

        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_DAMAGED)) &&
            (player->invincibilityTimer <= 0)) {
            CollisionCheck_SetAC(play, &play->colChkCtx, &player->cylinder.base);

            if (player->invincibilityTimer < 0) {
                CollisionCheck_SetAT(play, &play->colChkCtx, &player->cylinder.base);
            }
        }
    }

    if (player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_IN_ITEM_CS | PLAYER_STATE1_IN_CUTSCENE)) {
        player->actor.colChkInfo.mass = MASS_IMMOVABLE;
    } else {
        player->actor.colChkInfo.mass = 50;
    }

    Collider_ResetCylinderAC(play, &player->cylinder.base);
}

void DummyPlayer_Draw(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    if (client.sceneNum != gPlayState->sceneNum || !client.online || !client.isSaveLoaded) {
        return;
    }

    // Hack to account for usage of gSaveContext in Player_Draw
    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = client.linkAge;
    u8 originalButtonItem0 = gSaveContext.equips.buttonItems[0];
    gSaveContext.equips.buttonItems[0] = client.buttonItem0;

    Player_Draw((Actor*)player, play);
    gSaveContext.linkAge = originalAge;
    gSaveContext.equips.buttonItems[0] = originalButtonItem0;
}

void DummyPlayer_Destroy(Actor* actor, PlayState* play) {
    // Kill any phantom horse that was tracking this dummy player.
    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);
    if (Anchor::Instance->clientPhantomHorse.count(clientId)) {
        Actor* horse = Anchor::Instance->clientPhantomHorse[clientId];
        if (horse) {
            Actor_Kill(horse);
        }
        Anchor::Instance->clientPhantomHorse.erase(clientId);
    }

    // DummyPlayer Actors are initially spawned as ACTOR_PLAYER, but change their
    // ID shortly afterwards to ACTOR_EN_OE2. This would cause ACTOR_PLAYER's
    // ActorDB Entry's `numLoaded` to leak, which is mostly harmless but hits debug
    // asserts. Set the id back to ACTOR_PLAYER so that `numLoaded` will be decremented
    // correctly.
    actor->id = ACTOR_PLAYER;
}
