// Scripted two-client Story Co-op test mode. Included at the end of
// soh/soh/Network/ZeldaOnline/ZeldaOnlineClient.cpp so it compiles without touching the CMake source globs and can use
// the client's internals through a friend. Everything here is inert unless the process was started with --coop-test
// (or --coop-test-extract-only).
#include "soh/Enhancements/AutoTest/AutoTest.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include <fstream>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "global.h"
#include "macros.h"
void Sram_InitDebugSave(void);
}
extern "C" void GameInteractor_ExecuteOnLoadGame(int32_t fileNum);
extern "C" void GameInteractor_ExecuteOnExitGame(int32_t fileNum);

namespace ZeldaOnline {

// Friend of ZeldaOnlineClient and ZeldaOnlineRoomWindow: read-only views plus the same packets the UI sends.
struct AutoTestAccess {
    static ZeldaOnlineClient* C() {
        return ZeldaOnlineClient::Instance;
    }
    static ZeldaOnlineRoomWindow* W() {
        return ZeldaOnlineRoomWindow::Instance;
    }

    static void Connect() {
        C()->Connect();
    }
    static bool Connected() {
        return C()->isConnected;
    }
    static int NetId() {
        return C()->m_myNetworkID;
    }
    static uint32_t PartyId() {
        return C()->m_partyID;
    }
    static const PartySettings& Settings() {
        return C()->m_partySettings;
    }
    static bool StoryActive() {
        return C()->StoryCoopActive();
    }
    static std::vector<PlayerEntry>& Players() {
        return W()->m_players;
    }
    static bool HasParty() {
        return W()->m_hasParty;
    }
    static bool IsAdmin() {
        return W()->m_partyAdmin;
    }
    static uint32_t AdminId() {
        return W()->m_partyAdminID;
    }
    static int PartySize() {
        return W()->GetPartySize();
    }
    static int PlayerPuppets() {
        int n = 0;
        for (auto& entry : C()->m_networkedActors) {
            AbstractActorController* c = entry.second;
            if (c != nullptr && c->IsPlayer() && c->GetActor() != nullptr) {
                n++;
            }
        }
        return n;
    }
    static int PeersInScene(int sceneNum) {
        int n = 0;
        for (auto& p : Players()) {
            if (p.sceneNum == sceneNum) {
                n++;
            }
        }
        return n;
    }
    static bool HoldingAtPedestal() {
        return C()->m_storyHold;
    }
    static int VoteCount() {
        return C()->m_storyVoted;
    }
    static int VoteTotal() {
        return C()->m_storyTotal;
    }
    static int MoveEra() {
        return C()->m_storyMoveEra;
    }
    static const std::string& Nick() {
        return C()->m_nickName;
    }
    static int ServerDayTime() {
        return C()->m_serverDayTime;
    }
    static bool HasWorldTime() {
        return C()->m_hasWorldTime;
    }
    static int ServerTimeRate() {
        return C()->m_serverTimeRate;
    }
    // Leader (owns the actor) or puppet (mirrors the leader's copy): -1 when the actor is not networked.
    static int ActorRole(Actor* actor) {
        if (actor == nullptr || actor->zoController == nullptr) {
            return -1;
        }
        return static_cast<AbstractActorController*>(actor->zoController)->IsLeader() ? 1 : 0;
    }

    static void ApplySettings(const nlohmann::json& step) {
        PartySettings s = W()->m_partySettings;
        if (step.contains("story_coop")) s.storyCoop = step["story_coop"].get<bool>();
        if (step.contains("share_ammo")) s.shareAmmoRupeesHp = step["share_ammo"].get<bool>();
        if (step.contains("private_dungeons")) s.privateDungeons = step["private_dungeons"].get<bool>();
        if (step.contains("story_code")) {
            // "auto" is what the party window does when the admin leaves the code empty
            s.storyCode = step["story_code"].get<std::string>();
            if (s.storyCode == "auto") s.storyCode = GenerateStoryCode();
        }
        if (!s.storyCoop) s.shareAmmoRupeesHp = false;
        W()->m_partySettings = s;
        C()->SetPartySettings(s);
    }
    static bool CreateParty(const nlohmann::json& step) {
        ApplySettings(step);
        return C()->WritePacket(newPacket(CLIENT_PACKET_PARTY_CREATE)
                                << C()->BuildPartySettingsJSON(C()->m_partySettings));
    }
    static bool SendSettings(const nlohmann::json& step) {
        ApplySettings(step);
        return C()->WritePacket(newPacket(CLIENT_PACKET_UPDATE_PARTY_SETTINGS)
                                << C()->BuildPartySettingsJSON(C()->m_partySettings));
    }
    // Invite every listed player that is not in the party (or just `name`).
    static int InviteAll(const std::string& name) {
        int sent = 0;
        for (auto& p : Players()) {
            if (p.isInParty || (!name.empty() && p.name != name)) {
                continue;
            }
            C()->WritePacket(newPacket(CLIENT_PACKET_PARTY_INVITE) << PackedUInt2((uint16_t)p.networkID));
            sent++;
        }
        return sent;
    }
    static bool InvitePending() {
        for (auto& p : Players()) {
            if (p.pendingInvitePartyID != 0) {
                return true;
            }
        }
        return false;
    }
    static bool AcceptInvite() {
        for (auto& p : Players()) {
            if (p.pendingInvitePartyID != 0) {
                return C()->WritePacket(newPacket(CLIENT_PACKET_PARTY_JOIN)
                                        << PackedUInt2((uint16_t)p.networkID) << PackedUInt4(p.pendingInvitePartyID));
            }
        }
        return false;
    }
    static void Disconnect() {
        C()->Disconnect();
    }
    static bool LeaveParty() {
        return C()->WritePacket(newPacket(CLIENT_PACKET_PARTY_LEAVE));
    }
};

} // namespace ZeldaOnline

namespace AutoTest {

namespace {

using json = nlohmann::json;
using ZeldaOnline::AutoTestAccess;

struct State {
    bool enabled = false;
    bool extractOnly = false;
    std::string scenarioPath;
    std::string role = "A";
    std::string outPath;
    std::string romPath;
    std::string host;
    int port = 0;
    json scenario = json::object();
    json roleCfg = json::object();
    json steps = json::array();
    size_t stepIdx = 0;
    bool stepStarted = false;
    uint32_t stepFrames = 0;
    uint64_t frame = 0;
    uint32_t finishedFrames = 0;
    json observations = json::array();
    json events = json::array();
    bool finished = false;
    bool pass = false;
    std::string error;
    bool booted = false;
    int bootWaitFrames = 0;
    int warpTargetScene = -1;
    bool bootReset = false;
    json bootOverride = json::object();
};

State sState;

void WriteLog() {
    if (sState.outPath.empty()) {
        return;
    }
    json out;
    out["scenario"] = sState.scenario.value("name", "");
    out["role"] = sState.role;
    out["finished"] = sState.finished;
    out["pass"] = sState.pass;
    out["error"] = sState.error;
    out["frames"] = sState.frame;
    out["step"] = sState.stepIdx;
    out["observations"] = sState.observations;
    out["events"] = sState.events;
    std::ofstream f(sState.outPath, std::ios::trunc);
    if (f) {
        f << out.dump(2);
    }
}

void Event(const std::string& what) {
    json e;
    e["frame"] = sState.frame;
    e["msg"] = what;
    sState.events.push_back(e);
    SPDLOG_INFO("[AutoTest:{}] {}", sState.role, what);
}

void Finish(bool pass, const std::string& error) {
    if (sState.finished) {
        return;
    }
    sState.finished = true;
    sState.pass = pass;
    sState.error = error;
    Event(pass ? "scenario finished: pass" : "scenario finished: FAIL: " + error);
    WriteLog();
    auto ctx = Ship::Context::GetRawInstance();
    if (ctx != nullptr && ctx->GetWindow() != nullptr) {
        ctx->GetWindow()->Close();
    }
}

std::string ArgValue(int argc, char** argv, int& i) {
    if (i + 1 < argc) {
        return argv[++i];
    }
    return "";
}

// Play state is up, a save is active, not mid-transition.
bool PlayReady() {
    if (gPlayState == nullptr || !GameInteractor::IsSaveLoaded(true)) {
        return false;
    }
    if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gPlayState->transitionMode != 0) {
        return false;
    }
    return gPlayState->state.frames > 10;
}

bool ReadFlag(const std::string& type, int flag) {
    if (type == "switch") return Flags_GetSwitch(gPlayState, flag) != 0;
    if (type == "clear") return Flags_GetClear(gPlayState, flag) != 0;
    if (type == "collect") return Flags_GetCollectible(gPlayState, flag) != 0;
    if (type == "treasure") return Flags_GetTreasure(gPlayState, flag) != 0;
    if (type == "event_chk_inf") return Flags_GetEventChkInf(flag) != 0;
    if (type == "inf_table") return Flags_GetInfTable(flag) != 0;
    if (type == "item_get_inf") return Flags_GetItemGetInf(flag) != 0;
    if (type == "quest_item") return ((gSaveContext.inventory.questItems >> flag) & 1) != 0;
    return false;
}

bool WriteFlag(const std::string& type, int flag) {
    if (type == "switch") Flags_SetSwitch(gPlayState, flag);
    else if (type == "clear") Flags_SetClear(gPlayState, flag);
    else if (type == "collect") Flags_SetCollectible(gPlayState, flag);
    else if (type == "treasure") Flags_SetTreasure(gPlayState, flag);
    else if (type == "event_chk_inf") Flags_SetEventChkInf(flag);
    else if (type == "inf_table") Flags_SetInfTable(flag);
    else if (type == "item_get_inf") Flags_SetItemGetInf(flag);
    else return false;
    return true;
}

bool UnwriteFlag(const std::string& type, int flag) {
    if (type == "switch") Flags_UnsetSwitch(gPlayState, flag);
    else if (type == "clear") Flags_UnsetClear(gPlayState, flag);
    else return false;
    return true;
}

json Observe(const json& step) {
    json o;
    o["name"] = step.value("name", "obs");
    o["frame"] = sState.frame;
    o["connected"] = AutoTestAccess::Connected();
    o["net_id"] = AutoTestAccess::NetId();
    o["nickname"] = AutoTestAccess::Nick();
    o["party_id"] = AutoTestAccess::PartyId();
    o["has_party"] = AutoTestAccess::HasParty();
    o["party_admin"] = AutoTestAccess::IsAdmin();
    o["party_admin_id"] = AutoTestAccess::AdminId();
    o["party_size"] = AutoTestAccess::PartySize();
    const auto& ps = AutoTestAccess::Settings();
    o["story_coop"] = ps.storyCoop;
    o["share_ammo"] = ps.shareAmmoRupeesHp;
    o["story_code"] = ps.storyCode;
    o["story_active"] = AutoTestAccess::StoryActive();
    o["story_hold"] = AutoTestAccess::HoldingAtPedestal();
    o["vote_count"] = AutoTestAccess::VoteCount();
    o["vote_total"] = AutoTestAccess::VoteTotal();
    o["move_era"] = AutoTestAccess::MoveEra();
    o["peer_puppets"] = AutoTestAccess::PlayerPuppets();
    o["save_loaded"] = GameInteractor::IsSaveLoaded(true);
    o["file_num"] = gSaveContext.fileNum;
    json peers = json::array();
    for (auto& p : AutoTestAccess::Players()) {
        json pj;
        pj["id"] = p.networkID;
        pj["name"] = p.name;
        pj["in_party"] = p.isInParty;
        pj["scene"] = p.sceneNum;
        pj["room"] = p.roomIndex;
        pj["age"] = p.age; // 0 adult, 1 child
        peers.push_back(pj);
    }
    o["peers"] = peers;
    if (gPlayState != nullptr && GameInteractor::IsSaveLoaded(true)) {
        Player* p = GET_PLAYER(gPlayState);
        o["scene"] = gPlayState->sceneNum;
        o["room"] = gPlayState->roomCtx.curRoom.num;
        o["entrance"] = gSaveContext.entranceIndex;
        o["link_age"] = gSaveContext.linkAge; // 0 adult, 1 child
        o["peers_in_scene"] = AutoTestAccess::PeersInScene(gPlayState->sceneNum);
        o["day_time"] = gSaveContext.dayTime;
        o["night_flag"] = gSaveContext.nightFlag;
        o["server_day_time"] = AutoTestAccess::ServerDayTime();
        o["has_world_time"] = AutoTestAccess::HasWorldTime();
        o["server_time_rate"] = AutoTestAccess::ServerTimeRate();
        o["map_index"] = gSaveContext.mapIndex;
        {
            json keys = json::array();
            for (int i = 0; i < 19; i++) {
                keys.push_back(gSaveContext.inventory.dungeonKeys[i]);
            }
            o["dungeon_keys"] = keys;
            // the live words of the loaded scene and the copy the save keeps for it
            const ActorContext& ac = gPlayState->actorCtx;
            const SavedSceneFlags& sv = gSaveContext.sceneFlags[gPlayState->sceneNum];
            o["live"] = { { "swch", ac.flags.swch },       { "clear", ac.flags.clear },
                          { "collect", ac.flags.collect }, { "chest", ac.flags.chest },
                          { "temp_clear", ac.flags.tempClear } };
            o["saved"] = { { "swch", sv.swch }, { "clear", sv.clear }, { "collect", sv.collect }, { "chest", sv.chest } };
        }
        o["health"] = gSaveContext.health;
        o["health_capacity"] = gSaveContext.healthCapacity;
        o["rupees"] = gSaveContext.rupees;
        {
            const Inventory& inv = gSaveContext.inventory;
            json items = json::array();
            json ammo = json::array();
            for (int i = 0; i < 24; i++) {
                items.push_back(inv.items[i]);
            }
            for (int i = 0; i < 16; i++) {
                ammo.push_back(inv.ammo[i]);
            }
            o["items"] = items;
            o["ammo"] = ammo;
            o["upgrades"] = inv.upgrades;
            o["equipment"] = inv.equipment;
            o["quest_items"] = inv.questItems;
            o["dungeon_items"] = { inv.dungeonItems[0], inv.dungeonItems[1], inv.dungeonItems[2] };
            o["bgs_flag"] = gSaveContext.bgsFlag;
        }
        if (p != nullptr) {
            o["link_pos"] = { p->actor.world.pos.x, p->actor.world.pos.y, p->actor.world.pos.z };
        }
        if (step.contains("chests")) {
            // every chest of the loaded rooms: its flag, whether the save/actor context says it is open, and how far
            // the open animation is (a chest that spawned already open starts at the last frame)
            json chests = json::array();
            for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_CHEST].head; a != nullptr; a = a->next) {
                if (a->id != ACTOR_EN_BOX) continue;
                EnBox* box = (EnBox*)a;
                json cj;
                cj["flag"] = a->params & 0x1F;
                cj["room"] = a->room;
                cj["type"] = box->type;
                cj["open"] = Flags_GetTreasure(gPlayState, a->params & 0x1F) != 0;
                cj["anim_frame"] = box->skelanime.curFrame;
                cj["anim_end"] = box->skelanime.endFrame;
                cj["anim_speed"] = box->skelanime.playSpeed;
                cj["action"] = (uint32_t)(reinterpret_cast<uintptr_t>(box->actionFunc) & 0xFFFFFFFF);
                cj["unk_1F4"] = box->unk_1F4;
                cj["move_flags"] = box->movementFlags;
                cj["alpha"] = box->alpha;
                chests.push_back(cj);
            }
            o["chests"] = chests;
        }
        if (step.contains("actors")) {
            // actors of the given ids near Link: position, distance, and leader/puppet role
            json out = json::array();
            Player* pl = GET_PLAYER(gPlayState);
            for (auto& q : step["actors"]) {
                const int id = q.value("id", -1);
                const float radius = q.value("radius", 100000.0f);
                for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
                    for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
                        if (a->id != id) continue;
                        if (q.contains("params") && a->params != q["params"].get<int>()) continue;
                        float dx = a->world.pos.x - pl->actor.world.pos.x;
                        float dz = a->world.pos.z - pl->actor.world.pos.z;
                        float dist = sqrtf(dx * dx + dz * dz);
                        if (dist > radius) continue;
                        json aj;
                        aj["id"] = id;
                        aj["params"] = a->params;
                        aj["x"] = a->world.pos.x;
                        aj["y"] = a->world.pos.y;
                        aj["z"] = a->world.pos.z;
                        aj["dist"] = dist;
                        aj["role"] = AutoTestAccess::ActorRole(a); // 1 leader, 0 puppet, -1 local only
                        aj["speed"] = a->speedXZ;
                        aj["rot_y"] = a->shape.rot.y;
                        aj["hp"] = a->colChkInfo.health;
                        out.push_back(aj);
                    }
                }
            }
            o["actors"] = out;
            o["actor_count"] = out.size();
        }
        if (step.contains("flags")) {
            json flags = json::array();
            for (auto& f : step["flags"]) {
                json fj = f;
                fj["value"] = ReadFlag(f.value("type", "switch"), f.value("flag", 0));
                flags.push_back(fj);
            }
            o["flags"] = flags;
        }
    }
    return o;
}

// Clears everything the debug save hands out so a scenario can hand out items itself.
void ResetInventory() {
    Inventory& inv = gSaveContext.inventory;
    for (int i = 0; i < 24; i++) {
        inv.items[i] = ITEM_NONE;
    }
    for (int i = 0; i < 16; i++) {
        inv.ammo[i] = 0;
    }
    inv.equipment = 0;
    inv.upgrades = 0;
    inv.questItems = 0;
    for (int i = 0; i < 20; i++) {
        inv.dungeonItems[i] = 0;
    }
    for (int i = 0; i < 19; i++) {
        inv.dungeonKeys[i] = -1;
    }
    inv.defenseHearts = 0;
    inv.gsTokens = 0;
    for (int i = 1; i < 4; i++) {
        gSaveContext.equips.buttonItems[i] = ITEM_NONE;
    }
    gSaveContext.equips.equipment = 0;
    gSaveContext.healthCapacity = 0x30;
    gSaveContext.health = 0x30;
    gSaveContext.rupees = 0;
    gSaveContext.isMagicAcquired = 0;
    gSaveContext.isDoubleMagicAcquired = 0;
    gSaveContext.isDoubleDefenseAcquired = 0;
    gSaveContext.bgsFlag = 0;
}

void BootNow();

// Returns true when the step is complete.
bool RunStep(const json& step) {
    const std::string op = step.value("op", "");
    const uint32_t timeout = step.value("timeout_frames", 3600);
    const bool first = !sState.stepStarted;
    sState.stepStarted = true;

    auto timedOut = [&](const std::string& what) {
        if (sState.stepFrames > timeout) {
            Finish(false, "timeout in step " + std::to_string(sState.stepIdx) + " (" + op + "): " + what);
        }
        return false;
    };

    if (op == "wait_frames") {
        return sState.stepFrames >= (uint32_t)step.value("n", 1);
    }
    if (op == "wait_play") {
        if (PlayReady()) {
            return true;
        }
        return timedOut("play state not ready");
    }
    if (op == "connect") {
        AutoTestAccess::Connect();
        Event("connecting");
        return true;
    }
    if (op == "warp") {
        if (first) {
            if (!PlayReady()) {
                sState.stepStarted = false;
                return timedOut("warp: not ready");
            }
            sState.warpTargetScene = step.value("scene", -1);
            gPlayState->nextEntranceIndex = step.value("entrance", 0);
            gPlayState->transitionTrigger = TRANS_TRIGGER_START;
            gPlayState->transitionType = TRANS_TYPE_INSTANT;
            gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
            Event("warp to entrance " + std::to_string(step.value("entrance", 0)));
            return false;
        }
        if (PlayReady() && (sState.warpTargetScene < 0 || gPlayState->sceneNum == sState.warpTargetScene) &&
            sState.stepFrames > 5) {
            return true;
        }
        return timedOut("warp not completed");
    }
    if (op == "wait_scene") {
        if (PlayReady() && gPlayState->sceneNum == step.value("scene", 0)) {
            return true;
        }
        return timedOut("scene");
    }
    if (op == "wait_until") {
        const std::string cond = step.value("cond", "");
        const int n = step.value("n", 1);
        bool ok = false;
        if (cond == "connected") {
            ok = AutoTestAccess::Connected();
        } else if (cond == "net_id") {
            ok = AutoTestAccess::NetId() != 0;
        } else if (cond == "other_players") {
            ok = (int)AutoTestAccess::Players().size() >= n;
        } else if (cond == "has_party") {
            ok = AutoTestAccess::PartyId() != 0 && AutoTestAccess::HasParty();
        } else if (cond == "invite_pending") {
            ok = AutoTestAccess::InvitePending();
        } else if (cond == "party_size") {
            ok = AutoTestAccess::PartySize() >= n;
        } else if (cond == "story_active") {
            ok = AutoTestAccess::StoryActive() == step.value("value", true);
        } else if (cond == "share_ammo_setting") {
            ok = AutoTestAccess::Settings().shareAmmoRupeesHp == step.value("value", true);
        } else if (cond == "story_coop_setting") {
            ok = AutoTestAccess::Settings().storyCoop == step.value("value", true);
        } else if (cond == "story_code_set") {
            ok = !AutoTestAccess::Settings().storyCode.empty();
        } else if (cond == "disconnected") {
            ok = !AutoTestAccess::Connected();
        } else if (cond == "party_admin") {
            ok = AutoTestAccess::IsAdmin() == step.value("value", true);
        } else if (PlayReady()) {
            if (cond == "peers_in_scene") {
                ok = AutoTestAccess::PeersInScene(gPlayState->sceneNum) >= n;
            } else if (cond == "peer_puppets") {
                ok = AutoTestAccess::PlayerPuppets() >= n;
            } else if (cond == "item") {
                ok = gSaveContext.inventory.items[step.value("slot", 0)] == step.value("value", 0);
            } else if (cond == "ammo") {
                ok = gSaveContext.inventory.ammo[step.value("slot", 0)] == step.value("n", 0);
            } else if (cond == "rupees") {
                ok = gSaveContext.rupees == step.value("n", 0);
            } else if (cond == "health") {
                ok = gSaveContext.health == step.value("n", 0);
            } else if (cond == "health_capacity") {
                ok = gSaveContext.healthCapacity >= step.value("n", 0);
            } else if (cond == "quest_bit") {
                ok = ((gSaveContext.inventory.questItems >> step.value("bit", 0)) & 1) == (step.value("value", true) ? 1u : 0u);
            } else if (cond == "flag") {
                ok = ReadFlag(step.value("type", "switch"), step.value("flag", 0)) == step.value("value", true);
            } else {
                Finish(false, "unknown wait_until cond: " + cond);
                return true;
            }
        }
        if (ok) {
            return true;
        }
        return timedOut("cond " + cond);
    }
    if (op == "create_party") {
        if (!AutoTestAccess::CreateParty(step)) {
            return timedOut("create_party: not connected");
        }
        Event("party create sent");
        return true;
    }
    if (op == "invite") {
        if (AutoTestAccess::InviteAll(step.value("name", "")) == 0) {
            return timedOut("invite: nobody to invite");
        }
        Event("invite sent");
        return true;
    }
    if (op == "accept_invite") {
        if (!AutoTestAccess::AcceptInvite()) {
            return timedOut("accept_invite: no pending invite");
        }
        Event("invite accepted");
        return true;
    }
    if (op == "set_party_settings") {
        if (!AutoTestAccess::SendSettings(step)) {
            return timedOut("set_party_settings: not connected");
        }
        return true;
    }
    if (op == "leave_party") {
        AutoTestAccess::LeaveParty();
        return true;
    }
    if (op == "disconnect") {
        AutoTestAccess::Disconnect();
        Event("disconnecting");
        return true;
    }
    if (op == "to_title") {
        // The same thing the "reset" console command does: leave the save and go back to the title screen.
        if (gGameState == nullptr) return timedOut("to_title: no game state");
        SET_NEXT_GAMESTATE(gGameState, TitleSetup_Init, GameState);
        gGameState->running = false;
        GameInteractor_ExecuteOnExitGame(gSaveContext.fileNum);
        Event("back to title");
        return true;
    }
    if (op == "wait_title") {
        // The title screen runs its own cutscene world, so only "no save is loaded" tells that we are out of the game.
        if (!GameInteractor::IsSaveLoaded(true) && sState.stepFrames > 20) return true;
        return timedOut("title screen not reached");
    }
    if (op == "boot") {
        // Load a fresh (emptied) debug save again, like picking another save from the title screen.
        if (first) {
            sState.bootReset = step.value("reset", true);
            sState.bootOverride = step;
            BootNow();
            return false;
        }
        if (sState.booted && PlayReady()) return true;
        return timedOut("boot after title");
    }
    if (op == "observe") {
        sState.observations.push_back(Observe(step));
        WriteLog();
        return true;
    }
    if (op == "set_flag" || op == "unset_flag") {
        if (!PlayReady()) return timedOut("set_flag not ready");
        if (op == "set_flag" && !WriteFlag(step.value("type", "switch"), step.value("flag", 0))) {
            Finish(false, "unknown flag type");
        }
        if (op == "unset_flag" && !UnwriteFlag(step.value("type", "switch"), step.value("flag", 0))) {
            Finish(false, "unknown flag type for unset_flag (switch and clear only)");
        }
        return true;
    }
    if (op == "open_chest") {
        // What a chest does when it is opened: its treasure flag is set (the OnSceneFlagSet hook then tells the
        // server which item was inside), plus the item itself when `give` is true like a real pickup.
        if (!PlayReady()) return timedOut("open_chest not ready");
        for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_CHEST].head; a != nullptr; a = a->next) {
            if (a->id != ACTOR_EN_BOX) continue;
            if (step.contains("flag") && (a->params & 0x1F) != step["flag"].get<int>()) continue;
            Flags_SetTreasure(gPlayState, a->params & 0x1F);
            Event("opened chest " + std::to_string(a->params & 0x1F));
            return true;
        }
        Finish(false, "open_chest: no such chest in the loaded rooms");
        return true;
    }
    if (op == "goto_room") {
        // Load another room of the scene, like walking through a door (rooms load, their actors spawn).
        if (!PlayReady()) return timedOut("goto_room not ready");
        if (first) {
            if (gPlayState->roomCtx.status != 0) return timedOut("goto_room: room load busy");
            func_8009728C(gPlayState, &gPlayState->roomCtx, step.value("n", 0));
            return false;
        }
        if (gPlayState->roomCtx.status == 0 && gPlayState->roomCtx.curRoom.num == step.value("n", 0) &&
            sState.stepFrames > 5) {
            if (step.contains("near_chest")) {
                // Link keeps the coordinates of the room he came from: stand him in front of a chest of the new room
                for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_CHEST].head; a != nullptr; a = a->next) {
                    if (a->id != ACTOR_EN_BOX || (a->params & 0x1F) != step["near_chest"].get<int>() ||
                        a->room != step.value("n", 0)) {
                        continue;
                    }
                    Player* pl = GET_PLAYER(gPlayState);
                    pl->actor.world.pos.x = a->world.pos.x + Math_SinS(a->shape.rot.y) * 70.0f;
                    pl->actor.world.pos.y = a->world.pos.y + 5.0f;
                    pl->actor.world.pos.z = a->world.pos.z + Math_CosS(a->shape.rot.y) * 70.0f;
                    pl->actor.prevPos = pl->actor.world.pos;
                    pl->actor.velocity.x = pl->actor.velocity.y = pl->actor.velocity.z = 0.0f;
                    break;
                }
            }
            return true;
        }
        return timedOut("room did not load");
    }
    if (op == "spawn_collectible") {
        // A floor pickup that carries a collect flag (a key, heart piece, ...), placed a little way from Link.
        if (!PlayReady()) return timedOut("spawn_collectible not ready");
        Player* pl = GET_PLAYER(gPlayState);
        const int type = step.value("type", ITEM00_SMALL_KEY);
        const int flag = step.value("flag", 1);
        Actor* a = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_ITEM00, pl->actor.world.pos.x + 160.0f,
                               pl->actor.world.pos.y + 20.0f, pl->actor.world.pos.z, 0, 0, 0,
                               (s16)(type | (flag << 8)));
        if (a == nullptr) {
            Finish(false, "spawn_collectible: spawn failed");
        }
        return true;
    }
    if (op == "use_key") {
        // A small key spent on a door: same hook the door actors run.
        if (!PlayReady()) return timedOut("use_key not ready");
        if (step.contains("map_index")) gSaveContext.mapIndex = step["map_index"].get<int>();
        gSaveContext.inventory.dungeonKeys[gSaveContext.mapIndex]--;
        GameInteractor_ExecuteOnDungeonKeyUsedHooks(gSaveContext.mapIndex);
        return true;
    }
    if (op == "crash") {
        // A hard process death with no cleanup (no disconnect packet, no save). The log is written first, as a pass,
        // because dying here is what the scenario asked for.
        Event("crashing on purpose");
        sState.finished = true;
        sState.pass = true;
        WriteLog();
        TerminateProcess(GetCurrentProcess(), 3);
        return true;
    }
    if (op == "reset_inventory") {
        if (!PlayReady()) return timedOut("reset_inventory not ready");
        ResetInventory();
        return true;
    }
    if (op == "give_item") {
        // The same call a pickup or a chest ends in, so the OnItemReceive hooks fire like in play.
        if (!PlayReady()) return timedOut("give_item not ready");
        if (step.contains("map_index")) gSaveContext.mapIndex = step["map_index"].get<int>();
        Item_Give(gPlayState, (u8)step.value("item", 0));
        return true;
    }
    if (op == "set_ammo") {
        gSaveContext.inventory.ammo[step.value("slot", 0)] = (s8)step.value("n", 0);
        return true;
    }
    if (op == "set_slot") {
        gSaveContext.inventory.items[step.value("slot", 0)] = (u8)step.value("item", 0);
        return true;
    }
    if (op == "set_health") {
        gSaveContext.health = step.value("health", 48);
        return true;
    }
    if (op == "set_rupees") {
        gSaveContext.rupees = step.value("n", 0);
        return true;
    }
    if (op == "set_time") {
        if (step.contains("day_time")) gSaveContext.dayTime = step["day_time"].get<int>();
        if (step.contains("night_flag")) gSaveContext.nightFlag = step["night_flag"].get<int>();
        return true;
    }
    if (op == "log") {
        Event(step.value("msg", ""));
        return true;
    }
    if (op == "quit") {
        Finish(step.value("pass", true), step.value("error", ""));
        return true;
    }
    Finish(false, "unknown op: " + op);
    return true;
}

void OnFrame() {
    if (!sState.enabled || sState.scenario.empty()) {
        return;
    }
    // Title boot used a temporary file number; switch to slot 0 once the world is up.
    if (gPlayState != nullptr && gSaveContext.fileNum == 0xFF && sState.booted) {
        gSaveContext.fileNum = 0;
    }
    sState.frame++;
    // "god": keep Link alive, tests teleport him around rooms where he can fall out of the world
    if (gPlayState != nullptr && sState.roleCfg.value("god", false) && GameInteractor::IsSaveLoaded(true)) {
        gSaveContext.health = gSaveContext.healthCapacity;
    }
    if (sState.finished) {
        // Window close is requested; if the process is still alive shortly after, leave the hard way.
        if (++sState.finishedFrames > 600) {
            exit(sState.pass ? 0 : 1);
        }
        return;
    }
    if (sState.stepIdx >= sState.steps.size()) {
        Finish(true, "");
        return;
    }
    sState.stepFrames++;
    if (RunStep(sState.steps[sState.stepIdx]) && !sState.finished) {
        sState.stepIdx++;
        sState.stepStarted = false;
        sState.stepFrames = 0;
    }
}

// Straight into the world, bypassing logo/file select (same path as the boot-to-warp debug option).
void BootNow() {
    sState.booted = true;
    json boot = sState.scenario.value("boot", json::object());
    for (auto it = sState.bootOverride.begin(); it != sState.bootOverride.end(); ++it) {
        boot[it.key()] = it.value();
    }
    const int entrance = boot.value("entrance", 0x00BB);
    const std::string age = boot.value("age", "child");

    gSaveContext.gameMode = GAMEMODE_NORMAL;
    gSaveContext.fileNum = 0xFE;
    Sram_InitDebugSave();
    gSaveContext.magicFillTarget = gSaveContext.magic;
    gSaveContext.magic = 0;
    gSaveContext.magicCapacity = 0;
    gSaveContext.magicLevel = gSaveContext.magic;
    gSaveContext.fileNum = 0xFF;
    gSaveContext.sceneSetupIndex = 0;
    gSaveContext.cutsceneIndex = 0;
    if (sState.bootReset) ResetInventory();
    gSaveContext.linkAge = (age == "adult") ? 0 : 1;
    gSaveContext.nightFlag = 0;
    gSaveContext.skyboxTime = gSaveContext.dayTime = 0x8000;
    for (int i = 0; i < ARRAY_COUNT(gSaveContext.buttonStatus); i++) {
        gSaveContext.buttonStatus[i] = BTN_ENABLED;
    }
    gSaveContext.forceRisingButtonAlphas = 0;
    gSaveContext.unk_13E8 = gSaveContext.unk_13EA = gSaveContext.unk_13EC = 0;
    Audio_QueueSeqCmd(SEQ_PLAYER_BGM_MAIN << 24 | NA_BGM_STOP);
    gSaveContext.entranceIndex = entrance;
    gSaveContext.respawnFlag = 0;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = ENTR_LOAD_OPENING;
    gSaveContext.seqId = (u8)NA_BGM_DISABLED;
    gSaveContext.natureAmbienceId = 0xFF;
    gSaveContext.showTitleCard = false;
    gWeatherMode = 0;
    gGameState->running = false;
    SET_NEXT_GAMESTATE(gGameState, Play_Init, PlayState);
    GameInteractor_ExecuteOnLoadGame(gSaveContext.fileNum);
    Event("booted into entrance " + std::to_string(entrance));
}

void OnTitleUpdate(void* gameState) {
    if (!sState.enabled || sState.booted || sState.scenario.empty()) {
        return;
    }
    if (++sState.bootWaitFrames < 30) {
        return;
    }
    BootNow();
}

} // namespace

bool IsEnabled() {
    return sState.enabled;
}

bool ExtractOnly() {
    return sState.enabled && sState.extractOnly;
}

void ParseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--coop-test") {
            sState.scenarioPath = ArgValue(argc, argv, i);
            sState.enabled = true;
        } else if (arg == "--coop-test-extract-only") {
            sState.extractOnly = true;
            sState.enabled = true;
        } else if (arg == "--coop-test-role") {
            sState.role = ArgValue(argc, argv, i);
        } else if (arg == "--coop-test-out") {
            sState.outPath = ArgValue(argc, argv, i);
        } else if (arg == "--coop-test-rom") {
            sState.romPath = ArgValue(argc, argv, i);
        } else if (arg == "--coop-test-host") {
            sState.host = ArgValue(argc, argv, i);
        } else if (arg == "--coop-test-port") {
            sState.port = atoi(ArgValue(argc, argv, i).c_str());
        }
    }
    if (!sState.enabled) {
        return;
    }
    if (!sState.scenarioPath.empty()) {
        try {
            std::ifstream f(sState.scenarioPath);
            sState.scenario = json::parse(f);
            sState.roleCfg = sState.scenario["roles"][sState.role];
            sState.steps = sState.roleCfg.value("steps", json::array());
        } catch (const std::exception& e) {
            fprintf(stderr, "[AutoTest] failed to load scenario %s: %s\n", sState.scenarioPath.c_str(), e.what());
            exit(2);
        }
    }
}

std::vector<std::string> ExtractArgs() {
    std::vector<std::string> args;
    if (!sState.romPath.empty()) {
        args.push_back(sState.romPath);
    }
    return args;
}

void ApplyConfig() {
    if (!sState.enabled || sState.scenario.empty()) {
        return;
    }
    const json serverCfg = sState.scenario.value("server", json::object());
    const std::string host = !sState.host.empty() ? sState.host : serverCfg.value("host", "127.0.0.1");
    const int port = sState.port != 0 ? sState.port : serverCfg.value("port", 21050);
    const std::string name = sState.roleCfg.value("name", "Test" + sState.role);

    CVarSetInteger("gZeldaOnline.Enabled", 1);
    CVarSetString("gZeldaOnline.Host", host.c_str());
    CVarSetInteger("gZeldaOnline.Port", port);
    CVarSetString("gZeldaOnline.Nickname", name.c_str());
    CVarSetString("gZeldaOnline.FileServer", ""); // never download skins in tests
    CVarSetString("gZeldaOnline.Skin", "");
    CVarSetInteger("gZeldaOnline.AutoConnect", 0); // the script connects explicitly
    CVarSetInteger("gZeldaOnline.Party.PrivateDungeons", 0);
    CVarSetInteger("gZeldaOnline.Party.StoryCoop", 0);
    CVarSetInteger("gZeldaOnline.Party.ShareAmmoRupeesHp", 0);
    CVarSetString("gZeldaOnline.Party.StoryCode", "");
    CVarSetInteger(CVAR_SETTING("Volume.Master"), 0);

    // Arbitrary extra CVars: scenario "cvars" then role "cvars" (role wins).
    for (const json* src : { &sState.scenario["cvars"], &sState.roleCfg["cvars"] }) {
        if (!src->is_object()) continue;
        for (auto it = src->begin(); it != src->end(); ++it) {
            if (it.value().is_string()) {
                CVarSetString(it.key().c_str(), it.value().get<std::string>().c_str());
            } else if (it.value().is_number_float()) {
                CVarSetFloat(it.key().c_str(), it.value().get<float>());
            } else if (it.value().is_boolean() || it.value().is_number_integer()) {
                CVarSetInteger(it.key().c_str(), it.value().is_boolean() ? (int)it.value().get<bool>()
                                                                          : it.value().get<int>());
            }
        }
    }
}

void RegisterHooks() {
    if (!sState.enabled || sState.scenario.empty()) {
        return;
    }
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnZTitleUpdate>(OnTitleUpdate);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrame);
}

} // namespace AutoTest
