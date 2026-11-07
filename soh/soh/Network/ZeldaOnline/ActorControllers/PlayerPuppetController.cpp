#include "PlayerPuppetController.hpp"
#include "../ZeldaOnlineClient.hpp"
#include "../HorsePuppet.hpp"
#include "../Packet.hpp"
#include "soh/Enhancements/nametag.h"
#include <soh/ResourceManagerHelpers.h>
#include "soh/frame_interpolation.h"
#include <soh/Enhancements/PlayerSkin/PlayerSkin.h>
#include <spdlog/spdlog.h>
#include <ship/Context.h>
#include "ship/resource/ResourceManager.h"
#define ZO_PUPPET_HOOKSHOT_PARAMS 0x7F

extern "C" {
#include "src/overlays/actors/ovl_En_Horse/z_en_horse.h"
#include "src/overlays/actors/ovl_En_Boom/z_en_boom.h"
#include "src/overlays/actors/ovl_Arms_Hook/z_arms_hook.h"
#include "assets/objects/object_fish/object_fish.h"


void ArmsHook_Shoot(ArmsHook* thisx, PlayState* play);
void ArmsHook_Draw(Actor* thisx, PlayState* play);
void Player_ReapplySkeleton(Player* thisx, PlayState* play);
extern Color_RGB8 sTunicColors[];
extern Vec3f sRodTipOffset;
extern f32 sRodScales[];
extern f32 sRodBendRatios[];
extern s16 sRodCastState;
extern f32 sRodBendRotY;
extern f32 D_80B7A6AC;
extern f32 D_80B7A6B0;
extern f32 D_80B7A6B4;
extern f32 D_80B7A6B8;
extern f32 D_80B7A6BC;
extern f32 D_80B7A6C0;
extern f32 sFishingLineScale;
extern Vec3f sLurePos;
extern Vec3f sLureRot;
extern f32 sLure1Rotate; // lure type 1 is programmed to change this.
extern f32 sLurePosZOffset;
extern u8 sLureEquipped;
}

namespace ZeldaOnline {
typedef enum {
    /* 0x00 */ FS_LURE_STOCK,
    /* 0x01 */ FS_LURE_UNK, // hinted at with an "== 1"
    /* 0x02 */ FS_LURE_SINKING
} FishingLureTypes;

extern "C" {
void Player_UseItem(PlayState* play, Player* player, s32 item);
void Player_Draw(Actor* actor, PlayState* play);
void Fishing_DrawLureHook(PlayState* play, Vec3f* pos, Vec3f* refPos, u8 hookIndex);
void Player_UpdateBunnyEars(Player* thisx, BunnyEarKinematics* bunnyEarKinematics);

extern Vec3f D_808547A4;
extern Vec3f D_808547B0;
extern Color_RGBA8 D_808547BC;
extern Color_RGBA8 D_808547C0;
extern Gfx** sPlayerDListGroups[];
extern BunnyEarKinematics* gCurrentDrawBunnyEar;
}

static void Puppet_Vec3sCopy(Vec3s* dest, const Vec3s* src) {
    dest->x = src->x;
    dest->y = src->y;
    dest->z = src->z;
}

static ColliderQuadInit sPuppetSwordQuadInit = {
    {
        COLTYPE_METAL,
        AT_ON | AT_TYPE_ENEMY,
        AC_NONE,
        OC1_NONE,
        OC2_NONE,
        COLSHAPE_QUAD,
    },
    {
        ELEMTYPE_UNK2,
        { 0xFFCFFFFF, 0x00, 0x00 },
        { 0x00000000, 0x00, 0x00 },
        TOUCH_ON | TOUCH_NEAREST,
        BUMP_NONE,
        OCELEM_NONE,
    },
    { { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } } },
};

static void BoomerangPuppet_Update(Actor* actor, PlayState* play) {
    EnBoom* boom = reinterpret_cast<EnBoom*>(actor);

    boom->activeTimer++;
}
void HookshotPuppet_Update(Actor* thisx, PlayState* play) {
}

void HookshotPuppet_Draw(Actor* thisx, PlayState* play) {
    ArmsHook* hook = (ArmsHook*)thisx;

    Actor* owner = thisx->parent;
    if (owner == NULL) {
        return;
    }

    Actor* localPlayer = play->actorCtx.actorLists[ACTORCAT_PLAYER].head;
    play->actorCtx.actorLists[ACTORCAT_PLAYER].head = owner;
    ArmsHook_Draw(thisx, play);
    if (hook->timer > 0) {
        CollisionCheck_SetAT(play, &play->colChkCtx, &hook->collider.base);
    }

    //Restore local player back to head position
    play->actorCtx.actorLists[ACTORCAT_PLAYER].head = localPlayer;
}


void PlayerPuppetController::ApplyAppearance(const ByteStream& blob) {
    ByteStream data = blob;
     u8 newAge = (u8)(data.Read<PackedUInt1>().value());
    if (newAge < 2)
        m_linkAge = newAge;
    m_skinRef = data.ReadString(data.Read<PackedUInt1>().value());

    std::vector<std::string> archives;
    ZeldaOnlineClient::SplitSkinRef(m_skinRef, m_skinName, archives);

    if (m_actor != nullptr) {
        ((Player*)m_actor)->skin = PlayerSkin_Get(m_skinName.c_str());

        auto missingArchives = MissingArchives(archives);
        if (missingArchives.size()) {
            ZeldaOnlineClient::Instance->RequestSkinDownload(archives, m_skinName);
        } else if (m_actor != nullptr && gPlayState != nullptr)
            Player_ReapplySkeleton((Player*)m_actor, gPlayState);
    }
}


static Actor* FindLocalHorse(Player* player) {
    if ((player->stateFlags1 & PLAYER_STATE1_ON_HORSE) && player->rideActor != NULL &&
        player->rideActor->id == ACTOR_EN_HORSE) {
        return player->rideActor;
    }

    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BG].head; a != NULL; a = a->next) {
        if (a->id != ACTOR_EN_HORSE || a->update == HorsePuppet_Update) {
            continue;
        }

        EnHorse* horse = (EnHorse*)a;
        if (horse->type != HORSE_EPONA) {
            continue;
        }

        if (horse->action == ENHORSE_ACT_INACTIVE || (horse->stateFlags & ENHORSE_INACTIVE)) {
            continue;
        }

        if (a->draw == NULL) {
            continue;
        }

        return a;
    }
    return NULL;
}

void PlayerPuppetController::BuildLocalPlayerProperties(Player* player, const std::string& nickName,
                                                        ByteStream& out) {
    bool pvpEnabled = CVarGetInteger("gZeldaOnline.Pvp", 0);

    Actor* horseActor = FindLocalHorse(player);
    PackProperty(PROP_POS_X, PackedFloat4(player->actor.world.pos.x), out);
    PackProperty(PROP_POS_Y, PackedFloat4(player->actor.world.pos.y), out);
    PackProperty(PROP_POS_Z, PackedFloat4(player->actor.world.pos.z), out);
    PackProperty(PROP_ROT_X, PackedInt2(player->actor.world.rot.x), out);
    PackProperty(PROP_ROT_Y, PackedInt2(player->actor.world.rot.y), out);
    PackProperty(PROP_ROT_Z, PackedInt2(player->actor.world.rot.z), out);

    PackProperty(PROP_SHAPE_ROT,
                 ByteStream() << PackedInt2(player->actor.shape.rot.x) << PackedInt2(player->actor.shape.rot.y)
                              << PackedInt2(player->actor.shape.rot.z),
                 out);

    u8 onPlatform = 0;
    if ((player->actor.floorBgId != BGCHECK_SCENE) && (player->actor.bgCheckFlags & 1)) {
        DynaPolyActor* floorDyna = DynaPoly_GetActor(&gPlayState->colCtx, player->actor.floorBgId);
        if (floorDyna != NULL && floorDyna->actor.zoController != NULL) {
            onPlatform = 1;
        }
    }
    PackProperty(PPROP_ON_PLATFORM, PackedUInt1(onPlatform), out);
    PackProperty(PPROP_NICKNAME, ByteStream() << nickName, out);
    u8 paused =
        (gPlayState != NULL && (gPlayState->pauseCtx.state != 0 || gPlayState->msgCtx.msgMode != MSGMODE_NONE)) ? 1 : 0;
    PackProperty(PPROP_PAUSED, PackedUInt1(paused), out);

    PackProperty(PPROP_MOVEMENT_FLAGS, PackedUInt1(player->skelAnime.movementFlags), out);
    PackProperty(PPROP_REAL_ROOM_INDEX, PackedInt2(gPlayState->roomCtx.curRoom.num), out);

    PackProperty(PPROP_PREV_TRANSL,
                 ByteStream() << PackedInt2(player->skelAnime.prevTransl.x)
                              << PackedInt2(player->skelAnime.prevTransl.y)
                              << PackedInt2(player->skelAnime.prevTransl.z),
                 out);
    PackProperty(PPROP_UPPER_LIMB_ROT,
                 ByteStream() << PackedInt2(player->upperLimbRot.x) << PackedInt2(player->upperLimbRot.y)
                              << PackedInt2(player->upperLimbRot.z),
                 out);

    PackProperty(PPROP_UNK_3BC,
                 ByteStream() << PackedInt2(player->unk_3BC.x) << PackedInt2(player->unk_3BC.y)
                              << PackedInt2(player->unk_3BC.z),
                 out);

    {
        ByteStream pose;
        for (int i = 0; i < 24; i++) {
            pose << PackedInt2(player->skelAnime.jointTable[i].x) << PackedInt2(player->skelAnime.jointTable[i].y)
                 << PackedInt2(player->skelAnime.jointTable[i].z);
        }
        PackProperty(PPROP_POSE, pose, out);
    }
    PackProperty(PPROP_STATE_FLAGS1, PackedUInt4(player->stateFlags1), out);
    PackProperty(PPROP_STATE_FLAGS2, PackedUInt4(player->stateFlags2), out);
    PackProperty(PPROP_ITEM_ACTION, PackedInt1(player->itemAction), out);
    PackProperty(PPROP_HELD_ITEM_ACTION, PackedInt1(player->heldItemAction), out);
    PackProperty(PPROP_MODEL_GROUP, ByteStream() << PackedUInt1((u8)(player->modelGroup)), out);
    PackProperty(PPROP_LEFT_HAND_TYPE, ByteStream() << PackedUInt1((u8)(player->leftHandType)), out);
    PackProperty(PPROP_RIGHT_HAND_TYPE, ByteStream() << PackedUInt1((u8)(player->rightHandType)), out);
    PackProperty(PPROP_SHEATH_TYPE, ByteStream() << PackedUInt1((u8)(player->sheathType)), out);
    PackProperty(PPROP_INVINCIBILITY, ByteStream() << PackedInt1((s8)(player->invincibilityTimer)), out);
    PackProperty(PPROP_HELD_GET_ITEM, PackedInt2(player->unk_862), out);
    PackProperty(PPROP_CS_ACTION, ByteStream() << PackedUInt1((u8)(player->csAction)), out);
    PackProperty(PPROP_HELD_ITEM_SCALE, PackedFloat4(player->unk_85C), out);
    PackProperty(PPROP_STICK_FLAME_TIMER, PackedInt2(player->unk_860), out);
    PackProperty(PPROP_TUNIC, PackedInt1((s8)(player->currentTunic)), out);
    PackProperty(PPROP_BOOTS, PackedInt1((s8)(player->currentBoots)), out);
    PackProperty(PPROP_SHIELD, PackedInt1((s8)(player->currentShield)), out);
    PackProperty(PPROP_BUTTONITEM0, PackedUInt1(gSaveContext.equips.buttonItems[0]), out);
    PackProperty(PPROP_MASK, PackedUInt1(player->currentMask), out);
    PackProperty(PPROP_SPEEDXZ, PackedFloat4(player->actor.speedXZ), out);      //For bunny ear simulation
    {
        float freqMultiplier = CVarGetFloat(CVAR_AUDIO("LinkVoiceFreqMultiplier"), 1.0);
        if (freqMultiplier <= 0.0f) {
            freqMultiplier = 1.0f;
        }
        PackProperty(PPROP_SOUND_FREQUENCY, PackedInt2(int(freqMultiplier * 100)), out);
    }

    int tunic = TUNIC_EQUIP_TO_PLAYER(CUR_EQUIP_VALUE(EQUIP_TYPE_TUNIC));
    Color_RGB8 sTemp;
    Color_RGB8* color = &sTunicColors[tunic];
    if (tunic == PLAYER_TUNIC_KOKIRI && CVarGetInteger(CVAR_COSMETIC("Link.KokiriTunic.Changed"), 0)) {
        sTemp = CVarGetColor24(CVAR_COSMETIC("Link.KokiriTunic.Value"), sTunicColors[PLAYER_TUNIC_KOKIRI]);
        color = &sTemp;
    } else if (tunic == PLAYER_TUNIC_GORON && CVarGetInteger(CVAR_COSMETIC("Link.GoronTunic.Changed"), 0)) {
        sTemp = CVarGetColor24(CVAR_COSMETIC("Link.GoronTunic.Value"), sTunicColors[PLAYER_TUNIC_GORON]);
        color = &sTemp;
    } else if (tunic == PLAYER_TUNIC_ZORA && CVarGetInteger(CVAR_COSMETIC("Link.ZoraTunic.Changed"), 0)) {
        sTemp = CVarGetColor24(CVAR_COSMETIC("Link.ZoraTunic.Value"), sTunicColors[PLAYER_TUNIC_ZORA]);
        color = &sTemp;
    }

    PackProperty(PPROP_TUNIC_COLOUR,
                 ByteStream() << PackedUInt1(color->r) << PackedUInt1(color->g) << PackedUInt1(color->b), out);
    
    //Horse
    {
        u8 present = (horseActor != NULL) ? 1u : 0u;

        u8 mounted = 0, animId = 0, animFrame = 0;
        f32 px = 0, py = 0, pz = 0;
        s16 rx = 0, ry = 0, rz = 0;
        u8 horseState = 0;
        float horseSpeedXZ = 0.0f;

        if (horseActor != NULL) {
            EnHorse* horse = (EnHorse*)horseActor;
            mounted = (player->stateFlags1 & PLAYER_STATE1_ON_HORSE) && player->rideActor == horseActor;
            animId = (u8)(horse->animationIdx);
            animFrame = (u8)(horse->skin.skelAnime.curFrame);
            px = horse->actor.world.pos.x;
            py = horse->actor.world.pos.y;
            pz = horse->actor.world.pos.z;
            rx = horse->actor.shape.rot.x;
            ry = horse->actor.shape.rot.y;
            rz = horse->actor.shape.rot.z;
            horseSpeedXZ = horse->actor.speedXZ;
            horseState = horse->stateFlags;
        }
        PackProperty(PPROP_HORSE_PRESENT, PackedUInt1(present), out);
        PackProperty(PPROP_HORSE_MOUNTED, PackedUInt1(mounted), out);
        PackProperty(PPROP_HORSE_ANIM, PackedUInt1(animId), out);
        PackProperty(PPROP_HORSE_FRAME, PackedUInt1(animFrame), out);
        PackProperty(PPROP_HORSE_POS_X, PackedFloat4(px), out);
        PackProperty(PPROP_HORSE_POS_Y, PackedFloat4(py), out);
        PackProperty(PPROP_HORSE_POS_Z, PackedFloat4(pz), out);
        PackProperty(PPROP_HORSE_ROT_X, PackedInt2(rx), out);
        PackProperty(PPROP_HORSE_ROT_Y, PackedInt2(ry), out);
        PackProperty(PPROP_HORSE_ROT_Z, PackedInt2(rz), out);
        PackProperty(PPROP_HORSE_STATE_FLAGS, PackedUInt1(horseState), out);
        PackProperty(PPROP_HORSE_SPEEDXZ, PackedFloat4(horseSpeedXZ), out);
    }
    u32 heldId = 0;
    if (player->heldActor != nullptr && player->heldActor->zoController != nullptr)
        heldId = static_cast<AbstractActorController*>(player->heldActor->zoController)->NetworkID();

    PackProperty(PPROP_HELD_ACTOR, PackedUInt4(heldId), out);

    if (pvpEnabled &&
        ((player->meleeWeaponState > 0) &&
        ((player->meleeWeaponAnimation < 0x18) || (player->stateFlags2 & PLAYER_STATE2_SPIN_ATTACKING)) &&
        (player->meleeWeaponInfo[1].active))) {

        ByteStream payload;
        payload << PackedFloat4(player->meleeWeaponInfo[1].tip.x) << PackedFloat4(player->meleeWeaponInfo[1].tip.y)
                << PackedFloat4(player->meleeWeaponInfo[1].tip.z);
        payload << PackedFloat4(player->meleeWeaponInfo[1].base.x) << PackedFloat4(player->meleeWeaponInfo[1].base.y)
                << PackedFloat4(player->meleeWeaponInfo[1].base.z);
        payload << PackedUInt4(player->meleeWeaponQuads[0].info.toucher.dmgFlags);
        payload << PackedUInt1(player->meleeWeaponQuads[0].info.toucher.damage);
        PackProperty(PPROP_HITTING, payload, out);
    } else {
        PackNullProperty(PPROP_HITTING, out);
    }

    if ((player->stateFlags1 & PLAYER_STATE1_BOOMERANG_THROWN) && player->boomerangActor != nullptr) {
        Actor* boom = player->boomerangActor;
        ByteStream payload;
        payload << PackedFloat4(boom->world.pos.x) << PackedFloat4(boom->world.pos.y)
                << PackedFloat4(boom->world.pos.z);
        payload << PackedInt2(boom->shape.rot.x) << PackedInt2(boom->shape.rot.y) << PackedInt2(boom->shape.rot.z)
                << PackedUInt1(pvpEnabled);
        PackProperty(PPROP_BOOMERANG, payload, out);
    } else {
        PackNullProperty(PPROP_BOOMERANG, out);
    }

    // Hookshot
    {
        Actor* hook = nullptr;

        // Find OUR hookshot
        if (Player_HoldsHookshot(player)) {
            for (Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; it != nullptr; it = it->next) {
                if (it->id != ACTOR_ARMS_HOOK || it->params == ZO_PUPPET_HOOKSHOT_PARAMS) {
                    continue;
                }
                hook = it;
                break;
            }
        }

        if (hook != nullptr) {
            ArmsHook* armsHook = reinterpret_cast<ArmsHook*>(hook);

            ByteStream payload;
            payload << PackedFloat4(hook->world.pos.x) << PackedFloat4(hook->world.pos.y)
                    << PackedFloat4(hook->world.pos.z);
            payload << PackedInt2(hook->shape.rot.x) << PackedInt2(hook->shape.rot.y) << PackedInt2(hook->shape.rot.z);
            payload << PackedInt2(armsHook->timer);
            payload << PackedUInt1((armsHook->actionFunc == ArmsHook_Shoot) ? 1u : 0u);

            PackProperty(PPROP_HOOKSHOT, payload, out);
        } else {
            PackNullProperty(PPROP_HOOKSHOT, out);
        }
    }

    // Fishing rod
    {
        if (player->heldItemAction == PLAYER_IA_FISHING_POLE) {
            ByteStream payload;
            payload << PackedInt2(sRodCastState);
            payload << PackedFloat4(sRodBendRotY) << PackedFloat4(D_80B7A6AC) << PackedFloat4(D_80B7A6B8);
            payload << PackedFloat4(D_80B7A6BC) << PackedFloat4(D_80B7A6C0);
            payload << PackedFloat4(player->unk_858) << PackedFloat4(player->unk_85C);
            payload << PackedFloat4(sLurePos.x) << PackedFloat4(sLurePos.y) << PackedFloat4(sLurePos.z);
            payload << PackedFloat4(sLureRot.x) << PackedFloat4(sLureRot.y) << PackedFloat4(sLureRot.z);
            payload << PackedFloat4(sLure1Rotate) << PackedFloat4(sLurePosZOffset) << PackedFloat4(sFishingLineScale);
            payload << PackedUInt1((u8)(sLureEquipped));
            PackProperty(PPROP_FISHING_ROD, payload, out);
        } else {
            PackNullProperty(PPROP_FISHING_ROD, out);
        }
    }
}

void PlayerPuppetController::DrawSwordQuadDebug(PlayState* play) {
    if (!m_hitting || !m_hasValidHitQuad) {
        return;
    }
    Vec3f* q = m_swordQuad.dim.quad;
    Collider_DrawPoly(play->state.gfxCtx, &q[2], &q[3], &q[1], 0, 255, 0);
    Collider_DrawPoly(play->state.gfxCtx, &q[1], &q[0], &q[2], 0, 255, 0);
}

void PlayerPuppetController::UpdatePuppet(PlayState* play) {
    auto player = reinterpret_cast<Player*>(m_actor);
    if (m_horse != nullptr) {
        auto horse = (EnHorse*)m_horse;
        /* if (m_horseMounted) {
            m_horse->world.pos.x = m_actor->world.pos.x + m_horsePos.x;
            m_horse->world.pos.y = m_actor->world.pos.y + m_horsePos.y;
            m_horse->world.pos.z = m_actor->world.pos.z + m_horsePos.z;
        } else*/
        {
            m_horse->world.pos.x = m_horsePos.x;
            m_horse->world.pos.y = m_horsePos.y;
            m_horse->world.pos.z = m_horsePos.z;
        }
        m_horse->shape.rot = m_horseRot;
        m_horse->world.rot = m_horseRot;

        horse->stateFlags = m_horseStateFlags;
        horse->actor.speedXZ = m_horseSpeedXZ;
        HorsePuppet_SetAnimation(m_horse, m_horseAnimIndex);
        HorsePuppet_SetAnimFrame(m_horse, m_horseAnimFrame);
    }

    {
        Actor* want = nullptr;
        if (m_heldActorId) {
            auto controller = ZeldaOnlineClient::Instance->GetNetworkController(m_heldActorId);
            if (controller) {
                want = controller->GetActor();
            }
        }

        // Check if both local player and this player is trying to hold the same actor
        auto localPlayer = GET_PLAYER(play);
        if (want != nullptr && localPlayer->heldActor == want) {
            auto* ctl = static_cast<AbstractActorController*>(want->zoController);

            if (ctl != nullptr && ctl->IsLeader()) {
                want = nullptr;
            } else {
                if (want->parent == &localPlayer->actor) {
                    want->parent = nullptr;
                }
                localPlayer->actor.child = nullptr;
                localPlayer->heldActor = nullptr;
                localPlayer->interactRangeActor = nullptr;
                localPlayer->stateFlags1 &= ~PLAYER_STATE1_CARRYING_ACTOR;
            }
        }

        bool unresolved = (m_heldActorId != 0) && (want == nullptr);
        bool holdingOurHookshot = (player->heldActor != nullptr) && (player->heldActor == m_hookshot);

        if (!unresolved && !holdingOurHookshot && player->heldActor != want) {
            if (player->heldActor != nullptr) {
                if (player->heldActor->parent == m_actor) {
                    player->heldActor->parent = nullptr;
                }
                m_actor->child = nullptr;
                player->heldActor = nullptr;
                player->interactRangeActor = nullptr;
            }
            if (want != nullptr) {
                m_actor->child = want;
                want->parent = m_actor;
                player->heldActor = want;
                player->interactRangeActor = want;
            }
        }
    }
    m_originalUpdate(m_actor, play);

    UpdateSwordHitbox(play);

    if (m_chat.m_visibleTimer > 0)
    {
        if (--m_chat.m_visibleTimer == 0)
        {
            NameTag_RemoveAllByActorTag(m_actor, "chat");
        }
    }
}

void PlayerPuppetController::KillHorse() {
    if (m_horse != nullptr) {
        m_horse->parent = nullptr;
        gZeldaOnlineEngineCleanup = true;
        Actor_Kill(m_horse);
        gZeldaOnlineEngineCleanup = false;
        m_horse = nullptr;
    }
}
void PlayerPuppetController::KillHookshot() {
    if (m_hookshot == nullptr) {
        return;
    }

    Actor_Kill(m_hookshot);
    m_hookshot = nullptr;
}

void PlayerPuppetController::UpdateSwordHitbox(PlayState* play) {
    if (CVarGetInteger("gZeldaOnline.Pvp", 0)) {
        if (!m_hitting) {
            if (m_swordQuad.base.actor != nullptr) {
                Collider_ResetQuadAT(play, &m_swordQuad.base);
                Collider_DestroyQuad(play, &m_swordQuad);
                m_swordQuad.base.actor = nullptr;
            }
            return;
        }

        if (!m_hasValidHitQuad)
            return;

        if (m_swordQuad.base.actor != nullptr) {
            CollisionCheck_SetAT(play, &play->colChkCtx, &m_swordQuad.base);
        }
    }
}

bool PlayerPuppetController::ApplyCustomProperty(unsigned int index, ByteStream& data, unsigned int propLen) {

    switch (index) {
        case PPROP_MOVEMENT_FLAGS:
            m_movementFlags = (u8)(data.Read<PackedUInt1>().value());
            return true;

        case PPROP_ON_PLATFORM:
            m_onPlatform = (u8)(data.Read<PackedUInt1>().value());
            return true;

        case PPROP_REAL_ROOM_INDEX:
            m_realRoomIndex = (s16)(data.Read<PackedInt2>().value());
            return true;

        case PPROP_PREV_TRANSL:
            m_prevTransl.x = (s16)(data.Read<PackedInt2>().value());
            m_prevTransl.y = (s16)(data.Read<PackedInt2>().value());
            m_prevTransl.z = (s16)(data.Read<PackedInt2>().value());
            return true;
        case PPROP_UPPER_LIMB_ROT:
            m_upperLimbRot.x = (s16)(data.Read<PackedInt2>().value());
            m_upperLimbRot.y = (s16)(data.Read<PackedInt2>().value());
            m_upperLimbRot.z = (s16)(data.Read<PackedInt2>().value());
            return true;

        case PPROP_UNK_3BC:
            m_unk_3BC.x = (s16)(data.Read<PackedInt2>().value());
            m_unk_3BC.y = (s16)(data.Read<PackedInt2>().value());
            m_unk_3BC.z = (s16)(data.Read<PackedInt2>().value());
            return true;

        case PPROP_NICKNAME:
            m_nickName = data.ReadString(propLen);
            return true;

        case PPROP_PAUSED:
            m_paused = data.Read<PackedUInt1>().value();
            return true;

        case PPROP_POSE:
            for (int i = 0; i < 24; i++) {
                m_jointTable[i].x = (s16)(data.Read<PackedInt2>().value());
                m_jointTable[i].y = (s16)(data.Read<PackedInt2>().value());
                m_jointTable[i].z = (s16)(data.Read<PackedInt2>().value());
            }
            return true;
        case PPROP_STATE_FLAGS1:
            m_stateFlags1 = data.Read<PackedUInt4>().value();
            return true;
        case PPROP_STATE_FLAGS2:
            m_stateFlags2 = data.Read<PackedUInt4>().value();
            return true;
        case PPROP_ITEM_ACTION:
            m_itemAction = (s8)(data.Read<PackedInt1>().value());
            return true;
        case PPROP_HELD_ITEM_ACTION:
            m_heldItemAction = (s8)(data.Read<PackedInt1>().value());
            return true;
        case PPROP_MODEL_GROUP:
            m_modelGroup = (s32)(data.Read<PackedUInt1>().value());
            return true;
        case PPROP_LEFT_HAND_TYPE:
            m_modelLeftHandType = (u8)(data.Read<PackedUInt1>().value());
            return true;
        case PPROP_RIGHT_HAND_TYPE:
            m_modelRightHandType = (u8)(data.Read<PackedUInt1>().value());
            return true;
        case PPROP_SHEATH_TYPE:
            m_modelSheathType = (u8)(data.Read<PackedUInt1>().value());
            return true;
        case PPROP_INVINCIBILITY:
            m_invincibilityTimer = (s8)(data.Read<PackedInt1>().value());
            return true;
        case PPROP_HELD_GET_ITEM:
            m_heldGetItemId = (s16)(data.Read<PackedInt2>().value());
            return true;
        case PPROP_CS_ACTION:
            m_csAction = (u8)(data.Read<PackedUInt1>().value());
            return true;
        case PPROP_HELD_ITEM_SCALE:
            m_unk_85C = data.Read<PackedFloat4>().value();
            return true;
        case PPROP_STICK_FLAME_TIMER:
            m_unk860 = (s16)(data.Read<PackedInt2>().value());
            return true;
        case PPROP_TUNIC:
            m_currentTunic = (s8)(data.Read<PackedInt1>().value());
            return true;

        case PPROP_BOOTS:
            m_currentBoots = (s8)(data.Read<PackedInt1>().value());
            return true;

        case PPROP_SHIELD:
            m_currentShield = (s8)(data.Read<PackedInt1>().value());

            return true;

        case PPROP_BUTTONITEM0:
            m_buttonItem0 = (u8)(data.Read<PackedUInt1>().value());
            return true;

        case PPROP_HORSE_PRESENT:
            m_pendingHorsePresent = (u8)(data.Read<PackedUInt1>().value());
            return true;
        case PPROP_HORSE_MOUNTED:
            m_horseMounted = (u8)(data.Read<PackedUInt1>().value()) & 1;
            return true;
        case PPROP_HORSE_ANIM:
            m_horseAnimIndex = (u8)(data.Read<PackedUInt1>().value());
            return true;
        case PPROP_HORSE_FRAME:
            m_horseAnimFrame = (u8)(data.Read<PackedUInt1>().value());
            return true;
        case PPROP_HORSE_POS_X:
            m_horsePos.x = data.Read<PackedFloat4>().value();
            return true;
        case PPROP_HORSE_POS_Y:
            m_horsePos.y = data.Read<PackedFloat4>().value();
            return true;

        case PPROP_HORSE_POS_Z:
            m_horsePos.z = data.Read<PackedFloat4>().value();
            return true;
        case PPROP_HORSE_ROT_X:
            m_horseRot.x = (s16)(data.Read<PackedInt2>().value());
            return true;
        case PPROP_HORSE_ROT_Y:
            m_horseRot.y = (s16)(data.Read<PackedInt2>().value());
            return true;
        case PPROP_HORSE_ROT_Z:
            m_horseRot.z = (s16)(data.Read<PackedInt2>().value());
            return true;

        case PPROP_HORSE_STATE_FLAGS:
            m_horseStateFlags = (u8)data.Read<PackedUInt1>().value();

            return true;

        case PPROP_HORSE_SPEEDXZ:
            m_horseSpeedXZ = data.Read<PackedFloat4>().value();
        return true;

        case PPROP_HELD_ACTOR:
            m_heldActorId = data.Read<PackedUInt4>().value();
            return true;

        case PPROP_MASK:
            m_currentMask = data.Read<PackedUInt1>().value();
        return true;

        case PPROP_SOUND_FREQUENCY:
            m_soundFreqMultiplier = data.Read<PackedInt2>().value() / 100.0f;
            return true;

        case PPROP_HITTING: {
            if (propLen == 0) {
                m_hitting = m_hasValidHitQuad = false;
            } else {
                if (m_hitting) {
                    m_meleePrevTip = m_hittingTip;
                    m_meleePrevBase = m_hittingBase;
                    m_hasValidHitQuad = true;
                } else {
                    m_hasValidHitQuad = false;
                }
                m_hitting = true;

                m_hittingTip.x = data.Read<PackedFloat4>().value();
                m_hittingTip.y = data.Read<PackedFloat4>().value();
                m_hittingTip.z = data.Read<PackedFloat4>().value();
                m_hittingBase.x = data.Read<PackedFloat4>().value();
                m_hittingBase.y = data.Read<PackedFloat4>().value();
                m_hittingBase.z = data.Read<PackedFloat4>().value();
                m_hittingDmgFlags = data.Read<PackedUInt4>().value();
                m_hittingDamage = data.Read<PackedUInt1>().value();

                if (m_hasValidHitQuad) {
                    if (m_swordQuad.base.actor == nullptr) {
                        Collider_InitQuad(gPlayState, &m_swordQuad);
                        Collider_SetQuad(gPlayState, &m_swordQuad, m_actor, &sPuppetSwordQuadInit);
                    }
                    m_swordQuad.info.toucher.dmgFlags = m_hittingDmgFlags;
                    m_swordQuad.info.toucher.damage = m_hittingDamage * 4;
                    m_swordQuad.base.atFlags = AT_ON | AT_TYPE_ENEMY;

                    Collider_SetQuadVertices(&m_swordQuad, &m_hittingBase, &m_hittingTip, &m_meleePrevBase,
                                             &m_meleePrevTip);
                }
            }
            return true;
        }

        case PPROP_BOOMERANG: {
            if (propLen == 0) {
                KillBoomerang();
                return true;
            }

            Vec3f pos;
            Vec3s rot;
            pos.x = data.Read<PackedFloat4>().value();
            pos.y = data.Read<PackedFloat4>().value();
            pos.z = data.Read<PackedFloat4>().value();
            rot.x = (s16)(data.Read<PackedInt2>().value());
            rot.y = (s16)(data.Read<PackedInt2>().value());
            rot.z = (s16)(data.Read<PackedInt2>().value());
            bool pvp = data.Read<PackedUInt1>().value();

            bool pvpActive = pvp && CVarGetInteger("gZeldaOnline.Pvp", 0) != 0;

            if (m_boomerang == nullptr) {

                auto boom = reinterpret_cast<EnBoom*>(
                    Actor_SpawnDirect(&gPlayState->actorCtx, gPlayState, ACTOR_EN_BOOM, pos.x, pos.y, pos.z, rot.x,
                                      rot.y, rot.z, pos.x, pos.y, pos.z, rot.x, rot.y, rot.z, 0, 0));
                if (boom != nullptr) {
                    boom->actor.update = BoomerangPuppet_Update;
                    boom->actor.flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED;
                    boom->actor.room = -1;
                    boom->collider.info.toucher.damage = 4;
                    m_boomerang = (Actor*)boom;
                }
            }

            if (m_boomerang != nullptr) {
                m_boomerang->world.pos = pos;
                m_boomerang->shape.rot = rot;
                m_boomerang->world.rot = rot;

                auto boom = reinterpret_cast<EnBoom*>(m_boomerang);

                if (pvpActive) {
                    boom->collider.base.atFlags = AT_ON | AT_TYPE_ENEMY;
                } else {
                    boom->collider.base.atFlags &= ~AT_ON;
                }
            }
            return true;
        }

        case PPROP_HOOKSHOT: {
            if (propLen == 0) {
                KillHookshot();
                return true;
            }
            auto player = reinterpret_cast<Player*>(m_actor);
            Vec3f pos;
            Vec3s rot;
            pos.x = data.Read<PackedFloat4>().value();
            pos.y = data.Read<PackedFloat4>().value();
            pos.z = data.Read<PackedFloat4>().value();
            rot.x = (s16)(data.Read<PackedInt2>().value());
            rot.y = (s16)(data.Read<PackedInt2>().value());
            rot.z = (s16)(data.Read<PackedInt2>().value());

            s16 timer = (s16)(data.Read<PackedInt2>().value());
            u8 shooting = (u8)(data.Read<PackedUInt1>().value());

            if (m_hookshot == nullptr) {
                Actor* hook = Actor_SpawnAsChildDirect(&gPlayState->actorCtx, m_actor, gPlayState, ACTOR_ARMS_HOOK,
                                                       pos.x, pos.y, pos.z, rot.x, rot.y, rot.z, pos.x, pos.y, pos.z,
                                                       rot.x, rot.y, rot.z, ZO_PUPPET_HOOKSHOT_PARAMS, 0);
                if (hook != nullptr) {
                    hook->update = HookshotPuppet_Update;
                    hook->draw = HookshotPuppet_Draw;
                    hook->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED;
                    hook->room = -1;
                    m_hookshot = hook;
                    player->heldActor = m_hookshot;
                }
            }

            if (m_hookshot != nullptr) {
                ArmsHook* armsHook = reinterpret_cast<ArmsHook*>(m_hookshot);

                m_hookshot->world.pos = pos;
                m_hookshot->shape.rot = rot;
                m_hookshot->world.rot = rot;

                armsHook->timer = timer;
                armsHook->actionFunc = shooting ? ArmsHook_Shoot : nullptr;

                if (shooting) {
                    player->heldActor = nullptr;
                    m_actor->child = nullptr;
                } else {
                    player->heldActor = m_hookshot;
                    m_actor->child = m_hookshot;
                }
            }
            return true;
        }

        case PPROP_FISHING_ROD: {
            if (propLen == 0) {
                m_fishing.m_rodCastState = -1; // not holding a rod
                return true;
            }

            m_fishing.m_rodCastState = (s16)(data.Read<PackedInt2>().value());
            m_fishing.m_rodBendRotY = data.Read<PackedFloat4>().value();
            m_fishing.m_rodBendX = data.Read<PackedFloat4>().value();
            m_fishing.m_rodCastBend = data.Read<PackedFloat4>().value();
            m_fishing.m_rodHookBend = data.Read<PackedFloat4>().value();
            m_fishing.m_rodHitBend = data.Read<PackedFloat4>().value();
            m_fishing.m_rodStickX = data.Read<PackedFloat4>().value();
            m_fishing.m_rodStickY = data.Read<PackedFloat4>().value();
            m_fishing.m_lurePos.x = data.Read<PackedFloat4>().value();
            m_fishing.m_lurePos.y = data.Read<PackedFloat4>().value();
            m_fishing.m_lurePos.z = data.Read<PackedFloat4>().value();
            m_fishing.m_lureRot.x = data.Read<PackedFloat4>().value();
            m_fishing.m_lureRot.y = data.Read<PackedFloat4>().value();
            m_fishing.m_lureRot.z = data.Read<PackedFloat4>().value();
            m_fishing.m_lure1Rotate = data.Read<PackedFloat4>().value();
            m_fishing.m_lurePosZOffset = data.Read<PackedFloat4>().value();
            m_fishing.m_lineScale = data.Read<PackedFloat4>().value();
            m_fishing.m_lureEquipped = (u8)(data.Read<PackedUInt1>().value());
            return true;
        }

        case PPROP_TUNIC_COLOUR:
            m_tunicColour.r = data.Read<PackedUInt1>().value();
            m_tunicColour.g = data.Read<PackedUInt1>().value();
            m_tunicColour.b = data.Read<PackedUInt1>().value();
            return true;

        case PPROP_SPEEDXZ:
            m_actor->speedXZ = data.Read<PackedFloat4>().value();
            return true;
        default:
            return false;
    }
}

void PlayerPuppetController::OnPropertiesApplied(u64 changed) {
    m_horseType = HORSE_EPONA;

    if (changed & (1ull << PPROP_HORSE_PRESENT)) {
        u8 riding = m_pendingHorsePresent ? 1 : 0;
        if (riding != m_riding) {
            m_riding = riding;
            if (riding && m_horse == nullptr) {
                /* f32 spawnX = m_horseMounted ? m_actor->world.pos.x + m_horsePos.x : (f32)m_horsePos.x;
                f32 spawnY = m_horseMounted ? m_actor->world.pos.y + m_horsePos.y : (f32)m_horsePos.y;
                f32 spawnZ = m_horseMounted ? m_actor->world.pos.z + m_horsePos.z : (f32)m_horsePos.z;*/

                f32 spawnX = m_horsePos.x;
                f32 spawnY = m_horsePos.y;
                f32 spawnZ = m_horsePos.z;

                s16 spawnParams = (s16)(ZO_HORSE_PUPPET | ((m_horseType == HORSE_HNI) ? 0x8000 : 0));

                m_horse = Actor_SpawnAsChildDirect(&gPlayState->actorCtx, m_actor, gPlayState, ACTOR_EN_HORSE, spawnX,
                                                   spawnY, spawnZ, 0, m_horseRot.y, 0, spawnX, spawnY, spawnZ, 0,
                                                   m_horseRot.y, 0, spawnParams, 0);
            } else if (!riding && m_horse != nullptr) {
                KillHorse();
            }
        }
    }

    if (changed & (1ull << PPROP_NICKNAME)) {
        NameTagOptions options{};
        options.tag = "name";
        options.textColor = m_paused ? Color_RGBA8{ 0, 128, 255, 255 } : Color_RGBA8{ 255, 255, 255, 255 };
        NameTag_RemoveAllByActorTag(m_actor, "name");
        NameTag_RegisterForActorWithOptions(m_actor, m_nickName.c_str(), options);
    }

    if (changed & (1ull << PPROP_PAUSED)) {
        Color_RGBA8 colour = m_paused ? Color_RGBA8{ 0, 128, 255, 255 } : Color_RGBA8{ 255, 255, 255, 255 };
        NameTag_ChangeActorTextColour(m_actor, &colour, "name");
    }
    if (0) {
        Player* player = (Player*)m_actor;
        if ((player->stateFlags1 & (PLAYER_STATE1_IN_CUTSCENE)) && player->csAction != 0) {
            u32 h = (u32)m_networkID * 2654435761u;
            f32 angle = (h >> 16) * (2.0f * (f32)M_PI / 65536.0f);
            f32 radius = 40.0f + (h & 0x1F);
            m_actor->world.pos.x += cosf(angle) * radius;
            m_actor->world.pos.z += sinf(angle) * radius;
        }
    }
}

void PlayerPuppetController::DrawFishingRod(PlayState* play) {
    Player* player = reinterpret_cast<Player*>(m_actor);

    GraphicsContext* __gfxCtx = play->state.gfxCtx;
    Gfx* dispRefs[4];
    Graph_OpenDisps(dispRefs, play->state.gfxCtx, __FILE__, __LINE__);

    Gfx_SetupDL_25Opa(play->state.gfxCtx);

    gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gFishingRodMaterialDL);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, 155, 0, 255);

    Matrix_Mult(&player->mf_9E0, MTXMODE_NEW);

    if (m_linkAge != LINK_AGE_CHILD) {
        Matrix_Translate(0.0f, 400.0f, 0.0f, MTXMODE_APPLY);
    } else {
        Matrix_Translate(0.0f, 230.0f, 0.0f, MTXMODE_APPLY);
    }

    if (m_fishing.m_rodCastState == 5) {
        Matrix_RotateY(0.56f * (f32)M_PI, MTXMODE_APPLY);
    } else {
        Matrix_RotateY(0.41f * (f32)M_PI, MTXMODE_APPLY);
    }

    Matrix_RotateX(-(f32)M_PI / 5.0000003f, MTXMODE_APPLY);
    Matrix_RotateZ((m_fishing.m_rodStickX * 0.5f) + 3.0f * (f32)M_PI / 20.0f, MTXMODE_APPLY);
    Matrix_RotateX((m_fishing.m_rodHitBend + 20.0f) * 0.01f * (f32)M_PI, MTXMODE_APPLY);
    Matrix_Scale(0.70000005f, 0.70000005f, 0.70000005f, MTXMODE_APPLY);

    f32 bendX = (m_fishing.m_rodHookBend * (((m_fishing.m_rodStickY - 1.0f) * -0.25f) + 0.5f)) +
                (m_fishing.m_rodBendX + m_fishing.m_rodCastBend);

    Matrix_Translate(0.0f, 0.0f, -1300.0f, MTXMODE_APPLY);

    for (s16 i = 0; i < 22; i++) {
        Matrix_RotateY(sRodBendRatios[i] * m_fishing.m_rodBendRotY * 0.5f, MTXMODE_APPLY);
        Matrix_RotateX(sRodBendRatios[i] * bendX * 0.5f, MTXMODE_APPLY);

        Matrix_Push();
        Matrix_Scale(sRodScales[i], sRodScales[i], 0.52f, MTXMODE_APPLY);

        gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

        if (i < 5) {
            gDPLoadTextureBlock(POLY_OPA_DISP++, gFishingRodSegmentBlackTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 8, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, 3, G_TX_NOLOD, G_TX_NOLOD);
        } else if ((i < 8) || ((i % 2) == 0)) {
            gDPLoadTextureBlock(POLY_OPA_DISP++, gFishingRodSegmentWhiteTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 8, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, 3, G_TX_NOLOD, G_TX_NOLOD);
        } else {
            gDPLoadTextureBlock(POLY_OPA_DISP++, gFishingRodSegmentStripTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 8, 0,
                                G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP, 4, 3, G_TX_NOLOD, G_TX_NOLOD);
        }

        gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gFishingRodSegmentDL);

        Matrix_Pop();
        Matrix_Translate(0.0f, 0.0f, 500.0f, MTXMODE_APPLY);

        if (i == 21) {
            Matrix_MultVec3f(&sRodTipOffset, &m_fishing.m_rodTipPos);
        }
    }

    Graph_CloseDisps(dispRefs, play->state.gfxCtx, __FILE__, __LINE__);
}

void PlayerPuppetController::DrawFishingLureAndLine(PlayState* play) {
    GraphicsContext* __gfxCtx = play->state.gfxCtx;
    Gfx* dispRefs[4];
    Graph_OpenDisps(dispRefs, play->state.gfxCtx, __FILE__, __LINE__);

    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    Matrix_Push();

    if (m_fishing.m_lureEquipped != FS_LURE_SINKING) {
        Vec3f posSrc;
        Vec3f hookPos[2];

        Matrix_Translate(m_fishing.m_lurePos.x, m_fishing.m_lurePos.y, m_fishing.m_lurePos.z, MTXMODE_NEW);
        Matrix_RotateY(m_fishing.m_lureRot.y + m_fishing.m_lure1Rotate, MTXMODE_APPLY);
        Matrix_RotateX(m_fishing.m_lureRot.x, MTXMODE_APPLY);
        Matrix_Scale(0.0039999997f, 0.0039999997f, 0.0039999997f, MTXMODE_APPLY);
        Matrix_Translate(0.0f, 0.0f, m_fishing.m_lurePosZOffset, MTXMODE_APPLY);
        Matrix_RotateZ((f32)M_PI / 2, MTXMODE_APPLY);
        Matrix_RotateY((f32)M_PI / 2, MTXMODE_APPLY);

        Gfx_SetupDL_25Opa(play->state.gfxCtx);

        gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_OPA_DISP++, (Gfx*)gFishingLureFloatDL);

        posSrc.x = 500.0f;
        posSrc.y = 0.0f;
        posSrc.z = -300.0f;
        Matrix_MultVec3f(&posSrc, &hookPos[0]);
        Fishing_DrawLureHook(play, &hookPos[0], &m_fishing.m_lureHookRefPos[0], 0);

        posSrc.x = 2100.0f;
        posSrc.z = -50.0f;
        Matrix_MultVec3f(&posSrc, &hookPos[1]);
        Fishing_DrawLureHook(play, &hookPos[1], &m_fishing.m_lureHookRefPos[1], 1);
    }

    POLY_XLU_DISP = Gfx_SetupDL(POLY_XLU_DISP, 0x14);

    gDPSetCombineMode(POLY_XLU_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 255, 255, 55);

    {
        f32 dx = m_fishing.m_lurePos.x - m_fishing.m_rodTipPos.x;
        f32 dy = m_fishing.m_lurePos.y - m_fishing.m_rodTipPos.y;
        f32 dz = m_fishing.m_lurePos.z - m_fishing.m_rodTipPos.z;

        f32 ry = Math_FAtan2F(dx, dz);
        f32 dist = sqrtf(SQ(dx) + SQ(dz));
        f32 rx = -Math_FAtan2F(dy, dist);

        dist = sqrtf(SQ(dx) + SQ(dy) + SQ(dz)) * 0.001f;

        Matrix_Translate(m_fishing.m_rodTipPos.x, m_fishing.m_rodTipPos.y, m_fishing.m_rodTipPos.z, MTXMODE_NEW);
        Matrix_RotateY(ry, MTXMODE_APPLY);
        Matrix_RotateX(rx, MTXMODE_APPLY);
        Matrix_Scale(m_fishing.m_lineScale, 1.0f, dist, MTXMODE_APPLY);

        gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_XLU_DISP++, (Gfx*)gFishingLineModelDL);
    }

    Matrix_Pop();
    Gfx_SetupDL_25Xlu(play->state.gfxCtx);

    Graph_CloseDisps(dispRefs, play->state.gfxCtx, __FILE__, __LINE__);
}

void PlayerPuppetController::SetChatText(const std::string& text, const Color_RGBA8& colour) {
    m_chat.m_text = text;
    m_chat.m_visibleTimer = 20 * 8;

    NameTagOptions options{};
    options.tag = "chat";
    options.textColor = colour;
    options.yOffset = -10;
    options.scale = 0.5f;
    NameTag_RemoveAllByActorTag(m_actor, "chat");
    NameTag_RegisterForActorWithOptions(m_actor, text.c_str(), options);
}

void PlayerPuppetController::PuppetInit(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;
    auto* self = static_cast<PlayerPuppetController*>(actor->zoController);

    if (!self)
        return;

    u8 linkAge = self->m_linkAge;

    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = linkAge;

    actor->room = -1;
    player->itemAction = player->heldItemAction = -1;
    player->heldItemId = ITEM_NONE;
    Player_UseItem(play, player, ITEM_NONE);
    Player_SetModelGroup(player, Player_ActionToModelGroup(player, player->heldItemAction));


    player->skin = PlayerSkin_Get(self->SkinName().c_str());
    play->playerInit(player, play, player->skin->skel[linkAge]);

    Effect_Delete(play, player->meleeWeaponEffectIndex);
    player->meleeWeaponEffectIndex = TOTAL_EFFECT_COUNT;

    play->func_11D54(player, play);

    player->cylinder.base.acFlags = AC_ON | AC_TYPE_PLAYER;
    player->cylinder.base.ocFlags2 = OC2_TYPE_1;
    player->cylinder.info.bumperFlags = BUMP_ON | BUMP_HOOKABLE | BUMP_NO_HITMARK;
    player->actor.flags |= ACTOR_FLAG_HOOKSHOT_PULLS_PLAYER;

    player->actor.flags |= ACTOR_FLAG_CAN_PRESS_SWITCHES;

    player->cylinder.dim = GET_PLAYER(play)->cylinder.dim;

    gSaveContext.linkAge = originalAge;
}


void PlayerPuppetController::PuppetUpdate(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;
    auto* self = static_cast<PlayerPuppetController*>(actor->zoController);
    if (self == nullptr) {
        return;
    }

    auto sceneNum = gPlayState->sceneNum;

    if (IS_CUTSCENE_LAYER) {
        player->actor.draw = nullptr;
    } else {
        player->actor.draw = PuppetDraw;

        actor->shape.shadowAlpha = 255;

        player->skelAnime.movementFlags = self->m_movementFlags;
        Puppet_Vec3sCopy(&player->unk_3BC, &self->m_unk_3BC);
        Puppet_Vec3sCopy(&player->skelAnime.prevTransl, &self->m_prevTransl);
        player->currentBoots = self->m_currentBoots;
        player->currentShield = self->m_currentShield;
        player->heldItemId = self->m_buttonItem0;
        player->currentTunic = self->m_currentTunic;
        player->stateFlags1 = self->m_stateFlags1;
        player->stateFlags2 = self->m_stateFlags2 & ~PLAYER_STATE2_DISABLE_DRAW;
        player->csAction = self->m_csAction;
        player->itemAction = self->m_itemAction;
        player->heldItemAction = self->m_heldItemAction;

        player->unk_85C = self->m_unk_85C;
        player->invincibilityTimer = self->m_invincibilityTimer;
        player->unk_862 = (self->m_heldGetItemId > (s16)GID_MAXIMUM) ? (s16)GID_STONE_OF_AGONY : self->m_heldGetItemId;
        player->av1.actionVar1 = self->m_actionVar1;
        player->unk_860 = self->m_unk860;

        Vec3f diff;
        SkelAnime_UpdateTranslation(&player->skelAnime, &diff, player->actor.shape.rot.y);

        if (player->modelGroup != self->m_modelGroup && (u32)(self->m_modelGroup) < PLAYER_MODELGROUP_MAX) {
            s32 originalAge = gSaveContext.linkAge;
            gSaveContext.linkAge = self->m_linkAge;
            u8 originalButtonItem0 = gSaveContext.equips.buttonItems[0];
            gSaveContext.equips.buttonItems[0] = self->m_buttonItem0;
            Player_SetModelGroup(player, self->m_modelGroup);
            gSaveContext.linkAge = originalAge;
            gSaveContext.equips.buttonItems[0] = originalButtonItem0;
        }

        player->leftHandType = self->m_modelLeftHandType;
        player->rightHandType = self->m_modelRightHandType;
        player->sheathType = self->m_modelSheathType;

        u8 dlAge = (self->m_linkAge != 0) ? 1 : 0;

        PlayerSkin* skin = Player_GetSkin(player);

        if ((u32)self->m_modelLeftHandType < PLAYER_MODELTYPE_MAX) {
            player->leftHandDLists = &skin->dlistGroups[self->m_modelLeftHandType][dlAge];
        }
        if ((u32)self->m_modelRightHandType < PLAYER_MODELTYPE_MAX) {
            player->rightHandDLists = &skin->dlistGroups[self->m_modelRightHandType][dlAge];
        }
        if ((u32)self->m_modelSheathType < PLAYER_MODELTYPE_MAX) {
            player->sheathDLists = &skin->dlistGroups[self->m_modelSheathType][dlAge];
        }
        player->waistDLists = &skin->dlistGroups[PLAYER_MODELTYPE_WAIST][dlAge];
        player->currentMask = self->m_currentMask;

        actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;

        Collider_UpdateCylinder(&player->actor, &player->cylinder);
        if (player->actor.velocity.y > 0.0f) {
            player->actor.velocity.y = 0.0f;
        }

        Vec3f syncedPos = player->actor.world.pos;
        Actor_UpdateBgCheckInfo(play, &player->actor, 26.0f, 6.0f, player->ageProperties->ceilingCheckHeight, 7);

        if (!self->m_onPlatform)
            player->actor.world.pos = syncedPos;

        if (!(player->stateFlags2 & PLAYER_STATE2_FROZEN)) {
            if (!self->m_paused && !(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_HANGING_OFF_LEDGE |
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

        if (player->heldItemAction == PLAYER_IA_DEKU_STICK && player->unk_860 > 0) {
            f32 flameScale = 1.0f;
            if (player->unk_860 > 200) {
                flameScale = (210 - player->unk_860) / 10.0f;
            } else if (player->unk_860 < 20) {
                flameScale = player->unk_860 / 20.0f;
                player->unk_85C = flameScale;
            }
            func_8002836C(play, &player->meleeWeaponInfo[0].tip, &D_808547A4, &D_808547B0, &D_808547BC, &D_808547C0,
                          (s16)(flameScale * 200.0f), 0, 8);
        }

        Collider_ResetCylinderAC(play, &player->cylinder.base);

        if (player->currentMask == PLAYER_MASK_BUNNY) {
            Player_UpdateBunnyEars(player, &self->m_bunnyEarKinematics);
        }
    }
}

void PlayerPuppetController::PuppetDraw(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    auto* self = static_cast<PlayerPuppetController*>(actor->zoController);
    if (self == nullptr) {
        return;
    }


    /*
    if (self->m_onPlatform) {
        CollisionPoly* poly = NULL;
        s32 bgId = BGCHECK_SCENE;
        Vec3f from = player->actor.world.pos;
        from.y += 50.0f;

        f32 floorY = BgCheck_EntityRaycastFloor4(&play->colCtx, &poly, &bgId, &player->actor, &from);
        if (bgId != BGCHECK_SCENE) {
            player->actor.world.pos.y = floorY;
        }
    }*/

    Puppet_Vec3sCopy(&player->upperLimbRot, &self->m_upperLimbRot);
    for (s32 i = 0; i < PLAYER_LIMB_BUF_COUNT; i++) {
        player->skelAnime.jointTable[i] = self->m_jointTable[i];
    }

    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = self->m_linkAge;
    u8 originalButtonItem0 = gSaveContext.equips.buttonItems[0];
    gSaveContext.equips.buttonItems[0] = self->m_buttonItem0;

    
    auto oldBunnyEarKinermatics = gCurrentDrawBunnyEar;
    gCurrentDrawBunnyEar = &self->m_bunnyEarKinematics;
    Player_Draw((Actor*)player, play);
    gCurrentDrawBunnyEar = oldBunnyEarKinermatics;

    if (self->m_fishing.m_rodCastState >= 0) {
        self->DrawFishingRod(play);
        self->DrawFishingLureAndLine(play);
    }
    
    gSaveContext.linkAge = originalAge;
    gSaveContext.equips.buttonItems[0] = originalButtonItem0;
}

void PlayerPuppetController::PuppetDestroy(Actor* actor, PlayState* play) {
    actor->id = ACTOR_PLAYER;
}
} // namespace ZeldaOnline
