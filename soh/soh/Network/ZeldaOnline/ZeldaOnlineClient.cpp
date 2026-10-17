#include "ZeldaOnlineClient.hpp"
#include <ship/window/Window.h>
#include <ship/Context.h>
#include "soh/Enhancements/cosmetics/cosmeticsTypes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Notification/Notification.h"
#include "soh/frame_interpolation.h"
#include "soh/OTRGlobals.h"
#include <soh/SohGui/ImGuiUtils.h>
#include <soh/Enhancements/item-tables/ItemTableManager.h>
#include "ship/resource/ResourceManager.h"
#include "Packet.hpp"
#include "PacketTypes.hpp"
#include "ActorControllerFactory.hpp"
#include "ActorControllers/PlayerPuppetController.hpp"
#include "z64actor_enum.h"
#include "soh/Enhancements/nametag.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/Enhancements/randomizer/randomizer.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <cstdio>
#include "HorsePuppet.hpp"
#include "PacketStrings.hpp"
#include "assets/objects/gameplay_keep/gameplay_keep.h"
#include <soh/Extractor/Extract.h>
#include <soh/Enhancements/randomizer/logic.h>
#include <soh/ActorDB.h>
#include "soh/Enhancements/PlayerSkin/PlayerSkin.h"
#include <ship/utils/StringHelper.h>

#include "ZeldaOnlineRoomWindow.hpp"

#ifdef _WIN32
static void DbgPrintf(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
}
#else
static void DbgPrintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}
#endif


extern "C" {
#include "variables.h"
#include "z64.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Obj_Switch/z_obj_switch.h"
#include "src/overlays/actors/ovl_Door_Shutter/z_door_shutter.h"
#include "src/overlays/actors/ovl_En_Door/z_en_door.h"
#include "src/overlays/actors/ovl_En_Box/z_en_box.h"

 extern f32 D_80130F28;
extern PlayState* gPlayState;
extern SaveContext gSaveContext;
extern MapData* gMapData;
extern int gMapLoading;
u8 gZeldaOnlineEngineCleanup = 0;
float OTRGetDimensionFromLeftEdge(float v);
float OTRGetDimensionFromRightEdge(float v);
float OTRGetRectDimensionFromLeftEdge(float v);

void Player_ReapplySkeleton(Player* thisx, PlayState* play);
GetItemEntry ItemTable_Retrieve(int16_t getItemID);
}

static void Messagebox_ShowErrorBox(const char* title, const char* body) {
    Extractor::ShowErrorBox(title, body);
}

static void DrawScreenText(GraphicsContext* gfxCtx, const char* text, s16 x, s16 y, Color_RGBA8 color, f32 scale) {
    if (text == nullptr || scale <= 0.0f) {
        return;
    }

    std::string processed = text;
    processed.erase(std::remove_if(processed.begin(), processed.end(),
                                   [](const char& c) { return (uint8_t)c > 172 || (c < ' ' && c != '\0'); }),
                    processed.end());

    if (processed.empty()) {
        return;
    }

    Gfx_SetupDL_39Overlay(gfxCtx);

    GraphicsContext* __gfxCtx = gfxCtx;

    gDPSetAlphaCompare(OVERLAY_DISP++, G_AC_NONE);
    gDPSetCombineLERP(OVERLAY_DISP++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE, TEXEL0, 0,
                      PRIMITIVE, 0);
    gDPSetPrimColor(OVERLAY_DISP++, 0, 0, color.r, color.g, color.b, color.a);

    s16 dsdx = (s16)((1 << 10) / scale);
    s16 charW = (s16)(FONT_CHAR_TEX_WIDTH * scale);
    s16 charH = (s16)(FONT_CHAR_TEX_HEIGHT * scale);
    s16 lineH = (s16)(16.0f * scale);

    s16 originX = (s16)(OTRGetRectDimensionFromLeftEdge((f32)(x)));
    s16 penX = originX;
    s16 penY = y;

    for (size_t i = 0; i < processed.length(); i++) {
        if (processed[i] == '\n') {
            penX = originX;
            penY += lineH;
            continue;
        }

        uintptr_t texture = (uintptr_t)Ship_GetCharFontTexture(processed[i]);

        gDPLoadTextureBlock_4b(OVERLAY_DISP++, texture, G_IM_FMT_I, FONT_CHAR_TEX_WIDTH, FONT_CHAR_TEX_HEIGHT, 0,
                               G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK,
                               G_TX_NOLOD, G_TX_NOLOD);

        gSPWideTextureRectangle(OVERLAY_DISP++, penX << 2, penY << 2, (penX + charW) << 2, (penY + charH) << 2,
                                G_TX_RENDERTILE, 0, 0, dsdx, dsdx);

        penX += (s16)(Ship_GetCharFontWidth(processed[i]) * scale);
    }
}

namespace ZeldaOnline {

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#endif


// Some locations are using the same scene. We can make these locations unique based on some other data
// Grttos/Fairy locations
static int GetSceneVariant(int sceneNum) {
    switch (sceneNum) {
        case SCENE_GROTTOS:
            return gSaveContext.respawn[RESPAWN_MODE_RETURN].data & 0xFF;

        case SCENE_GREAT_FAIRYS_FOUNTAIN_MAGIC:
        case SCENE_FAIRYS_FOUNTAIN:
        case SCENE_GREAT_FAIRYS_FOUNTAIN_SPELLS:
        case SCENE_GRAVE_WITH_FAIRYS_FOUNTAIN:
            return gSaveContext.entranceIndex & 0xFFFF;
        case SCENE_LON_LON_RANCH: {
            if (!Flags_GetEventChkInf(EVENTCHKINF_EPONA_OBTAINED)) {
                s32 ingoState = gSaveContext.eventInf[0] & 0xF;

                if (ingoState == 5 || ingoState == 6)
                    return 2;

                if (ingoState == 1 || ingoState == 3 || ingoState == 4 || ingoState == 7)
                    return 1;
            }
            return IS_CUTSCENE_LAYER;
        }
        default:
            return IS_CUTSCENE_LAYER;
    }
}


static bool IsDungeonScene(int sceneNum) {
    return sceneNum == SCENE_DEKU_TREE || sceneNum == SCENE_DODONGOS_CAVERN || sceneNum == SCENE_JABU_JABU ||
           sceneNum == SCENE_FOREST_TEMPLE || sceneNum == SCENE_FIRE_TEMPLE || sceneNum == SCENE_WATER_TEMPLE ||
           sceneNum == SCENE_SPIRIT_TEMPLE || sceneNum == SCENE_SHADOW_TEMPLE || sceneNum == SCENE_BOTTOM_OF_THE_WELL ||
           sceneNum == SCENE_ICE_CAVERN || sceneNum == SCENE_THIEVES_HIDEOUT || sceneNum == SCENE_INSIDE_GANONS_CASTLE;
}

static bool IsBossScene(int sceneNum) {
    return sceneNum == SCENE_DEKU_TREE_BOSS || sceneNum == SCENE_DODONGOS_CAVERN_BOSS ||
           sceneNum == SCENE_JABU_JABU_BOSS || sceneNum == SCENE_FOREST_TEMPLE_BOSS ||
           sceneNum == SCENE_FIRE_TEMPLE_BOSS || sceneNum == SCENE_WATER_TEMPLE_BOSS ||
           sceneNum == SCENE_SPIRIT_TEMPLE_BOSS || sceneNum == SCENE_SHADOW_TEMPLE_BOSS ||
           sceneNum == SCENE_GANONDORF_BOSS || sceneNum == SCENE_GANON_BOSS;
}

static bool IsPartyScene(int sceneNum) {
    return IsDungeonScene(sceneNum) || IsBossScene(sceneNum);
}
// Can we call ReloadSceneInPlace? Not while paused or in a cutscene
static bool CanReloadSceneNow(PlayState* play) {
    Player* player = GET_PLAYER(play);
    return play->transitionTrigger == TRANS_TRIGGER_OFF && play->transitionMode == TRANS_MODE_OFF &&
           play->msgCtx.msgMode == MSGMODE_NONE && play->pauseCtx.state == 0 && play->csCtx.state == CS_STATE_IDLE &&
           player != NULL && !(player->stateFlags1 & (PLAYER_STATE1_IN_ITEM_CS | PLAYER_STATE1_IN_CUTSCENE));
}

// Reload the current scene and keep the same position.
// Used for day/night transitions and disconnects/reconnects
static void ReloadSceneInPlace(PlayState* play) {
    if (play == nullptr)
        return;

    Player* player = GET_PLAYER(play);
    if (player == NULL)
        return;

    if ((player->stateFlags1 & PLAYER_STATE1_ON_HORSE) && player->rideActor != NULL) {
        gSaveContext.horseData.scene = play->sceneNum;
        gSaveContext.horseData.pos.x = (int16_t)player->rideActor->world.pos.x;
        gSaveContext.horseData.pos.y = (int16_t)player->rideActor->world.pos.y;
        gSaveContext.horseData.pos.z = (int16_t)player->rideActor->world.pos.z;
        gSaveContext.horseData.angle = player->rideActor->world.rot.y;
    }
    
    gSaveContext.respawnFlag = 1;
    play->nextEntranceIndex = gSaveContext.entranceIndex;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = play->nextEntranceIndex;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex = play->roomCtx.curRoom.num;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].pos = player->actor.world.pos;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw = player->actor.shape.rot.y;
    if (play->roomCtx.curRoom.behaviorType2 < 4) {
        gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0x0DFF;
    } else {
        Camera* camera = GET_ACTIVE_CAM(play);
        gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0x0D00 | camera->camDataIdx;
    }
    play->transitionTrigger = TRANS_TRIGGER_START;
    play->transitionType = TRANS_TYPE_INSTANT;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;

    static int hookId = 0;
    hookId = REGISTER_VB_SHOULD(VB_INFLICT_VOID_DAMAGE, {
        *should = false;
        GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::OnVanillaBehavior>(hookId);
    });
}


// Must be 10 characters
static const std::string CURRENT_VERSION = "BETA000003";
ZeldaOnlineClient::ZeldaOnlineClient() {
    m_blockedParentSpawners.insert(ACTOR_BG_SPOT01_OBJECTS2);
}

void ZeldaOnlineClient::Connect() {
    m_host = CVarGetString("gZeldaOnline.Host", "awu.fks.mybluehost.me");
    m_port = CVarGetInteger("gZeldaOnline.Port", 21050);
    m_nickName = CVarGetString("gZeldaOnline.Nickname", "Player");


    std::vector<std::string> archives;
    SplitSkinRef(m_skinRef, m_skinName, archives);
    ZeldaOnlineRoomWindow::Instance->SetConnecting(true);
    ZNetworking::Enable(m_host.c_str(), m_port);

    m_fileServerUrl = CVarGetString("gZeldaOnline.FileServer", "http://awu.fks.mybluehost.me/");
    RegisterNetworkingHook(true);

    auto missingArchives = MissingArchives(archives);
    if (missingArchives.size()) {
        RequestSkinDownload(archives, m_skinName);
    }
    m_autoReconnect = true;

}

void ZeldaOnlineClient::Disconnect() {
    m_autoReconnect = false;
    
    ZNetworking::Disable();
    ReloadSceneInPlace(gPlayState);


}

void ZeldaOnlineClient::Enable() {
    m_skinRef = CVarGetString("gZeldaOnline.Skin", "");
    LoadPartySettings();

    ZeldaOnlineRoomWindow::Instance->CenterAndExpand();

    ZeldaOnlineRoomWindow::Instance->Show();


    ZeldaOnlineRoomWindow::Instance->SetActionHandler([this](const PlayerListActionEvent* e) {
        switch (e->action) {
            case PlayerListAction::Invite: {
                auto* inviteAction = reinterpret_cast<const PlayerListActionEventInvite*>(e);

                WritePacket(newPacket(CLIENT_PACKET_PARTY_INVITE) << PackedUInt2((uint16_t)inviteAction->networkID));
                break;
            }

            case PlayerListAction::ChangeName: {
                auto* textAction = reinterpret_cast<const PlayerListActionEventText*>(e);
                m_nickName = textAction->text;

                if (gPlayState) {
                    NameTag_RemoveAllByActorTag(&GET_PLAYER(gPlayState)->actor, "name");
                    NameTagOptions options{};
                    options.tag = "name";
                    options.textColor =  Color_RGBA8{ 255, 255, 255, 255 };

                    NameTag_RegisterForActorWithOptions(&GET_PLAYER(gPlayState)->actor, m_nickName.c_str(), options);
                    m_removeNametagTimer = 20 * 5;
                }
                ZeldaOnlineRoomWindow::Instance->SetDisplayName("");
                CVarSetString("gZeldaOnline.Nickname", m_nickName.c_str());
                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                break;
            }

            /* case PlayerListAction::SetPartyScenes: {
                auto* toggle = reinterpret_cast<const PlayerListActionEventToggle*>(e);
                WritePacket(newPacket(CLIENT_PACKET_PARTY_ENABLE_SCENES) << PackedUInt1(toggle->enabled ? 1 : 0));

                if (gPlayState && IsPartyScene(gPlayState->sceneNum))
                    ReloadSceneInPlace(gPlayState);
                break;
            }*/

            case PlayerListAction::CreateParty: {
                WritePacket(newPacket(CLIENT_PACKET_PARTY_CREATE) << BuildPartySettingsJSON(m_partySettings));
                break;
            }

            case PlayerListAction::LeaveParty: {
                WritePacket(newPacket(CLIENT_PACKET_PARTY_LEAVE));
                break;
            }

            case PlayerListAction::TeleportTo: {
                auto* target = reinterpret_cast<const PlayerListActionEventInvite*>(e);
                WritePacket(newPacket(CLIENT_PACKET_TELEPORT_TO_PARTY_MEMBER) << PackedUInt2(target->networkID));
                break;
            }

            case PlayerListAction::Connect: {
                this->Connect();
                break;
            }

            case PlayerListAction::Disconnect: {
                this->Disconnect();
                break;
            }
            case PlayerListAction::AcceptInvite: {
                auto* inviteAction = reinterpret_cast<const PlayerListActionEventInvite*>(e);

                WritePacket(newPacket(CLIENT_PACKET_PARTY_JOIN)
                            << PackedUInt2((uint16_t)inviteAction->networkID) << PackedUInt4(inviteAction->partyID));
                break;
            }

            case PlayerListAction::DeclineInvite: {
                auto* inviteAction = reinterpret_cast<const PlayerListActionEventInvite*>(e);

                WritePacket(newPacket(CLIENT_PACKET_PARTY_DECLINE)
                            << PackedUInt2((uint16_t)inviteAction->networkID) << PackedUInt4(inviteAction->partyID));

                ZeldaOnlineRoomWindow::Instance->SetPendingPartyInvite(inviteAction->networkID, 0);
                break;
            }

            case PlayerListAction::ToggleSkinPitch: {
                auto* multiVar = reinterpret_cast<const PlayerListActionEventMultiVar*>(e);

                if (multiVar->intValue)
                    CVarSetFloat(CVAR_AUDIO("LinkVoiceFreqMultiplier"), multiVar->floatValue);
                break;
            
            }
            case PlayerListAction::ChangeSkin: {
                auto* multiVar = reinterpret_cast<const PlayerListActionEventMultiVar*>(e);
                m_skinRef = multiVar->text;
                std::vector<std::string> archives;
                SplitSkinRef(m_skinRef, m_skinName, archives);

                if (gPlayState != nullptr) {
                    Player* player = GET_PLAYER(gPlayState);

                    if (m_skinName == "") {
                        player->skin = nullptr;
                        Player_ReapplySkeleton(player, gPlayState);
                        ZeldaOnlineRoomWindow::Instance->SetCurrentSkin("link");

                        if (multiVar->intValue)
                            CVarSetFloat(CVAR_AUDIO("LinkVoiceFreqMultiplier"), 1.0f);
                        break;
                    }

                    player->skin = PlayerSkin_Get(m_skinName.c_str());

                    if (multiVar->intValue)
                        CVarSetFloat(CVAR_AUDIO("LinkVoiceFreqMultiplier"), multiVar->floatValue);
                }

                auto missingArchives = MissingArchives(archives);
                if (missingArchives.size()) {
                    RequestSkinDownload(archives, m_skinName);
                }
                if (gPlayState != nullptr)
                    Player_ReapplySkeleton(GET_PLAYER(gPlayState), gPlayState);

                ZeldaOnlineRoomWindow::Instance->SetCurrentSkin(m_skinRef);

                break;
            }

            case PlayerListAction::Chat: {
                auto* messageEvent = reinterpret_cast<const PlayerListActionEventText*>(e);

                if (!isConnected)
                    break;

                WritePacket(newPacket(CLIENT_PACKET_CHAT_MESSAGE) << messageEvent->text);

                m_chatVisibleTimer = 8 * 20;
                
                if (gPlayState) {
                    auto playerActor = &GET_PLAYER(gPlayState)->actor;
                    NameTagOptions options{};
                    options.tag = "chat";
                    options.textColor = { 255, 255, 0, 255 };
                    options.yOffset = -10;
                    options.scale = 0.5f;
                    NameTag_RemoveAllByActorTag(playerActor, "chat");
                    NameTag_RegisterForActorWithOptions(playerActor, messageEvent->text.c_str(), options);
                }

                break;
            }

            case PlayerListAction::UnstuckMe: {
                if (gPlayState == nullptr)
                    break;

                Player_SetCsActionWithHaltedActors(gPlayState, NULL, 7);
                gPlayState->nextEntranceIndex = gSaveContext.entranceIndex;
                gPlayState->transitionTrigger = TRANS_TRIGGER_START;
                gPlayState->transitionType = TRANS_TYPE_FADE_BLACK;
            } break;
           
            case PlayerListAction::PartySettingChanged: {
                PartySettings& partySettings = ZeldaOnlineRoomWindow::Instance->GetPartySettings();

                WritePacket(newPacket(CLIENT_PACKET_UPDATE_PARTY_SETTINGS) << BuildPartySettingsJSON(partySettings));
                SetPartySettings(partySettings);

                CVarSetInteger("gZeldaOnline.Party.PrivateDungeons", m_partySettings.privateDungeons ? 1 : 0);
                CVarSetInteger("gZeldaOnline.Party.ExtraEnemies", m_partySettings.extraEnemies ? 1 : 0);
                CVarSetFloat("gZeldaOnline.Party.ExtraEnemyWeight", m_partySettings.extraEnemyWeight);
                CVarSetInteger("gZeldaOnline.Party.HealthMultiplier", m_partySettings.healthMultiplier ? 1 : 0);
                CVarSetFloat("gZeldaOnline.Party.EnemyHealthWeight", m_partySettings.enemyHealthWeight);
                CVarSetFloat("gZeldaOnline.Party.BossHealthWeight", m_partySettings.bossHealthWeight);
                CVarSetInteger("gZeldaOnline.Party.StoryCoop", m_partySettings.storyCoop ? 1 : 0);
                CVarSetInteger("gZeldaOnline.Party.ShareAmmoRupeesHp", m_partySettings.shareAmmoRupeesHp ? 1 : 0);

                Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
            } break;

            default:
                break;
        }
    });
    /*
    ZeldaOnlineRoomWindow::Instance->SetAvailableSkins({
        { "Link", "link" },
        { "Amara64 by Jameriquiah", "amara64" },
        { "Christmas Malon by MalonRose", "christmalon" },
        { "Linkle by DanatheElf", "linkle" },
        { "Kris by BanzMarten", "kris" },
        { "Malon by MalonRose", "malon1" },
        { "Mario by cy888", "mario" },
        { "Turezi by Emkay", "turezi" },
        { "Malon (alt) by ascendantlight", "malon2" },
        { "Zelda by BungleTavern", "zelda" },
    });*/

    ZeldaOnlineRoomWindow::Instance->SetCurrentSkin(m_skinRef);

    if (CVarGetInteger("gZeldaOnline.AutoConnect", 0) != 0)
    {
        ZeldaOnlineRoomWindow::Instance->BeginAutoConnect();
    }
}

std::string ZeldaOnlineClient::BuildPartySettingsJSON(const PartySettings& partySettings) {
    nlohmann::json settings;
    settings["privateDungeons"] = partySettings.privateDungeons;
    settings["extraEnemies"] = partySettings.extraEnemies;
    settings["extraEnemyWeight"] = partySettings.extraEnemyWeight;
    settings["healthMultiplier"] = partySettings.healthMultiplier;
    settings["enemyHealthWeight"] = partySettings.enemyHealthWeight;
    settings["bossHealthWeight"] = partySettings.bossHealthWeight;
    settings["storyCoop"] = partySettings.storyCoop;
    settings["shareAmmoRupeesHp"] = partySettings.shareAmmoRupeesHp;

    return settings.dump();
}

bool ZeldaOnlineClient::ParsePartySettingsJSON(const std::string& partySettings, PartySettings& out) {
    auto settings = nlohmann::json::parse(partySettings);

    if (!settings.is_object())
        return false;

    out.privateDungeons = settings.value("privateDungeons", false);
    out.extraEnemies = settings.value("extraEnemies", false);
    out.extraEnemyWeight = settings.value("extraEnemyWeight", 1.0f);
    out.healthMultiplier = settings.value("healthMultiplier", false);
    out.enemyHealthWeight = settings.value("enemyHealthWeight", 1.0f);
    out.bossHealthWeight = settings.value("bossHealthWeight", 1.0f);
    out.storyCoop = settings.value("storyCoop", false);
    out.shareAmmoRupeesHp = settings.value("shareAmmoRupeesHp", false);

    if (out.extraEnemyWeight < 0.0f)
        out.extraEnemyWeight = 0.0f;
    if (out.extraEnemyWeight > 5.0f)
        out.extraEnemyWeight = 5.0f;

    if (out.enemyHealthWeight < 0.0f)
        out.enemyHealthWeight = 0.0f;
    if (out.enemyHealthWeight > 5.0f)
        out.enemyHealthWeight = 5.0f;

    if (out.bossHealthWeight < 0.0f)
        out.bossHealthWeight = 0.0f;
    if (out.bossHealthWeight > 5.0f)
        out.bossHealthWeight = 5.0f;
    return true;

}

void ZeldaOnlineClient::SetPartySettings(const PartySettings& settings) {
    if (m_partySettings.privateDungeons != settings.privateDungeons) {
        if (gPlayState && IsPartyScene(gPlayState->sceneNum))
            ReloadSceneInPlace(gPlayState);
    }
    m_partySettings = settings;
    StoryRefreshState();
}

void ZeldaOnlineClient::LoadPartySettings() {
    m_partySettings.privateDungeons = CVarGetInteger("gZeldaOnline.Party.PrivateDungeons", 0) != 0;
    m_partySettings.extraEnemies = CVarGetInteger("gZeldaOnline.Party.ExtraEnemies", 0) != 0;
    m_partySettings.extraEnemyWeight = CVarGetFloat("gZeldaOnline.Party.ExtraEnemyWeight", 1.0f);
    m_partySettings.healthMultiplier = CVarGetInteger("gZeldaOnline.Party.HealthMultiplier", 0) != 0;
    m_partySettings.enemyHealthWeight = CVarGetFloat("gZeldaOnline.Party.EnemyHealthWeight", 1.0f);
    m_partySettings.bossHealthWeight = CVarGetFloat("gZeldaOnline.Party.BossHealthWeight", 1.0f);
    m_partySettings.storyCoop = CVarGetInteger("gZeldaOnline.Party.StoryCoop", 0) != 0;
    m_partySettings.shareAmmoRupeesHp = CVarGetInteger("gZeldaOnline.Party.ShareAmmoRupeesHp", 0) != 0;

    if (ZeldaOnlineRoomWindow::Instance != nullptr)
        ZeldaOnlineRoomWindow::Instance->GetPartySettings() = m_partySettings;
}

void ZeldaOnlineClient::OnConnected() {
    DbgPrintf("OnConnected()\n");
    m_sendFullPlayerProps = true;
    m_lastAppearanceBlob.Clear();
    m_lastPlayerProps.Clear();
    RegisterHooks(true);
    ActorControllerFactory::Instance().RegisterActorHooks(true);
    ZeldaOnlineRoomWindow::Instance->SetConnected(true);
    ZeldaOnlineRoomWindow::Instance->SetCurrentSkin(m_skinRef);
    recvBuffer.Clear();

    outgoingBuffer.Clear();
    m_myNetworkID = 0;
    WritePacket(newPacket(CLIENT_PACKET_AUTH) << CURRENT_VERSION << PackedUInt1(m_didDisconnect) << PackedUInt8(m_clientGUID));

    //We reconnected after a disconnect. Reload the scene
    if (m_didDisconnect)
        ReloadSceneInPlace(gPlayState);
    m_showConnectionStatusTimer = 5 * 20;

    
}

void ZeldaOnlineClient::OnDisconnected() {
    ZeldaOnlineRoomWindow::Instance->Reset();
    DbgPrintf("OnDisconnected()\n");
    m_didDisconnect = true;
    m_hasWorldTime = false;
    m_partyID = 0U;
    StoryRefreshState();
    printf("PLAYER DISCONNECTED\n");
    RegisterNetworkingHook(false);
    RegisterHooks(false);
    ActorControllerFactory::Instance().RegisterActorHooks(false);

    //If we disconnected then reconnect, unless we were kicked (due to wrong version for example)
    if (m_autoReconnect && CVarGetInteger("gZeldaOnline.Enabled", 0)) {
        Connect();
    }
}

void ZeldaOnlineClient::OnConnectionClosedBeforeConnect() {
    OnDisconnected();
}


extern "C" {
void ObjSwitch_FloorPressInit(ObjSwitch* objSwitch);
void ObjSwitch_FloorReleaseInit(ObjSwitch* objSwitch);
}
static void NudgeFloorSwitches(int flag, u8 pressed) {
    Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_SWITCH].head;
    for (; actor != NULL; actor = actor->next) {
        if (actor->id != ACTOR_OBJ_SWITCH)
            continue;
        ObjSwitch* sw = (ObjSwitch*)actor;
        s32 type = sw->dyna.actor.params & 7;
        if (type != OBJSWITCH_TYPE_FLOOR && type != OBJSWITCH_TYPE_FLOOR_RUSTY)
            continue;
        if (((sw->dyna.actor.params >> 8) & 0x3F) != flag)
            continue;
        if (pressed && sw->dyna.actor.scale.y >= (33.0f / 200.0f))
            ObjSwitch_FloorPressInit(sw);
        else if (!pressed && sw->dyna.actor.scale.y <= (33.0f / 2000.0f))
            ObjSwitch_FloorReleaseInit(sw);
    }
}

// Anchor put puppet players into a different category. This caused problems when holding objects (the object would lag
// behind) We are going to correctly put them in the player category but fix the ordering so the local player is always
// at head
static void RelinkPuppetBehindPlayer(Actor* actor) {
    ActorListEntry* list = &gPlayState->actorCtx.actorLists[ACTORCAT_PLAYER];
    if (list->head == actor && actor->next != NULL) {
        Actor* realPlayer = actor->next;

        list->head = realPlayer;
        realPlayer->prev = NULL;

        actor->next = realPlayer->next;
        actor->prev = realPlayer;

        if (realPlayer->next != NULL) {
            realPlayer->next->prev = actor;
        }
        realPlayer->next = actor;
    }
}

void ZeldaOnlineClient::OnIncomingPacket(ByteStream& packet) {
    auto serverPacketID = packet.Read<PackedUInt1>().value();

    switch (serverPacketID) {

        case SERVER_PACKET_DISCONNECT: {
            auto reason = packet.ReadString();
            m_autoReconnect = false;
            Messagebox_ShowErrorBox("Disconnected",
                                    ("You have been disconnected from the server. Reason:" + reason).c_str());
        } break;


        // new code
        case SERVER_PACKET_ACTOR_PROPERTIES: {
            if (packet.BytesLeft() < 2)
                break;

            int networkID = (int)(packet.Read<PackedUInt2>().value());

            auto it = m_networkedActors.find(networkID);
            if (it == m_networkedActors.end())
                break;

            AbstractActorController* controller = it->second;
            if (controller->IsLeader())
                return;
            ByteStream props = packet.Read(packet.BytesLeft());
            if (controller->GetActor()) {
                controller->AppendPendingProperties(props);

                //Paused, just apply the properties now
                if (gPlayState->pauseCtx.state != 0)
                    controller->ApplyPendingProperties();
            }

        } break;

        case SERVER_PACKET_ACTOR_SPAWN:
        case SERVER_PACKET_ACTOR_SPAWN_AS_CHILD: {

            int parentID =
                serverPacketID == SERVER_PACKET_ACTOR_SPAWN_AS_CHILD ? (s16)(packet.Read<PackedUInt2>().value()) : 0;

            auto sceneKey = (int)(packet.Read<PackedUInt4>().value());
            int roomIndex = (int)(packet.Read<PackedInt1>().value());
            int networkID = (int)(packet.Read<PackedUInt2>().value());
            s16 actorId = (s16)(packet.Read<PackedUInt2>().value());

            printf("SERVER_PACKET_SPAWN_ACTOR ACTOR: %i(%i) PARENT ID: %i\n", int(actorId), networkID, parentID);
            s16 params = (s16)(packet.Read<PackedInt2>().value());

            PosRot posRot = { { packet.Read<PackedFloat4>().value(), packet.Read<PackedFloat4>().value(),
                                packet.Read<PackedFloat4>().value() },
                              { (s16)(packet.Read<PackedInt2>().value()), (s16)(packet.Read<PackedInt2>().value()),
                                (s16)(packet.Read<PackedInt2>().value()) } };

            PosRot home = { { packet.Read<PackedFloat4>().value(), packet.Read<PackedFloat4>().value(),
                              packet.Read<PackedFloat4>().value() },
                            { (s16)(packet.Read<PackedInt2>().value()), (s16)(packet.Read<PackedInt2>().value()),
                              (s16)(packet.Read<PackedInt2>().value())

                            } };

            if (gPlayState == NULL) {
                break;
            }

            auto existing = m_networkedActors.find(networkID);
            if (existing != m_networkedActors.end()) {
                SPDLOG_INFO("[ZeldaOnline] re-introduction of netID {}: replacing predecessor", networkID);
                DetachAndKill(existing->second);
            }

            auto localKey =
                MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));

            int leaderID = packet.Read<PackedUInt2>().value();

            int isActorLeader = leaderID == m_myNetworkID;

            if (sceneKey != localKey) {
                printf("MISMATCH SCENE KEY %i:%i\n", localKey, sceneKey);
                break;
            }

            if (actorId == ACTOR_PLAYER) {


                auto appearanceLen = packet.ReadVarUInt();
                ByteStream appearance = packet.Read(appearanceLen);
                auto propertiesLen = packet.BytesLeft();
                ByteStream properties = packet.Read(propertiesLen);

                Actor* actor =
                    Actor_SpawnDirect(&gPlayState->actorCtx, gPlayState, ACTOR_PLAYER, posRot.pos.x, posRot.pos.y,
                                      posRot.pos.z, posRot.rot.x, posRot.rot.y, posRot.rot.z, home.pos.x, home.pos.y,
                                      home.pos.z, home.rot.x, home.rot.y, home.rot.z, 0, 1);

                if (actor == NULL || actor->update == NULL) {
                    break;
                }
                InitPuppetPlayer(actor);
                 
                actor->world = posRot;
                Math_Vec3f_Copy(&actor->prevPos, &actor->world.pos);
                actor->room = -1;

                auto controller = new PlayerPuppetController(actor, networkID, sceneKey, appearance);
                m_networkedActors[networkID] = controller;
                controller->AppendPendingProperties(properties);
                break;
            }

            if (sceneKey != localKey || (roomIndex != -1 && roomIndex != gPlayState->roomCtx.curRoom.num)) {
                break;
            }

            const bool movedFromHome =
                posRot.pos.x != home.pos.x || posRot.pos.y != home.pos.y || posRot.pos.z != home.pos.z;
            Actor* actor = nullptr;

            //Our own clear flags should not interfere in this spawn
            auto oldClearFlags = gPlayState->actorCtx.flags.clear;
            gPlayState->actorCtx.flags.clear = 0U;
            if (parentID == 0) {
                printf("Actor_SpawnDirect\n");
                actor = Actor_SpawnDirect(&gPlayState->actorCtx, gPlayState, actorId, home.pos.x, home.pos.y,
                                          home.pos.z, home.rot.x, home.rot.y, home.rot.z, home.pos.x, home.pos.y,
                                          home.pos.z, home.rot.x, home.rot.y, home.rot.z, params, 1);
            }
            else {
                auto it = m_networkedActors.find(parentID);
                if (it != m_networkedActors.end()) {
                    auto parentActor = it->second->GetActor();

                    if (parentActor != nullptr) {
                        printf("Actor_SpawnAsChildDirect\n");
                        actor = Actor_SpawnAsChildDirect(&gPlayState->actorCtx, parentActor, gPlayState, actorId,
                                                         home.pos.x, home.pos.y, home.pos.z, home.rot.x, home.rot.y,
                                                         home.rot.z, home.pos.x, home.pos.y, home.pos.z, home.rot.x,
                                                         home.rot.y, home.rot.z, params, 1);
                    } else {
                        SPDLOG_WARN("[ZeldaOnline] parent {} missing for child spawn, degrading", parentID);
                        printf("Actor_SpawnDirect\n");
                        actor =
                            Actor_SpawnDirect(&gPlayState->actorCtx, gPlayState, actorId, home.pos.x, home.pos.y,
                                              home.pos.z, home.rot.x, home.rot.y, home.rot.z, home.pos.x, home.pos.y,
                                              home.pos.z, home.rot.x, home.rot.y, home.rot.z, params, 1);
                    }
                }
            }
            //Restore old flags
            gPlayState->actorCtx.flags.clear = oldClearFlags;

            if (actor == NULL || actor->update == NULL) {
                break;
            }
            actor->room = roomIndex;

            AbstractActorController* controller = ActorControllerFactory::Instance().Create(
                actorId, actor, networkID, sceneKey, roomIndex, isActorLeader);

            if (controller == nullptr) {
                SPDLOG_ERROR("[ZeldaOnline] no controller registered for actor {:#06x}", actor->id);
                break;
            }
            controller->SetSpawnPosRot(posRot);

            auto isLocked = packet.Read<PackedUInt1>().value();
            auto isCreator = packet.Read<PackedUInt1>().value();
            printf("IS CREATOR: %i\n", int(isCreator));
            controller->SetCreator(isCreator);
            ByteStream statePacket = packet.Read(packet.BytesLeft());

            controller->AppendPendingProperties(statePacket);
            controller->SetLocked(isLocked);

            m_networkedActors[networkID] = controller;

            if (!GameInteractor_ShouldActorDelayInit(actor))
            {
                if (Object_IsLoaded(&gPlayState->objectCtx, actor->objBankIndex)) {
                    Actor_SetObjectDependency(gPlayState, actor);

                    if (GameInteractor_ShouldActorInit(actor)) {
                        actor->init(actor, gPlayState);
                        actor->init = NULL;

                        GameInteractor_ExecuteOnActorInit(actor);
                    } else {
                        actor->init = NULL;
                        Actor_Kill(actor);
                    }
                }
            }

        } break; 

        case SERVER_PACKET_UPDATE_APPEARANCE: {
            if (packet.BytesLeft() < 2)
                break;
            int networkID = (int)(packet.Read<PackedUInt2>().value());
            auto it = m_networkedActors.find(networkID);
            if (it == m_networkedActors.end())
                break;
            if (!it->second->IsPlayer())
                break;
            static_cast<PlayerPuppetController*>(it->second)->ApplyAppearance(packet.Read(packet.BytesLeft()));
        } break;

        case SERVER_PACKET_AUTH_RESPONSE: {
            if (packet.BytesLeft() < 2)
                break;
            m_myNetworkID = (int)(packet.Read<PackedUInt2>().value());

            UpdateAppearance();
            printf("MY NETWORK ID SET TO %i\n", int(m_myNetworkID));

            WritePacket(newPacket(CLIENT_PACKET_INIT_PLAYER_LIST));
        } break;

        case SERVER_PACKET_DESTROY_ACTOR: {
            if (packet.BytesLeft() < 2)
                break;
            int networkID = (int)(packet.Read<PackedUInt2>().value());

            auto it = m_networkedActors.find(networkID);
            if (it == m_networkedActors.end())
                break;

            AbstractActorController* controller = it->second;

            if (controller->IsRunningLocally()) {
                DetachAndKill(controller);
                break;
            }

            controller->ApplyPendingProperties();
            controller->OnServerDestroy();
            DetachAndKill(controller);
        } break;

                case SERVER_PACKET_ACTOR_TRIGGER: {
            if (packet.BytesLeft() < 3)
                break;

            int networkID = (int)(packet.Read<PackedUInt2>().value());
            bool fromPuppet = packet.Read<PackedUInt1>().value() == 1;

            auto it = m_networkedActors.find(networkID);
            if (it == m_networkedActors.end())
                break;

            AbstractActorController* controller = it->second;

            if (controller == nullptr)
                break;

            if (fromPuppet && !controller->IsLeader())
                break;

            else if (!fromPuppet && controller->IsLeader())
                break;

            unsigned int nameLen = packet.Read<PackedUInt1>().value();
            if (packet.BytesLeft() < nameLen)
                break;
            std::string name = packet.ReadString(nameLen);

            controller->ApplyPendingProperties();
            controller->OnTrigger(name, packet);
        } break;

        case SERVER_PACKET_SET_ACTOR_LEADER: {
            if (packet.BytesLeft() < 4)
                break;
            int networkID = (int)(packet.Read<PackedUInt2>().value());
            int ownerID = (int)(packet.Read<PackedUInt2>().value());
            auto it = m_networkedActors.find(networkID);
            if (it == m_networkedActors.end()) {
                printf("SERVER_PACKET_SET_ACTOR_LEADER: Invalid network ID: %i:%i\n", networkID, int(ownerID == m_myNetworkID));
                if (ownerID == m_myNetworkID) {
                    SendDeclineActorLeader(networkID);
                }

                break;
            }

            auto controller = it->second;

            if (ownerID == m_myNetworkID && !controller->CanAcceptLeadership()) {
                SendDeclineActorLeader(networkID);
                break;
            }

            controller->ApplyPendingProperties();
            controller->SetLeader(ownerID != 0 && ownerID == m_myNetworkID);
        } break;

        case SERVER_PACKET_SCENE_FLAG: {
            if (packet.BytesLeft() < 2)
                break;

            u8 flagType = packet.Read<PackedUInt1>().value();
            u16 flag = packet.Read<PackedUInt2>().value();
            if (gPlayState == NULL)
                break;

            u8 setFlag = 1;
            if (packet.BytesLeft() >= 1)
                setFlag = packet.Read<PackedUInt1>().value();

            printf("SCENE FLAG PACKET: %i:%i\n", int(flagType), int(setFlag));

           
            if (setFlag) {
                if (flagType == FLAG_SCENE_SWITCH || flagType == FLAG_SCENE_CLEAR) {
                    GameInteractionEffect::SetSceneFlag effect;
                    effect.parameters[0] = gPlayState->sceneNum;
                    effect.parameters[1] = flagType;
                    effect.parameters[2] = flag;
                    effect.Apply();

                    if (flagType == FLAG_SCENE_SWITCH)
                        NudgeFloorSwitches(flag, setFlag);

                }

                else if (flagType == FLAG_INF_TABLE || flagType == FLAG_EVENT_CHECK_INF) {
                    GameInteractionEffect::SetFlag effect;
                    effect.parameters[0] = flagType;
                    effect.parameters[1] = flag;
                    effect.Apply();
                }

            } else {
                if (flagType == FLAG_SCENE_SWITCH || flagType == FLAG_SCENE_CLEAR) {
                    GameInteractionEffect::UnsetSceneFlag effect;
                    effect.parameters[0] = gPlayState->sceneNum;
                    effect.parameters[1] = flagType;
                    effect.parameters[2] = flag;
                    effect.Apply();
                }

                else if (flagType == FLAG_INF_TABLE || flagType == FLAG_EVENT_CHECK_INF) {
                    GameInteractionEffect::UnsetFlag effect;
                    effect.parameters[0] = flagType;
                    effect.parameters[1] = flag;
                    effect.Apply();
                }
            }

        } break;

        case SERVER_PACKET_WORLD_TIME: {
            if (packet.BytesLeft() < 2)
                break;

            m_serverDayTime = (u16)(packet.Read<PackedUInt2>().value());

            u8 crossed = 0;
            if (packet.BytesLeft() >= 1)
                crossed = (u8)(packet.Read<PackedUInt1>().value());
            if (packet.BytesLeft() >= 2)
                m_serverTimeRate = (u16)(packet.Read<PackedUInt2>().value());

            if (gPlayState == nullptr || !ShouldFreezeTime(gPlayState->sceneNum, GetSceneVariant(gPlayState->sceneNum))) {
                s16 drift = (s16)(m_serverDayTime - gSaveContext.dayTime);
                if (!m_hasWorldTime || crossed || drift > 600 || drift < -600) {
                    gSaveContext.skyboxTime = gSaveContext.dayTime = m_serverDayTime;
                }
            }
            m_hasWorldTime = true;

        } break;

        case SERVER_PACKET_SCENE_TRIGGER: {
            if (packet.BytesLeft() < 6 || gPlayState == NULL) {
                break;
            }

            auto sceneKey = packet.Read<PackedUInt4>().value();
            auto localKey = MakeSceneKey(gPlayState->sceneNum, !LINK_IS_ADULT ? 0 : 1, GetSceneVariant(gPlayState->sceneNum));
            if (localKey != sceneKey) {
                break;
            }

            auto fromID = packet.Read<PackedUInt2>().value();

            auto fromPlayer = fromID != 0 ? GetNetworkController(fromID) : nullptr;

            u8 nameLen = packet.Read<PackedUInt1>().value();
            if (packet.BytesLeft() < nameLen) {
                break;
            }
            std::string name = packet.ReadString(nameLen);
            OnSceneTrigger(fromPlayer, name, packet);
        } break;

        case SERVER_PACKET_ROOM_TEMP_FLAGS: {
            if (packet.BytesLeft() < 9 || gPlayState == NULL) {
                break;
            }
            auto sceneKey = packet.Read<PackedUInt4>().value();
            s8 roomIndex = (packet.Read<PackedInt1>().value());
            u32 mask = packet.Read<PackedUInt4>().value();

            auto localKey =
                MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));
            if (localKey != sceneKey || roomIndex != (gPlayState->roomCtx.curRoom.num)) {
                break;
            }

            gPlayState->actorCtx.flags.tempSwch = (gPlayState->actorCtx.flags.tempSwch & 0x00FFFFFF) | (mask & 0xFF000000);
        } break;

        case SERVER_PACKET_SET_ACTOR_LOCKED: {
            if (packet.BytesLeft() < 3)
                break;
            int networkID = (int)(packet.Read<PackedUInt2>().value());
            bool locked = packet.Read<PackedUInt1>().value() != 0;
            auto it = m_networkedActors.find(networkID);
            if (it != m_networkedActors.end())
                it->second->SetLocked(locked);
        } break;

        case SERVER_PACKET_ASSIGN_ACTOR_ID: {
            auto sceneKey = packet.Read<PackedUInt4>().value();
            s8 roomIndex = (packet.Read<PackedInt1>().value());
            unsigned int localID = packet.Read<PackedUInt4>().value();
            int networkID = (int)(packet.Read<PackedUInt2>().value());

            if (gPlayState == NULL) {
                break;
            }
            if (localID == 0)
                break;

            Actor* match = nullptr;
            for (int cat = 0; cat < ACTORCAT_MAX && match == nullptr; cat++) {
                for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != NULL; a = a->next) {
                    if (a->zoLocalId == localID) {
                        match = a;
                        break;
                    }
                }
            }

            if (match == nullptr || match->update == nullptr) {
                SPDLOG_DEBUG("[ZeldaOnline] ASSIGN_ACTOR_ID: no local actor for localID {}", localID);
                break;
            }

            if (networkID == 0) {
                DetachAndKill(static_cast<AbstractActorController*>(match->zoController));
                break;
            }

            if (match->zoController == nullptr) {
                AbstractActorController* controller = ActorControllerFactory::Instance().Create(
                    match->id, match, networkID, (int)(sceneKey), gPlayState->roomCtx.curRoom.num, true);
                if (controller) {
                    controller->SetCreator(true);

                    auto existing = m_networkedActors.find(networkID);
                    if (existing != m_networkedActors.end()) {
                        SPDLOG_INFO("[ZeldaOnline] re-introduction of netID {}: replacing predecessor", networkID);
                        DetachAndKill(existing->second);
                    }

                    m_networkedActors[networkID] = controller;
                }
                break;
            }

            auto existing = m_networkedActors.find(networkID);
            if (existing != m_networkedActors.end()) {
                SPDLOG_INFO("[ZeldaOnline] re-introduction of netID {}: replacing predecessor", networkID);
                DetachAndKill(existing->second);
            }


            AbstractActorController* controller = static_cast<AbstractActorController*>(match->zoController);
            controller->SetNetworkID(networkID);

            m_networkedActors[networkID] = controller;


        } break;

        case SERVER_PACKET_ACTOR_SOUND: {
            int networkID = (int)(packet.Read<PackedUInt2>().value());
            u16 sfxId = (u16)(packet.Read<PackedUInt2>().value());

            if (gPlayState == nullptr) {
                break;
            }
            AbstractActorController* ctl = GetNetworkController(networkID);

            if (ctl && ctl->GetActor()) {

                if (ctl->IsPlayer()) {
                    static int sFrequenciesIndex = 0;
                    static float sFrequencies[10];


                    auto playerController = static_cast<PlayerPuppetController*>(ctl);
                    sFrequencies[sFrequenciesIndex] = playerController->SoundFrequencyMultiplier();

                    Audio_PlaySoundGeneral(sfxId, &ctl->GetActor()->projectedPos, 4, &sFrequencies[sFrequenciesIndex],
                                           &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
                    sFrequenciesIndex = (sFrequenciesIndex + 1) % 10;

                }
                else Audio_PlaySoundGeneral(sfxId, &ctl->GetActor()->projectedPos, 4, &gSfxDefaultFreqAndVolScale, &gSfxDefaultFreqAndVolScale, &gSfxDefaultReverb);
            }
            break;
        }

        // Anchor
        case SERVER_PACKET_OCARINA_SFX: {
            int networkID = packet.Read<PackedUInt2>().value();
            u8 note = packet.Read<PackedUInt1>().value();
            f32 modulator = packet.Read<PackedFloat4>().value();
            s8 bend = (s8)(packet.Read<PackedInt1>().value());

            AbstractActorController* ctl = GetNetworkController(networkID);
            if (ctl != nullptr && ctl->IsPlayer() && ctl->GetActor()) {
                PlayerPuppetController* puppet = static_cast<PlayerPuppetController*>(ctl);
                puppet->m_ocarinaModulator = modulator;
                puppet->m_ocarinaBend = bend;

                if (note != 0xFF && puppet->m_ocarinaNote != note) {
                    Audio_QueueCmdS8(0x6 << 24 | SEQ_PLAYER_SFX << 16 | 0xD07, puppet->m_ocarinaBend - 1);
                    Audio_QueueCmdS8(0x6 << 24 | SEQ_PLAYER_SFX << 16 | 0xD05, note);

                    Audio_PlaySoundGeneral(NA_SE_OC_OCARINA, &ctl->GetActor()->projectedPos, 4,
                                           &puppet->m_ocarinaModulator, &D_80130F28, &gSfxDefaultReverb);
                } else if (puppet->m_ocarinaNote != 0xFF && note == 0xFF) {
                    Audio_StopSfxById(NA_SE_OC_OCARINA);
                }
                puppet->m_ocarinaNote = note;
            }
            break;
        }

        case SERVER_PACKET_SCENE_FLAGS: {
            if (gPlayState == nullptr)
                break;

            auto sceneKey = packet.Read<PackedUInt4>().value();
            auto localKey =
                MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));
            m_blockSceneSetupActors = -1;
            printf("UNBLOCKING SETUP ACTORS\n");

            if (sceneKey == localKey) {
                
                auto sceneFlags = packet.Read<PackedUInt4>().value();
                auto clearFlags = packet.Read<PackedUInt4>().value();
                auto tempSceneFlags = packet.Read<PackedUInt4>().value();
                auto doorMask = packet.Read<PackedUInt4>().value();
                auto sessionID = packet.Read<PackedUInt8>().value();
                auto keyCount = packet.Read<PackedUInt1>().value();

                if (IsDungeonScene(gPlayState->sceneNum) || IsBossScene(gPlayState->sceneNum)) {
                    //Dungeon has reset since our last visit (or this is a newone)
                    if (sessionID != m_dungeonSessions[sceneKey]) {
                        //Clear our chests, locked doors and keys
                        gPlayState->actorCtx.flags.chest = m_savedChests[gPlayState->sceneNum];
                        m_savedSwch = 0U;   
                        gSaveContext.inventory.dungeonKeys[gSaveContext.mapIndex] = keyCount;
                        m_dungeonSessions[sceneKey] = sessionID;

                        static constexpr s32 shadowTempleJarKeyFlag = 1;
                        // Reset the key in the jar in shadow temp
                        if (gPlayState->sceneNum == SCENE_SHADOW_TEMPLE) {
                            gPlayState->actorCtx.flags.collect &= ~(1 << (shadowTempleJarKeyFlag - 1));
                        }
                    }

                    //Adopt the servers scene flags (ignore locked doors flags)
                    gPlayState->actorCtx.flags.swch = sceneFlags | (m_savedSwch & doorMask);
                    gPlayState->actorCtx.flags.clear = clearFlags;
                    StoryApplyDungeonFlags(gPlayState);
                    gPlayState->actorCtx.flags.tempSwch = (gPlayState->actorCtx.flags.tempSwch & 0xFF000000) | (tempSceneFlags & 0x00FFFFFF);
                } else {
                    //Not a dungeon, just OR the flags
                    gPlayState->actorCtx.flags.swch |= sceneFlags;
                    gPlayState->actorCtx.flags.clear |= clearFlags;
                    gPlayState->actorCtx.flags.tempSwch |= (tempSceneFlags & 0x00FFFFFF);
                }
                
            }
            break;
        }

        case SERVER_PACKET_INF_FLAGS_LIST: {
            auto sceneKey = packet.Read<PackedUInt4>().value();
            auto localKey =
                MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));

            if (sceneKey == localKey) {

                auto count = packet.ReadVarUInt();

                for (auto i = 0U; i < count; ++i) {
                    auto flagType = packet.Read<PackedUInt1>().value();
                    auto flag = packet.Read<PackedUInt2>().value();
                    auto value = packet.Read<PackedUInt1>().value();

                    if (value) {
                        GameInteractionEffect::SetFlag effect;
                        effect.parameters[0] = flagType;
                        effect.parameters[1] = flag;
                        effect.Apply();
                    } else {
                        GameInteractionEffect::UnsetFlag effect;
                        effect.parameters[0] = flagType;
                        effect.parameters[1] = flag;
                        effect.Apply();
                    }
                }
            }
            break;
        }

        case SERVER_PACKET_PLAYER_LIST_APPEND:
        {
            if (packet.BytesLeft() < 1)
                break;

            auto* window = ZeldaOnlineRoomWindow::Instance;
            int count = packet.Read<PackedInt4>().value();

            for (int i = 0; i < count; i++) {
                if (packet.BytesLeft() < 3)
                    break;

                PlayerEntry entry;
                entry.networkID = (uint32_t)(packet.Read<PackedUInt2>().value());

                unsigned int nameLen = packet.Read<PackedUInt2>().value();
                if (packet.BytesLeft() < nameLen)
                    break;

                entry.name = packet.ReadString(nameLen);
                entry.sceneNum = packet.Read<PackedInt2>().value();
                entry.roomIndex = packet.Read<PackedUInt1>().value();
                entry.age = packet.Read<PackedUInt1>().value();

                if (entry.name.empty())
                    entry.name = "Player " + std::to_string(entry.networkID);

                if (window != nullptr)
                    window->AddPlayer(entry);

            }
        } break;

        case SERVER_PACKET_PLAYER_LIST_REMOVE: {
            if (packet.BytesLeft() < 2)
                break;

            uint32_t networkID = (uint32_t)(packet.Read<PackedUInt2>().value());

            if (ZeldaOnlineRoomWindow::Instance != nullptr)
                ZeldaOnlineRoomWindow::Instance->RemovePlayer(networkID);

            
        } break;

        case SERVER_PACKET_PLAYER_LIST_UPDATE: {
            uint32_t networkID = (uint32_t)(packet.Read<PackedUInt2>().value());
            auto newName = packet.ReadString(packet.Read<PackedUInt2>().value());

            auto sceneNum = packet.Read<PackedInt2>().value();
            auto roomIndex = packet.Read<PackedUInt1>().value();
            uint8_t age = packet.Read<PackedUInt1>().value();
            if (ZeldaOnlineRoomWindow::Instance != nullptr)
                ZeldaOnlineRoomWindow::Instance->UpdateEntry(networkID, newName, sceneNum, roomIndex, age);
        } break;

        case SERVER_PACKET_PARTY_JOIN: {
            if (packet.BytesLeft() < 5)
                break;

            uint32_t previousPartyID = m_partyID;

            m_partyID = (uint32_t)(packet.Read<PackedUInt4>().value());
            auto partyAdmin = packet.Read<PackedUInt2>().value();

            std::string partySettingsJSON = packet.ReadString(packet.BytesLeft());

            bool isAdmin = partyAdmin == m_myNetworkID;

            if (m_partyID == previousPartyID)
                break;

            if (ZeldaOnlineRoomWindow::Instance != nullptr) {
                ZeldaOnlineRoomWindow::Instance->SetHasParty(m_partyID != 0);
                ZeldaOnlineRoomWindow::Instance->ClearPendingInvites();

                if (m_partyID == 0 || isAdmin)
                    ZeldaOnlineRoomWindow::Instance->ClearPartyFlags();

                ZeldaOnlineRoomWindow::Instance->SetPartyAdmin(isAdmin);
                ZeldaOnlineRoomWindow::Instance->SetPartyAdminID(partyAdmin);
                m_partySize = ZeldaOnlineRoomWindow::Instance->GetPartySize();

                if (partySettingsJSON.length() > 0) {
                    PartySettings& partySettings = ZeldaOnlineRoomWindow::Instance->GetPartySettings();

                    if (ParsePartySettingsJSON(partySettingsJSON, partySettings))
                        SetPartySettings(partySettings);
                }
            } else
                m_partySize = 1;

            StoryRefreshState();

            if (gPlayState && (IsDungeonScene(gPlayState->sceneNum) || IsBossScene(gPlayState->sceneNum)))
                ReloadSceneInPlace(gPlayState);

            
        } break;

        case SERVER_PACKET_PARTY_INVITE: {
            if (packet.BytesLeft() < 6)
                break;

            uint32_t fromNetworkID = (uint32_t)(packet.Read<PackedUInt2>().value());
            uint32_t partyID = (uint32_t)(packet.Read<PackedUInt4>().value());

            if (partyID == 0 || partyID == m_partyID)
                break;

            if (ZeldaOnlineRoomWindow::Instance != nullptr)
                ZeldaOnlineRoomWindow::Instance->SetPendingPartyInvite(fromNetworkID, partyID);
        } break;

        case SERVER_PACKET_PARTY_MEMBER_ADD: {
            if (packet.BytesLeft() < 2)
                break;

            uint32_t memberNetworkID = (uint32_t)(packet.Read<PackedUInt2>().value());
            

            if (ZeldaOnlineRoomWindow::Instance != nullptr)
                ZeldaOnlineRoomWindow::Instance->SetInParty(memberNetworkID, true);

            m_partySize = ZeldaOnlineRoomWindow::Instance->GetPartySize();
        } break;

        case SERVER_PACKET_PARTY_MEMBER_REMOVE: {
            if (packet.BytesLeft() < 2)
                break;

            uint32_t memberNetworkID = (uint32_t)(packet.Read<PackedUInt2>().value());
            auto partyAdmin = packet.Read<PackedUInt2>().value(); 
            bool isAdmin = partyAdmin == m_myNetworkID;

            if (ZeldaOnlineRoomWindow::Instance != nullptr) {
                ZeldaOnlineRoomWindow::Instance->SetInParty(memberNetworkID, false);
                ZeldaOnlineRoomWindow::Instance->SetPartyAdmin(isAdmin);
                ZeldaOnlineRoomWindow::Instance->SetPartyAdminID(partyAdmin);
                m_partySize = ZeldaOnlineRoomWindow::Instance->GetPartySize();
            } else
                m_partySize = 1;

            
        } break;


        case SERVER_PACKET_TELEPORT_PLAYER: {
            auto entranceID = packet.Read<PackedInt4>().value();
            auto sceneNum = packet.Read<PackedUInt2>().value();
            auto roomIndex = packet.Read<PackedUInt1>().value();

            auto x = packet.Read<PackedFloat4>().value();
            auto y = packet.Read<PackedFloat4>().value();
            auto z = packet.Read<PackedFloat4>().value();
            
            if (!gPlayState)
                break;

            Player* player = GET_PLAYER(gPlayState);
            if (player == NULL)
                return;

            if ((player->stateFlags1 & PLAYER_STATE1_ON_HORSE) && player->rideActor != NULL) {
                gSaveContext.horseData.scene = gPlayState->sceneNum;
                gSaveContext.horseData.pos.x = (int16_t)player->rideActor->world.pos.x;
                gSaveContext.horseData.pos.y = (int16_t)player->rideActor->world.pos.y;
                gSaveContext.horseData.pos.z = (int16_t)player->rideActor->world.pos.z;
                gSaveContext.horseData.angle = player->rideActor->world.rot.y;
            }

            gSaveContext.respawnFlag = 1;
            gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = gPlayState->nextEntranceIndex = entranceID;
            gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex = roomIndex;
            gSaveContext.respawn[RESPAWN_MODE_DOWN].pos = Vec3f_{x, y, z};
            gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw = player->actor.shape.rot.y;
            gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0x0DFF;
            /*
            if (gPlayState->roomCtx.curRoom.behaviorType2 < 4) {
                gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0x0DFF;
            } else {
                Camera* camera = GET_ACTIVE_CAM(gPlayState);
                gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0x0D00 | camera->camDataIdx;
            }*/
            gPlayState->transitionTrigger = TRANS_TRIGGER_START;
            gPlayState->transitionType = TRANS_TYPE_INSTANT;
            gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;

            static int hookId = 0;
            hookId = REGISTER_VB_SHOULD(VB_INFLICT_VOID_DAMAGE, {
                *should = false;
                GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::OnVanillaBehavior>(hookId);
            });
            m_blockSceneSetupActors = sceneNum;
            break;


        } break;

        case SERVER_PACKET_SET_PLAYERLIST_STATUS:
        {
            auto red = packet.Read<PackedUInt1>().value();
            auto green = packet.Read<PackedUInt1>().value();
            auto blue = packet.Read<PackedUInt1>().value();
            auto text = packet.ReadString();

            ZeldaOnlineRoomWindow::Instance->SetStatus(text, ImVec4{red / 255.0f, green / 255.0f, blue / 255.0f, 1.0f});
            m_clearPlayerListStatusTimer = 5 * 20;

        }
        break;

        case SERVER_PACKET_ACTOR_STATIC_SPAWN:
        {
            if (gPlayState == nullptr)
                break;

            auto sceneKey = packet.Read<PackedUInt4>().value();
            auto localKey = MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));
            auto roomIndex = packet.Read<PackedInt1>().value();

            if (sceneKey == localKey && roomIndex == gPlayState->roomCtx.curRoom.num) {
                auto actorID = (int16_t)packet.Read<PackedUInt2>().value();
                auto x = packet.Read<PackedFloat4>().value();
                auto y = packet.Read<PackedFloat4>().value();
                auto z = packet.Read<PackedFloat4>().value();
                auto rotX = packet.Read<PackedInt2>().value();
                auto rotY = packet.Read<PackedInt2>().value();
                auto rotZ = packet.Read<PackedInt2>().value();
                int16_t params = packet.Read<PackedInt2>().value();

                Actor_SpawnDirect(&gPlayState->actorCtx, gPlayState, actorID, x, y, z, rotX, rotY, rotZ, x, y, z, rotX, rotY, rotZ, params, false);
            }
            
            break;
        }

        case SERVER_PACKET_POPULATE_SKINS: {
            if (packet.BytesLeft() < 2)
                break;

            unsigned int count = packet.Read<PackedUInt2>().value();
            std::vector<SkinOption> skins;

            for (unsigned int i = 0; i < count; i++) {
                if (packet.BytesLeft() < 2) {
                    break;
                }

                unsigned int displayLen = packet.Read<PackedUInt2>().value();
                std::string displayName = packet.ReadString(displayLen);


                unsigned int referenceLen = packet.Read<PackedUInt2>().value();
                std::string reference = packet.ReadString(referenceLen);

                float pitch = packet.Read<PackedFloat4>().value();
                skins.push_back(SkinOption{ displayName, reference, pitch });
            }

            if (ZeldaOnlineRoomWindow::Instance != nullptr)
                ZeldaOnlineRoomWindow::Instance->SetAvailableSkins(skins);
        } break;

        //Anchor
        case SERVER_PACKET_CHEST_OPENED: {
            u16 networkID = (u16)(packet.Read<PackedUInt2>().value());
            u32 sceneNum = packet.Read<PackedUInt4>().value();
            s8 roomIndex = (s8)(packet.Read<PackedInt1>().value());
            u16 mapIndex = packet.Read<PackedUInt2>().value();
            u8 flag = packet.Read<PackedUInt1>().value();
            u16 modId = (u16)(packet.Read<PackedUInt2>().value());
            u16 getItemId = (u16)(packet.Read<PackedUInt2>().value());

            if (gPlayState == NULL)
                break;

            GetItemEntry getItemEntry;
            if (modId == MOD_NONE) {
                getItemEntry = ItemTableManager::Instance->RetrieveItemEntry(MOD_NONE, getItemId);
            } else {
                getItemEntry = Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(getItemId)).GetGIEntry_Copy();
            }

            if (getItemEntry.getItemId == GI_NONE)
                break;

            bool isTreasureSet = false;
            if (sceneNum == gPlayState->sceneNum)
                isTreasureSet = gPlayState->actorCtx.flags.chest & (1 << flag);
            else
                isTreasureSet = gSaveContext.sceneFlags[sceneNum].chest & (1 << flag);

            if (!isTreasureSet)
            {
                // Story Co-op shares the item through the progress snapshot, so consumables in a chest stay with the
                // opener and shared items are not applied twice
                if (!StoryCoopActive()) {
                if (getItemEntry.modIndex == MOD_NONE) {
                    if (getItemEntry.getItemId == GI_SWORD_BGS) {
                        gSaveContext.bgsFlag = true;
                        gSaveContext.swordHealth = 8;
                    }

                    auto oldMapIndex = gSaveContext.mapIndex;
                    gSaveContext.mapIndex = mapIndex;
                    m_skipItemGive = true;
                    Item_Give(gPlayState, static_cast<u8>(getItemEntry.itemId));
                    m_skipItemGive = false;
                    gSaveContext.mapIndex = oldMapIndex;

                } else if (getItemEntry.modIndex == MOD_RANDOMIZER) {
                    if (getItemEntry.getItemId == RG_ICE_TRAP) {
                        gSaveContext.ship.pendingIceTrapCount++;
                    } else {
                        m_skipItemGive = true;
                        Randomizer_Item_Give(gPlayState, getItemEntry);
                        m_skipItemGive = false;
                    }
                }

                if (getItemEntry.gid == GID_HEART_CONTAINER || getItemEntry.gid == GID_HEART_PIECE) {
                    gSaveContext.healthAccumulator = 0x140;
                }

                s32 heartPieces = (s32)(gSaveContext.inventory.questItems & 0xF0000000) >> (QUEST_HEART_PIECE + 4);
                if (heartPieces >= 4) {
                    gSaveContext.inventory.questItems &= ~0xF0000000;
                    gSaveContext.inventory.questItems += (heartPieces % 4) << (QUEST_HEART_PIECE + 4);
                    gSaveContext.healthCapacity += 0x10 * (heartPieces / 4);
                    gSaveContext.health += 0x10 * (heartPieces / 4);
                }
                }

                GameInteractionEffect::SetSceneFlag effect;
                effect.parameters[0] = sceneNum;
                effect.parameters[1] = FLAG_SCENE_TREASURE;
                effect.parameters[2] = flag;
                effect.Apply();

                m_savedChests[sceneNum] |= (1U << flag);
            }

            if (getItemEntry.getItemCategory != ITEM_CATEGORY_JUNK) {
                std::string opener = ZeldaOnlineRoomWindow::Instance != nullptr
                                         ? ZeldaOnlineRoomWindow::Instance->NameOf(networkID)
                                         : std::string();

                if (getItemEntry.modIndex == MOD_NONE) {
                    Notification::Emit({
                        .itemIcon = GetTextureForItemId(getItemEntry.itemId),
                        .prefix = opener,
                        .message = "found",
                        .suffix = SohUtils::GetItemName(getItemEntry.itemId),
                    });
                } else if (getItemEntry.modIndex == MOD_RANDOMIZER) {
                    Notification::Emit({
                        .prefix = opener, .message = "found",
                        .suffix = Rando::StaticData::RetrieveItem((RandomizerGet)(getItemEntry.getItemId))
                                      .GetName()
                                      .GetEnglish()
                    });
                }
            }
        } break;


        case SERVER_PACKET_CHAT_MESSAGE: {
            u16 fromNetworkID = (u16)(packet.Read<PackedUInt2>().value());
            std::string text = packet.ReadString(packet.BytesLeft());

            if (text.empty())
                break;

            AbstractActorController* ctl = GetNetworkController(fromNetworkID);
            if (ctl == nullptr || !ctl->IsPlayer())
                break;

            static_cast<PlayerPuppetController*>(ctl)->SetChatText(text, { 255, 255, 0, 255 });

            break;
        }

        case SERVER_PACKET_UPDATE_PARTY_SETTINGS: {
            std::string jsonSettings = packet.ReadString(packet.BytesLeft());

            if (jsonSettings.empty())
                break;

            if (ZeldaOnlineRoomWindow::Instance == nullptr)
                break;

            try {


                PartySettings& partySettings = ZeldaOnlineRoomWindow::Instance->GetPartySettings();

                if (ParsePartySettingsJSON(jsonSettings, partySettings))
                    SetPartySettings(partySettings);

            } catch (const std::exception& e) { printf("ZeldaOnline: bad party settings (%s)\n", e.what()); }
        } break;

        // (a client packet id used to be handled here by mistake, its value 37 is now SERVER_PACKET_STORY_VOTE_STATUS)

        case SERVER_PACKET_UPDATE_DUNGEON_KEYS: {
            auto mapIndex = packet.Read<PackedUInt1>().value();
            auto count = packet.Read<PackedUInt1>().value();

            if (mapIndex < 19)
                gSaveContext.inventory.dungeonKeys[mapIndex] = count;
        } break;
        case SERVER_PACKET_STORY_STATE:
            OnStoryState(packet);
            break;

        case SERVER_PACKET_STORY_SHARE_ITEM:
            OnStoryShareItem(packet);
            break;

        case SERVER_PACKET_STORY_VOTE_STATUS:
            OnStoryVoteStatus(packet);
            break;

        case SERVER_PACKET_STORY_TRAVEL:
            OnStoryTravel(packet);
            break;

        case SERVER_PACKET_STORY_SET_ERA:
            OnStorySetEra(packet);
            break;

        default:
            SPDLOG_DEBUG("[ZeldaOnline] unhandled server packet id {}", serverPacketID);
            break;
    }
}

void ZeldaOnlineClient::RequestSkinDownload(const std::vector<std::string>& archiveNames, const std::string& skinName) {
    if (m_fileServerUrl.empty() || archiveNames.empty()) {
        return;
    }

    if (!ResourceDownloader::IsSafeFileName(skinName)) {
        SPDLOG_WARN("rejected skin download: bad skin name {}", skinName);
        return;
    }

    std::string base = m_fileServerUrl;
    if (base.back() != '/') {
        base += '/';
    }

    std::vector<std::string> urls;
    urls.reserve(archiveNames.size());

    for (const std::string& archiveName : archiveNames) {
        if (!ResourceDownloader::IsSafeFileName(archiveName)) {
            SPDLOG_WARN("rejected skin download: bad archive name {}", archiveName);
            return;
        }
        urls.push_back(base + archiveName);
    }

    m_downloader.Request(urls, archiveNames, skinName, DOWNLOAD_RESOURCE_TYPE_PLAYER_SKIN);
}

bool ZeldaOnlineClient::WritePacket(const ByteStream& data) {
    if (!isConnected) {
        return false;
    }
    if (data.Length() > MAX_FRAME_PAYLOAD) {
        SPDLOG_ERROR("[FramedNetwork] Packet payload too large: {} bytes", data.Length());
        return false;
    }

    outgoingBuffer.Reserve(FRAME_HEADER_SIZE + data.Length());
    outgoingBuffer << PackedUInt2(data.Length());
    outgoingBuffer.Write(data.Text(), data.Length());

    m_bytesSent += FRAME_HEADER_SIZE + data.Length();
    return true;
}

void ZeldaOnlineClient::SendSceneTrigger(const std::string& name, const ByteStream& payload) {

    auto sceneKey = MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));
    ByteStream packet = newPacket(CLIENT_PACKET_SCENE_TRIGGER);
    packet << PackedUInt4(sceneKey);
    packet << PackedUInt1(0);
    packet << PackedUInt1((unsigned int)(name.length())) << name << payload;
    WritePacket(packet);
}

void ZeldaOnlineClient::SendRoomTrigger(const std::string& name, const ByteStream& payload) {
    auto sceneKey = MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));
    ByteStream packet = newPacket(CLIENT_PACKET_SCENE_TRIGGER);
    packet << PackedUInt4(sceneKey);
    packet << PackedUInt1(1);
    packet << PackedUInt1((unsigned int)(name.length())) << name << payload;
    WritePacket(packet);
}

void ZeldaOnlineClient::SendHitTrigger(ActorID actorID, f32 posX, f32 posY, f32 posZ) {
    ByteStream payload;
    payload << PackedUInt2(actorID) << PackedInt2(int(posX)) << PackedInt2(int(posY)) << PackedInt2(int(posZ));
    SendSceneTrigger("hit", payload);
}

void ZeldaOnlineClient::ProcessOutgoingPackets() {
    if (outgoingBuffer.Length() == 0) {
        return;
    }
    ByteStream toSend = std::move(outgoingBuffer);
    outgoingBuffer.Clear();

    SendDataToRemote(toSend.Text(), (int)(toSend.Length()));
}



void ZeldaOnlineClient::OnIncomingData(const char* payload, int length) {
    recvBuffer.Write(payload, (unsigned int)(length));

    m_bytesReceived += static_cast<uint64_t>(length);

    ByteStream packet;
    while (NextPacket(packet)) {
        OnIncomingPacket(packet);
    }

    recvBuffer.Compact();
}

bool ZeldaOnlineClient::NextPacket(ByteStream& out) {
    if (recvBuffer.BytesLeft() < FRAME_HEADER_SIZE) {
        return false;
    }

    PackedUInt2 lenField;
    memcpy(&lenField, recvBuffer.Text() + recvBuffer.TellRead(), FRAME_HEADER_SIZE);
    unsigned int payloadLen = lenField.value();

    if (recvBuffer.BytesLeft() < FRAME_HEADER_SIZE + payloadLen) {
        return false;
    }

    recvBuffer.SeekRead(FRAME_HEADER_SIZE, ByteStream::ORIGIN_CUR);
    out = recvBuffer.Read(payloadLen);
    return true;
}



void ZeldaOnlineClient::SendPacket_PlayerUpdate() {
    if (!isConnected || gPlayState == NULL) {
        return;
    }

    if (gPlayState->actorCtx.actorLists[ACTORCAT_PLAYER].length > 0) {
        Player* player = GET_PLAYER(gPlayState);
        if (player == NULL) {
            return;
        }

        ByteStream frame;
        PlayerPuppetController::BuildLocalPlayerProperties(player, m_nickName, frame);

        unsigned int changedCount = 0;
        ByteStream delta =
            AbstractActorController::DiffProperties(m_lastPlayerProps, frame, m_sendFullPlayerProps, &changedCount);
        m_lastPlayerProps = frame;

        m_sendFullPlayerProps = false;

        if (delta.Length()) {
            WritePacket(newPacket(CLIENT_PACKET_UPDATE_PLAYER) << delta);
            ZNetworking::FlushSendBuffer();
        }
    }

    
}

void ZeldaOnlineClient::DrawOverlay(GraphicsContext* gfxCtx) {
    if (!isConnected)
        DrawScreenText(gfxCtx, "Disconnected", 5, 5, { 255, 64, 64, 255 }, 0.5f);
    else if (m_showConnectionStatusTimer > 0)
        DrawScreenText(gfxCtx, "Connected", 5, 5, { 0, 255, 0, 255 }, 0.5f);
    if (m_showConnectionStatusTimer > 0)
        --m_showConnectionStatusTimer;

    if (m_storyHold)
        DrawScreenText(gfxCtx, StoryHoldText().c_str(), 12, 170, { 255, 255, 255, 255 }, 0.45f);
}

void ZeldaOnlineClient::UpdateAppearance() {
    if (!isConnected || m_myNetworkID == 0) {
        return;
    }

    ByteStream blob;
    blob << PackedUInt1(LINK_IS_ADULT ? 0u : 1u);
    blob << PackedUInt1((uint8_t)(m_skinRef.length())) << m_skinRef;
    blob << PackedUInt1((uint8_t)m_nickName.length()) << m_nickName;

    if (blob.compare(m_lastAppearanceBlob) == 0)
        return;

    m_lastAppearanceBlob = blob;

    WritePacket(newPacket(CLIENT_PACKET_UPDATE_APPEARANCE) << blob);
}

void ZeldaOnlineClient::NetworkHook() 
{
    ZNetworking::Poll();

    if (--m_keepAliveTimer <= 0) {
        m_keepAliveTimer = 3 * 20;
        WritePacket(newPacket(CLIENT_PACKET_KEEP_ALIVE));
    }
    //Handle downloads
    for (const DownloadResult& r : m_downloader.TakeCompleted()) {

        //Load the new archive files
        for (const DownloadedFile& file : r.files) {
            if (!file.success) {
                SPDLOG_WARN("download failed: {} ({})", file.fileName, file.error);
                continue;
            }

            auto archive =
                Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->AddArchive(file.filePath);
            if (archive == nullptr) {
                continue;
            }

            for (const auto& entry : *archive->ListFiles()) {
                const std::string& rPath = entry.second;

                std::vector<std::string> raw = StringHelper::Split(rPath, ".");
                std::string ext = raw[raw.size() - 1];
                std::string nPath = rPath.substr(0, rPath.size() - (ext.size() + 1));
                std::replace(nPath.begin(), nPath.end(), '\\', '/');

                ExtensionCache[nPath] = { rPath, ext };
            }
        }

        if (r.type != DOWNLOAD_RESOURCE_TYPE_PLAYER_SKIN) {
            continue;
        }

        //Reload the skin
        PlayerSkin* reloaded = PlayerSkin_Reload(r.resourceName.c_str());

        if (gPlayState == nullptr) {
            continue;
        }

        //Reapply the skeleton to those who are using this skin
        for (Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_PLAYER].head; it != nullptr; it = it->next) {
            Player* p = (Player*)it;
            if (p->skin == reloaded) {
                Player_ReapplySkeleton(p, gPlayState);
            }
        }
    }
}

void ZeldaOnlineClient::RegisterNetworkingHook(bool enabled) {
    //ExecuteOnGameStateMainStart
    COND_HOOK(OnGameStateMainStart, enabled, [this] {
        this->NetworkHook();
        
    });

    COND_HOOK(OnDrawOverlay, enabled, [&](GraphicsContext* gfxCtx) { DrawOverlay(gfxCtx); });
}

void ZeldaOnlineClient::RegisterHooks(bool enabled) {
    if (enabled == m_hooksEnabled)
        return;

    m_hooksEnabled = enabled;
    COND_HOOK(OnPlayDrawEnd, enabled, [&]() { SendPacket_PlayerUpdate(); });



    COND_HOOK(OnTransitionEnd, enabled, [&](int16_t) {
        if (m_boundaryCueOnLoad != 0) {
            Sfx_PlaySfxCentered2(m_boundaryCueOnLoad == 1 ? NA_SE_EV_CHICKEN_CRY_M : NA_SE_EV_DOG_CRY_EVENING);
            m_boundaryCueOnLoad = 0;
        }
    });

    COND_HOOK(OnGameFrameUpdate, enabled, [&] { OnGameFrameUpdate(); });

    COND_HOOK(OnActorKill, enabled, [&](void* refActor) {
        Actor* actor = static_cast<Actor*>(refActor);

        OnActorKill(actor);
    });


    COND_HOOK(ShouldActorDelayInit, enabled, [&](void* refActor, bool* should) {
        Actor* actor = static_cast<Actor*>(refActor);

        if (actor->zoController) {
            auto controller = static_cast<AbstractActorController*>(actor->zoController);
            if (controller->ShouldDelayInit()) {
                *should = true;
            }
        }
    });



    COND_ID_HOOK(ShouldActorInit, ACTOR_EN_HORSE, enabled, [&](void* actorRef, bool*) {
        Actor* actor = (Actor*)actorRef;
        if (!(actor->params & ZO_HORSE_PUPPET)) {
            return;
        }
        actor->params &= ~ZO_HORSE_PUPPET;
        actor->init = HorsePuppet_Init;
        actor->update = HorsePuppet_Update;
        actor->destroy = HorsePuppet_Destroy;
    });

    COND_HOOK(OnDungeonKeyUsed, enabled, [&](uint16_t mapIndex) {
        WritePacket(newPacket(CLIENT_PACKET_UPDATE_DUNGEON_KEYS)
                    << PackedUInt1(gSaveContext.mapIndex)
                    << PackedUInt1(gSaveContext.inventory.dungeonKeys[gSaveContext.mapIndex]));
    });

    COND_HOOK(OnItemReceive, isConnected, [&](GetItemEntry itemEntry) {
        if (m_skipItemGive)
            return;

        if (itemEntry.modIndex == MOD_NONE)
        {
            if (itemEntry.itemId == ITEM_KEY_SMALL) {
                WritePacket(newPacket(CLIENT_PACKET_UPDATE_DUNGEON_KEYS)
                            << PackedUInt1(gSaveContext.mapIndex)
                            << PackedUInt1(gSaveContext.inventory.dungeonKeys[gSaveContext.mapIndex]));
                return;
            }
        }

        else {
            RandomizerGet item = (RandomizerGet)itemEntry.getItemId;
 
            if ((item >= RG_FOREST_TEMPLE_SMALL_KEY) && (item <= RG_GANONS_CASTLE_SMALL_KEY)) {
                auto mapIndex = Rando::Logic::RandoGetToDungeonScene.at(item);
                if (mapIndex >= 0 && mapIndex < 19) {
                    WritePacket(newPacket(CLIENT_PACKET_UPDATE_DUNGEON_KEYS) << PackedUInt1(mapIndex) << PackedUInt1(gSaveContext.inventory.dungeonKeys[mapIndex]));
                }
            }
        }
    });

    // Story Co-op: these only mark the progress snapshot dirty, StoryUpdate does the work
    COND_HOOK(OnItemReceive, enabled, [&](GetItemEntry itemEntry) { StoryOnItemReceive(itemEntry); });
    COND_HOOK(OnLoadGame, enabled, [&](int32_t) { StoryOnSaveChanged(true); });
    COND_HOOK(OnExitGame, enabled, [&](int32_t) { StoryOnSaveChanged(false); });
    COND_HOOK(OnFlagSet, enabled, [&](s16, s16) { StoryMarkDirty(); });
    COND_HOOK(OnSceneFlagSet, enabled, [&](s16, s16, s16) { StoryMarkDirty(); });

    COND_HOOK(OnFlagSet, enabled, [&](s16 flagType, s16 flag) {
        if (gPlayState == NULL)
            return;

        if (flagType == FLAG_INF_TABLE || flagType == FLAG_EVENT_CHECK_INF) {
            auto sceneKey =
                MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));

            ByteStream packet = newPacket(CLIENT_PACKET_SCENE_FLAG);
            packet << PackedUInt4(sceneKey);
            packet << PackedInt1((gPlayState->roomCtx.curRoom.num));
            packet << PackedUInt1((u8)(flagType));
            packet << PackedUInt2(flag);
            packet << PackedUInt1(1u);
            WritePacket(packet);
        }
    });

    COND_HOOK(OnSceneFlagSet, enabled, [&](s16 sceneNum, s16 flagType, s16 flag) {
        if (gPlayState == NULL || sceneNum != gPlayState->sceneNum) {
            return;
        }
        if (flagType != FLAG_SCENE_SWITCH && flagType != FLAG_SCENE_CLEAR && flagType != FLAG_INF_TABLE &&
            flagType != FLAG_SCENE_TREASURE) {
            return;
        }

        auto sceneKey = MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));

        if (flagType == FLAG_SCENE_TREASURE) {
            for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_CHEST].head; actor != NULL; actor = actor->next) {
                if (actor->id != ACTOR_EN_BOX || (actor->params & 0x1F) != flag) {
                    continue;
                }

                EnBox* chest = (EnBox*)actor;
                ByteStream packet = newPacket(CLIENT_PACKET_CHEST_OPENED);
                packet << PackedUInt4(gPlayState->sceneNum);
                packet << PackedInt1((gPlayState->roomCtx.curRoom.num));
                packet << PackedUInt2(gSaveContext.mapIndex);
                packet << PackedUInt1((u8)(flag));
                packet << PackedUInt2((u16)(chest->getItemEntry.modIndex));
                packet << PackedUInt2((u16)(chest->getItemEntry.getItemId));
                WritePacket(packet);
                break;
            }
            return;
        }

        u8 options = SCENE_FLAG_OPT_SET;
        if (flagType == FLAG_SCENE_SWITCH && flag < 32 && (m_sceneLockedDoorFlags & (1u << flag)) != 0) {
            options |= SCENE_FLAG_OPT_LOCKED_DOOR;
        }

        ByteStream packet = newPacket(CLIENT_PACKET_SCENE_FLAG);
        packet << PackedUInt4(sceneKey);
        packet << PackedInt1((gPlayState->roomCtx.curRoom.num));
        packet << PackedUInt1((u8)(flagType));
        packet << PackedUInt2(flag);
        packet << PackedUInt1(options);
        WritePacket(packet);
    });

    COND_HOOK(OnSceneFlagUnset, enabled, [&](s16 sceneNum, s16 flagType, s16 flag) {
        if (gPlayState == NULL || sceneNum != gPlayState->sceneNum)
            return;
        if (flagType != FLAG_SCENE_SWITCH && flagType != FLAG_SCENE_CLEAR)
            return;

        ByteStream packet = newPacket(CLIENT_PACKET_SCENE_FLAG);
        packet << PackedUInt1((u8)(flagType));
        packet << PackedUInt2(flag);
        packet << PackedUInt1(0u);
        WritePacket(packet);
    });

    {
        static HOOK_ID hookId = 0;
        GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::OnVanillaBehavior>(hookId);

        if (enabled) {
            GameInteractor::Instance->RegisterGameHookForID<GameInteractor::OnVanillaBehavior>(
                VB_ADVANCE_DAYTIME, [this](GIVanillaBehavior, bool* should, va_list) { *should = false; });
        }
    }

    // Story Co-op time travel: both handlers do nothing unless the party has Story Co-op on
    {
        static HOOK_ID pedestalHookId = 0;
        static HOOK_ID switchAgeHookId = 0;
        GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::OnVanillaBehavior>(pedestalHookId);
        GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::OnVanillaBehavior>(switchAgeHookId);
        pedestalHookId = 0;
        switchAgeHookId = 0;

        if (enabled) {
            pedestalHookId = GameInteractor::Instance->RegisterGameHookForID<GameInteractor::OnVanillaBehavior>(
                VB_MASTER_SWORD_PEDESTAL_USE, [this](GIVanillaBehavior, bool* should, va_list originalArgs) {
                    va_list args;
                    va_copy(args, originalArgs);
                    Actor* pedestal = va_arg(args, Actor*);
                    va_end(args);
                    StoryOnPedestalUse(pedestal, should);
                });
            switchAgeHookId = GameInteractor::Instance->RegisterGameHookForID<GameInteractor::OnVanillaBehavior>(
                VB_SWITCH_AGE, [this](GIVanillaBehavior, bool* should, va_list) { StoryOnSwitchAge(should); });
        }
    }

    COND_HOOK(OnPlayPostInit, enabled, [&](int16_t sceneNum) { OnSceneInited(sceneNum); });

    COND_HOOK(OnSceneLoad, enabled, [&](int16_t sceneNum) {
        m_reloadPending = false;
        if (!ShouldFreezeTime(sceneNum, GetSceneVariant(sceneNum)))
            gSaveContext.skyboxTime = gSaveContext.dayTime = m_serverDayTime;

        
        if (((void)0, gSaveContext.dayTime) > 0xC000 || ((void)0, gSaveContext.dayTime) < 0x4555) {
            ((void)0, gSaveContext.nightFlag = 1);
        } else {
            ((void)0, gSaveContext.nightFlag = 0);
        }
   

    });




    COND_HOOK(OnTransitionRoom, enabled, [&]() {

        printf("ON TRANSITION SCENE: Current Room: %i\nLast Room: %i\nCurrent Scene: %i\nLast Scene: %i\n",
               gPlayState->roomCtx.curRoom.num, m_lastRoom, gPlayState->sceneNum, m_lastScene);

        RequestRoomSceneChange(true);
    })




    COND_HOOK(ShouldLoadSetupActors, enabled, [&](bool* should) {
        if (m_blockSceneSetupActors >= 0)
            *should = false;
    });

    COND_ID_HOOK(ShouldActorInit, ACTOR_EN_DOOR, enabled, [&](void* refActor, bool*) {
        Actor* door = static_cast<Actor*>(refActor);
        s32 doorType = (door->params >> 7) & 7;
        if (doorType == DOOR_LOCKED) {
            m_sceneLockedDoorFlags |= 1U << (door->params & 0x3F);

        }
    });

    COND_ID_HOOK(ShouldActorInit, ACTOR_DOOR_SHUTTER, enabled, [&](void* refActor, bool*) {
        Actor* shutter = static_cast<Actor*>(refActor);
        s32 doorType = (shutter->params >> 6) & 0xF;
        if (doorType == SHUTTER_KEY_LOCKED || doorType == SHUTTER_BOSS) {
            m_sceneLockedDoorFlags |= 1U << (shutter->params & 0x3F);
        }
    });

    COND_HOOK(OnPlayerSfx, enabled, [&](u16 sfxId) {
        if (gPlayState == nullptr) {
            return;
        }
        TransmitActorSound(m_myNetworkID, sfxId);
    });

    COND_HOOK(OnOcarinaNote, enabled, [&](u8 note, f32 modulator, s8 bend) {
        ByteStream p = newPacket(CLIENT_PACKET_OCARINA_SFX);
        p << PackedUInt2(m_myNetworkID);
        p << PackedUInt1(note);
        p << PackedFloat4(modulator);
        p << PackedInt1(bend);
        WritePacket(p);
    });

    COND_HOOK(OnMinimapDrawCompassIcons, enabled, [&]() {
        struct CompassIcon {
            Vec3f pos;
            Vec3s rot;
            f32 scale;
            Color_RGB8 color;
        };
        std::vector<CompassIcon> compassIcons;

        bool isInDungeon = IsDungeonScene(gPlayState->sceneNum);

        s8 displayedRoomNum =
            gPlayState->roomCtx.prevRoom.num >= 0 ? gPlayState->roomCtx.prevRoom.num : gPlayState->roomCtx.curRoom.num;

        for (auto& [netId, controller] : m_networkedActors) {
            if (controller == nullptr || !controller->IsPlayer())
                continue;

            PlayerPuppetController* playerController = static_cast<PlayerPuppetController*>(controller);
            if (isInDungeon && playerController->RealRoomIndex() != displayedRoomNum)
                continue;

            Actor* actor = playerController->GetActor();
            compassIcons.push_back(
                CompassIcon{ actor->world.pos, actor->shape.rot, 0.3f, playerController->GetTunicColour() });
        }

        Player* player = GET_PLAYER(gPlayState);
        compassIcons.push_back(CompassIcon{ player->actor.world.pos, player->actor.shape.rot, 0.4f,
                                            CVarGetColor24(CVAR_COSMETIC("HUD.Minimap.Color"), { 100, 255, 100 }) });

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

        {
            GraphicsContext* __gfxCtx = gPlayState->state.gfxCtx;
            Gfx* dispRefs[4];
            (void)__gfxCtx;
            Graph_OpenDisps(dispRefs, gPlayState->state.gfxCtx, __FILE__, __LINE__);
            Gfx_SetupDL_42Overlay(gPlayState->state.gfxCtx);

            for (auto& compassIcon : compassIcons) {
                gSPMatrix(OVERLAY_DISP++, &gMtxClear, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
                gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                                  PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
                gDPSetEnvColor(OVERLAY_DISP++, 0, 0, 0, 255);
                gDPSetCombineMode(OVERLAY_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);

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
                        }
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
                        }
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
                    Matrix_Translate(
                        OTRGetDimensionFromRightEdge((tempXOffset + (xMarginsMinimap * 10) + tempX) / 10.0f),
                        (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ) / 10.0f, 0.0f, MTXMODE_NEW);
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

            Graph_CloseDisps(dispRefs, gPlayState->state.gfxCtx, __FILE__, __LINE__);
        }
    });
    RegisterActorHooks(enabled);
}

void ZeldaOnlineClient::OnActorKill(Actor* actor) {

    if (actor->zoController == nullptr || gZeldaOnlineEngineCleanup) {
        return;
    }
    auto* controller = static_cast<AbstractActorController*>(actor->zoController);
    if (!controller->IsLeader()) {
        return;
    }

    //Push our properties before the kill command
    controller->SendUpdate();
    SendActorDied(controller->NetworkID());
}

void ZeldaOnlineClient::ReportBandwidth() {
    time_t now = time(nullptr);
    if (now == m_lastReportTime)
        return;
    m_lastReportTime = now;

    uint64_t sent = m_bytesSent;
    uint64_t received = m_bytesReceived;
    m_bytesSent = 0;
    m_bytesReceived = 0;

    printf("[ZeldaOnline] bandwidth: up %.2f KB/s (%llu B)  down %.2f KB/s (%llu B)\n", sent / 1024.0,
           static_cast<unsigned long long>(sent), received / 1024.0, static_cast<unsigned long long>(received));
}

void ZeldaOnlineClient::OnGameFrameUpdate() {
    if (gPlayState == nullptr)
        return;

    auto localPlayer = GET_PLAYER(gPlayState);

    StoryUpdate();

   // ReportBandwidth();
    if (m_hasWorldTime) {
        int sceneVariant = GetSceneVariant(gPlayState->sceneNum);

        if (!ShouldFreezeTime(gPlayState->sceneNum, sceneVariant)) {
            u16 rate = m_serverTimeRate / 20;
            u16 next = (u16)(gSaveContext.dayTime + rate);
            if (gSaveContext.dayTime > (u16)(0xFFFF - rate)) {
                gSaveContext.dayTime = 0xFFFF;
            } else if (IsNightTime(gSaveContext.dayTime) != IsNightTime(next)) {
            } else {
                gSaveContext.dayTime = next;
            }
            gSaveContext.skyboxTime = gSaveContext.dayTime;

            if (m_sceneLoadedAtNight != IsNightTime(m_serverDayTime) &&
                IsDayNightReloadScene(gPlayState->sceneNum, sceneVariant)) {
                m_reloadPending = true;
            }

            if (m_reloadPending && IsDayNightReloadScene(gPlayState->sceneNum, sceneVariant) &&
                CanReloadSceneNow(gPlayState)) {
                gSaveContext.skyboxTime = gSaveContext.dayTime = m_serverDayTime;
                m_boundaryCueOnLoad = IsNightTime(m_serverDayTime) ? 2 : 1;
                m_reloadPending = false;
                printf("RELOADING\n");
                ReloadSceneInPlace(gPlayState);
            }
        }
    }


    if (m_removeNametagTimer > 0)
    {
        if (--m_removeNametagTimer == 0)
        {
            NameTag_RemoveAllByActorTag(&GET_PLAYER(gPlayState)->actor, "name");
        }
    }

    if (m_chatVisibleTimer > 0) {
        if (--m_chatVisibleTimer == 0) {
            NameTag_RemoveAllByActorTag(&GET_PLAYER(gPlayState)->actor, "chat");
        }
    }

    if (m_clearPlayerListStatusTimer > 0)
    {
        if (--m_clearPlayerListStatusTimer == 0) {
            ZeldaOnlineRoomWindow::Instance->SetStatus("");
        }
    }

    u8 simulationPaused = ((gPlayState->pauseCtx.state != 0) || (localPlayer->stateFlags1 & (PLAYER_STATE1_TALKING | PLAYER_STATE1_DEAD | PLAYER_STATE1_IN_ITEM_CS))) ? 1 : 0;

    if (simulationPaused != m_wasSimulationPaused) {
        WritePacket(newPacket(CLIENT_PACKET_SET_PAUSE_STATE) << PackedUInt1(simulationPaused));
        m_wasSimulationPaused = simulationPaused;
    }

    if (simulationPaused) {
        ByteStream release;
        int count = 0;

        for (auto& entry : m_networkedActors) {
            AbstractActorController* c = entry.second;
            if (!c->IsPlayer() && c->IsLeader() && ((gPlayState->pauseCtx.state != 0) || c->CanRelinquishLeadership())) {
                release << PackedUInt2((unsigned int)(c->NetworkID()));
                count++;
                c->SetLeader(false);
            }
        }

        if (count > 0) {
            ByteStream p = newPacket(CLIENT_PACKET_RELINQUISH_ACTOR_LEADER);
            p << PackedUInt2((unsigned int)(count));
            p << release;
            WritePacket(p);
        }
    }

    UpdateAppearance();

    if (false) {
        if (ImGui::IsKeyDown(ImGuiKey_T)) {
            for (auto& entry : m_networkedActors) {
                AbstractActorController* c = entry.second;
                if (!c->IsPlayer())
                    continue;

                auto play = gPlayState;

                auto otherPlayerController = static_cast<PlayerPuppetController*>(c);

                Player* player = GET_PLAYER(play);
                if (player == NULL)
                    return;

                if ((player->stateFlags1 & PLAYER_STATE1_ON_HORSE) && player->rideActor != NULL) {
                    gSaveContext.horseData.scene = play->sceneNum;
                    gSaveContext.horseData.pos.x = (int16_t)player->rideActor->world.pos.x;
                    gSaveContext.horseData.pos.y = (int16_t)player->rideActor->world.pos.y;
                    gSaveContext.horseData.pos.z = (int16_t)player->rideActor->world.pos.z;
                    gSaveContext.horseData.angle = player->rideActor->world.rot.y;
                }

                gSaveContext.respawnFlag = 1;
                play->nextEntranceIndex = gSaveContext.entranceIndex;
                gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = play->nextEntranceIndex;
                gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex = otherPlayerController->RealRoomIndex();
                gSaveContext.respawn[RESPAWN_MODE_DOWN].pos = c->GetActor()->world.pos;
                gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw = c->GetActor()->shape.rot.y;
                if (play->roomCtx.curRoom.behaviorType2 < 4) {
                    gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0x0DFF;
                } else {
                    Camera* camera = GET_ACTIVE_CAM(play);
                    gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0x0D00 | camera->camDataIdx;
                }
                play->transitionTrigger = TRANS_TRIGGER_START;
                play->transitionType = TRANS_TYPE_INSTANT;
                gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;

                static int hookId = 0;
                hookId = REGISTER_VB_SHOULD(VB_INFLICT_VOID_DAMAGE, {
                    *should = false;
                    GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::OnVanillaBehavior>(hookId);
                });

                break;
            }
        }

        // O -- save the current spot to zo_warp.txt
        if (ImGui::IsKeyDown(ImGuiKey_O)) {
            Player* player = gPlayState != nullptr ? GET_PLAYER(gPlayState) : nullptr;
            if (player != nullptr) {
                s16 playerParams;
                if (gPlayState->roomCtx.curRoom.behaviorType2 < 4) {
                    playerParams = 0x0DFF;
                } else {
                    playerParams = 0x0D00 | GET_ACTIVE_CAM(gPlayState)->camDataIdx;
                }

                FILE* f = fopen("zo_warp.txt", "w");
                if (f != nullptr) {
                    fprintf(f, "%d %d %d %.3f %.3f %.3f %d %d\n", gPlayState->sceneNum, gSaveContext.entranceIndex,
                            gPlayState->roomCtx.curRoom.num, player->actor.world.pos.x, player->actor.world.pos.y,
                            player->actor.world.pos.z, player->actor.shape.rot.y, playerParams);
                    fclose(f);
                    SPDLOG_INFO("[ZeldaOnline] warp point saved");
                }
            }
        }

        // L -- warp to whatever O last saved
        if (ImGui::IsKeyDown(ImGuiKey_L)) {
            FILE* f = fopen("zo_warp.txt", "r");
            if (f != nullptr) {
                int sceneNum, entranceIndex, roomIndex, yaw, playerParams;
                float x, y, z;

                if (fscanf(f, "%d %d %d %f %f %f %d %d", &sceneNum, &entranceIndex, &roomIndex, &x, &y, &z, &yaw,
                           &playerParams) == 8) {

                    gSaveContext.respawnFlag = 1;
                    gPlayState->nextEntranceIndex = entranceIndex;
                    gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = entranceIndex;
                    gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex = roomIndex;
                    gSaveContext.respawn[RESPAWN_MODE_DOWN].pos.x = x;
                    gSaveContext.respawn[RESPAWN_MODE_DOWN].pos.y = y;
                    gSaveContext.respawn[RESPAWN_MODE_DOWN].pos.z = z;
                    gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw = (s16)yaw;
                    gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = (s16)playerParams;

                    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
                    gPlayState->transitionType = TRANS_TYPE_INSTANT;
                    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;

                    static int warpHookId = 0;
                    warpHookId = REGISTER_VB_SHOULD(VB_INFLICT_VOID_DAMAGE, {
                        *should = false;
                        GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::OnVanillaBehavior>(
                            warpHookId);
                    });
                }
                fclose(f);
            }
        }
    }
}

void ZeldaOnlineClient::InitPuppetPlayer(Actor* actor) {
    RelinkPuppetBehindPlayer(actor);

    //actor->id = ACTOR_EN_OE2;
    actor->init = PlayerPuppetController::PuppetInit;
    actor->update = PlayerPuppetController::PuppetUpdate;
    actor->draw = PlayerPuppetController::PuppetDraw;
    actor->destroy = PlayerPuppetController::PuppetDestroy;
}

void ZeldaOnlineClient::OnSceneInited(int sceneNum) {
    StoryOnSceneInit();
    m_sceneLoadedAtNight = gSaveContext.nightFlag != 0;
    m_sceneLockedDoorFlags = 0U;
    m_savedSwch = 0U;
    m_blockSceneSetupActors = -1;


    UpdateAppearance();
    printf("ON TRANSITION SCENE: Current Room: %i\nLast Room: %i\nCurrent Scene: %i\nLast Scene: %i\n",
           gPlayState->roomCtx.curRoom.num, m_lastRoom, gPlayState->sceneNum, m_lastScene);
    RequestRoomSceneChange(false);



    if (!isConnected || !IsDungeonScene(sceneNum)) {
        return;
    }
    printf("BLOCKING SETUP ACTORS\n");
    m_blockSceneSetupActors = sceneNum;
    SavedSceneFlags* saved = &gSaveContext.sceneFlags[sceneNum];

    // Story Co-op: chests the party opened are part of the save already (StoryApply), keep them closed when the
    // dungeon instance resets
    if (StoryCoopActive())
        m_savedChests[sceneNum] |= saved->chest;

    m_savedSwch = saved->swch;

    StoryRememberDungeonFlags(sceneNum, saved->swch, saved->clear);

    saved->swch = 0;
    saved->clear = 0;
}


Actor* ZeldaOnlineClient::RequestSpawnActor(s16 actorId, f32 posX, f32 posY, f32 posZ, s16 rotX, s16 rotY, s16 rotZ,
                                            s16 params) {

    if (gPlayState == nullptr)
        return nullptr;

    if (!ActorControllerFactory::Instance().IsNetworked(actorId, params) || !isConnected ||
        gPlayState->roomCtx.curRoom.num < 0) {
        return Actor_SpawnDirect(&gPlayState->actorCtx, gPlayState, actorId, posX, posY, posZ, rotX, rotY, rotZ, posX,
                                 posY, posZ, rotX, rotY, rotZ, params, 0);
    }

    auto& dbEntry = ActorDB::Instance->RetrieveEntry(actorId).entry;

    if (dbEntry.valid) {
        if (dbEntry.category == ACTORCAT_ENEMY && Flags_GetClear(gPlayState, gPlayState->roomCtx.curRoom.num)) {
            return nullptr;
        }
    }

    SPDLOG_DEBUG("[ZeldaOnline] networked request spawn request: actor {:#06x}", actorId);

    auto sceneKey = MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));

    ByteStream packet = newPacket(CLIENT_PACKET_REQUEST_ACTOR_SPAWN);
    packet << PackedUInt4(sceneKey);
    packet << PackedInt1((gPlayState->roomCtx.curRoom.num));
    packet << PackedUInt2((u16)(actorId));
    packet << PackedInt2(params);
    packet << PackedFloat4(posX) << PackedFloat4(posY) << PackedFloat4(posZ);
    packet << PackedInt2(rotX) << PackedInt2(rotY) << PackedInt2(rotZ);

    auto* ctx = AbstractActorController::CurrentLeaderContext();
    packet << PackedUInt2(ctx != nullptr ? (unsigned int)(ctx->NetworkID()) : 0u);
    WritePacket(packet);
    return nullptr;
}

Actor* ZeldaOnlineClient::RequestSpawnActorAsChild(AbstractActorController* parent, s16 actorId, f32 posX, f32 posY,
                                                   f32 posZ, s16 rotX, s16 rotY, s16 rotZ, s16 params) {

    auto& dbEntry = ActorDB::Instance->RetrieveEntry(actorId).entry;

    if (dbEntry.valid) {
        if (dbEntry.category == ACTORCAT_ENEMY && Flags_GetClear(gPlayState, gPlayState->roomCtx.curRoom.num)) {
            return nullptr;
        }
    }

    SPDLOG_DEBUG("[ZeldaOnline] networked request spawn as child request: actor {:#06x}", actorId);

    auto sceneKey = MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));

    ByteStream packet = newPacket(CLIENT_PACKET_REQUEST_ACTOR_SPAWN_AS_CHILD);
    packet << PackedUInt4(sceneKey);
    packet << PackedInt1(gPlayState->roomCtx.curRoom.num);
    packet << PackedUInt2((u16)(parent->NetworkID()));
    packet << PackedUInt2((u16)(actorId));
    packet << PackedInt2(params);
    packet << PackedFloat4(posX) << PackedFloat4(posY) << PackedFloat4(posZ);
    packet << PackedInt2(rotX) << PackedInt2(rotY) << PackedInt2(rotZ);

    auto* ctx = AbstractActorController::CurrentLeaderContext();
    packet << PackedUInt2(ctx != nullptr ? (unsigned int)(ctx->NetworkID()) : 0u);
    WritePacket(packet);
    return nullptr;
}

static bool IsClusterDedupActor(s16 actorId) {
    return actorId == ACTOR_EN_ISHI || actorId == ACTOR_EN_KUSA;
}


Actor* ZeldaOnlineClient::SpawnActor(s16 actorId, f32 posX, f32 posY, f32 posZ, s16 rotX, s16 rotY, s16 rotZ,
                                     s16 params, bool neighbourSpawn) {
    if (gPlayState == nullptr)
        return nullptr;


    if (!isConnected || !ActorControllerFactory::Instance().IsNetworked(actorId, params))
    {

        auto currentExecutingController = AbstractActorController::CurrentLeaderContext();
        if (currentExecutingController != nullptr) {
            if (currentExecutingController->CanSpawnActorOverNetwork(actorId, params)) {
                SendRoomTrigger("spawn", ByteStream()
                                                << PackedUInt2(actorId) << PackedFloat4(posX) << PackedFloat4(posY)
                                                << PackedFloat4(posZ) << PackedInt2(rotX) << PackedInt2(rotY)
                                                << PackedInt2(rotZ) << PackedInt2(params));
            }

            if (actorId == ACTOR_DOOR_WARP1 || actorId == ACTOR_ITEM_B_HEART ||
                currentExecutingController->CanAddStaticActorOverNetwork(actorId, params)) {
                WritePacket(newPacket(CLIENT_PACKET_ADD_STATIC_ACTOR)
                            << PackedUInt4(MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0,
                                                        GetSceneVariant(gPlayState->sceneNum)))
                            << PackedInt1(gPlayState->roomCtx.curRoom.num) << PackedUInt2(currentExecutingController->NetworkID())<< PackedUInt2((u16)(actorId))
                            << PackedFloat4(posX) << PackedFloat4(posY) << PackedFloat4(posZ) << PackedInt2(rotX)
                            << PackedInt2(rotY) << PackedInt2(rotZ) << PackedInt2(params));
            }
        }

        return Actor_SpawnDirect(&gPlayState->actorCtx, gPlayState, actorId, posX, posY, posZ, rotX, rotY, rotZ, posX,
                                 posY, posZ, rotX, rotY, rotZ, params, 0);
    }


    if (IsClusterDedupActor(actorId)) {
        if (Actor* existing = FindExistingActor(actorId, params, ACTORCAT_PROP, posX, posZ)) {
            DbgPrintf("FOUND EXISTING ACTOR: %i\n", actorId);
            return nullptr;
        }
    }

    auto currentExecutingController = AbstractActorController::CurrentLeaderContext();

    if (currentExecutingController != nullptr && currentExecutingController->GetActor()->init != nullptr &&
        !currentExecutingController->IsCreator()) {
        //Init calls are called by both leaders and puppets
        //If we are inside "init", only the actors "creator" is allowed to spawn
        return nullptr;
    }

    bool instantSpawn = actorId == ACTOR_EN_BOM || actorId == ACTOR_EN_BOMBF || actorId == ACTOR_EN_BOM_CHU;


    Actor* actor = Actor_SpawnDirect(&gPlayState->actorCtx, gPlayState, actorId, posX, posY, posZ, rotX, rotY, rotZ,
                                     posX, posY, posZ, rotX, rotY, rotZ, params, !instantSpawn);

    if (actor == nullptr)
        return nullptr;

    auto sceneKey = MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));
    actor->zoLocalId = m_nextLocalID++;

    AbstractActorController* controller = !instantSpawn ? ActorControllerFactory::Instance().Create(actorId, actor, 0, (int)(sceneKey), gPlayState->roomCtx.curRoom.num, true)
                      : nullptr;

    //No controller means the actor runs straight through its normal init process ASAP. A controller will mean it waits until network ID arrives
    if (controller) {
        controller->SetCreator(true);

        //This actor is spawned as part of a neighbour spawn
        //Do not allow this actor to also spawn neighbours
        if (neighbourSpawn)
            controller->DisableNeighbourSpawn();
    }
    DbgPrintf("[ZeldaOnline] networked spawn: actor {:#06x}", actorId);

    ByteStream packet = newPacket(CLIENT_PACKET_ACTOR_SPAWN);
    packet << PackedUInt4(sceneKey);
    packet << PackedInt1(gPlayState->roomCtx.curRoom.num);
    packet << PackedUInt2((u16)(actorId));
    packet << PackedUInt4((u32)(actor->zoLocalId));
    packet << PackedInt2(params);
    packet << PackedFloat4(posX) << PackedFloat4(posY) << PackedFloat4(posZ);
    packet << PackedInt2(rotX) << PackedInt2(rotY) << PackedInt2(rotZ);
    auto* ctx = AbstractActorController::CurrentLeaderContext();
    packet << PackedUInt2(ctx != nullptr ? (unsigned int)(ctx->NetworkID()) : 0u);

    WritePacket(packet);

    return actor;
}

Actor* ZeldaOnlineClient::SpawnActorAsChild(Actor* parent, s16 actorId, f32 posX, f32 posY, f32 posZ, s16 rotX,
                                            s16 rotY, s16 rotZ, s16 params) {
    if (gPlayState == nullptr)
        return nullptr;

    if (actorId == ACTOR_DOOR_WARP1 || actorId == ACTOR_ITEM_B_HEART) {
        auto currentExecutingController = AbstractActorController::CurrentLeaderContext();
        if (currentExecutingController != nullptr)
        {
            WritePacket(newPacket(CLIENT_PACKET_ADD_STATIC_ACTOR)
                        << PackedUInt4(MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0,
                                                    GetSceneVariant(gPlayState->sceneNum)))
                        << PackedInt1(gPlayState->roomCtx.curRoom.num) << PackedUInt2(currentExecutingController->NetworkID())
                        << PackedUInt2((u16)(actorId))
                        << PackedFloat4(posX) << PackedFloat4(posY) << PackedFloat4(posZ) << PackedInt2(rotX)
                        << PackedInt2(rotY) << PackedInt2(rotZ) << PackedInt2(params));

            return Actor_SpawnAsChildDirect(&gPlayState->actorCtx, parent, gPlayState, actorId, posX, posY, posZ, rotX,
                                            rotY, rotZ, posX, posY, posZ, rotX, rotY, rotZ, params, 0);
        }
    }

    bool shouldNotNetworkSpawn = !ActorControllerFactory::Instance().IsNetworked(actorId, params) || !isConnected ||
                                 gPlayState->roomCtx.curRoom.num < 0;

    if (!shouldNotNetworkSpawn) {
        auto currentExecutingController = AbstractActorController::CurrentLeaderContext();

        const bool insideParentInit = parent->init != nullptr;

        bool maySpawnDirectly = false;

        if (insideParentInit) {
            maySpawnDirectly = currentExecutingController != nullptr && currentExecutingController->IsCreator();
        } else if (currentExecutingController != nullptr &&
                   currentExecutingController->CanSpawnActorOverNetwork(actorId, params)) {
            maySpawnDirectly = true;
        }

        if (!maySpawnDirectly && (insideParentInit || m_blockedParentSpawners.contains(parent->id))) {
            if (parent->zoController != nullptr) {
                return RequestSpawnActorAsChild(static_cast<AbstractActorController*>(parent->zoController), actorId,
                                                posX, posY, posZ, rotX, rotY, rotZ, params);
            }
            return RequestSpawnActor(actorId, posX, posY, posZ, rotX, rotY, rotZ, params);
        }
    }
    bool instantSpawn = actorId == ACTOR_EN_BOM || actorId == ACTOR_EN_BOMBF || actorId == ACTOR_EN_BOM_CHU;
    Actor* actor =
        Actor_SpawnAsChildDirect(&gPlayState->actorCtx, parent, gPlayState, actorId, posX, posY, posZ, rotX, rotY, rotZ,
                                 posX, posY, posZ, rotX, rotY, rotZ, params, shouldNotNetworkSpawn ? 0 : 1);
    if (actor == nullptr)
        return nullptr;
    if (shouldNotNetworkSpawn) {
        auto currentExecutingController = AbstractActorController::CurrentLeaderContext();
        if (currentExecutingController != nullptr &&
            currentExecutingController->CanSpawnActorOverNetwork(actorId, params)) {
            SendRoomTrigger("spawnC", ByteStream() << PackedUInt2(actorId) << PackedFloat4(posX) << PackedFloat4(posY)
                                                   << PackedFloat4(posZ) << PackedInt2(rotX) << PackedInt2(rotY)
                                                   << PackedInt2(rotZ) << PackedInt2(params)
                                                   << PackedUInt2(currentExecutingController->NetworkID()));
        }

        return actor;
    }

    //for bombchu, params = 1 means its OUR bombchu. So after we create it on our end, the others will receive params 0. This
    //Is required to stop remotebombchu from attaching to other players bombchu
    if (actorId == ACTOR_EN_BOM_CHU)
        params = 0;

    auto sceneKey = MakeSceneKey(gPlayState->sceneNum, LINK_IS_ADULT ? 1 : 0, GetSceneVariant(gPlayState->sceneNum));


    AbstractActorController* controller = !instantSpawn ? ActorControllerFactory::Instance().Create(actorId, actor, 0, (int)(sceneKey), gPlayState->roomCtx.curRoom.num, true) : nullptr;

    if (controller) {
        controller->SetCreator(true);
    }

    int packetType = CLIENT_PACKET_ACTOR_SPAWN_AS_CHILD;
    actor->zoLocalId = m_nextLocalID++;

    int parentID = 0;

    if (parent->id == ACTOR_PLAYER && parent->zoController == nullptr) {
        parentID = m_myNetworkID;
    } else {

        auto parentController = static_cast<AbstractActorController*>(parent->zoController);
        if (parentController == nullptr) {
            packetType = CLIENT_PACKET_ACTOR_SPAWN;
        } else
            parentID = parentController->NetworkID();
    }

    SPDLOG_DEBUG("[ZeldaOnline] networked spawn as child request: actor {:#06x}", actorId);

    ByteStream packet = newPacket(packetType);
    packet << PackedUInt4(sceneKey);
    packet << PackedInt1(gPlayState->roomCtx.curRoom.num);

    if (packetType == CLIENT_PACKET_ACTOR_SPAWN_AS_CHILD) {
        packet << PackedUInt2((u16)(parentID));
        packet << PackedUInt4(parent->zoLocalId);
    }

    packet << PackedUInt2((u16)(actorId));
    packet << PackedUInt4((u32)(actor->zoLocalId));
    packet << PackedInt2(params);
    packet << PackedFloat4(posX) << PackedFloat4(posY) << PackedFloat4(posZ);
    packet << PackedInt2(rotX) << PackedInt2(rotY) << PackedInt2(rotZ);
    auto* ctx = AbstractActorController::CurrentLeaderContext();
    packet << PackedUInt2(ctx != nullptr ? (unsigned int)(ctx->NetworkID()) : 0u);

    WritePacket(packet);

    return actor;
}

bool ZeldaOnlineClient::RequestRoomSceneChange(bool roomTransition) {

    int roomIndex = gPlayState->roomCtx.curRoom.num;

    if (!roomTransition) {
        UpdateAppearance();


    }

    m_lastRoom = gPlayState->roomCtx.curRoom.num;
    m_lastScene = gPlayState->sceneNum;

    ByteStream packet = newPacket(CLIENT_PACKET_SET_ROOM_SCENE);
    packet << PackedUInt2((u16)(gPlayState->sceneNum));
    packet << PackedUInt1(LINK_IS_ADULT ? 1u : 0u);
    packet << PackedUInt2((u16)(GetSceneVariant(gPlayState->sceneNum)));
    packet << PackedInt1((s8)(roomIndex));
    packet << PackedInt4(gSaveContext.entranceIndex);
    packet << PackedUInt2(gSaveContext.mapIndex);
    packet << PackedUInt1(roomTransition ? 1u : 0u);

    if (!roomTransition)
    {
        packet << PackedUInt4(gSaveContext.sceneFlags[gPlayState->sceneNum].swch);
        packet << PackedUInt4(gSaveContext.sceneFlags[gPlayState->sceneNum].clear);
        if (gPlayState->actorCtx.actorLists[ACTORCAT_PLAYER].length > 0)
        {
            Player* player = GET_PLAYER(gPlayState);
            ByteStream frame;
            PlayerPuppetController::BuildLocalPlayerProperties(player, m_nickName, frame);

            unsigned int changedCount = 0;
            ByteStream delta =
                AbstractActorController::DiffProperties(m_lastPlayerProps, frame, m_sendFullPlayerProps, &changedCount);
            m_lastPlayerProps = frame;
            m_sendFullPlayerProps = false;
            packet << delta;
        }
    }
    WritePacket(packet);
    return true;
}

Actor* ZeldaOnlineClient::FindExistingActor(s16 actorId, s16 params, int category, f32 homeX, f32 homeZ) {
    ActorListEntry* list = &gPlayState->actorCtx.actorLists[category];
    Actor* actor = list->head;
    while (actor != nullptr) {
        if (actor->update && actor->id == actorId && actor->params == params &&
            fabsf(actor->home.pos.x - homeX) < 1.0f && fabsf(actor->home.pos.z - homeZ) < 1.0f) {
            return actor;
        }
        actor = actor->next;
    }
    return nullptr;
}


void ZeldaOnlineClient::SendActorDied(int networkID) {
    ByteStream packet = newPacket(CLIENT_PACKET_ACTOR_DIED);
    packet << PackedUInt2((u16)(networkID));
    WritePacket(packet);
}

void ZeldaOnlineClient::TransmitActorSound(int networkID, u16 sfxId) {
    if (!isConnected || gPlayState == nullptr) {
        return;
    }

    ByteStream packet = newPacket(CLIENT_PACKET_ACTOR_SOUND);
    packet << PackedUInt2((u16)(networkID));
    packet << PackedUInt2((u16)(sfxId));
    WritePacket(packet);
}

void ZeldaOnlineClient::SendDeclineActorLeader(int networkID) {
    ByteStream p = newPacket(CLIENT_PACKET_DECLINE_ACTOR_LEADER);
    p << PackedUInt2((unsigned int)(networkID));
    WritePacket(p);
}

void ZeldaOnlineClient::DetachAndKill(AbstractActorController* controller) {
    if (controller == nullptr) {
        return;
    }

    bool isRunningLocally = controller->IsRunningLocally();
    Actor* actor = controller->Detach();
    RemoveNetworkedActor(controller->NetworkID(), controller);
    delete controller;

    if (actor == nullptr) {
        return;
    }


    if (!isRunningLocally && actor->update != nullptr) {
        gZeldaOnlineEngineCleanup = true;
        Actor_Kill(actor);
        gZeldaOnlineEngineCleanup = false;
    }
}

int ZeldaOnlineClient::RollNeighbourCount(f32 weight) const {
    if (m_partyID == 0 || !m_partySettings.privateDungeons || !m_partySettings.extraEnemies)
        return 0;

    if (!IsPartyScene(gPlayState->sceneNum))
        return 0;

    if (weight <= 0.0f || m_partySettings.extraEnemyWeight <= 0.0f)
        return 0;

    f32 chance = m_partySize * m_neighbourChancePerPlayer * weight * m_partySettings.extraEnemyWeight;

    if (chance <= 0.0f)
        return 0;

    if (chance >= 1.0f)
        return m_maxNeighbours;

    f32 roll = Rand_ZeroOne();

    if (roll <= 0.0f)
        return m_maxNeighbours;

    int count = (int)(std::log(roll) / std::log(chance));

    return count > m_maxNeighbours ? m_maxNeighbours : count;
}

float ZeldaOnlineClient::RollHealthMultiplier(f32 weight, f32 settingWeight) const {
    if (m_partyID == 0 || !m_partySettings.privateDungeons || !m_partySettings.healthMultiplier)
        return 1.0f;

    if (!IsPartyScene(gPlayState->sceneNum))
        return 1.0f;

    if (weight <= 0.0f || settingWeight <= 0.0f)
        return 1.0f;

    if (m_partySize < 1)
        return 1.0f;

    f32 multiplier = 1.0f + (m_partySize * m_healthWeightPerPlayer * weight * settingWeight);

    if (multiplier < 1.0f)
        multiplier = 1.0f;

    if (multiplier > 3.0f)
        multiplier = 3.0f;

    return multiplier;
}

float ZeldaOnlineClient::RollEnemyHealthMultiplier(f32 weight) const {
    return RollHealthMultiplier(weight, m_partySettings.enemyHealthWeight);
}

float ZeldaOnlineClient::RollBossHealthMultiplier(f32 weight) const {
    return RollHealthMultiplier(weight, m_partySettings.bossHealthWeight);
}

std::vector<std::string> MissingArchives(const std::vector<std::string>& archiveNames) {
    std::unordered_set<std::string> loaded;

    auto archives = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->GetArchives();
    if (archives != nullptr) {
        for (const auto& archive : *archives) {
            if (archive == nullptr) {
                continue;
            }

            const std::string& path = archive->GetPath();
            size_t slash = path.find_last_of("/\\");
            std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);

            std::transform(base.begin(), base.end(), base.begin(), ::tolower);
            loaded.insert(base);
        }
    }

    std::vector<std::string> missing;
    for (const std::string& name : archiveNames) {
        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        if (loaded.count(key) == 0) {
            missing.push_back(name);
        }
    }

    return missing;
}

} // namespace ZeldaOnline

extern "C" Actor* ZeldaOnlineClient_SpawnActor(s16 actorId, f32 posX, f32 posY, f32 posZ, s16 rotX, s16 rotY, s16 rotZ,
                                               s16 params) {
    auto instance = ZeldaOnline::ZeldaOnlineClient::Instance;

    if (gMapLoading)
        return instance->RequestSpawnActor(actorId, posX, posY, posZ, rotX, rotY, rotZ, params);

    return instance->SpawnActor(actorId, posX, posY, posZ, rotX, rotY, rotZ, params);
}

extern "C" Actor* ZeldaOnlineClient_SpawnActorAsChild(Actor* parent, s16 actorId, f32 posX, f32 posY, f32 posZ,
                                                      s16 rotX, s16 rotY, s16 rotZ, s16 params) {

    auto instance = ZeldaOnline::ZeldaOnlineClient::Instance;
    return instance->SpawnActorAsChild(parent, actorId, posX, posY, posZ, rotX, rotY, rotZ, params);
}

extern "C" int ZeldaOnlineClient_RequestRoomSceneChange(int freshLoad) {

    auto instance = ZeldaOnline::ZeldaOnlineClient::Instance;
    return 0;
}

extern "C" const char* ZeldaOnline_LocalSkinName() {
    auto* client = ZeldaOnline::ZeldaOnlineClient::Instance;
    if (client == nullptr || !client->isConnected) {
        return "";
    }
    return client->LocalSkinName().c_str();
}