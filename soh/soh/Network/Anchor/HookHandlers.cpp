#include "Anchor.h"
#include <libultraship/libultraship.h>
#include <chrono>
#include "soh/Enhancements/cosmetics/cosmeticsTypes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/custom-message/CustomMessageManager.h"
#include "soh/Notification/Notification.h"
#include "soh/frame_interpolation.h"
#include "soh/OTRGlobals.h"
#include "soh/Network/Anchor/BossSync/BossSyncDispatch.h"
#include "soh/Network/Anchor/BossSync/BossSyncRegistry.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_En_Horse/z_en_horse.h"
#include "src/overlays/actors/ovl_Bg_Bombwall/z_bg_bombwall.h"
#include "src/overlays/actors/ovl_Bg_Breakwall/z_bg_breakwall.h"
#include "src/overlays/actors/ovl_Bg_Haka_Zou/z_bg_haka_zou.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Hamstep/z_bg_hidan_hamstep.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Hrock/z_bg_hidan_hrock.h"
#include "src/overlays/actors/ovl_Bg_Ice_Shelter/z_bg_ice_shelter.h"
#include "src/overlays/actors/ovl_Bg_Jya_Bombchuiwa/z_bg_jya_bombchuiwa.h"
#include "src/overlays/actors/ovl_Bg_Jya_Bombiwa/z_bg_jya_bombiwa.h"
#include "src/overlays/actors/ovl_Bg_Mizu_Bwall/z_bg_mizu_bwall.h"
#include "src/overlays/actors/ovl_Bg_Spot08_Bakudankabe/z_bg_spot08_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Spot11_Bakudankabe/z_bg_spot11_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Spot17_Bakudankabe/z_bg_spot17_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Ydan_Maruta/z_bg_ydan_maruta.h"
#include "src/overlays/actors/ovl_Bg_Ydan_Sp/z_bg_ydan_sp.h"
#include "src/overlays/actors/ovl_Bg_Ydan_Hasi/z_bg_ydan_hasi.h"
#include "src/overlays/actors/ovl_Bg_Mizu_Water/z_bg_mizu_water.h"
#include "src/overlays/actors/ovl_Door_Shutter/z_door_shutter.h"
#include "src/overlays/actors/ovl_En_Door/z_en_door.h"
#include "src/overlays/actors/ovl_En_Si/z_en_si.h"
#include "src/overlays/actors/ovl_En_Sw/z_en_sw.h"
#include "src/overlays/actors/ovl_En_Ru1/z_en_ru1.h"
#include "src/overlays/actors/ovl_En_Okarina_Tag/z_en_okarina_tag.h"
#include "src/overlays/actors/ovl_Item_B_Heart/z_item_b_heart.h"
#include "src/overlays/actors/ovl_Obj_Bombiwa/z_obj_bombiwa.h"
#include "src/overlays/actors/ovl_Obj_Hamishi/z_obj_hamishi.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Dalm/z_bg_hidan_dalm.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Kowarerukabe/z_bg_hidan_kowarerukabe.h"
#include "objects/gameplay_keep/gameplay_keep.h"

extern PlayState* gPlayState;
extern MapData* gMapData;

// Water/level actors whose derived state (colHeader waterBoxes ySurface, timers,
// draw=NULL, Flags_UnsetSwitch) is only written inside Update. They must run
// locally on every client (driven by already-synced switch/event flags), not be
// frozen and position-keyframed. Post-Init params: Bg_Ydan_Hasi = params&0xFF,
// Bg_Spot06_Objects = (params>>8)&0xFF (2 == LHO_WATER_PLANE).
// Bg_Ydan_Hasi params 0 (floating block) stays keyframe-driven (local sin phase).
bool IsFlagDrivenWaterBgActor(const Actor* a) {
    if (a == nullptr) {
        return false;
    }
    switch (a->id) {
        case ACTOR_BG_YDAN_HASI:
            return a->params == HASI_WATER || a->params == HASI_THREE_BLOCKS;
        case ACTOR_BG_SPOT06_OBJECTS:
            return a->params == 2;
        case ACTOR_BG_MIZU_WATER:
            // Level is flag-driven (0x1C-0x1E); keyframes would fight StepToF.
            return ((const BgMizuWater*)a)->type == 0;
        default:
            return false;
    }
}

bool ShouldKeepDungeonBgActorUpdating(const Actor* actor) {
    if (actor == nullptr) {
        return false;
    }

    if (IsFlagDrivenWaterBgActor(actor)) {
        return true;
    }

    // Only the Octo-platform (params==0, drives the Big Octo child-state
    // handshake) and the water-level actor (params==2, BgBdanObjects_WaitForSwitch
    // -> ChangeWaterBoxLevel, raises/lowers colHeader->waterBoxes[7] based on the
    // already flag-synced switchFlag) need to keep running locally. The elevator
    // (params==1) and falling-platform variant are plain moving platforms and
    // must stay network-position-driven like any other BG mover, otherwise their
    // motion visibly desyncs between clients.
    if (actor->id == ACTOR_BG_BDAN_OBJECTS) {
        return actor->params == 0 || actor->params == 2;
    }

    switch (actor->id) {
        case ACTOR_BG_BDAN_SWITCH:
        case ACTOR_BG_BOMBWALL:
        case ACTOR_BG_HIDAN_FIREWALL:
        case ACTOR_BG_HIDAN_DALM:
        case ACTOR_BG_HIDAN_HROCK:
        case ACTOR_BG_HIDAN_ROCK:
        case ACTOR_BG_HIDAN_RSEKIZOU:
        case ACTOR_BG_HIDAN_SEKIZOU:
        case ACTOR_BG_HIDAN_SIMA:
        case ACTOR_BG_HIDAN_SYOKU:
        case ACTOR_BG_HIDAN_KOUSI:
        case ACTOR_BG_HIDAN_HAMSTEP:
        case ACTOR_BG_HIDAN_FWBIG:
        case ACTOR_BG_HIDAN_KOWARERUKABE:
        case ACTOR_BG_MORI_ELEVATOR:
        case ACTOR_BG_MORI_BIGST:
        case ACTOR_BG_MORI_HINERI:
        case ACTOR_BG_MORI_KAITENKABE:
        case ACTOR_BG_MORI_RAKKATENJO:
        case ACTOR_BG_MORI_HASHIGO:
        case ACTOR_BG_MORI_HASHIRA4:
        case ACTOR_BG_MORI_IDOMIZU:
        case ACTOR_BG_HIDAN_FSLIFT:
        case ACTOR_BG_MIZU_MOVEBG:
        case ACTOR_BG_MIZU_BWALL:
        case ACTOR_BG_MIZU_WATER:
        case ACTOR_BG_MIZU_UZU:
        case ACTOR_BG_MIZU_SHUTTER:
        case ACTOR_BG_HAKA_WATER:
        case ACTOR_BG_HAKA_GATE:
        case ACTOR_BG_HAKA_MEGANE:
        case ACTOR_BG_HAKA_MEGANEBG:
        case ACTOR_BG_HAKA_SHIP:
        case ACTOR_BG_HAKA_SGAMI:
        case ACTOR_BG_HAKA_TUBO:
        case ACTOR_BG_HAKA_TRAP:
        case ACTOR_BG_HAKA_HUTA:
        case ACTOR_BG_HAKA_ZOU:
        case ACTOR_BG_GJYO_BRIDGE:
        case ACTOR_BG_GATE_SHUTTER:
        case ACTOR_BG_SPOT01_FUSYA:
        case ACTOR_BG_SPOT01_IDOHASHIRA:
        case ACTOR_BG_SPOT01_IDOMIZU:
        case ACTOR_BG_SPOT01_IDOSOKO:
        case ACTOR_BG_SPOT01_OBJECTS2:
        case ACTOR_BG_SPOT08_BAKUDANKABE:
        case ACTOR_BG_SPOT11_BAKUDANKABE:
        case ACTOR_BG_SPOT15_SAKU:
        case ACTOR_BG_SPOT15_RRBOX:
        case ACTOR_BG_SPOT16_DOUGHNUT:
        case ACTOR_BG_SPOT16_BOMBSTONE:
        case ACTOR_BG_SPOT17_BAKUDANKABE:
        case ACTOR_BG_SPOT18_FUTA:
        case ACTOR_BG_SPOT18_SHUTTER:
        case ACTOR_BG_ICE_OBJECTS:
        case ACTOR_BG_ICE_SHUTTER:
        case ACTOR_BG_GND_NISEKABE:
        case ACTOR_BG_GND_ICEBLOCK:
        case ACTOR_BG_BOM_GUARD:
        case ACTOR_BG_JYA_LIFT:
        case ACTOR_BG_JYA_1FLIFT:
        case ACTOR_BG_JYA_GOROIWA:
        case ACTOR_BG_JYA_ZURERUKABE:
        case ACTOR_BG_JYA_COBRA:
        case ACTOR_BG_JYA_KANAAMI:
        case ACTOR_BG_JYA_MEGAMI:
        case ACTOR_BG_JYA_BOMBCHUIWA:
        case ACTOR_BG_JYA_AMISHUTTER:
        case ACTOR_BG_JYA_BOMBIWA:
        case ACTOR_BG_JYA_IRONOBJ:
        case ACTOR_BG_JYA_BLOCK:
        case ACTOR_BG_JYA_HAHENIRON:
        case ACTOR_BG_BOWL_WALL:
        case ACTOR_BG_SST_FLOOR:
        case ACTOR_OBJ_ELEVATOR:
        case ACTOR_OBJ_LIFT:
        case ACTOR_OBJ_OSHIHIKI:
        case ACTOR_OBJ_MAKEOSHIHIKI:
        case ACTOR_OBJ_WARP2BLOCK:
        case ACTOR_OBJ_HSBLOCK:
            return true;
        default:
            return false;
    }
}

bool IsLinkMovablePuzzleActor(const Actor* actor) {
    if (actor == nullptr) {
        return false;
    }

    switch (actor->id) {
        case ACTOR_OBJ_OSHIHIKI:
        case ACTOR_OBJ_MAKEOSHIHIKI:
        case ACTOR_OBJ_WARP2BLOCK:
        case ACTOR_OBJ_HSBLOCK:
        case ACTOR_BG_PUSHBOX:
            return true;
        default:
            return false;
    }
}

bool TryGetLinkPuzzleSwitchFlag(const Actor* actor, s16* outSwitchFlag) {
    if (actor == nullptr || outSwitchFlag == nullptr) {
        return false;
    }

    // Obj_Oshihiki encodes the puzzle switch in bits 8..13 (0..63).
    // Reuse the same decoding for makeoshihiki-driven puzzle setups.
    if (actor->id != ACTOR_OBJ_OSHIHIKI && actor->id != ACTOR_OBJ_MAKEOSHIHIKI) {
        return false;
    }

    const s16 switchFlag = (actor->params >> 8) & 0x3F;
    if (switchFlag < 0 || switchFlag > 0x3F) {
        return false;
    }

    *outSwitchFlag = switchFlag;
    return true;
}

// Rolling boulder actors (EN_BW = Death Mountain path, EN_GOROIWA = Dodongos Cavern)
// use spawn-only sync and run their scripted route locally on every client.
// This helper is used throughout RegisterHooks to exclude them from all
// continuous sync loops (position, HP, aggro, BG-keyframe).
static bool IsRollingBoulderActor(s16 actorId) {
    return actorId == ACTOR_EN_BW || actorId == ACTOR_EN_GOROIWA;
}

void func_8086ED70(BgBombwall* bgBombwall, PlayState* play);
void BgBreakwall_Wait(BgBreakwall* bgBreakwall, PlayState* play);
void BgHakaZou_WaitForHit(BgHakaZou* bgHakaZou, PlayState* play);
void func_808887C4(BgHidanHamstep* bgHidanHamstep, PlayState* play);
void func_808896B8(BgHidanHrock* bgHidanHrock, PlayState* play);
void BgIceShelter_Idle(BgIceShelter* bgIceShelter, PlayState* play);
void BgIceShelter_SetupMelt(BgIceShelter* bgIceShelter);
void ObjBombiwa_Break(ObjBombiwa* objBombiwa, PlayState* play);
void ObjHamishi_Break(ObjHamishi* objHamishi, PlayState* play);
void BgJyaBombchuiwa_WaitForExplosion(BgJyaBombchuiwa* bgJyaBombchuiwa, PlayState* play);
void BgMizuBwall_Idle(BgMizuBwall* bgMizuBwall, PlayState* play);
void func_808B6BC0(BgSpot17Bakudankabe* bgSpot17Bakudankabe, PlayState* play);
void func_808BF078(BgYdanMaruta* bgYdanMaruta, PlayState* play);
void BgYdanSp_FloorWebIdle(BgYdanSp* bgYdanSp, PlayState* play);
void BgYdanSp_WallWebIdle(BgYdanSp* bgYdanSp, PlayState* play);
void BgYdanSp_BurnWeb(BgYdanSp* bgYdanSp, PlayState* play);
void EnDoor_Idle(EnDoor* enDoor, PlayState* play);
float OTRGetDimensionFromLeftEdge(float v);
float OTRGetDimensionFromRightEdge(float v);
}

// Orphaned-cutscene watchdog state (see Cutscene Sync region).
static u32 csOrphanFrames = 0;
static u32 csFrozenFrames = 0;

void Anchor::RegisterHooks() {

    // #region Hooks that are required for basic Anchor functionality

    COND_HOOK(OnSceneSpawnActors, isConnected, [&]() {
        SendPacket_UpdateClientState();

        if (IsSaveLoaded()) {
            SendPacket_RoomJoin(); // Announce room presence; host assigns room master.
            RefreshClientActors();
        }
    });

    COND_HOOK(OnPresentFileSelect, isConnected, [&]() { SendPacket_UpdateClientState(); });

    COND_ID_HOOK(ShouldActorInit, ACTOR_PLAYER, isConnected, [&](void* actorRef, bool* should) {
        Actor* actor = (Actor*)actorRef;

        if (spawningDummyPlayerForClientId != 0) {
            SetDummyPlayerClientId(actor, spawningDummyPlayerForClientId);

            // By the time we get here, the actor was already added to the ACTORCAT_PLAYER list, so we need to move it
            Actor_ChangeCategory(gPlayState, &gPlayState->actorCtx, actor, ACTORCAT_NPC);
            actor->id = ACTOR_EN_OE2;
            actor->category = ACTORCAT_NPC;
            actor->init = DummyPlayer_Init;
            actor->update = DummyPlayer_Update;
            actor->draw = DummyPlayer_Draw;
            actor->destroy = DummyPlayer_Destroy;
        }
    });

    COND_HOOK(OnPlayerUpdate, isConnected, [&]() {
        static bool lastStateValid = false;
        static s16 lastSceneNum = SCENE_ID_MAX;
        static s8 lastRoomNum = -1;
        static s32 lastEntranceIndex = 0;
        static u16 lastTimeIncrement = 0;
        static u32 lastStateSyncFrame = 0;

        if (justLoadedSave) {
            justLoadedSave = false;
            SendPacket_RequestTeamState();
        }

        if (shouldRefreshActors) {
            shouldRefreshActors = false;
            RefreshClientActors();
        }

        if (IsSaveLoaded()) {
            s16 sceneNum = gPlayState->sceneNum;
            s8 roomNum = gPlayState->roomCtx.curRoom.num;
            s32 entranceIndex = gSaveContext.entranceIndex;
            u16 timeIncrement = (u16)gTimeIncrement;
            u32 currentFrame = gPlayState->state.frames;
            bool stateChanged = !lastStateValid || lastSceneNum != sceneNum || lastRoomNum != roomNum ||
                                lastEntranceIndex != entranceIndex || lastTimeIncrement != timeIncrement;
            bool heartbeatDue = !lastStateValid || (currentFrame - lastStateSyncFrame) >= 40;

            if (stateChanged || heartbeatDue) {
                SendPacket_UpdateClientState();
                lastStateValid = true;
                lastSceneNum = sceneNum;
                lastRoomNum = roomNum;
                lastEntranceIndex = entranceIndex;
                lastTimeIncrement = timeIncrement;
                lastStateSyncFrame = currentFrame;
            }
        } else {
            lastStateValid = false;
        }

        SendPacket_PlayerUpdate();
    });

    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        ProcessIncomingPacketQueue();
        ProcessPendingBoulderSpawns();
    });

    // Periodic PING broadcast for RTT measurement and host-election grace-period timer.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        const auto now = Clock::now();
        // Send PING every PING_INTERVAL (5 s) to all peers.
        if (lastPingSentAt == Clock::time_point{} ||
            (now - lastPingSentAt) >= PING_INTERVAL) {
            SendPacket_Ping();
        }
        // If the grace-period countdown is active, check for expiry.
        if (hostElectionArmed) {
            ElectNewHostIfNeeded();
        }
    });

    COND_HOOK(OnPlayerSfx, isConnected, [&](u16 sfxId) { SendPacket_PlayerSfx(sfxId); });
    COND_HOOK(OnOcarinaNote, isConnected,
              [&](uint8_t note, float modulator, int8_t bend) { SendPacket_OcarinaSfx(note, modulator, bend); });

    COND_HOOK(OnLoadGame, isConnected, [&](s16 fileNum) { justLoadedSave = true; });

    COND_HOOK(OnSaveFile, isConnected, [&](s16 fileNum, int sectionID) {
        if (sectionID == 0) {
            SendPacket_UpdateTeamState();
        }
    });

    COND_HOOK(OnFlagSet, isConnected,
              [&](s16 flagType, s16 flag) { SendPacket_SetFlag(SCENE_ID_MAX, flagType, flag); });

    COND_HOOK(OnFlagUnset, isConnected,
              [&](s16 flagType, s16 flag) { SendPacket_UnsetFlag(SCENE_ID_MAX, flagType, flag); });

    COND_HOOK(OnSceneFlagSet, isConnected,
              [&](s16 sceneNum, s16 flagType, s16 flag) { SendPacket_SetFlag(sceneNum, flagType, flag); });

    COND_HOOK(OnSceneFlagUnset, isConnected,
              [&](s16 sceneNum, s16 flagType, s16 flag) { SendPacket_UnsetFlag(sceneNum, flagType, flag); });

    COND_HOOK(OnRandoSetCheckStatus, isConnected, [&](RandomizerCheck rc, RandomizerCheckStatus status) {
        if (!isHandlingUpdateTeamState) {
            SendPacket_SetCheckStatus(rc);
        }
    });

    COND_HOOK(OnRandoSetIsSkipped, isConnected, [&](RandomizerCheck rc, bool isSkipped) {
        if (!isHandlingUpdateTeamState) {
            SendPacket_SetCheckStatus(rc);
        }
    });

    COND_HOOK(OnRandoEntranceDiscovered, isConnected,
              [&](u16 entranceIndex, u8 isReversedEntrance) { SendPacket_EntranceDiscovered(entranceIndex); });

    COND_ID_HOOK(OnBossDefeat, ACTOR_BOSS_GANON2, isConnected, [&](void* refActor) {
        SendPacket_GameComplete();
        // Battle Royale: erster Ganondorf-Sieg = Match-Sieg.
        // Der lokale Spieler sendet MATCH_END mit sich selbst als Sieger.
        if (roomState.battleRoyaleMode && brMatchActive) {
            brMatchActive = false;  // Doppel-Trigger verhindern (z.B. Ganon-Neustart)
            SPDLOG_INFO("[Anchor:BR] Ganondorf besiegt! Match-Sieg fuer Client {}", ownClientId);
            SendPacket_BattleRoyaleEvent("MATCH_END", { { "winnerClientId", ownClientId } });
        }
    });

    COND_HOOK(OnItemReceive, isConnected, [&](GetItemEntry itemEntry) {
        // Handle vanilla dungeon items a bit differently
        if (itemEntry.modIndex == MOD_NONE &&
            (itemEntry.itemId >= ITEM_KEY_BOSS && itemEntry.itemId <= ITEM_KEY_SMALL)) {
            SendPacket_UpdateDungeonItems();
            return;
        }

        SendPacket_GiveItem(itemEntry.tableId, itemEntry.getItemId);
    });

    COND_HOOK(OnDungeonKeyUsed, isConnected, [&](uint16_t mapIndex) {
        // Handle vanilla dungeon items a bit differently
        SendPacket_UpdateDungeonItems();
    });

    COND_VB_SHOULD(VB_APPLY_TUNIC_COLOR, isConnected, {
        Actor* myPlayer = (Actor*)GET_PLAYER(gPlayState);
        Actor* actor = va_arg(args, Actor*);
        Color_RGB8* color = va_arg(args, Color_RGB8*);

        if (actor == myPlayer) {
            Color_RGBA8 ownColor = CVarGetColor(CVAR_REMOTE_ANCHOR("Color.Value"), { 100, 255, 100 });
            color->r = ownColor.r;
            color->g = ownColor.g;
            color->b = ownColor.b;
            return;
        }

        uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

        if (!Anchor::Instance->clients.contains(clientId)) {
            return;
        }

        AnchorClient& client = Anchor::Instance->clients[clientId];
        color->r = client.color.r;
        color->g = client.color.g;
        color->b = client.color.b;
    });

    // #endregion

    // #region Hooks that are purely to sync actor states across the clients, not super essential

    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_ITEM00, isConnected, [&](void* refActor) {
        EnItem00* actor = static_cast<EnItem00*>(refActor);

        if (Flags_GetCollectible(gPlayState, actor->collectibleFlag)) {
            Actor_Kill(&actor->actor);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_BOMBWALL, isConnected, [&](void* refActor, bool* should) {
        BgBombwall* actor = static_cast<BgBombwall*>(refActor);

        if (actor->actionFunc == func_8086ED70 && Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_BREAKWALL, isConnected, [&](void* refActor, bool* should) {
        BgBreakwall* actor = static_cast<BgBreakwall*>(refActor);

        if (actor->actionFunc == BgBreakwall_Wait && Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HAKA_ZOU, isConnected, [&](void* refActor, bool* should) {
        BgHakaZou* actor = static_cast<BgHakaZou*>(refActor);

        if (actor->actionFunc == BgHakaZou_WaitForHit && Flags_GetSwitch(gPlayState, actor->switchFlag)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HIDAN_HAMSTEP, isConnected, [&](void* refActor, bool* should) {
        BgHidanHamstep* actor = static_cast<BgHidanHamstep*>(refActor);

        if (actor->actionFunc == func_808887C4 && Flags_GetSwitch(gPlayState, (actor->dyna.actor.params >> 8) & 0xFF)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HIDAN_HROCK, isConnected, [&](void* refActor, bool* should) {
        BgHidanHrock* actor = static_cast<BgHidanHrock*>(refActor);

        if (actor->actionFunc == func_808896B8 && Flags_GetSwitch(gPlayState, actor->unk_16A)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_ICE_SHELTER, isConnected, [&](void* refActor, bool* should) {
        BgIceShelter* actor = static_cast<BgIceShelter*>(refActor);

        if (actor->actionFunc == BgIceShelter_Idle && Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            BgIceShelter_SetupMelt(actor);
            Audio_PlayActorSound2(&actor->dyna.actor, NA_SE_EV_ICE_MELT);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_JYA_BOMBCHUIWA, isConnected, [&](void* refActor, bool* should) {
        BgJyaBombchuiwa* actor = static_cast<BgJyaBombchuiwa*>(refActor);

        if (actor->actionFunc == BgJyaBombchuiwa_WaitForExplosion &&
            Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_JYA_BOMBIWA, isConnected, [&](void* refActor, bool* should) {
        BgJyaBombiwa* actor = static_cast<BgJyaBombiwa*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_MIZU_BWALL, isConnected, [&](void* refActor, bool* should) {
        BgMizuBwall* actor = static_cast<BgMizuBwall*>(refActor);

        if (actor->actionFunc == BgMizuBwall_Idle &&
            Flags_GetSwitch(gPlayState, ((u16)actor->dyna.actor.params >> 8) & 0x3F)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT08_BAKUDANKABE, isConnected, [&](void* refActor, bool* should) {
        BgSpot08Bakudankabe* actor = static_cast<BgSpot08Bakudankabe*>(refActor);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params & 0x3F))) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT11_BAKUDANKABE, isConnected, [&](void* refActor, bool* should) {
        BgSpot11Bakudankabe* actor = static_cast<BgSpot11Bakudankabe*>(refActor);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params & 0x3F))) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT17_BAKUDANKABE, isConnected, [&](void* refActor, bool* should) {
        BgSpot17Bakudankabe* actor = static_cast<BgSpot17Bakudankabe*>(refActor);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params & 0x3F))) {
            func_808B6BC0(actor, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->dyna.actor.world.pos, 40, NA_SE_EV_WALL_BROKEN);
            Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
            Actor_Kill(&actor->dyna.actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_YDAN_MARUTA, isConnected, [&](void* refActor, bool* should) {
        BgYdanMaruta* actor = static_cast<BgYdanMaruta*>(refActor);

        if (actor->actionFunc == func_808BF078 && Flags_GetSwitch(gPlayState, actor->switchFlag)) {
            actor->collider.base.acFlags |= AC_HIT;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_YDAN_SP, isConnected, [&](void* refActor, bool* should) {
        BgYdanSp* actor = static_cast<BgYdanSp*>(refActor);

        if ((actor->actionFunc == BgYdanSp_FloorWebIdle || actor->actionFunc == BgYdanSp_WallWebIdle) &&
            Flags_GetSwitch(gPlayState, actor->isDestroyedSwitchFlag)) {
            BgYdanSp_BurnWeb(actor, gPlayState);
        }
    });

    // Detect: web transitions from idle → burning (any player, any client).
    // Broadcasts ROOM_EVENT("WEB_BURNED") so all other clients also burn their
    // local copy immediately — independent of syncItemsAndFlags flag-sync.
    COND_ID_HOOK(OnActorUpdate, ACTOR_BG_YDAN_SP, isConnected, ([&](void* refActor) {
        if (!IsSaveLoaded() || !gPlayState) return;
        BgYdanSp* actor = static_cast<BgYdanSp*>(refActor);
        const std::string key = GetActorKey((Actor*)actor, gPlayState->sceneNum);

        static std::unordered_map<std::string, bool> lastWebIdleState;
        const bool wasIdle = lastWebIdleState.count(key) ? lastWebIdleState[key] : true;
        const bool isIdle  = (actor->actionFunc == BgYdanSp_FloorWebIdle ||
                               actor->actionFunc == BgYdanSp_WallWebIdle);
        lastWebIdleState[key] = isIdle;

        if (wasIdle && !isIdle) {
            // Web just started burning locally — notify all clients.
            SendPacket_RoomEvent("WEB_BURNED", key, nlohmann::json{}, false);
        }
    }));

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_DOOR_SHUTTER, isConnected, [&](void* refActor, bool* should) {
        DoorShutter* actor = static_cast<DoorShutter*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->dyna.actor.params & 0x3F)) {
            DECR(actor->unlockTimer);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_DOOR, isConnected, [&](void* refActor, bool* should) {
        EnDoor* actor = static_cast<EnDoor*>(refActor);

        if (actor->actionFunc == EnDoor_Idle && Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            DECR(actor->lockTimer);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_SI, isConnected, [&](void* refActor, bool* should) {
        EnSi* actor = static_cast<EnSi*>(refActor);

        if (GET_GS_FLAGS((actor->actor.params & 0x1F00) >> 8) & (actor->actor.params & 0xFF)) {
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_SW, isConnected, [&](void* refActor, bool* should) {
        EnSw* actor = static_cast<EnSw*>(refActor);

        if (GET_GS_FLAGS((actor->actor.params & 0x1F00) >> 8) & (actor->actor.params & 0xFF)) {
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_ITEM_B_HEART, isConnected, [&](void* refActor, bool* should) {
        ItemBHeart* actor = static_cast<ItemBHeart*>(refActor);

        if (Flags_GetCollectible(gPlayState, 0x1F)) {
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_BOMBIWA, isConnected, [&](void* refActor, bool* should) {
        ObjBombiwa* actor = static_cast<ObjBombiwa*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            ObjBombiwa_Break(actor, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->actor.world.pos, 80, NA_SE_EV_WALL_BROKEN);
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_HAMISHI, isConnected, [&](void* refActor, bool* should) {
        ObjHamishi* actor = static_cast<ObjHamishi*>(refActor);

        if (Flags_GetSwitch(gPlayState, actor->actor.params & 0x3F)) {
            ObjHamishi_Break(actor, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &actor->actor.world.pos, 40, NA_SE_EV_WALL_BROKEN);
            Actor_Kill(&actor->actor);
            *should = false;
        }
    });

    // Position-corrected update (replaces the former full AI block, Phase 2).
    //
    // PROBLEM with blocking (ShouldActorUpdate = false):
    //   The actor's update() never runs, so Collider_UpdateCylinder / CollisionCheck_SetAC
    //   are never called → the enemy has no registered AC collider → the player's sword
    //   passes straight through.  focus.pos also stays at the last locally-computed
    //   position → Z-targeting arrow points to a completely wrong spot.
    //
    // SOLUTION — let the update run, but pin the position:
    //   OnBeforeActorUpdate: overwrite world.pos with the last received network position
    //     and zero velocity / speedXZ so the actor's own Move calls cannot drift it.
    //   The update then runs → Collider_UpdateCylinder called at the correct spot →
    //     sword hits register, Z-targeting reticle is accurate.
    //   OnActorUpdate: restore world.pos to the stored network value in case the AI
    //     overwrote it (e.g. pathfinding), and keep focus.pos in sync every frame.
    COND_HOOK(OnBeforeActorUpdate, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || IsEnemyAuthority()) return;
        if (!roomState.syncEnemies || !gPlayState) return;
        Actor* actor = static_cast<Actor*>(actorRef);
        if (actor->category != ACTORCAT_ENEMY && actor->category != ACTORCAT_BOSS) return;
        if (!ShouldActorBeNetworkDriven(actor)) return;
        const std::string key = GetActorKey(actor, gPlayState->sceneNum);
        auto it = remoteEnemyPos.find(key);
        if (it == remoteEnemyPos.end()) return; // no network data yet — let first frame run freely
        // Pin world.pos to the authoritative position before the update touches it.
        actor->world.pos = it->second;
        // Zero velocity so Actor_MoveXZGravity / Actor_MoveWithoutGravity can't drift the actor.
        actor->velocity.x = 0.0f;
        actor->velocity.y = 0.0f;
        actor->velocity.z = 0.0f;
        actor->speedXZ    = 0.0f;
    });

    // After the actor update ran: restore world.pos from the last network value (the AI
    // might have overwritten it via pathfinding or movement), and keep focus.pos aligned
    // so the Z-targeting reticle tracks the enemy's actual synced position every frame.
    COND_HOOK(OnActorUpdate, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || IsEnemyAuthority()) return;
        if (!roomState.syncEnemies || !gPlayState) return;
        Actor* actor = static_cast<Actor*>(actorRef);
        if (actor->category != ACTORCAT_ENEMY && actor->category != ACTORCAT_BOSS) return;
        if (!ShouldActorBeNetworkDriven(actor)) return;
        const std::string key = GetActorKey(actor, gPlayState->sceneNum);
        auto it = remoteEnemyPos.find(key);
        if (it == remoteEnemyPos.end()) return;
        // Derive the focus Y offset the actor computed during its own update.
        // We cannot use a named field (focusYoffset only exists on EnAObj, not Actor).
        // Since OnBeforeActorUpdate already pinned world.pos = remote_pos, the actor's
        // update() ran at the correct position and its focus.pos is already relative
        // to that position.  We just capture the current delta before restoring world.pos.
        const float focusOffsetY = actor->focus.pos.y - actor->world.pos.y;
        // Restore world.pos — the update may have moved the actor via its own AI.
        actor->world.pos = it->second;
        // Keep focus.pos (Z-targeting reticle) at the synced position.
        actor->focus.pos.x = it->second.x;
        actor->focus.pos.y = it->second.y + focusOffsetY;
        actor->focus.pos.z = it->second.z;
        // Zero velocity again so the next frame's pre-update pin finds the actor clean.
        actor->velocity.x = 0.0f;
        actor->velocity.y = 0.0f;
        actor->velocity.z = 0.0f;
        actor->speedXZ    = 0.0f;
    });

    // Central BG-actor freeze gate (Phase 2).
    // Suppresses the native Actor_Update for moving ACTORCAT_BG actors (platforms,
    // elevators, doors) once the first keyframe has been received.
    // ACTORCAT_PROP actors (pots, chests, pushblocks, the Master Sword pedestal)
    // are intentionally excluded: their update must run so AC colliders are
    // registered every frame and players can break/interact with them locally.
    // ACTOR_EN_HORSE (Epona) lives in ACTORCAT_BG but mounting logic must run on
    // every client, so we never freeze it regardless of keyframe data.
    // Flag-trigger BG actors (Breakwall, Bombiwa, …) never receive keyframes and
    // are therefore never frozen here — their existing explicit hooks handle them.
    COND_HOOK(ShouldActorUpdate, isConnected, [&](void* refActor, bool* should) {
        Actor* actor = static_cast<Actor*>(refActor);
        // Only freeze true moving-platform BG actors.
        if (actor->category != ACTORCAT_BG) return;
        // Some dungeon movers rely on their native Update() for local contact,
        // collision, or state progression. Freezing them breaks platform motion
        // on non-master clients, so keep those actors live.
        if (ShouldKeepDungeonBgActorUpdating(actor)) return;
        // Epona must never be frozen — mounting/dismounting must work on all clients.
        if (actor->id == ACTOR_EN_HORSE) return;
        if (!roomState.syncBGObjects || IsEnemyAuthority() || !gPlayState) return;
        const std::string key = GetActorKey(actor, gPlayState->sceneNum);
        if (bgActorKeyframeTarget.count(key)) {
            *should = false;
        }
    });

    COND_VB_SHOULD(VB_HAMMER_TOTEM_BREAK, isConnected, {
        BgHidanDalm* actor = va_arg(args, BgHidanDalm*);

        if (Flags_GetSwitch(gPlayState, actor->switchFlag)) {
            *should = true;
        }
    });

    COND_VB_SHOULD(VB_FIRE_TEMPLE_BOMBABLE_WALL_BREAK, isConnected, {
        BgHidanKowarerukabe* actor = va_arg(args, BgHidanKowarerukabe*);

        if (Flags_GetSwitch(gPlayState, (actor->dyna.actor.params >> 8) & 0x3F)) {
            *should = true;
        }
    });

    // #endregion

    // #region Hooks for visual effects that don't affect gameplay

    struct CompassIcon {
        Vec3f pos;
        Vec3s rot;
        float scale;
        Color_RGB8 color;
    };

    COND_HOOK(OnMinimapDrawCompassIcons, isConnected, [&]() {
        if (!CVarGetInteger(CVAR_REMOTE_ANCHOR("ShowOtherPlayersOnMinimap"), 1) ||
            Anchor::Instance->roomState.showLocationsMode == 0) {
            return;
        }

        std::vector<CompassIcon> compassIcons;

        bool isInDungeon = gPlayState->sceneNum == SCENE_DEKU_TREE || gPlayState->sceneNum == SCENE_DODONGOS_CAVERN ||
                           gPlayState->sceneNum == SCENE_JABU_JABU || gPlayState->sceneNum == SCENE_FOREST_TEMPLE ||
                           gPlayState->sceneNum == SCENE_FIRE_TEMPLE || gPlayState->sceneNum == SCENE_WATER_TEMPLE ||
                           gPlayState->sceneNum == SCENE_SPIRIT_TEMPLE || gPlayState->sceneNum == SCENE_SHADOW_TEMPLE ||
                           gPlayState->sceneNum == SCENE_BOTTOM_OF_THE_WELL || gPlayState->sceneNum == SCENE_ICE_CAVERN;
        std::string teamId = CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");

        // When transitioning to a new room via a door, curRoom.num updates immediately but the minimap still shows the
        // previous room while fading out
        s8 displayedRoomNum =
            gPlayState->roomCtx.prevRoom.num >= 0 ? gPlayState->roomCtx.prevRoom.num : gPlayState->roomCtx.curRoom.num;

        for (auto& [clientId, client] : Anchor::Instance->clients) {
            // Show compass icons for other players in the current scene. Also require them to be in the current room
            // within dungeons. If showLocationsMode isn't all players (2), only show compass icons for players of the
            // same team
            if (!client.self && client.online && client.player && client.sceneNum == gPlayState->sceneNum &&
                (!isInDungeon || client.curRoomNum == displayedRoomNum) &&
                (Anchor::Instance->roomState.showLocationsMode == 2 || client.teamId == teamId)) {
                compassIcons.push_back(
                    CompassIcon{ client.player->actor.world.pos, client.player->actor.shape.rot, 0.3f, client.color });
            }
        }

        // The local player's compass icon is always last so it gets drawn above the others
        Player* player = GET_PLAYER(gPlayState);
        compassIcons.push_back(CompassIcon{ player->actor.world.pos, player->actor.shape.rot, 0.4f,
                                            CVarGetColor24(CVAR_REMOTE_ANCHOR("Color.Value"), { 100, 255, 100 }) });

        // Adapted internals of Minimap_DrawCompassIcons()
        s16 leftMinimapMargin = CVarGetInteger(CVAR_COSMETIC("HUD.Margin.L"), 0);
        s16 rightMinimapMargin = CVarGetInteger(CVAR_COSMETIC("HUD.Margin.R"), 0);
        s16 bottomMinimapMargin = CVarGetInteger(CVAR_COSMETIC("HUD.Margin.B"), 0);

        s16 xMarginsMinimap;
        s16 yMarginsMinimap;
        if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.UseMargins"), 0) != 0) {
            if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ORIGINAL_LOCATION) {
                xMarginsMinimap = rightMinimapMargin;
            }
            yMarginsMinimap = bottomMinimapMargin;
        } else {
            xMarginsMinimap = 0;
            yMarginsMinimap = 0;
        }

        s16 mapWidth = isInDungeon ? R_DGN_MINIMAP_X : R_OW_MINIMAP_X;
        s16 mapStartPosX = isInDungeon ? 96 : gMapData->owMinimapWidth[R_MAP_INDEX];

        OPEN_DISPS(gPlayState->state.gfxCtx);
        Gfx_SetupDL_42Overlay(gPlayState->state.gfxCtx);

        for (auto& compassIcon : compassIcons) {
            gSPMatrix(OVERLAY_DISP++, &gMtxClear, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
            gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                              PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
            gDPSetEnvColor(OVERLAY_DISP++, 0, 0, 0, 255);
            gDPSetCombineMode(OVERLAY_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);

            // The compass offset value is a factor of 10 compared to N64 screen pixels and originates in the center of
            // the screen Compute the additional mirror offset value by normalizing the original offset position and
            // taking it's distance to the center of the map, duplicating that result and casting back to a factor of 10
            s16 mirrorOffset =
                ((mapWidth / 2) - ((R_COMPASS_OFFSET_X / 10) - (mapStartPosX - SCREEN_WIDTH / 2))) * 2 * 10;

            s16 tempX = (s16)compassIcon.pos.x;
            s16 tempZ = (s16)compassIcon.pos.z;
            tempX /= R_COMPASS_SCALE_X * (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? -1 : 1);
            tempZ /= R_COMPASS_SCALE_Y;

            s16 tempXOffset =
                R_COMPASS_OFFSET_X + (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? mirrorOffset : 0);
            if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) != ORIGINAL_LOCATION) {
                if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ANCHOR_LEFT) {
                    if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.UseMargins"), 0) != 0) {
                        xMarginsMinimap = leftMinimapMargin;
                    };
                    Matrix_Translate(
                        OTRGetDimensionFromLeftEdge((tempXOffset + (xMarginsMinimap * 10) + tempX +
                                                     (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosX"), 0) * 10)) /
                                                    10.0f),
                        (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ +
                         ((CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosY"), 0) * 10) * -1)) /
                            10.0f,
                        0.0f, MTXMODE_NEW);
                } else if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ANCHOR_RIGHT) {
                    if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.UseMargins"), 0) != 0) {
                        xMarginsMinimap = rightMinimapMargin;
                    };
                    Matrix_Translate(
                        OTRGetDimensionFromRightEdge((tempXOffset + (xMarginsMinimap * 10) + tempX +
                                                      (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosX"), 0) * 10)) /
                                                     10.0f),
                        (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ +
                         ((CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosY"), 0) * 10) * -1)) /
                            10.0f,
                        0.0f, MTXMODE_NEW);
                } else if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ANCHOR_NONE) {
                    Matrix_Translate(
                        (tempXOffset + tempX + (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosX"), 0) * 10) / 10.0f),
                        (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ +
                         ((CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosY"), 0) * 10) * -1)) /
                            10.0f,
                        0.0f, MTXMODE_NEW);
                }
            } else {
                Matrix_Translate(OTRGetDimensionFromRightEdge((tempXOffset + (xMarginsMinimap * 10) + tempX) / 10.0f),
                                 (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ) / 10.0f, 0.0f,
                                 MTXMODE_NEW);
            }
            Matrix_Scale(compassIcon.scale, compassIcon.scale, compassIcon.scale, MTXMODE_APPLY);
            Matrix_RotateX(-1.6f, MTXMODE_APPLY);
            s16 rotation = ((0x7FFF - compassIcon.rot.y) / 0x400) *
                           (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) ? -1 : 1);
            Matrix_RotateY(rotation / 10.0f, MTXMODE_APPLY);
            gSPMatrix(OVERLAY_DISP++, MATRIX_NEWMTX(gPlayState->state.gfxCtx),
                      G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

            gDPSetPrimColor(OVERLAY_DISP++, 0, 0xFF, compassIcon.color.r, compassIcon.color.g, compassIcon.color.b,
                            255);
            gSPDisplayList(OVERLAY_DISP++, (Gfx*)gCompassArrowDL);
        }

        CLOSE_DISPS(gPlayState->state.gfxCtx);
    });

    // #endregion

    // #region Enemy Authority - sync enemy health and deaths to all clients

    // Clear tracked health maps whenever the scene changes so stale keys don't accumulate.
    COND_HOOK(OnSceneInit, isConnected, [&](s16 sceneNum) {
        csOrphanFrames = 0;
        csFrozenFrames = 0;
        trackedEnemyHealth.clear();
        trackedNonAuthEnemyHealth.clear();
        pendingRemoteHealthOverride.clear();
        trackedEnemyPos.clear();
        remoteEnemyPos.clear();
        trackedEnemyDrawState.clear();
        savedEnemyDrawFuncs.clear();
        trackedBgActors.clear();
        bgActorKeyframeTarget.clear();
        gndLastCurPainting.clear();
        pendingBoulderSpawns.clear();
        lastBoulderTriggerFrameByKey.clear();
        hasMasterFrameSync = false;
        masterFrameToLocalOffset = 0;
        // Phase 6a: processed room events are scene-scoped — clear on every scene change
        // so the same event can fire fresh when the room is re-entered.
        processedRoomEvents.clear();
        puzzleSwitchStateByKey.clear();
        announcedPuzzleSwitchSolveByKey.clear();
        lastBossEventSeqByKey.clear();
        bossSnapshotStateByKey.clear();
        // Reset all boss adapter tracking maps so 'initialized=true' doesn't
        // persist across scene reloads (e.g., player dies and restarts boss fight).
        // Without this, CaptureTransition skips BOSS_STAGE_ENTER on the fresh actor.
        AnchorBossSync::ResetAllBossSyncAdapters();
        recentCollectibleSpawns.clear();
        // Enemies respawn on every room entry, so per-scene kill lists are stale
        // after a scene transition.  Clear to avoid phantom kills on next visit.
        pendingRoomKills.clear();
        // The Epona proxy actor belongs to the unloaded scene — reset state machine.
        eponaExchange = {};
        // All phantom horse actors belong to the unloaded scene.  Their Actor*
        // pointers are now dangling — clear the map so DummyPlayer_Update spawns
        // fresh phantoms when riders re-enter.
        clientPhantomHorse.clear();
        // Reset PvP attacker attribution: the previous scene's attacker is no
        // longer relevant after a scene transition.
        lastPvpAttackerClientId = 0;
    });

    // RoomMaster: maintain a baseline boss-state cache every frame so
    // late-join snapshots always contain at least one boss entry when a
    // boss fight is currently active.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!IsSaveLoaded() || !gPlayState) return;
        if (!roomState.syncEnemies) return;
        if (!IsRoomMaster()) return;

        const s16 sceneNum = gPlayState->sceneNum;
        const s8 curRoom = (s8)gPlayState->roomCtx.curRoom.num;
        const std::string roomPrefix = BuildRoomKey(sceneNum, curRoom) + "_";

        Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_BOSS].head;
        while (actor != nullptr) {
            if (actor->update != nullptr && (actor->room == curRoom || actor->room == -1)) {
                const std::string bossActorKey = GetActorKey(actor, sceneNum);
                const std::string roomBossKey = roomPrefix + bossActorKey;

                nlohmann::json& state = bossSnapshotStateByKey[roomBossKey];
                state["bossActorKey"] = bossActorKey;
                state["bossActorId"] = (int)actor->id;
                state["sceneNum"] = sceneNum;
                state["roomNum"] = curRoom;
                state["lastEventType"] = state.value("lastEventType", std::string("BOSS_STAGE_ENTER"));
                state["lastSeq"] = state.value("lastSeq", (uint32_t)gPlayState->state.frames);
                state["masterFrame"] = (uint32_t)gPlayState->state.frames;
                state["lateJoinCanSkipIntro"] = true;
                state["hp"] = (int)actor->colChkInfo.health;
            }
            actor = actor->next;
        }
    });

    // Non-authority: intercept damage BEFORE the actor processes it.
    // When enemy sync is ON: ALWAYS forward damage to the authority regardless
    // of syncRadius — syncRadius only gates position-broadcast throttling, NOT
    // damage authority.  Applying damage locally for out-of-radius enemies would
    // silently diverge HP between clients whenever both can reach the same enemy.
    COND_HOOK(OnBeforeActorUpdate, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || IsEnemyAuthority()) return;
        if (!roomState.syncEnemies) return; // sync off → vanilla behaviour
        // No authority in this scene/room: take damage locally (kills are queued
        // in pendingRoomKills), otherwise hits on local-only enemies are lost.
        if (!IsOwnerInSameRoom()) return;

        // Avoid dropping client hits when the owner is briefly unreachable.
        bool ownerReachable = false;
        for (auto& [id, client] : clients) {
            if (id != roomState.ownerClientId) continue;
            ownerReachable = client.online && client.isSaveLoaded;
            break;
        }
        if (!ownerReachable) return;

        Actor* actor = (Actor*)actorRef;
        if (actor->category != ACTORCAT_ENEMY && actor->category != ACTORCAT_BOSS) return;
        if (actor->colChkInfo.damage == 0) return; // no hit this frame
        // Rolling boulders are scripted hazards with fixed routes — they run
        // entirely locally after spawn and must not be damage-intercepted.
        if (IsRollingBoulderActor(actor->id)) return;

        // For bosses: forward the hit but keep local damage so the boss's own
        // hit-response logic (stagger, invuln window, multi-part transitions)
        // still plays correctly.  The authority will override HP via a
        // BOSS_WEAKPOINT_HIT event which corrects any local drift.
        // For regular enemies: zero local damage — HP is fully authority-driven.
        const bool isBoss = (actor->category == ACTORCAT_BOSS);
        u8 damage = actor->colChkInfo.damage;
        if (!isBoss) {
            actor->colChkInfo.damage = 0; // Prevent local HP reduction on enemies
        }
        std::string actorKey = GetActorKey(actor, gPlayState->sceneNum);
        SPDLOG_INFO("[Anchor:EnemySync] CLIENT: Hit intercepted | actorKey={} | damage={} | boss={} | health={}", 
                    actorKey, (int)damage, isBoss, (int)actor->colChkInfo.health);
        SendPacket_PlayerAttackActor(actor, damage);
        Actor_SetColorFilter(actor, 0x4000, 0xFF, 0, 8);
    });

    // Authority: aggro-fake — set each enemy's target to whichever player
    // (host-Link or a connected client's dummy) is closest before the actor
    // runs its own AI.  Only active when enemy sync is ON and the enemy is
    // within syncRadius of at least one client in the same room.
    COND_HOOK(OnBeforeActorUpdate, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || !IsEnemyAuthority()) return;
        if (!roomState.syncEnemies) return;

        Actor* actor = (Actor*)actorRef;
        if (actor->category != ACTORCAT_ENEMY && actor->category != ACTORCAT_BOSS) return;
        if (!gPlayState) return;
        // Rolling boulders have fixed scripted routes — no aggro target needed.
        if (IsRollingBoulderActor(actor->id)) return;

        // Skip aggro-fake for enemies outside the sync radius of all clients.
        if (roomState.syncRadius > 0) {
            f32 rSq = (f32)roomState.syncRadius * (f32)roomState.syncRadius;
            bool anyClientNearby = false;
            for (auto& [id, client] : clients) {
                if (client.self || !client.online || !client.player) continue;
                if (client.sceneNum != gPlayState->sceneNum) continue;
                if (client.curRoomNum != (s8)gPlayState->roomCtx.curRoom.num) continue;
                if (Math3D_Vec3fDistSq(&actor->world.pos, &client.player->actor.world.pos) <= rSq) {
                    anyClientNearby = true;
                    break;
                }
            }
            if (!anyClientNearby) return;
        }

        Player* hostLink = GET_PLAYER(gPlayState);
        Actor*  nearest  = (Actor*)hostLink;
        Vec3f   enemyPos = actor->world.pos;
        f32 nearestDistSq = Math3D_Vec3fDistSq(&enemyPos, &hostLink->actor.world.pos);

        for (auto& [id, client] : clients) {
            if (client.self || !client.online || !client.player) continue;
            if (client.sceneNum != gPlayState->sceneNum) continue;
            if (client.curRoomNum != (s8)gPlayState->roomCtx.curRoom.num) continue;

            Vec3f clientPos = client.player->actor.world.pos;
            f32 distSq = Math3D_Vec3fDistSq(&enemyPos, &clientPos);
            if (distSq < nearestDistSq) {
                nearestDistSq = distSq;
                nearest = (Actor*)client.player;
            }
        }

        // Override the pre-computed distance/angle fields so the enemy AI
        // believes the nearest player is the one we chose above.
        // These fields are written by the engine before actor->update() runs,
        // so overriding them in OnBeforeActorUpdate is safe and effective.
        if (nearest != (Actor*)hostLink) {
            Vec3f& nPos = nearest->world.pos;
            f32 dx = nPos.x - enemyPos.x;
            f32 dz = nPos.z - enemyPos.z;
            actor->xyzDistToPlayerSq = nearestDistSq;
            actor->xzDistToPlayer = sqrtf(dx * dx + dz * dz);
            actor->yDistToPlayer = nPos.y - enemyPos.y;
            actor->yawTowardsPlayer = (s16)(atan2f(dx, dz) * (32768.0f / (float)M_PI));
        }
    });

    // Authority: broadcast enemy HP/kills always (when sync is on) and
    // broadcast position only for enemies within syncRadius, throttled by
    // enemySyncTickRate.
    COND_HOOK(OnActorUpdate, isConnected, ([&](void* actorRef) {
        using Clock = std::chrono::steady_clock;
        static std::unordered_map<std::string, Clock::time_point> lastEnemyPosSyncAt;

        if (!IsSaveLoaded() || !IsEnemyAuthority()) return;
        if (!roomState.syncEnemies) return;
        if (!IsAnyClientInSameRoom()) return;

        Actor* actor = (Actor*)actorRef;
        if (actor->category != ACTORCAT_ENEMY && actor->category != ACTORCAT_BOSS) return;
        // Rolling boulders run locally on every client after spawn-sync — do not
        // broadcast their HP or position, which would fight local physics.
        if (IsRollingBoulderActor(actor->id)) return;
        // NOTE: Do NOT skip health==0 actors here. When a lethal hit sets health to 0,
        // the enemy runs its native death animation for several frames before Actor_Kill
        // is called. We must continue broadcasting position during that time so the
        // client sees the falling animation. OnActorKill sends ActorKilled at the end.

        std::string key = GetActorKey(actor, gPlayState->sceneNum);
        u8 currentHealth = actor->colChkInfo.health;

        // ── HP sync: always, regardless of radius or tick rate ──────────────
        bool healthChanged = false;
        auto hpIt = trackedEnemyHealth.find(key);
        if (hpIt != trackedEnemyHealth.end() && hpIt->second != currentHealth) {
            healthChanged = true;
        }
        trackedEnemyHealth[key] = currentHealth;
        if (healthChanged) {
            SendPacket_ActorStateUpdate(actor);
        }

        // ── Draw-state sync: fire immediately when visibility toggles ────────
        // Covers Deku Scrubs emerging/hiding, any enemy that sets draw = NULL.
        // Bosses must not use this network draw override, because a false hide
        // state would make them invisible and untargetable for all clients.
        const bool isBossActor = (actor->category == ACTORCAT_BOSS);
        if (!isBossActor) {
            bool currentDraw = (actor->draw != nullptr);
            {
                auto drawIt = trackedEnemyDrawState.find(key);
                bool drawChanged = (drawIt != trackedEnemyDrawState.end())
                                   && (drawIt->second != currentDraw);
                trackedEnemyDrawState[key] = currentDraw;
                if (drawChanged) {
                    // Reuse the position packet — it now carries drawEnabled too.
                    SendPacket_EnemyPositionUpdate(actor);
                    trackedEnemyPos[key] = actor->world.pos; // suppress duplicate pos update
                }
            }
        }

        // ── Position sync: room-gated + radius-gated + tick-rate throttled ──
        // Health sync above must always run, even if room-state is briefly stale.
        if (!IsAnyClientInSameRoom()) return;

        // ── Position sync: radius-gated + tick-rate throttled ───────────────
        // Check if any client is within syncRadius of this enemy.
        bool inRadius = true;
        if (roomState.syncRadius > 0) {
            f32 rSq = (f32)roomState.syncRadius * (f32)roomState.syncRadius;
            inRadius = false;
            for (auto& [id, client] : clients) {
                if (client.self || !client.online || !client.player) continue;
                if (client.sceneNum != gPlayState->sceneNum) continue;
                if (client.curRoomNum != (s8)gPlayState->roomCtx.curRoom.num) continue;
                if (Math3D_Vec3fDistSq(&actor->world.pos, &client.player->actor.world.pos) <= rSq) {
                    inRadius = true;
                    break;
                }
            }
        }

        if (!inRadius) return; // outside radius: client runs local AI, no pos update

        // Time-based throttle: 0=5Hz(200ms), 1=10Hz(100ms), 2=20Hz(50ms)
        u8 tickIdx = roomState.enemySyncTickRate < 3 ? roomState.enemySyncTickRate : 2;
        auto interval = std::chrono::milliseconds((tickIdx == 0) ? 200 : (tickIdx == 1) ? 100 : 50);
        auto now = Clock::now();
        auto syncIt = lastEnemyPosSyncAt.find(key);
        if (syncIt != lastEnemyPosSyncAt.end() && (now - syncIt->second) < interval) return;

        // Position-change threshold (still apply even with tick rate to avoid
        // flooding identical data on frames that do fire).
        bool posChanged = false;
        auto posIt = trackedEnemyPos.find(key);
        if (posIt == trackedEnemyPos.end()) {
            posChanged = true;
        } else {
            f32 distSq = Math3D_Vec3fDistSq(&actor->world.pos, &posIt->second);
            posChanged = (distSq > 4.0f);
        }

        if (posChanged) {
            SendPacket_EnemyPositionUpdate(actor);
            trackedEnemyPos[key] = actor->world.pos;
            lastEnemyPosSyncAt[key] = now;
        }
    }));

    // RoomMaster: capture boss stage/weakpoint transitions via adapter layer
    // and publish canonical BOSS_* events over the room event channel.
    COND_HOOK(OnActorUpdate, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || !gPlayState) return;
        if (!roomState.syncEnemies) return;
        if (!IsRoomMaster()) return;

        Actor* actor = static_cast<Actor*>(actorRef);
        const bool isBossActor = actor->category == ACTORCAT_BOSS;
        const bool isSupportedMiniBoss =
            (actor->id == ACTOR_EN_BIGOKUTA && gPlayState->sceneNum == SCENE_JABU_JABU);
        if (!isBossActor && !isSupportedMiniBoss) return;
        if (actor->room != -1 && actor->room != gPlayState->roomCtx.curRoom.num) return;

        nlohmann::json event = AnchorBossSync::CaptureBossTransition(gPlayState, actor);
        if (!event.is_object() || event.empty()) return;

        const std::string eventType = event.value("eventType", std::string(""));
        if (eventType.empty()) return;

        const std::string eventKey = event.value("eventKey", GetActorKey(actor, gPlayState->sceneNum));
        SendPacket_RoomEvent(eventType, eventKey, event, false);
    });

    // Phantom Ganon painting-select sync (Forest Temple Boss).
    // ACTOR_EN_FHG (the horse) lives in ACTORCAT_BG and is never frozen, so every
    // client's BossGanondrof_Neutral -> EnfHG_Retreat independently rolls its own
    // Rand_ZeroOne() for "which painting is real" / "where the decoy hides".
    // Without forcing this, each client sees the boss emerge from a different
    // painting with a differently-placed (or absent-looking) decoy, which both
    // breaks the guessing minigame and makes it trivially easy for non-master
    // clients (whichever painting glows locally IS the real one for them).
    COND_HOOK(OnActorUpdate, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || !gPlayState || !IsRoomMaster()) return;
        if (gPlayState->sceneNum != SCENE_FOREST_TEMPLE_BOSS) return;

        Actor* actor = static_cast<Actor*>(actorRef);
        constexpr s16 kGndRealBossParam = 1;         // GND_REAL_BOSS
        constexpr s16 kGndFakeBossBase = 10;          // GND_FAKE_BOSS
        constexpr size_t kGndCurPaintingOffset = 0x01C2; // EnfHG::curPainting

        if (actor->id != ACTOR_EN_FHG || actor->params != kGndRealBossParam) return;

        const int16_t curPainting = *(const int16_t*)((const char*)actor + kGndCurPaintingOffset);
        if (curPainting < 0 || curPainting > 5) return; // not initialized yet

        const std::string key = GetActorKey(actor, gPlayState->sceneNum);
        auto it = gndLastCurPainting.find(key);
        const bool isFirstObservation = (it == gndLastCurPainting.end());
        if (!isFirstObservation && it->second == curPainting) return;
        gndLastCurPainting[key] = curPainting;
        if (isFirstObservation) return; // seed only — first real cycle already ran before we could sync it

        // actor->child is a single pointer (last actor spawned via
        // Actor_SpawnAsChild), not a sibling list — the fresh decoy rider
        // spawned earlier this same frame inside EnfHG_Retreat.
        int16_t fakeIdx = -1;
        Actor* child = actor->child;
        if (child != nullptr && child->id == ACTOR_BOSS_GANONDROF && child->params >= kGndFakeBossBase) {
            fakeIdx = (int16_t)(child->params - kGndFakeBossBase);
        }
        if (fakeIdx < 0) return;

        nlohmann::json event;
        event["realIdx"] = (int)curPainting;
        event["fakeIdx"] = (int)fakeIdx;
        SendPacket_RoomEvent("GND_PAINTING_SELECT", key, event, false);
    });

    // Non-authority: freeze enemy AI when the authority is in the same scene+room.
    // Non-authority: keep tracking remote HP overrides so we don't echo them back.
    COND_HOOK(OnActorUpdate, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || IsEnemyAuthority()) return;
        if (!roomState.syncEnemies) return;

        Actor* actor = (Actor*)actorRef;
        if (actor->category != ACTORCAT_ENEMY && actor->category != ACTORCAT_BOSS) return;

        std::string key = GetActorKey(actor, gPlayState->sceneNum);
        u8 currentHealth = actor->colChkInfo.health;

        // Rolling boulders run entirely locally on non-master clients after spawn.
        // Do not pin their position (EN_BW) or enforce HP — this would fight the
        // local scripted physics and cause the glitch/spin-in-place behaviour.
        if (IsRollingBoulderActor(actor->id)) return;

        // Clear a confirmed remote override so it doesn't linger.
        auto remoteIt = pendingRemoteHealthOverride.find(key);
        if (remoteIt != pendingRemoteHealthOverride.end()) {
            // HP enforcement: if a remote override is pending (authority sent us a
            // lower HP), clamp local HP to that value.  This prevents non-authority
            // enemies from regenerating HP between network packets.
            if (currentHealth > remoteIt->second) {
                actor->colChkInfo.health = remoteIt->second;
            } else if (currentHealth == remoteIt->second) {
                pendingRemoteHealthOverride.erase(remoteIt);
            }
        }

        trackedNonAuthEnemyHealth[key] = actor->colChkInfo.health;
    });

    // Authority: broadcast enemy deaths so all clients can kill their local copy.
    // Also track any collectibles that spawned in the same frame via recentCollectibleSpawns.
    COND_ID_HOOK(OnActorSpawn, ACTOR_EN_ITEM00, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || !IsEnemyAuthority()) return;
        if (!roomState.syncEnemies || !IsAnyClientInSameRoom()) return;
        if (isSpawningRemoteCollectible) return;
        Actor* item = (Actor*)actorRef;
        recentCollectibleSpawns.push_back({item->params, item->world.pos});
    });

    // Both sides: when a rolling boulder spawns (Death Mountain path etc.),
    // relay a spawn event so peers can spawn the same obstacle deterministically.
    COND_ID_HOOK(OnActorSpawn, ACTOR_EN_BW, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || !roomState.syncEnemies || !gPlayState) return;
        if (!IsRoomMaster()) return;
        if (isSpawningRemoteBoulder) return;
        Actor* actor = (Actor*)actorRef;
        if (actor->room != -1 && actor->room != gPlayState->roomCtx.curRoom.num) return;
        SendPacket_BoulderSpawn(actor);
    });

    COND_ID_HOOK(OnActorSpawn, ACTOR_EN_GOROIWA, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || !roomState.syncEnemies || !gPlayState) return;
        if (!IsRoomMaster()) return;
        if (isSpawningRemoteBoulder) return;
        Actor* actor = (Actor*)actorRef;
        if (actor->room != -1 && actor->room != gPlayState->roomCtx.curRoom.num) return;
        SendPacket_BoulderSpawn(actor);
    });

    // Jabu-Jabu special spawn sync: Big Octo / electrified tentacles can be
    // spawned by local platform scripts or cutscene state. Ensure every client
    // receives a deterministic spawn event from the room master.
    COND_HOOK(OnActorSpawn, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || !gPlayState) return;
        if (!IsRoomMaster()) return;
        if (!roomState.syncEnemies) return;
        if (!IsAnyClientInSameRoom()) return;
        if (gPlayState->sceneNum != SCENE_JABU_JABU) return;

        Actor* actor = static_cast<Actor*>(actorRef);
        if (actor->id != ACTOR_EN_BIGOKUTA && actor->id != ACTOR_EN_BX) return;

        nlohmann::json data;
        data["actorId"] = (int)actor->id;
        data["params"] = (int)actor->params;
        data["x"] = actor->world.pos.x;
        data["y"] = actor->world.pos.y;
        data["z"] = actor->world.pos.z;
        data["rotX"] = (int)actor->world.rot.x;
        data["rotY"] = (int)actor->world.rot.y;
        data["rotZ"] = (int)actor->world.rot.z;

        SendPacket_RoomEvent("JABU_ACTOR_SPAWN", GetActorKey(actor, gPlayState->sceneNum), data, false);
    });

    // Authority: broadcast enemy deaths so all clients can kill their local copy.
    COND_HOOK(OnActorKill, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || !IsEnemyAuthority()) return;
        if (!roomState.syncEnemies) return;

        Actor* actor = (Actor*)actorRef;
        std::string key = GetActorKey(actor, gPlayState->sceneNum);
        // Allow through actors whose CURRENT category is no longer ENEMY/BOSS if
        // we previously tracked them as one (trackedEnemyHealth keyed by the same
        // category-free key). En_Test/Stalfos flips to ACTORCAT_PROP while
        // "downed" and may call Actor_Kill() for its permanent death in that
        // category — without this, the kill would never be broadcast at all
        // (the BG/PROP kill hook only fires when syncBGObjects is separately on).
        bool wasTrackedEnemy = trackedEnemyHealth.count(key) > 0;
        if (actor->category != ACTORCAT_ENEMY && actor->category != ACTORCAT_BOSS && !wasTrackedEnemy) return;

        SPDLOG_INFO("[Anchor:EnemySync] HOST: Actor killed | actorKey={} | pos=({:.1f},{:.1f},{:.1f})", 
                    key, actor->world.pos.x, actor->world.pos.y, actor->world.pos.z);
        trackedEnemyHealth.erase(key);
        trackedEnemyPos.erase(key);
        SendPacket_ActorKilled(actor);

        // Relay any collectibles that dropped from this enemy this frame so the
        // client can spawn matching EN_ITEM00 actors and compete for the loot.
        if (IsAnyClientInSameRoom() && !recentCollectibleSpawns.empty()) {
            constexpr f32 kDropRadiusSq = 300.0f * 300.0f;
            for (auto& drop : recentCollectibleSpawns) {
                if (Math3D_Vec3fDistSq(&actor->world.pos, &drop.pos) <= kDropRadiusSq) {
                    SendPacket_EnemyDropItem(drop.params, drop.pos.x, drop.pos.y, drop.pos.z);
                }
            }
        }

        // If no client is in our room, record this kill so it can be
        // re-applied via ROOM_KILL_SYNC when a client enters later.
        if (!IsAnyClientInSameRoom() && gPlayState) {
            std::string roomKey = std::to_string(gPlayState->sceneNum) + "_" +
                                  std::to_string((s8)gPlayState->roomCtx.curRoom.num);
            pendingRoomKills[roomKey].insert(key);
        }
    });

    // Non-authority: track all locally-killed enemies so the authority never
    // sees them alive again — regardless of whether it was in the room or not.
    // If the authority is currently in the same room (e.g. the enemy was outside
    // the sync radius), send ROOM_KILL_SYNC immediately.  Otherwise the kill is
    // queued and flushed when the authority next enters this room.
    COND_HOOK(OnActorKill, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || IsEnemyAuthority()) return;
        if (!roomState.syncEnemies) return;
        if (!gPlayState) return;

        Actor* actor = (Actor*)actorRef;
        // Bosses: their death is authoritative on the room master.  The room
        // master sends ACTOR_KILLED when the boss dies; clients kill their local
        // copy from that packet.  Sending ROOM_KILL_SYNC from the client for a
        // boss creates a "already dead" flood loop on the authority — skip it.
        if (actor->category != ACTORCAT_ENEMY) return;

        std::string actorKey = GetActorKey(actor, gPlayState->sceneNum);
        std::string roomKey = std::to_string(gPlayState->sceneNum) + "_" +
                              std::to_string((s8)gPlayState->roomCtx.curRoom.num);
        pendingRoomKills[roomKey].insert(actorKey);
        SPDLOG_INFO("[Anchor:EnemySync] CLIENT: Local kill queued | actorKey={} | ownerPresent={} | pos=({:.1f},{:.1f},{:.1f})", 
                    actorKey, IsOwnerInSameRoom() ? "yes" : "no", actor->world.pos.x, actor->world.pos.y, actor->world.pos.z);

        // Authority already present → flush immediately so it kills the actor now.
        if (IsOwnerInSameRoom()) {
            SPDLOG_INFO("[Anchor:EnemySync] CLIENT: ROOM_KILL_SYNC sent immediately (owner in room)");
            SendPacket_RoomKillSync();
        }
    });

    // Authority: broadcast BG/PROP actor kills (destroyable objects — bombable
    // walls, heavy blocks, collapsing platforms, etc.) so all clients remove the
    // same actor from their local scene.
    //
    // This extends the ACTOR_KILLED packet (originally enemy-only) to also cover
    // ACTORCAT_BG and ACTORCAT_PROP.  No ROOM_KILL_SYNC needed for BG objects
    // because permanent destruction is already covered by scene-flag sync
    // (Bg_Breakwall sets a flag when destroyed); this packet handles live, in-room
    // destruction during shared gameplay.
    COND_HOOK(OnActorKill, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || !IsEnemyAuthority()) return;
        if (!roomState.syncBGObjects) return;
        if (!gPlayState) return;

        Actor* actor = (Actor*)actorRef;
        if (actor->category != ACTORCAT_BG && actor->category != ACTORCAT_PROP) return;
        if (!IsAnyClientInSameRoom()) return;

        std::string key = GetActorKey(actor, gPlayState->sceneNum);
        SPDLOG_INFO("[Anchor:BgSync] HOST: BG actor killed | actorKey={} | actorId={} | pos=({:.1f},{:.1f},{:.1f})",
                    key, (int)actor->id, actor->world.pos.x, actor->world.pos.y, actor->world.pos.z);

        // Clean up authority-side tracking so the dead actor is never re-sent.
        trackedBgActors.erase(key);
        bgActorKeyframeTarget.erase(key); // harmless on authority; defensive cleanup

        SendPacket_ActorKilled(actor);
    });

    // Both sides: when a dropped collectible is picked up, remove the matching
    // item on the other side so only the faster player gets it.
    COND_ID_HOOK(OnActorKill, ACTOR_EN_ITEM00, isConnected, [&](void* actorRef) {
        if (!IsSaveLoaded() || !gPlayState) return;
        if (isRemovingRemoteItem) return; // this kill was triggered by HandlePacket_ItemPickup
        Actor* actor = (Actor*)actorRef;
        if (actor->category != ACTORCAT_MISC) return;
        SendPacket_ItemPickup(actor);
    });

    // Both sides: detect when the other player enters our room and immediately
    // flush the pending kill list via ROOM_KILL_SYNC so they see the same
    // enemy state we have.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        using Clock = std::chrono::steady_clock;
        static bool lastOwnerInSameRoom    = false;
        static bool lastClientInSameRoom   = false;
        static Clock::time_point lastRoomKillSyncAt = Clock::time_point::min();

        if (!IsSaveLoaded()) {
            lastOwnerInSameRoom  = false;
            lastClientInSameRoom = false;
            lastRoomKillSyncAt = Clock::time_point::min();
            return;
        }

        bool ownerNow  = !IsEnemyAuthority() && IsOwnerInSameRoom();
        bool clientNow =  IsEnemyAuthority() && IsAnyClientInSameRoom();

        // Transition false → true: the other player just entered the room
        if (!IsEnemyAuthority() && !lastOwnerInSameRoom && ownerNow)
            SendPacket_RoomKillSync();
        if (IsEnemyAuthority() && !lastClientInSameRoom && clientNow)
            SendPacket_RoomKillSync();

        // Reliability resend while peers share a room and we still have pending kills.
        // This avoids missed edge-trigger packets causing persistent desync.
        if (gPlayState) {
            std::string roomKey = std::to_string(gPlayState->sceneNum) + "_" +
                                  std::to_string((s8)gPlayState->roomCtx.curRoom.num);
            auto pendingIt = pendingRoomKills.find(roomKey);
            bool hasPending = pendingIt != pendingRoomKills.end() && !pendingIt->second.empty();
            bool roomShared = IsEnemyAuthority() ? clientNow : ownerNow;
            auto now = Clock::now();
            constexpr auto kRoomKillResendInterval = std::chrono::milliseconds(350);

            if (hasPending && roomShared && (lastRoomKillSyncAt == Clock::time_point::min() || (now - lastRoomKillSyncAt) >= kRoomKillResendInterval)) {
                SendPacket_RoomKillSync();
                lastRoomKillSyncAt = now;
            }
        }

        lastOwnerInSameRoom  = ownerNow;
        lastClientInSameRoom = clientNow;
    });

    // #region Time sync
    // HOST: freeze dayTime when either player is in a timeless scene and broadcast to clients.
    // CLIENT: accept dayTime from host and suppress local advancement when frozen.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!IsSaveLoaded() || !roomState.syncDayTime) return;

        // A timeless scene has gTimeIncrement == 0 (set by the engine for all
        // dungeons, indoor areas, villages, etc.).
        bool localTimeless  = (gTimeIncrement == 0);
        bool remoteTimeless = false;
        for (auto& [clientId, client] : clients) {
            if (!client.self && client.online && client.isSaveLoaded && client.timeIncrement == 0) {
                remoteTimeless = true;
                break;
            }
        }
        bool shouldFreeze = localTimeless || remoteTimeless;

        if (IsEnemyAuthority()) {
            // HOST -------------------------------------------------------
            // Environment_Update() already ran this frame (OnGameFrameUpdate
            // fires after Play_Update).  If time should be frozen, restore
            // dayTime to what it was at the END of the previous frame.
            static u16 lastDayTimeHost = 0;
            if (shouldFreeze) {
                gSaveContext.dayTime = lastDayTimeHost;
            } else {
                lastDayTimeHost = gSaveContext.dayTime;
            }

            // Broadcast TIME_SYNC whenever the freeze state changes (dungeon ↔
            // overworld transition) and as a periodic safety-net every ~3 seconds
            // (60 frames). This was previously gated at 18000 frames (~15 min at
            // 20fps), which let host/client dayTime drift by several minutes
            // between syncs — 60 frames matches the packet's own documented
            // cadence (see TimeSync.cpp) and keeps drift imperceptible.
            // Per-frame broadcast is intentionally avoided (causes drawbridge jitter).
            static bool  lastShouldFreeze   = false;
            static u32   lastTimeSyncFrame  = 0;
            const  u32   curFrame          = (u32)gPlayState->state.frames;
            static u8    lastNightHost      = 0xFF;
            const  bool  nightChanged       = (lastNightHost != (u8)gSaveContext.nightFlag);
            if ((shouldFreeze != lastShouldFreeze || nightChanged || (curFrame - lastTimeSyncFrame) >= 60u)
                && IsAnyClientInSameRoom()) {
                SendPacket_TimeSync();
                lastTimeSyncFrame = curFrame;
            }
            lastShouldFreeze = shouldFreeze;
            lastNightHost = (u8)gSaveContext.nightFlag;

        } else {
            // CLIENT -----------------------------------------------------
            // The host's TIME_SYNC packet sets remoteTimeFrozen and overwrites
            // dayTime.  Between syncs we also suppress local advancement so
            // there is no drift while the host has time frozen.
            static u16 lastDayTimeClient = 0;
            if (remoteTimeFrozen) {
                gSaveContext.dayTime = lastDayTimeClient;
            } else {
                lastDayTimeClient = gSaveContext.dayTime;
            }
            // nightFlag is re-derived locally from dayTime every frame; force the
            // host's value (while syncs are fresh) so both clients flip together.
            if (!shouldFreeze && hasRemoteNightFlag && gTimeIncrement != 0 &&
                (std::chrono::steady_clock::now() - lastTimeSyncRecvAt) < std::chrono::seconds(6)) {
                gSaveContext.nightFlag = remoteNightFlag;
            }
        }
    });  // end time-sync OnGameFrameUpdate
    // #endregion

    // #region Physical Item Exchange
    // Check proximity every frame: when a remote player is within
    // PHYSICAL_EXCHANGE_DIST_SQ of the local player, pop one item from
    // the queue and also flush all buffered flags.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!roomState.physicalItemExchange) return;
        if (!IsSaveLoaded() || !gPlayState) return;
        if (physicalItemQueue.empty() && physicalFlagQueue.empty()) return;

        Player* player = GET_PLAYER(gPlayState);
        bool anyPlayerClose = false;
        for (auto& [clientId, client] : clients) {
            if (client.self || !client.online || !client.player) continue;
            f32 distSq = Math3D_Vec3fDistSq(&player->actor.world.pos,
                                             &client.player->actor.world.pos);
            if (distSq <= PHYSICAL_EXCHANGE_DIST_SQ) {
                anyPlayerClose = true;
                break;
            }
        }
        if (!anyPlayerClose) return;

        if (!physicalItemQueue.empty()) {
            GiveNextPhysicalExchangeItem();
        }
        // Flags are applied all at once as soon as a player is close.
        if (!physicalFlagQueue.empty()) {
            FlushPhysicalFlagQueue();
        }
    });

    // Epona Mode B: advance proxy actor through the arc and hand over item on arrival.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (eponaExchange.phase == EponaExchangePhase::IDLE) return;
        if (!IsSaveLoaded() || !gPlayState) return;
        Player* player = GET_PLAYER(gPlayState);

        if (eponaExchange.phase == EponaExchangePhase::WHINNEYING) {
            // Wait ~30 frames for the neigh animation, then spawn the proxy.
            if (gPlayState->state.frames - eponaExchange.frameStart < 30) return;

            // Prefer spawning from the sender's dummy position; fall back to receiver.
            Vec3f spawnPos = player->actor.world.pos;
            spawnPos.y += 80.0f;
            for (auto& [cid, client] : clients) {
                if (!client.self && client.online && client.player &&
                    client.name == eponaExchange.item.senderName) {
                    spawnPos = client.player->actor.world.pos;
                    spawnPos.y += 80.0f;
                    break;
                }
            }

            Actor* proxy = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_ITEM00,
                                       spawnPos.x, spawnPos.y, spawnPos.z, 0, 0, 0,
                                       ITEM00_SOH_GIVE_ITEM_ENTRY);
            if (proxy) {
                EnItem00* item00 = (EnItem00*)proxy;
                item00->itemEntry  = eponaExchange.entry; // pre-resolved in StartEponaExchange
                item00->unk_154    = 999;                 // prevent auto-collect
                proxy->speedXZ     = 0.0f;
                proxy->gravity     = 0.0f;
            }
            eponaExchange.proxyActor = proxy;
            eponaExchange.proxyTarget = player->actor.world.pos;
            eponaExchange.proxyTarget.y += 60.0f;
            eponaExchange.phase      = EponaExchangePhase::PROXY_FLYING;
            eponaExchange.frameStart = gPlayState->state.frames;

        } else if (eponaExchange.phase == EponaExchangePhase::PROXY_FLYING) {
            if (!eponaExchange.proxyActor) {
                // Proxy was destroyed unexpectedly (scene change etc.) — requeue.
                physicalItemQueue.push_front(eponaExchange.item);
                eponaExchange.phase = EponaExchangePhase::IDLE;
                return;
            }

            u32   t        = gPlayState->state.frames - eponaExchange.frameStart;
            float progress = (t < 40) ? (float)t / 40.0f : 1.0f;

            Actor* proxy = eponaExchange.proxyActor;
            // Track player position in real-time.
            eponaExchange.proxyTarget = player->actor.world.pos;
            eponaExchange.proxyTarget.y += 60.0f;
            Vec3f& tgt = eponaExchange.proxyTarget;

            // Smooth XZ approach.
            proxy->world.pos.x += (tgt.x - proxy->world.pos.x) * 0.12f;
            proxy->world.pos.z += (tgt.z - proxy->world.pos.z) * 0.12f;
            // Parabolic Y arc: lerp toward target + sine-based lift peak at midpoint.
            float arc = sinf(progress * (float)M_PI) * 80.0f;
            proxy->world.pos.y += (tgt.y + arc - proxy->world.pos.y) * 0.12f;

            // Arrival: XZ within 20 units or flight time exhausted.
            float dx = proxy->world.pos.x - player->actor.world.pos.x;
            float dz = proxy->world.pos.z - player->actor.world.pos.z;
            if ((dx * dx + dz * dz < 400.0f) || progress >= 1.0f) {
                // Guard: player must be able to receive the item.
                // If not ready, keep proxy hovering close and retry next frame.
                bool playerBusy = (player->stateFlags1 &
                    (PLAYER_STATE1_GETTING_ITEM | PLAYER_STATE1_IN_ITEM_CS |
                     PLAYER_STATE1_IN_CUTSCENE | PLAYER_STATE1_DEAD)) != 0;
                if (playerBusy) {
                    // Hover the proxy directly above the player until they're free.
                    proxy->world.pos.x += (player->actor.world.pos.x - proxy->world.pos.x) * 0.2f;
                    proxy->world.pos.y += ((player->actor.world.pos.y + 60.0f) - proxy->world.pos.y) * 0.2f;
                    proxy->world.pos.z += (player->actor.world.pos.z - proxy->world.pos.z) * 0.2f;
                    return;
                }

                Actor_Kill(proxy);
                eponaExchange.proxyActor = nullptr;

                GetItemEntry giveEntry = eponaExchange.entry;
                GiveItemEntryWithoutActor(gPlayState, giveEntry);
                Notification::Emit({
                    .prefix = eponaExchange.item.senderName,
                    .message = "hat dir übergeben:",
                    .suffix = eponaExchange.item.itemName,
                });

                eponaExchange.phase      = EponaExchangePhase::WAITING_DIALOG;
                eponaExchange.frameStart = gPlayState->state.frames;
            }

        } else if (eponaExchange.phase == EponaExchangePhase::WAITING_DIALOG) {
            // Wait for the item CS / textbox to finish, or time out after 300 frames.
            bool done = !(player->stateFlags1 &
                (PLAYER_STATE1_GETTING_ITEM | PLAYER_STATE1_IN_ITEM_CS));
            u32 elapsed = gPlayState->state.frames - eponaExchange.frameStart;
            if (done || elapsed > 300) {
                eponaExchange.phase = EponaExchangePhase::IDLE;
            }
        }
    });
    // #endregion

    // #region BG Actor Keyframe Sync
    // Link-movable puzzle solve guard:
    // Detect local switch transitions caused by push-block puzzles and
    // broadcast a one-shot room event so every client in the same room can
    // apply the solved switch state.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!IsSaveLoaded() || !gPlayState) return;
        if (!roomState.syncBGObjects) return;

        const s16 sceneNum = gPlayState->sceneNum;
        const s8 roomNum = gPlayState->roomCtx.curRoom.num;

        for (int cat : { ACTORCAT_BG, ACTORCAT_PROP }) {
            Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
            while (actor != nullptr) {
                if (actor->room != -1 && actor->room != roomNum) {
                    actor = actor->next;
                    continue;
                }

                s16 switchFlag = -1;
                if (!TryGetLinkPuzzleSwitchFlag(actor, &switchFlag)) {
                    actor = actor->next;
                    continue;
                }

                const std::string stateKey = BuildRoomKey(sceneNum, roomNum) + "_" + std::to_string((int)switchFlag);
                const bool isSolved = Flags_GetSwitch(gPlayState, switchFlag);

                auto stateIt = puzzleSwitchStateByKey.find(stateKey);
                if (stateIt == puzzleSwitchStateByKey.end()) {
                    puzzleSwitchStateByKey[stateKey] = isSolved;
                    if (isSolved) {
                        announcedPuzzleSwitchSolveByKey.insert(stateKey);
                    }
                    actor = actor->next;
                    continue;
                }

                const bool wasSolved = stateIt->second;
                stateIt->second = isSolved;
                if (wasSolved || !isSolved) {
                    actor = actor->next;
                    continue;
                }

                if (!announcedPuzzleSwitchSolveByKey.count(stateKey)) {
                    nlohmann::json data;
                    data["switchFlag"] = switchFlag;
                    data["actorId"] = actor->id;
                    data["actorKey"] = GetActorKey(actor, sceneNum);
                    data["solverClientId"] = ownClientId;

                    SendPacket_RoomEvent("PUZZLE_SWITCH_SOLVED", std::string("puzzle_sw_") + std::to_string((int)switchFlag),
                                         data, /*streaming=*/false);
                    announcedPuzzleSwitchSolveByKey.insert(stateKey);
                }

                actor = actor->next;
            }
        }
    });

    // Authority: scan all ACTORCAT_BG and ACTORCAT_PROP actors every frame.
    // Send a keyframe packet when:
    //   a) The actor has moved and the heartbeat interval has elapsed, OR
    //   b) The actor's velocity has reversed direction (platform turnaround).
    // Static actors (zero movement) are skipped automatically.
    // Guarded by roomState.syncBGObjects (separate from syncEnemies) so BG and enemy sync
    // can be toggled independently.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!IsSaveLoaded() || !gPlayState) return;
        if (!roomState.syncBGObjects) return;
        if (!IsAnyClientInSameRoom()) return;
        const bool isAuthority = IsEnemyAuthority();

        for (int cat : { ACTORCAT_BG, ACTORCAT_PROP }) {
            Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
            while (actor != nullptr) {
                // Epona (ACTOR_EN_HORSE) lives in ACTORCAT_BG but each client
                // runs its own Epona independently — her position must NOT be
                // broadcast as a BG keyframe, otherwise non-master clients have
                // their local Epona dragged to the master's Epona position while
                // the player is riding it (causing all three clone bugs: erratic
                // controls, underground tilt, intermittent failure).
                if (actor->id == ACTOR_EN_HORSE) {
                    actor = actor->next;
                    continue;
                }
                // EN_GOROIWA (rolling stone, ACTORCAT_PROP) has fixed scripted
                // routes and runs locally on each client after spawn-sync.
                // Broadcasting its position would fight local physics (spin/glitch).
                if (actor->id == ACTOR_EN_GOROIWA) {
                    actor = actor->next;
                    continue;
                }
                if (IsFlagDrivenWaterBgActor(actor)) {
                    actor = actor->next;
                    continue;
                }
                // Regular BG actors (platforms, doors, elevators) are only ever
                // broadcast by the authority. Link-movable puzzle blocks are the
                // exception: they are pushed by whichever client stands next to
                // them (never frozen, see ShouldKeepDungeonBgActorUpdating), so
                // ANY client must be able to broadcast its own local push —
                // otherwise only the master's pushes are ever seen by others and
                // the block silently desyncs the moment a non-master pushes it.
                if (!isAuthority && !IsLinkMovablePuzzleActor(actor)) {
                    actor = actor->next;
                    continue;
                }
                std::string key = GetActorKey(actor, gPlayState->sceneNum);
                BgKeyframe& kf  = trackedBgActors[key];

                // Squared distance from last tracked position.
                float dx = actor->world.pos.x - kf.pos.x;
                float dy = actor->world.pos.y - kf.pos.y;
                float dz = actor->world.pos.z - kf.pos.z;
                float distSq = dx*dx + dy*dy + dz*dz;

                if (distSq > BG_POS_CHANGE_THRESHOLD_SQ) {
                    // Zeit-basiertes Heartbeat-Intervall: unabhaengig von der Framerate.
                    auto  now          = Clock::now();
                    auto  msSinceSent  = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             now - kf.lastSentAt).count();

                    // Link-verschiebbare Puzzleobjekte brauchen dichtere Keyframes,
                    // damit Schieben/Ziehen fuer Clients direkt und stabil wirkt.
                    const uint32_t heartbeatMs = IsLinkMovablePuzzleActor(actor) ? 120u : BG_KEYFRAME_INTERVAL_MS;

                    // Velocity direction reversal detection (dot product sign flip).
                    float dot = actor->velocity.x * kf.vel.x
                              + actor->velocity.y * kf.vel.y
                              + actor->velocity.z * kf.vel.z;
                    float magCur  = fabsf(actor->velocity.x) + fabsf(actor->velocity.y) + fabsf(actor->velocity.z);
                    float magLast = fabsf(kf.vel.x) + fabsf(kf.vel.y) + fabsf(kf.vel.z);
                    bool  velReversed = (magCur > 0.001f && magLast > 0.001f && dot < 0.0f);

                    if (velReversed || static_cast<uint32_t>(msSinceSent) >= heartbeatMs) {
                        SendPacket_BgKeyframeSync(actor);
                        kf.lastSentAt = now;
                    }
                }

                kf.pos = actor->world.pos;
                kf.vel = actor->velocity;
                actor  = actor->next;
            }
        }
    });

    // Client: every frame, blend all tracked BG actors toward their last
    // received keyframe target using Math_ApproachF for smooth position correction.
    // world.rot.y is snapped directly (no blend) because rotation reversals must be
    // immediate — blending a rotation that just reversed would show the wrong direction.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!IsSaveLoaded() || !gPlayState) return;
        if (!roomState.syncBGObjects || bgActorKeyframeTarget.empty()) return;
        const bool isAuthority = IsEnemyAuthority();

        for (int cat : { ACTORCAT_BG, ACTORCAT_PROP }) {
            Actor* actor = gPlayState->actorCtx.actorLists[cat].head;
            while (actor != nullptr) {
                // Skip Epona: each client owns their own Epona instance.
                // Blending her toward the master's position fights the local
                // rider AI and causes the clone-Epona bugs.
                if (actor->id == ACTOR_EN_HORSE) {
                    actor = actor->next;
                    continue;
                }
                // EN_GOROIWA: spawn-only sync, runs locally after that.
                if (actor->id == ACTOR_EN_GOROIWA) {
                    actor = actor->next;
                    continue;
                }
                if (IsFlagDrivenWaterBgActor(actor)) {
                    actor = actor->next;
                    continue;
                }
                // The authority is the source of truth for regular BG actors
                // (platforms, doors, elevators) and never blends toward anyone
                // else's keyframe for them. Link-movable puzzle blocks are the
                // exception: the authority must also blend toward a push made
                // by a non-master client, otherwise the master (and every other
                // client relying on the master's own broadcasts) never sees it.
                if (isAuthority && !IsLinkMovablePuzzleActor(actor)) {
                    actor = actor->next;
                    continue;
                }
                std::string key = GetActorKey(actor, gPlayState->sceneNum);
                auto it = bgActorKeyframeTarget.find(key);
                if (it != bgActorKeyframeTarget.end()) {
                    // Expire stale entries: when master stops sending keyframes
                    // (actor was thrown / released), let local physics take over
                    // so the throw arc plays correctly on non-master clients.
                    if (it->second.lastReceivedAt != Clock::time_point{}) {
                        auto msSince = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - it->second.lastReceivedAt).count();
                        if (msSince > 600) {
                            bgActorKeyframeTarget.erase(it);
                            actor = actor->next;
                            continue;
                        }
                    }
                    const BgKeyframeTarget& target = it->second;
                    const bool responsivePuzzleLerp = IsLinkMovablePuzzleActor(actor);
                    const f32 lerpFraction = responsivePuzzleLerp ? 0.30f : BG_LERP_FRACTION;
                    const f32 lerpMaxStep = responsivePuzzleLerp ? 12.0f : BG_LERP_MAX_STEP;
                    Math_ApproachF(&actor->world.pos.x, target.pos.x, lerpFraction, lerpMaxStep);
                    Math_ApproachF(&actor->world.pos.y, target.pos.y, lerpFraction, lerpMaxStep);
                    Math_ApproachF(&actor->world.pos.z, target.pos.z, lerpFraction, lerpMaxStep);
                    actor->world.rot.y = target.rotY; // snap — avoids lag on direction reversals
                }
                actor = actor->next;
            }
        }
    });
    // #endregion

    // #region Cutscene Sync
    // Only the RoomMaster sends TRIGGER_CUTSCENE packets.
    // Non-masters must NOT send because:
    //   1. Their empty (no-script) CS quickly transitions to IDLE and they would
    //      send CS_STATE_IDLE back, prematurely aborting the master's cutscene.
    //   2. Feedback loops: packet → cs-start → cs-end → packet → ...
    // The only packet we need from master is CS_STATE_IDLE: a release signal
    // for any client stuck in a non-IDLE CS state (locally-triggered CS whose
    // ending actor is frozen, or residual state from a previous visit).
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!roomState.syncCutscenes || !roomState.syncEnemies) return;
        if (!IsSaveLoaded() || !gPlayState) return;
        if (!IsRoomMaster()) return; // only master sends

        static u8 prevCsState = CS_STATE_IDLE;
        u8 curCsState = (u8)gPlayState->csCtx.state;

        // When the master's cutscene ends, broadcast CS_STATE_IDLE so any
        // non-master stuck in a locally-triggered CS is cleanly released.
        if (prevCsState != CS_STATE_IDLE && curCsState == CS_STATE_IDLE) {
            SendPacket_TriggerCutscene(CS_STATE_IDLE);
        }

        prevCsState = curCsState;
    });
    // #endregion

    // Orphaned-cutscene watchdog (non-master clients only, independent of syncCutscenes).
    // An actor-driven cutscene (cutsceneIndex < 0xFFF0) can only be ended by its owning
    // actor; if that actor was killed mid-cutscene by a remote kill packet, nothing ends it.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!IsSaveLoaded() || !gPlayState || IsRoomMaster() || clients.empty()) {
            csOrphanFrames = 0;
            csFrozenFrames = 0;
            return;
        }
        if (gSaveContext.cutsceneIndex >= 0xFFF0 || gPlayState->transitionTrigger != TRANS_TRIGGER_OFF ||
            gPlayState->sceneNum == SCENE_FOREST_TEMPLE_BOSS) {
            csOrphanFrames = 0;
            csFrozenFrames = 0;
            return;
        }

        Player* player = GET_PLAYER(gPlayState);
        if (gPlayState->csCtx.state == CS_STATE_IDLE) {
            csOrphanFrames = 0;
            // Link frozen by a boss csAction whose actor is dead, with csCtx already idle.
            if (player != NULL && player->csAction != 0 && player->csAction != 7 && player->csActor != NULL &&
                player->csActor->update == NULL) {
                if (++csFrozenFrames >= 20) {
                    csFrozenFrames = 0;
                    ReleaseOrphanedCutscene("player csActor gone");
                }
            } else {
                csFrozenFrames = 0;
            }
            return;
        }
        csFrozenFrames = 0;

        if (CutsceneOwnerAlive()) {
            csOrphanFrames = 0;
            return;
        }

        // Master reported its cutscene over within the last 2 s and we have no owner: release at once.
        bool masterIdleRecent = csMasterIdleScene == (s16)gPlayState->sceneNum &&
                                (u32)(gPlayState->state.frames - csMasterIdleFrame) < 40;
        if (masterIdleRecent || ++csOrphanFrames >= 20) {
            csOrphanFrames = 0;
            ReleaseOrphanedCutscene(masterIdleRecent ? "master idle latch" : "owner actor gone");
        }
    });
    // #endregion

    // Clear per-frame collectible-spawn tracking used by ENEMY_DROP_ITEM.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        recentCollectibleSpawns.clear();
    });

    // ── Ocarina song forwarding ───────────────────────────────────────────────
    // Any client that successfully plays an ocarina song must tell every other
    // client in the room: BG actors that respond to songs (waterfalls, Jabu-Jabu,
    // temple triggers) are frozen for non-master clients and need the master to
    // drive them, while unfrozen NPCs (e.g. Darunia reacting to Saria's Song) need
    // EVERY client — master included — to react locally. Skip while we're just
    // applying someone else's remote song (see suppressOcarinaRebroadcast) to
    // avoid an infinite echo between all clients in the room.
    COND_HOOK(OnOcarinaSongAction, isConnected, [&]() {
        if (!IsSaveLoaded() || !gPlayState) return;
        if (suppressOcarinaRebroadcast) return;

        nlohmann::json data;
        data["ocarinaMode"]    = (int)gPlayState->msgCtx.ocarinaMode;
        data["ocarinaAction"]  = (int)gPlayState->msgCtx.ocarinaAction;
        data["lastPlayedSong"] = (int)gPlayState->msgCtx.lastPlayedSong;
        SendPacket_RoomEvent("OCARINA_SONG_ACTION", "ocarina_song_action", data, false);

        // EnOkarinaTag (song-check spots — e.g. Zora's River waterfall) is a
        // per-actor, proximity-gated local state machine: only the actively
        // playing client's own instance ever reaches OCARINA_MODE_03 and calls
        // Flags_SetSwitch. Forwarding msgCtx above does NOT make a remote
        // client's own idle EnOkarinaTag react — so directly replicate the
        // success (switch flag + confirmation chime) for every peer within
        // sync range, as if they had played the song themselves.
        if (gPlayState->msgCtx.ocarinaMode == OCARINA_MODE_03) {
            Player* player = GET_PLAYER(gPlayState);
            Actor* node = player ? gPlayState->actorCtx.actorLists[ACTORCAT_PROP].head : nullptr;
            while (node != nullptr) {
                if (node->id == ACTOR_EN_OKARINA_TAG) {
                    EnOkarinaTag* tag = (EnOkarinaTag*)node;
                    if (tag->ocarinaSong == gPlayState->msgCtx.lastPlayedSong &&
                        node->xzDistToPlayer < (90.0f + tag->interactRange) &&
                        fabsf(player->actor.world.pos.y - node->world.pos.y) < 80.0f) {
                        nlohmann::json tagData;
                        tagData["switchFlag"] = (int)tag->switchFlag;
                        tagData["type"] = (int)tag->type;
                        tagData["x"] = node->world.pos.x;
                        tagData["y"] = node->world.pos.y;
                        tagData["z"] = node->world.pos.z;
                        // streaming=true: bypasses the idempotency dedup keyed on
                        // sceneNum_roomNum_eventType_eventKey. The eventKey here is a
                        // fixed constant (not per-actor-instance), so without
                        // streaming only the FIRST tag completed in a room visit
                        // would ever reach peers — any other/later tag (or the same
                        // tag replayed) would be silently dropped as a "duplicate".
                        SendPacket_RoomEvent("OCARINA_TAG_COMPLETED", "ocarina_tag_completed", tagData, true);
                        break;
                    }
                }
                node = node->next;
            }

            // Song of Time blocks (ObjTimeblock) are the same story: their
            // songObserverFunc only advances for the client whose own player is
            // physically in range and attempting to play — forwarding msgCtx above
            // does not make a remote peer's copy of the same block react, so the
            // block only ever disappears for whoever actually played the song.
            // Force-complete every nearby block directly for every peer.
            if (player && gPlayState->msgCtx.lastPlayedSong == OCARINA_SONG_TIME) {
                Actor* bgNode = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head;
                while (bgNode != nullptr) {
                    if (bgNode->id == ACTOR_OBJ_TIMEBLOCK && bgNode->xzDistToPlayer < 320.0f &&
                        fabsf(player->actor.world.pos.y - bgNode->world.pos.y) < 120.0f) {
                        nlohmann::json blockData;
                        blockData["x"] = bgNode->world.pos.x;
                        blockData["y"] = bgNode->world.pos.y;
                        blockData["z"] = bgNode->world.pos.z;
                        std::string key = "timeblock_" + std::to_string((int)bgNode->world.pos.x) + "_" +
                                          std::to_string((int)bgNode->world.pos.y) + "_" +
                                          std::to_string((int)bgNode->world.pos.z);
                        // streaming=true: Song of Time blocks TOGGLE visibility/switch
                        // state each time the song is played (not a one-shot event).
                        // Without streaming, the idempotency dedup (same position-based
                        // key) would silently drop every replay of this exact block
                        // after the first successful completion in this room visit —
                        // which is exactly the "works once, then nearby peers stop
                        // seeing it" symptom.
                        SendPacket_RoomEvent("TIMEBLOCK_SONG_COMPLETE", key, blockData, true);
                    }
                    bgNode = bgNode->next;
                }
            }
        }
    });

    // Non-room-master minigame proxy:
    // Some minigames (notably Zora diving / Lon Lon style counters) are fully
    // local to the interacting client and do not change the room master's
    // gSaveContext directly. Forward local state deltas to the room master,
    // which then emits the canonical MINIGAME_* events for all peers.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!IsSaveLoaded() || !gPlayState) return;
        if (!roomState.syncMinigames) return;
        if (IsRoomMaster()) return;
        if (!IsOwnerInSameRoom()) return;

        static s16 lastScene = -1;
        static s8  lastRoom = -1;
        static u16 lastState = 0;
        static u16 lastScore = 0;

        const s16 curScene = gPlayState->sceneNum;
        const s8  curRoom = gPlayState->roomCtx.curRoom.num;
        const u16 curState = gSaveContext.minigameState;
        const u16 curScore = gSaveContext.minigameScore;

        if (curScene != lastScene || curRoom != lastRoom) {
            lastScene = curScene;
            lastRoom = curRoom;
            lastState = curState;
            lastScore = curScore;
            return;
        }

        const bool changed = (curState != lastState) || ((curState != 0) && (curScore != lastScore));
        if (!changed) return;

        nlohmann::json data;
        data["minigameId"] = (int)curState;
        data["score"]      = (int)curScore;
        SendPacket_RoomEvent("MINIGAME_PROXY_STATE", "minigame_proxy_state", data, /*streaming=*/true);

        lastState = curState;
        lastScore = curScore;
    });

    // Ruto carry-state stream:
    // Ruto's local carry/fall state machine is very sensitive and tied to local
    // actor update order. We avoid driving her full AI remotely and instead
    // stream the carry pose/position so all clients see coherent pickup movement.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!IsSaveLoaded() || !gPlayState) return;
        if (gPlayState->sceneNum != SCENE_JABU_JABU) return;
        if (!IsAnyClientInSameRoom()) return;

        Player* player = GET_PLAYER(gPlayState);
        if (!player) return;

        const bool carryingRuto =
            (player->heldActor != nullptr) &&
            (player->heldActor->id == ACTOR_EN_RU1) &&
            ((player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) != 0);

        static bool lastCarryingRuto = false;
        static u32  lastSentFrame = 0;
        const u32 curFrame = gPlayState->state.frames;

        // Send on state change immediately; while carrying, refresh at 10 Hz.
        const bool periodicDue = carryingRuto && (curFrame - lastSentFrame >= 6);
        if (!periodicDue && carryingRuto == lastCarryingRuto) return;

        nlohmann::json data;
        data["carrying"] = carryingRuto;

        if (carryingRuto) {
            Actor* held = player->heldActor;
            data["x"] = held->world.pos.x;
            data["y"] = held->world.pos.y;
            data["z"] = held->world.pos.z;
            data["rotY"] = (int)held->world.rot.y;
            // Included so a remote client can respawn her if her local room
            // unloaded and destroyed her mid-carry (room transitions elsewhere
            // in the dungeon) — her actor.params encodes her cutscene/type state.
            data["params"] = (int)held->params;
        }

        SendPacket_RoomEvent("RUTO_CARRY_STATE", "ruto_carry_state", data, /*streaming=*/true);
        lastCarryingRuto = carryingRuto;
        lastSentFrame = curFrame;
    });

    // ── Phase 6a: Minigame state monitoring ──────────────────────────────────
    // The Room Master watches gSaveContext.minigameState / minigameScore for
    // transitions and broadcasts ROOM_EVENT packets so every client in the same
    // room keeps the same minigame state and score.
    //
    // All three event types use streaming=true so:
    //  • Dedup is bypassed  → a minigame can be replayed without resync issues.
    //  • quiet=true is set  → no log spam for per-hit SCORE updates.
    //
    // Affected minigames (non-exhaustive):
    //   minigameState=1  Gerudo Horseback Archery — minigameScore = hit count
    //   minigameState=2  Bombchu Bowling target phase
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!IsSaveLoaded() || !gPlayState) return;
        if (!IsRoomMaster())          return;  // only the room authority emits these
        if (!IsAnyClientInSameRoom()) return;  // nobody to sync with
        if (!roomState.syncMinigames) return;  // feature disabled by admin

        // Reset tracking vars when the scene changes so a stale lastMinigameState
        // from the previous scene can't trigger a phantom MINIGAME_END.
        static u16 lastMinigameState = 0;
        static u16 lastMinigameScore = 0;
        static s16 lastSceneNum      = -1;

        const s16 curScene = gPlayState->sceneNum;
        if (curScene != lastSceneNum) {
            lastMinigameState = 0;
            lastMinigameScore = 0;
            lastSceneNum      = curScene;
            return;  // skip event generation on the first frame of the new scene
        }

        const u16 curState = gSaveContext.minigameState;
        const u16 curScore = gSaveContext.minigameScore;

        // ── State transitions ─────────────────────────────────────────────────
        if (lastMinigameState == 0 && curState != 0) {
            // 0 → active: minigame just started.
            nlohmann::json data;
            data["minigameId"] = (int)curState;
            data["score"]      = 0;
            // Minigame-ID 1 = Gerudo Horseback Archery: erfordert Epona-Sync
            // damit Remote-Spieler sehen, dass der Room-Master auf einem Pferd sitzt.
            // Ohne syncEpona das Event nicht senden — kein funktionierender Sync moeglich.
            if (curState != 1 || roomState.syncEpona) {
                SendPacket_RoomEvent("MINIGAME_START", "minigame_start", data, /*streaming=*/true);
            }

        } else if (lastMinigameState != 0 && curState == 0) {
            // active → 0: minigame ended.  Send final score so clients can apply
            // it before the NPC dialog reads gSaveContext.minigameScore.
            const bool won = (lastMinigameScore > 0);  // Heuristik: Score > 0 = Sieg
            nlohmann::json data;
            data["minigameId"] = (int)lastMinigameState;
            data["finalScore"] = (int)lastMinigameScore;
            data["won"]        = won;
            SendPacket_RoomEvent("MINIGAME_END", "minigame_end", data, /*streaming=*/true);
            // Notification fuer den Room Master (der selbst das Minispiel gespielt hat).
            Notification::Emit({
                .prefix      = won ? "Minispiel gewonnen!" : "Minispiel beendet",
                .prefixColor = won ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                                   : ImVec4(0.55f, 0.55f, 0.55f, 1.0f),
                .message     = "Punkte: " + std::to_string((int)lastMinigameScore),
                .remainingTime = 5.0f,
            });
        }

        // ── Live score updates ────────────────────────────────────────────────
        if (curState != 0 && curScore != lastMinigameScore) {
            nlohmann::json data;
            data["minigameId"] = (int)curState;
            data["score"]      = (int)curScore;
            // streaming=true → dedup bypassed + quiet=true (may fire every hit frame)
            SendPacket_RoomEvent("MINIGAME_SCORE", "minigame_score", data, /*streaming=*/true);
        }

        lastMinigameState = curState;
        lastMinigameScore = curScore;
    });

    // ── Phase 6b: Battle Royale — Tod-Erkennung ───────────────────────────────
    // Ueberwacht den PLAYER_STATE1_DEAD-Flag des lokalen Spielers. Wechselt er von
    // alive → dead UND ist ein letzter PvP-Angreifer gespeichert, wird ein
    // PLAYER_KILLED-Paket an alle Peers geschickt. Der Host verarbeitet es
    // autoritativ (Kill-Streak, WANTED_SET, PLAYER_ELIM).
    //
    // Warum Polling statt Hook: Es gibt keinen dedizierten OnPlayerDeath-Hook in
    // GameInteractor. PLAYER_STATE1_DEAD ist das N64-native Todesflag und wird vom
    // Player-Overlay selbst gesetzt – Polling in OnGameFrameUpdate ist die
    // zuverlaessigste und einfachste Methode.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!IsSaveLoaded() || !gPlayState || !roomState.battleRoyaleMode) return;
        if (!brMatchActive)               return;  // kein aktives Match, kein Kill
        if (brEliminated)                 return;  // bereits eliminiert, kein doppeltes PLAYER_KILLED
        if (Clock::now() < brStartProtectionUntil) return;  // Startschutz laeuft noch
        // pvpMode=0 blockiert Kills nur ausserhalb des BR (BR impliziert PvP).
        if (!roomState.pvpMode && !roomState.battleRoyaleMode) return;
        if (lastPvpAttackerClientId == 0) return;  // kein ausstehender Kill zu melden

        Player* player = GET_PLAYER(gPlayState);
        if (!player) return;

        // brWasAlive (Member-Variable) verfolgt den Alive/Dead-Uebergang.
        // Wird auf MATCH_START zurückgesetzt, damit ein Rematch sauber beginnt.
        const bool isDead = (player->stateFlags1 & PLAYER_STATE1_DEAD) != 0;

        if (brWasAlive && isDead) {
            // Übergang alive → dead mit bekanntem PvP-Angreifer: Kill melden.
            // Inventar-Snapshot mitschicken, damit der Host lootbare Items auswählen kann.
            nlohmann::json victimInventory = nlohmann::json::array();
            for (int s = 0; s < 24; s++) {
                victimInventory.push_back(static_cast<int>(gSaveContext.inventory.items[s]));
            }
            SPDLOG_INFO("[Anchor:BR] Lokaler Spieler getoetet von Client {}",
                        lastPvpAttackerClientId);
            SendPacket_BattleRoyaleEvent("PLAYER_KILLED", {
                { "attackerClientId", lastPvpAttackerClientId },
                { "victimInventory",  victimInventory         }
            });
            brEliminated            = true;   // Temporaer bis PLAYER_ELIM empfangen
            lastPvpAttackerClientId = 0;       // Attribution zurücksetzen
        }

        brWasAlive = !isDead;
    });

    // ── Phase 6b: Battle Royale — Wanted-NPC-Aggro ───────────────────────────
    // Wenn der lokale Spieler "wanted" ist (wantedClients enthaelt ownClientId),
    // werden NPCs in WANTED_NPC_AGGRO_RADIUS nach jedem Update-Frame auf den
    // Spieler ausgerichtet und mit Mindestgeschwindigkeit versehen.
    //
    // Implementierung via OnActorUpdate (nach dem eigentlichen NPC-Update):
    // Setzt world.rot.y / shape.rot.y und speedXZ, damit NPCs die in Gehrichtung
    // gehen, automatisch auf den Spieler zulaufen.  Keine einzelnen Overlay-Eingriffe.
    COND_HOOK(OnActorUpdate, isConnected, [&](void* refActor) {
        if (!IsSaveLoaded() || !gPlayState || !roomState.battleRoyaleMode) return;
        if (!wantedClients.count(ownClientId)) return;  // lokaler Spieler nicht wanted

        Actor* actor = static_cast<Actor*>(refActor);
        if (actor->category != ACTORCAT_NPC) return;

        Player* player = GET_PLAYER(gPlayState);
        if (!player) return;

        const float dx     = player->actor.world.pos.x - actor->world.pos.x;
        const float dz     = player->actor.world.pos.z - actor->world.pos.z;
        const float distSq = dx * dx + dz * dz;
        const float rSq    = BR_WANTED_NPC_AGGRO_RADIUS * BR_WANTED_NPC_AGGRO_RADIUS;

        if (distSq < 1.0f || distSq > rSq) return;

        // Drehe den NPC zum Spieler — viele OoT-NPCs nehmen shape.rot.y
        // als Bewegungsrichtung, wenn sie vorwaerts gehen.
        const s16 yawToPlayer = Math_Atan2S(dx, dz);
        actor->world.rot.y    = yawToPlayer;
        actor->shape.rot.y    = yawToPlayer;

        // Minimale Gehgeschwindigkeit, damit der NPC sich tatsaechlich bewegt.
        // Der eigene Actor-Update hat speedXZ bereits gesetzt; nur erhoehen, nie
        // senken, um die Actor-KI nicht zu stoeren.
        if (actor->speedXZ >= 0.0f && actor->speedXZ < 2.0f) {
            actor->speedXZ = 2.0f;
        }
    });

    // ── Phase 6b: Battle Royale — Cheats deaktivieren ────────────────────────
    // Solange battleRoyaleMode aktiv ist, werden alle gameplay-relevanten Cheats
    // pro Frame auf 0 erzwungen. Damit kann kein Spieler im kompetitiven Modus
    // Vorteile durch Infinite-Health, Moon-Jump o.ae. erlangen.
    //
    // Mechanismus: COND_HOOK laeuft jeden Frame; wenn der CVar != 0 ist, wird er
    // per CVarSetInteger gecleared. Der jeweilige Cheat-Hook prueft seinen eigenen
    // CVar als Bedingung (COND_HOOK), sodass dessen Effekt im naechsten Frame
    // ebenfalls nicht mehr ausgeloest wird.
    COND_HOOK(OnGameFrameUpdate, isConnected, [&]() {
        if (!roomState.battleRoyaleMode) return;

        if (CVarGetInteger(CVAR_CHEAT("InfiniteHealth"), 0) != 0) {
            CVarSetInteger(CVAR_CHEAT("InfiniteHealth"), 0);
        }
        if (CVarGetInteger(CVAR_CHEAT("InfiniteAmmo"), 0) != 0) {
            CVarSetInteger(CVAR_CHEAT("InfiniteAmmo"), 0);
        }
        if (CVarGetInteger(CVAR_CHEAT("InfiniteMagic"), 0) != 0) {
            CVarSetInteger(CVAR_CHEAT("InfiniteMagic"), 0);
        }
        if (CVarGetInteger(CVAR_CHEAT("InfiniteNayru"), 0) != 0) {
            CVarSetInteger(CVAR_CHEAT("InfiniteNayru"), 0);
        }
        if (CVarGetInteger(CVAR_CHEAT("InfiniteMoney"), 0) != 0) {
            CVarSetInteger(CVAR_CHEAT("InfiniteMoney"), 0);
        }
        if (CVarGetInteger(CVAR_CHEAT("MoonJumpOnL"), 0) != 0) {
            CVarSetInteger(CVAR_CHEAT("MoonJumpOnL"), 0);
        }
        if (CVarGetInteger(CVAR_CHEAT("NoRestrictItems"), 0) != 0) {
            CVarSetInteger(CVAR_CHEAT("NoRestrictItems"), 0);
        }
        if (CVarGetInteger(CVAR_CHEAT("DekuStick"), 0) != 0) {
            CVarSetInteger(CVAR_CHEAT("DekuStick"), 0);
        }
    });

    // #endregion  // end RegisterHooks
}
